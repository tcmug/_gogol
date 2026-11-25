-- gogol.nvim — semantic code search via the `gogol` CLI, as a LIVE snacks picker.
--
-- Behaves like built-in live_grep: open the picker, type in its input box, and results
-- update as you type — except gogol ranks by *meaning* (hybrid cosine + keyword, A3
-- blend) across the whole indexed repo, not by text match.
--
-- Scope: gogol is spawned with nvim's cwd, so it auto-resolves the current git repo.
--
-- Usage:
--   :Gogol            open the live picker (type to search)
--   :Gogol <query>    open pre-seeded with a query
--   <leader>sz        open the live picker
--   <leader>sZ        open pre-seeded with the word under cursor
--   (sg/sG stay LazyVim's live_grep; gogol lives on sz/sZ)

local M = {}

-- Extra delay (ms) before a live query is sent to gogol. Snacks adds its own 200ms
-- input debounce on top; raise this to wait longer before committing a search.
local GOGOL_DEBOUNCE_MS = 500

local function gogol_bin()
  if vim.fn.executable("gogol") == 1 then
    return "gogol"
  end
  -- Fallback: the build tree this plugin lives in (nvim/lua/gogol.lua → repo/build/gogol).
  local src = debug.getinfo(1, "S").source:sub(2)
  local repo = vim.fn.fnamemodify(vim.uv.fs_realpath(src) or src, ":p:h:h:h")
  local fallback = repo .. "/build/gogol"
  if vim.fn.executable(fallback) == 1 then
    return fallback
  end
  return nil
end

-- Custom list formatter: prepend a cosine% column to the built-in grep-style file line.
-- This is the gogol-specific triage signal — how semantically close each hit is — that
-- makes the list worth more than a plain grep result. Keyword-only hits (no cosine)
-- show "kw". Returns the snacks highlight-tuple list the picker renders.
local function gogol_format(item, picker)
  local ret = {}

  -- cosine column, right-aligned to 4 chars (e.g. " 72%", "  kw"), color by strength.
  local label, hl
  if type(item.cosine) == "number" then
    local pct = math.floor(item.cosine * 100 + 0.5)
    label = string.format("%3d%%", pct)
    -- Green when strong, dim when weak — a quick visual confidence cue.
    hl = pct >= 70 and "SnacksPickerGitStatusAdded"
      or pct >= 50 and "SnacksPickerLabel"
      or "SnacksPickerDimmed"
  else
    label = "  kw"
    hl = "SnacksPickerSpecial"
  end
  ret[#ret + 1] = { label, hl }
  ret[#ret + 1] = { " ", virtual = true }

  if item.note then
    -- Notes have no file: show a "note" tag + first line of the content instead.
    ret[#ret + 1] = { "note ", "SnacksPickerSpecial" }
    ret[#ret + 1] = { item.first_line or "", "SnacksPickerComment" }
    return ret
  end

  -- Append the built-in file formatter (path + :line, grep-style) after our column.
  vim.list_extend(ret, require("snacks.picker.format").file(item, picker))
  return ret
end

-- Fetch the full content of a note by id (recall snippets are truncated).
local function fetch_note(async, bin, id)
  local res
  vim.system({ bin, "get", id }, { text = true }, function(r)
    res = r
    -- Resume the finder coroutine on the main loop (callback runs in a fast context).
    vim.schedule(function()
      if async then async:resume() end
    end)
  end)
  if async then async:suspend() end
  if not res or res.code ~= 0 then
    return nil
  end
  local ok, decoded = pcall(vim.json.decode, res.stdout or "")
  if not ok or type(decoded) ~= "table" then
    return nil
  end
  return decoded.content
end

-- Open a note in a scratch buffer (not backed by any file; :w is disabled).
local function open_note_buffer(item)
  vim.cmd("enew")
  local buf = vim.api.nvim_get_current_buf()
  vim.bo[buf].buftype = "nofile"
  vim.bo[buf].bufhidden = "wipe"
  vim.bo[buf].swapfile = false
  vim.bo[buf].filetype = "markdown"
  vim.api.nvim_buf_set_name(buf, "gogol-note://" .. (item.note_id or "?"))
  vim.api.nvim_buf_set_lines(buf, 0, -1, false, vim.split(item.note or "", "\n", { plain = true }))
  vim.bo[buf].modifiable = false
end

-- A live finder: snacks calls this with the current input in ctx.filter.search and
-- re-calls it as the user types. We spawn gogol async and feed each hit to cb().
local function gogol_finder(opts, ctx)
  local query = ctx.filter.search
  -- Nothing typed yet → empty result set (picker shows its prompt, no spinner churn).
  if not query or query == "" then
    return function() end
  end

  local bin = gogol_bin()
  if not bin then
    return function() end
  end

  -- Return an async finder: snacks runs this inside its own coroutine (ctx.async).
  -- We spawn gogol with vim.system's callback, then suspend the finder coroutine until
  -- the process completes and resumes it — never blocking the UI thread (no :wait()).
  return function(cb)
    local async = ctx.async
    local result

    -- Extra debounce on top of snacks' built-in 200ms input debounce: wait, then bail
    -- if the query changed meanwhile (the newer keystroke's finder will run instead).
    if async then
      vim.defer_fn(function()
        pcall(function() async:resume() end)
      end, GOGOL_DEBOUNCE_MS)
      async:suspend()
    end
    if ctx.filter.search ~= query then
      return
    end

    vim.system(
      { bin, "recall", query, "--top", "20" },
      { text = true },
      function(res)
        result = res
        -- Resume the suspended finder coroutine on the main loop.
        vim.schedule(function()
          if async then async:resume() end
        end)
      end
    )

    -- Yield until the callback above resumes us.
    if async then async:suspend() end

    if not result or result.code ~= 0 then
      return
    end

    local ok, decoded = pcall(vim.json.decode, result.stdout or "")
    if not ok or type(decoded) ~= "table" or not decoded.results then
      return
    end

    for _, r in ipairs(decoded.results) do
      if r.source and r.source ~= "" then
        cb({
          text = r.source,          -- matcher/display
          file = r.source,          -- grep-style path + jump target
          pos = { r.line or 1, 0 }, -- jump + preview position
          preview = "file",         -- snacks: preview the file at pos
          cosine = r.cosine,        -- semantic similarity (nil for keyword-only hits)
          symbol = r.symbol,        -- chunk symbol, shown in the list
        })
      elseif r.type == "note" and r.id then
        -- Notes have no source file ("" in gogol's output). Fetch full content and
        -- show it in the preview; confirm opens it in a scratch buffer instead of a file.
        local content = fetch_note(async, bin, r.id) or r.snippet or ""
        local first = vim.split(content, "\n", { plain = true })[1] or ""
        cb({
          text = "note " .. content, -- matcher/display
          note = content,            -- full note body (scratch buffer + preview)
          note_id = r.id,
          first_line = first:sub(1, 200),
          cosine = r.cosine,
          preview = { text = content, ft = "markdown", loc = false },
        })
      end
    end
  end
end

-- Open the live picker. `seed` (optional) pre-fills the input box.
local function open(seed)
  local ok, Snacks = pcall(require, "snacks")
  if not ok then
    vim.notify("snacks.nvim is required", vim.log.levels.ERROR, { title = "gogol" })
    return
  end

  Snacks.picker({
    title = "gogol",
    finder = gogol_finder,
    format = gogol_format, -- cosine% column + grep-style file line
    preview = "preview",   -- per-item preview: file hits use "file", notes use item.preview text
    actions = {
      -- Notes open in a scratch buffer; file hits fall through to snacks' default jump.
      confirm = function(picker, item, action)
        local sel = picker:selected({ fallback = true })
        local first = sel[1] or item
        if first and first.note then
          picker:close()
          open_note_buffer(first)
          return
        end
        require("snacks.picker.actions").jump(picker, item, action)
      end,
    },
    live = true,           -- re-run finder as the user types (THE key setting)
    search = seed or "",   -- optional initial query
    -- gogol already ranks (A3); don't re-sort by snacks' matcher score.
  })
end

function M.open()
  open()
end

function M.open_cword()
  local word = vim.fn.expand("<cword>")
  if word == nil or word == "" then
    vim.notify("no word under cursor", vim.log.levels.WARN, { title = "gogol" })
    return
  end
  open(word)
end

function M.setup()
  vim.api.nvim_create_user_command("Gogol", function(opts)
    open(opts.args ~= "" and opts.args or nil)
  end, { nargs = "*", desc = "Semantic code search (gogol recall)" })
end

return M
