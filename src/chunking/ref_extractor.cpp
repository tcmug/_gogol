// ref_extractor.cpp — reference extractor implementations
#include "chunking/ref_extractor.h"

// --- MarkdownLinkExtractor ---

bool MarkdownLinkExtractor::supports(const std::string &ext) const {
    return ext == ".md" || ext == ".mdx";
}

std::vector<DocRef> MarkdownLinkExtractor::extract(const std::string &content) const {
    std::vector<DocRef> refs;
    bool in_code = false;

    size_t i = 0;
    while (i < content.size()) {
        // Find end of line
        size_t eol = content.find('\n', i);
        if (eol == std::string::npos)
            eol = content.size();
        std::string line = content.substr(i, eol - i);

        // Track code fences (same pattern as chunk_markdown)
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            in_code = !in_code;
            i = eol + 1;
            continue;
        }

        if (!in_code) {
            // Scan the line for [text](target), possibly multiple per line.
            size_t p = 0;
            while (p < line.size()) {
                size_t open_br = line.find('[', p);
                if (open_br == std::string::npos)
                    break;

                size_t close_br = line.find(']', open_br + 1);
                if (close_br == std::string::npos)
                    break;

                // The '(' must immediately follow ']'
                if (close_br + 1 >= line.size() || line[close_br + 1] != '(') {
                    p = open_br + 1;
                    continue;
                }

                size_t open_par = close_br + 1;
                size_t close_par = line.find(')', open_par + 1);
                if (close_par == std::string::npos)
                    break;

                // Skip images: ![text](target) — a link preceded by '!'.
                if (open_br > 0 && line[open_br - 1] == '!') {
                    p = close_par + 1;
                    continue;
                }

                std::string text = line.substr(open_br + 1, close_br - open_br - 1);
                std::string target =
                    line.substr(open_par + 1, close_par - open_par - 1);

                // Trim whitespace from target
                size_t ts = target.find_first_not_of(" \t");
                size_t te = target.find_last_not_of(" \t");
                if (ts == std::string::npos)
                    target.clear();
                else
                    target = target.substr(ts, te - ts + 1);

                refs.push_back({text, target});
                p = close_par + 1;
            }
        }

        i = eol + 1;
    }

    return refs;
}

// --- default_ref_extractors ---

std::vector<std::unique_ptr<IRefExtractor>> default_ref_extractors() {
    std::vector<std::unique_ptr<IRefExtractor>> extractors;
    extractors.push_back(std::make_unique<MarkdownLinkExtractor>());
    return extractors;
}
