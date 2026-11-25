#include "mcp/mcp_server.h"
#include "mcp/json.h"

#include <arpa/inet.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace gogol { namespace mcp {

static const char* kStatusPageHtml() {
    return R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>gogol status</title>
  <style>
    :root {
      --bg: #10141a; --panel: #151d28; --panel-2: #1b2633; --line: #2a3b4d;
      --text: #edf5ff; --muted: #9fb5c9; --accent: #7cc7ff; --good: #7fe7a8; --warn: #f4c95d;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0; font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: var(--bg); color: var(--text); line-height: 1.5;
    }
    .wrap { max-width: 1100px; margin: 0 auto; padding: 24px 20px 40px; }
    h1 { margin: 0 0 20px; font-size: 2rem; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 16px; margin-bottom: 24px; }
    .card { background: var(--panel); border: 1px solid var(--line); border-radius: 12px; padding: 16px; }
    .label { color: var(--muted); font-size: .78rem; text-transform: uppercase; letter-spacing: .08em; }
    .value { font-size: 2rem; font-weight: 700; margin-top: 10px; }
    .panel { background: var(--panel); border: 1px solid var(--line); border-radius: 12px; padding: 18px; }
    .toolbar { display: flex; gap: 12px; flex-wrap: wrap; margin-bottom: 12px; }
    input, button { font: inherit; }
    input {
      flex: 1 1 250px; background: var(--panel-2); color: var(--text);
      border: 1px solid var(--line); border-radius: 8px; padding: 10px 12px;
    }
    button {
      background: var(--accent); color: #09131d; border: 0; border-radius: 8px;
      padding: 10px 16px; font-weight: 600; cursor: pointer;
    }
    table { width: 100%; border-collapse: collapse; margin-top: 12px; }
    th, td { text-align: left; padding: 10px 12px; border-bottom: 1px solid var(--line); vertical-align: top; }
    th { color: var(--muted); font-size: .8rem; letter-spacing: .05em; text-transform: uppercase; }
    .badge {
      display: inline-block; border-radius: 999px; padding: 2px 8px; font-size: .75rem;
      background: rgba(124,199,255,.2); color: var(--accent); border: 1px solid rgba(124,199,255,.4);
    }
    .muted { color: var(--muted); }
    .snippet { color: var(--text); }
    .hit { border-top: 1px solid var(--line); padding-top: 12px; margin-top: 12px; }
    .warn { color: var(--warn); }
    .statusline { margin: 8px 0 0; color: var(--muted); }
    .md { color: var(--text); }
    .md h1, .md h2, .md h3, .md h4, .md h5, .md h6 { margin: 1em 0 .5em; line-height: 1.2; }
    .md p, .md ul, .md ol, .md pre { margin: 0 0 1em; }
    .md pre { background: var(--panel-2); border: 1px solid var(--line); padding: 12px; border-radius: 8px; overflow-x: auto; }
    /* Code-type entries render as a bare <pre><code> in #entry-content (NOT under .md),
       so they'd otherwise get the UA default: no wrap, no overflow, no height bound ->
       long lines spill past the panel and tall files have no scroll. Contain both axes. */
    #entry-content pre {
      background: var(--panel-2); border: 1px solid var(--line); padding: 12px;
      border-radius: 8px; margin: 0; max-width: 100%; max-height: 70vh;
      overflow: auto; white-space: pre;
    }
    #entry-content pre code { white-space: pre; }
    .md code { background: rgba(255,255,255,.06); border-radius: 4px; padding: 2px 4px; }
    .md a { color: var(--accent); }
    .md ul, .md ol { padding-left: 1.5em; }
    .md table { display: block; overflow-x: auto; }
    .md blockquote {
      margin: 0 0 1em; padding-left: 12px; border-left: 3px solid var(--line); color: var(--muted);
    }
    .view-toggle { display: inline-flex; gap: 8px; margin-left: 12px; }
    .view-toggle button {
      padding: 5px 8px; border-radius: 6px; border: 1px solid var(--line);
      background: var(--panel-2); color: var(--text); cursor: pointer;
    }
    .view-toggle button.active { background: var(--accent); color: #09131d; border-color: var(--accent); }
    .hljs { color: #dfeaf3; background: transparent; }
    .hljs-keyword, .hljs-selector-tag, .hljs-subst { color: var(--accent); }
    .hljs-string, .hljs-attr, .hljs-symbol, .hljs-bullet, .hljs-addition { color: #7fe7a8; }
    .hljs-number, .hljs-literal, .hljs-type, .hljs-params { color: #f4c95d; }
    .hljs-comment, .hljs-quote { color: #7a90a6; font-style: italic; }
    .hljs-function, .hljs-title, .hljs-class .hljs-title { color: #ffd3a1; }
    .hljs-tag, .hljs-name, .hljs-attribute { color: #c78cff; }
    .hljs-link { color: #7cc7ff; }
    .code-view {
      display: block; overflow: auto; background: var(--panel-2); border: 1px solid var(--line);
      border-radius: 8px; padding: 8px 0; max-width: 100%;
    }
    .code-line { display: table-row; font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: 12px; line-height: 1.5; }
    .code-linenum { display: table-cell; width: 48px; padding: 0 10px 0 12px; color: var(--muted); text-align: right; user-select: none; }
    .code-content { display: table-cell; white-space: pre; padding-right: 12px; min-width: 320px; }
    @media (max-width: 640px) { .wrap { padding: 16px 12px 32px; } }
  </style>
</head>
<body>
  <div class="wrap">
    <div id="dashboard">
      <h1>gogol</h1>
      <div id="cards" class="grid"></div>
      <div class="panel" style="margin-bottom: 22px;">
        <h2 style="margin-top:0;">Index status</h2>
        <div id="index-table-wrap"></div>
      </div>
      <div class="panel" style="margin-bottom: 22px;">
        <h2 style="margin-top:0;">Agents <span id="bus-summary" class="muted" style="font-size: 13px; font-weight: normal;"></span></h2>
        <div id="bus-table-wrap"></div>
      </div>
      <div class="panel">
        <h2 style="margin-top:0;">Browser recall</h2>
        <div class="toolbar">
          <input id="recall-query" type="text" placeholder="Search code or notes..." />
          <input id="recall-scope" type="text" placeholder="Optional scope (path or repo name)" />
          <button id="search-button">Search</button>
        </div>
        <div id="recall-results" class="hits"></div>
      </div>
    </div>
    <div id="detail" class="panel" style="display:none"></div>
  </div>

  <script>
    function escapeHtml(s) {
      return String(s == null ? '' : s)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/\"/g, '&quot;');
    }

    let markedReady = null;
    let hljsReady = null;

    function loadMarked() {
      if (markedReady) return Promise.resolve(markedReady);
      markedReady = new Promise(function(resolve) {
        if (window.marked) return resolve(window.marked);
        const s = document.createElement('script');
        s.src = 'https://cdn.jsdelivr.net/npm/marked/marked.min.js';
        s.onload = function() { resolve(window.marked || null); };
        s.onerror = function() { resolve(null); };
        document.head.appendChild(s);
      });
      return markedReady;
    }

    function loadHljs() {
      if (hljsReady) return Promise.resolve(hljsReady);
      hljsReady = new Promise(function(resolve) {
        if (window.hljs) return resolve(window.hljs);
        const s = document.createElement('script');
        s.src = 'https://cdn.jsdelivr.net/npm/@highlightjs/cdn-assets@11/highlight.min.js';
        s.onload = function() { resolve(window.hljs || null); };
        s.onerror = function() { resolve(null); };
        document.head.appendChild(s);
      });
      return hljsReady;
    }

    function renderPlainText(content) {
      return '<pre><code>' + escapeHtml(content == null ? '' : String(content)) + '</code></pre>';
    }

    function languageFromSource(source) {
      const ext = String(source || '').split('.').pop().toLowerCase();
      const map = {
        ts: 'typescript', js: 'javascript', jsx: 'javascript', tsx: 'typescript',
        py: 'python', go: 'go', rs: 'rust', cpp: 'cpp', c: 'cpp', h: 'cpp', hpp: 'cpp',
        cc: 'cpp', php: 'php', md: 'markdown', yml: 'yaml', yaml: 'yaml', json: 'json',
        sh: 'bash', bash: 'bash', zsh: 'bash', ps1: 'powershell', java: 'java', cs: 'csharp'
      };
      return map[ext] || '';
    }

    function renderCodeEntry(content, source) {
      const lang = languageFromSource(source);
      const attr = lang ? ' class="language-' + escapeHtml(lang) + '"' : '';
      return '<pre><code' + attr + '>' + escapeHtml(content == null ? '' : String(content)) + '</code></pre>';
    }

    async function renderDetailContent(type, source, content) {
      if (type === 'code') {
        const wrap = document.createElement('div');
        wrap.innerHTML = renderCodeEntry(content, source);
        const hl = await loadHljs();
        if (hl) {
          wrap.querySelectorAll('pre code').forEach(function(block) {
            try { hl.highlightElement(block); } catch (e) { /* ignore */ }
          });
        }
        return wrap.innerHTML;
      }

      const marked = await loadMarked();
      if (marked) {
        try {
          marked.setOptions({ gfm: true, breaks: false, headerIds: false, mangle: false });
          let html = marked.parse(String(content == null ? '' : content));
          html = String(html).replace(/<script[\s\S]*?<\/script>/gi, '');
          html = String(html).replace(/<iframe[\s\S]*?<\/iframe>/gi, '');
          html = String(html).replace(/<object[\s\S]*?<\/object>/gi, '');
          const wrap = document.createElement('div');
          wrap.className = 'md';
          wrap.innerHTML = html;
          const hl = await loadHljs();
          if (hl) {
            wrap.querySelectorAll('pre code').forEach(function(block) {
              try { hl.highlightElement(block); } catch (e) { /* ignore */ }
            });
          }
          return wrap.outerHTML;
        } catch (e) {
          return '<div class="md">' + renderPlainText(content) + '</div>';
        }
      }
      return '<div class="md">' + renderPlainText(content) + '</div>';
    }

    async function applyCodeHighlight(root) {
      const hl = await loadHljs();
      if (!hl || !root) return;
      root.querySelectorAll('pre code').forEach(function(block) {
        try { hl.highlightElement(block); } catch (e) { /* ignore */ }
      });
    }

    function rpcCall(method, params) {
      return fetch('/mcp', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ jsonrpc: '2.0', id: Date.now(), method: method, params: params || {} })
      }).then(function(r) { return r.text(); }).then(function(txt) {
        const obj = JSON.parse(txt);
        if (obj && obj.result && obj.result.content && obj.result.content[0] && obj.result.content[0].text) {
          const text = obj.result.content[0].text;
          try { return JSON.parse(text); }
          catch (e) { return text; }
        }
        return obj;
      });
    }

    function fmtNumber(v) { return v == null ? '0' : Number(v).toLocaleString(); }

    function renderCards(store) {
      const cards = document.getElementById('cards');
      const values = [
        ['entries', store.entries],
        ['vec entries', store.vec_entries || 0],
        ['edges', store.edges],
        ['indexes', store.indexes],
        ['notes', store.notes]
      ];
      cards.innerHTML = values.map(function(item) {
        return '<div class="card"><div class="label">' + item[0] + '</div><div class="value">' + fmtNumber(item[1]) + '</div></div>';
      }).join('');
    }

    function renderIndexTable(indexList) {
      const wrap = document.getElementById('index-table-wrap');
      if (!indexList || !indexList.length) {
        wrap.innerHTML = '<div class="statusline">No indexes yet.</div>';
        return;
      }
      const rows = indexList.map(function(info) {
        const dirs = (info.dirs || []).map(function(d) {
          const badge = d.overlay ? '<span class="badge">overlay</span>' : '';
          return '<div>' + escapeHtml(d.dir) + ' ' + badge + '</div>';
        }).join('');
        const repoName = (info.dirs && info.dirs[0] && info.dirs[0].dir)
          ? info.dirs[0].dir.split(/[\\/]/).filter(Boolean).pop() || info.dirs[0].dir
          : (info.idx || '').split(/[\\/]/).filter(Boolean).pop() || info.idx || 'repo';
        const repoLabel = String(repoName).replace(/\.(git)$/i, '');
        const sha = info.root_sha || '';
        const display = sha
          ? '<span title="' + escapeHtml(sha) + '">' + escapeHtml(repoLabel) + '</span>'
          : '<span title="' + escapeHtml(info.idx || repoLabel) + '">' + escapeHtml(repoLabel) + '</span>';
        return '<tr><td>' + display + '</td><td>' + fmtNumber(info.entries) + '</td><td>' + dirs + '</td></tr>';
      }).join('');
      wrap.innerHTML = '<table><thead><tr><th>repo</th><th>entries</th><th>dirs</th></tr></thead><tbody>' + rows + '</tbody></table>';
    }

    function fmtAgo(ts) {
      if (!ts) return '—';
      const s = Math.max(0, Math.floor(Date.now() / 1000) - Number(ts));
      if (s < 60) return s + 's ago';
      if (s < 3600) return Math.floor(s / 60) + 'm ago';
      if (s < 86400) return Math.floor(s / 3600) + 'h ago';
      return Math.floor(s / 86400) + 'd ago';
    }

    function renderBus(bus) {
      const summary = document.getElementById('bus-summary');
      const wrap = document.getElementById('bus-table-wrap');
      bus = bus || {};
      const agents = bus.agents_list || [];
      summary.textContent = '· ' + (bus.online || 0) + ' online / ' + (bus.agents || 0)
        + ' seen · ' + fmtNumber(bus.total_sent || 0) + ' sent · ' + (bus.queued || 0) + ' queued';
      if (!agents.length) {
        wrap.innerHTML = '<div class="statusline">No agents have connected.</div>';
        return;
      }
      const rows = agents.map(function(a) {
        const dot = a.online
          ? '<span class="badge" style="background:#1f6f3f;color:#d6ffe4;">online</span>'
          : '<span class="muted">offline</span>';
        const q = (a.inbox_depth || 0) > 0
          ? '<strong>' + a.inbox_depth + '</strong>' : '0';
        return '<tr><td>' + escapeHtml(a.name) + '</td><td>' + dot + '</td><td>'
          + fmtNumber(a.sent || 0) + '</td><td>' + fmtNumber(a.received || 0) + '</td><td>'
          + q + '</td><td class="muted">' + fmtAgo(a.last_seen) + '</td></tr>';
      }).join('');
      wrap.innerHTML = '<table><thead><tr><th>agent</th><th>status</th><th>sent</th>'
        + '<th>received</th><th>queued</th><th>last seen</th></tr></thead><tbody>'
        + rows + '</tbody></table>';
    }

    function renderRecall(results) {
      const host = document.getElementById('recall-results');
      const hits = results && results.results ? results.results : [];
      if (!hits.length) {
        host.innerHTML = '<div class="statusline">No hits.</div>';
        return;
      }
      host.innerHTML = hits.map(function(hit) {
        const rawType = String(hit.type || 'doc');
        const rawId = String(hit.id || '');
        const href = '#/' + rawType + '/' + rawId;
        const score = (hit.cosine != null) ? hit.cosine : (hit.score != null ? hit.score : 0);
        const type = escapeHtml(rawType);
        const source = escapeHtml(hit.source || 'source');
        const symbol = escapeHtml(hit.symbol || '');
        const snippet = escapeHtml(hit.snippet || '');
        return '<div class="hit" data-href="' + escapeHtml(href) + '" onclick="window.location.hash = this.dataset.href;" style="cursor:pointer;">'
          + '<div><span class="badge">' + type + '</span> <strong>' + source + ':' + (hit.line || 0) + '</strong> — ' + symbol + ' <span class="muted">score=' + Number(score).toFixed(4) + '</span></div>'
          + '<div class="snippet">' + snippet + '</div>'
          + '</div>';
      }).join('');
    }

    async function openEntry(id) {
      try {
        const entry = await rpcCall('tools/call', { name: 'get', arguments: { id: id } });
        const panel = document.getElementById('detail');
        const type = (entry && entry.type) ? entry.type : 'note';
        const source = (entry && entry.source) ? entry.source : '(note)';
        const content = entry && entry.content ? entry.content : '';

        if (entry && entry.error) {
          panel.innerHTML = '<div style="margin-bottom: 12px;"><a href="#/">back</a></div><div class="statusline warn">' + escapeHtml(entry.error) + '</div>';
          document.getElementById('dashboard').style.display = 'none';
          panel.style.display = 'block';
          return;
        }

        const mdPreferred = type === 'note' || /(\.(md|mdx))$/i.test(source);
        const badge = '<span class="badge">' + escapeHtml(type) + '</span>';
        const controls = mdPreferred ? '<div class="view-toggle">'
          + '<button class="active" data-mode="code">Code</button>'
          + '<button data-mode="markdown">Markdown</button>'
          + '</div>' : '';

        panel.innerHTML = '<div style="margin-bottom: 16px; display:flex; align-items:center; flex-wrap:wrap;">'
          + '<div><a href="#/">back</a> &nbsp; ' + badge + ' <span class="muted">' + escapeHtml(source) + '</span> <span class="muted">' + escapeHtml(id) + '</span></div>'
          + controls
          + '</div>'
          + '<div id="entry-content">loading…</div>';

        const entryBody = document.getElementById('entry-content');
        const codeHtml = renderCodeEntry(content, source);
        const markdownHtml = await renderDetailContent(type, source, content);
        const modeButtons = panel.querySelectorAll('[data-mode]');

        function setMode(targetMode) {
          modeButtons.forEach(function(btn) {
            btn.classList.toggle('active', btn.getAttribute('data-mode') === targetMode);
          });
          entryBody.innerHTML = targetMode === 'markdown' ? markdownHtml : codeHtml;
          applyCodeHighlight(entryBody);
        }

        if (mdPreferred) {
          modeButtons.forEach(function(btn) {
            btn.addEventListener('click', function() {
              setMode(btn.getAttribute('data-mode'));
            });
          });
          setMode('code');
        } else {
          entryBody.innerHTML = codeHtml;
          applyCodeHighlight(entryBody);
        }

        document.getElementById('dashboard').style.display = 'none';
        panel.style.display = 'block';
        if (window.location.hash !== '#/' + type + '/' + id) {
          window.location.hash = '#/' + type + '/' + id;
        }
      } catch (err) {
        const panel = document.getElementById('detail');
        panel.innerHTML = '<div style="margin-bottom: 12px;"><a href="#/">back</a></div><div class="statusline warn">Unable to load entry: ' + escapeHtml(String(err)) + '</div>';
        document.getElementById('dashboard').style.display = 'none';
        panel.style.display = 'block';
      }
    }

    function route() {
      const match = (window.location.hash || '').match(/^#\/(note|doc|code)\/(.+)$/);
      if (match) {
        openEntry(match[2]);
        return;
      }
      document.getElementById('dashboard').style.display = '';
      document.getElementById('detail').style.display = 'none';
    }

    function refreshDashboard() {
      return rpcCall('tools/call', { name: 'status' }).then(function(data) {
        const store = (data && data.store) ? data.store : {};
        renderCards(store);
        renderIndexTable(store.index_list || []);
        renderBus((data && data.bus) ? data.bus : {});
      }).catch(function(err) {
        document.getElementById('cards').innerHTML = '<div class="card"><div class="label">status</div><div class="value warn">error</div></div>';
        document.getElementById('index-table-wrap').innerHTML = '<div class="statusline">Unable to load status: ' + String(err) + '</div>';
      });
    }

    function loadStatus() {
      refreshDashboard().then(function() { route(); });
    }

    // Live refresh: poll the dashboard data every few seconds, but only while the
    // dashboard is actually visible (not when drilled into a detail entry) and the tab
    // is in the foreground — so the Agents/message stats update without a manual reload.
    var LIVE_MS = 3000;
    setInterval(function() {
      const onDetail = /^#\/(note|doc|code)\//.test(window.location.hash || '');
      if (onDetail) return;
      if (document.hidden) return;
      refreshDashboard();
    }, LIVE_MS);

    function doRecall() {
      const q = document.getElementById('recall-query').value.trim();
      const scope = document.getElementById('recall-scope').value.trim();
      const args = { text: q, limit: 10 };
      if (scope) args.scope = scope;
      rpcCall('tools/call', { name: 'recall', arguments: args }).then(function(data) {
        renderRecall(data || {});
      }).catch(function(err) {
        document.getElementById('recall-results').innerHTML = '<div class="statusline warn">Recall failed: ' + String(err) + '</div>';
      });
    }

    document.getElementById('search-button').addEventListener('click', doRecall);
    document.getElementById('recall-query').addEventListener('keydown', function(e) {
      if (e.key === 'Enter') doRecall();
    });
    window.addEventListener('hashchange', route);

    loadStatus();
  </script>
</body>
</html>
)HTML";
}

static bool read_http_request(int fd, std::string& method, std::string& path, std::string& body) {
    std::string buf;
    char tmp[4096];
    while (true) {
        size_t hdr_end = buf.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            std::string headers = buf.substr(0, hdr_end);
            size_t sp1 = headers.find(' ');
            size_t sp2 = headers.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
            method = headers.substr(0, sp1);
            path = headers.substr(sp1 + 1, sp2 - sp1 - 1);

            size_t clen = 0;
            std::string low = headers;
            for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            size_t p = low.find("content-length:");
            if (p != std::string::npos) {
                char* end = nullptr;
                clen = static_cast<size_t>(std::strtoul(low.c_str() + p + 15, &end, 10));
                (void)end;
            }
            body = buf.substr(hdr_end + 4);
            while (body.size() < clen) {
                ssize_t n = ::read(fd, tmp, sizeof(tmp));
                if (n <= 0) return false;
                body.append(tmp, n);
            }
            return true;
        }
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n <= 0) return false;
        buf.append(tmp, n);
        if (buf.size() > (1u << 20)) return false;
    }
}

static void write_http(int fd, int status, const char* status_text,
                       const std::string& ctype, const std::string& body) {
    std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n";
    resp += "Content-Type: " + ctype + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Connection: close\r\n\r\n";
    resp += body;
    ::write(fd, resp.data(), resp.size());
}

static void handle_conn(int fd) {
    std::string method, path, body;
    if (!read_http_request(fd, method, path, body)) {
        ::close(fd);
        return;
    }

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        write_http(fd, 200, "OK", "text/html; charset=utf-8", kStatusPageHtml());
    } else if (method == "POST" && (path == "/mcp" || path == "/")) {
        // Route through the shared handler (parse + dispatch + error-wrap in one place).
        // A parse error comes back as a framed JSON-RPC error in the 200 body (per
        // JSON-RPC, errors travel in the response, not the HTTP status). nullopt = a
        // notification -> 202 with no body.
        if (auto resp = handle_request_json(body)) {
            write_http(fd, 200, "OK", "application/json", *resp);
        } else {
            write_http(fd, 202, "Accepted", "application/json", "");
        }
    } else {
        write_http(fd, 404, "Not Found", "text/plain", "not found");
    }
    ::close(fd);
}

int run_http_server(const std::string& host, int port) {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        fprintf(stderr, "bad host %s\n", host.c_str());
        return 1;
    }
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (::listen(srv, 16) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr, "gogol serve: http://%s:%d/  (MCP endpoint: POST /mcp)\n",
            host.c_str(), port);
    for (;;) {
        int fd = ::accept(srv, nullptr, nullptr);
        if (fd < 0) continue;
        std::thread(handle_conn, fd).detach();
    }
    return 0;
}

}} // namespace gogol::mcp
