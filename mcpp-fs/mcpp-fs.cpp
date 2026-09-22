#include "mcp_server.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <mutex>
#include <atomic>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;
using namespace mcpp_async;

#if defined(_WIN32)
#define MCP_POPEN  _popen
#define MCP_PCLOSE _pclose
#else
#define MCP_POPEN  popen
#define MCP_PCLOSE pclose
#endif

namespace {

    std::string toU8(const fs::path& p) {
        auto s = p.u8string();
        return std::string(s.begin(), s.end());
    }

    std::string toU8gen(const fs::path& p) {
        auto s = p.generic_u8string();
        return std::string(s.begin(), s.end());
    }

    fs::path fromU8(const std::string& s) {
#if defined(__cpp_lib_char8_t)
        return fs::path(std::u8string(s.begin(), s.end()));
#else
        return fs::u8path(s);
#endif
    }

    std::mutex g_wdMx;
    fs::path g_workdir = fs::current_path();

    // lock the entire session to the root, if we are given a root parameter
    fs::path g_rootLock;
    bool g_rootLocked = false;

    fs::path workdir() {
        std::lock_guard<std::mutex> lk(g_wdMx);
        return g_workdir;
    }

    // We stash the workdir in a temp file so a forced restart (The client could relaunch us at any given moment) doesn't drop us back to the process cwd
    // Written whenever it changes, read back on startup
    fs::path workdirFile() {
        std::error_code ec;
        return fs::temp_directory_path(ec) / "fs-mcp-workdir.txt";
    }

    void setWorkdir(const fs::path& p) {
        {
            std::lock_guard<std::mutex> lk(g_wdMx);
            g_workdir = p;
        }
        std::ofstream f(workdirFile(), std::ios::binary);
        if (f) {
            f << toU8(p);
        }
    }

    void loadPersistedWorkdir() {
        std::ifstream f(workdirFile(), std::ios::binary);
        if (!f) return;

        std::ostringstream ss;
        ss << f.rdbuf();
        std::string p = ss.str();

        // trim any trailing whitespace/newlines the file picked up
        while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' ' || p.back() == '\t')) {
            p.pop_back();
        }

        std::error_code ec;
        fs::path fp = fromU8(p);
        if (!p.empty() && fs::is_directory(fp, ec)) {
            std::lock_guard<std::mutex> lk(g_wdMx);
            g_workdir = fp;
        }
    }

    // Turn a path arg from the model into a real path. Absolute paths are used as-is;
    // anything relative (including "") hangs off the current workdir.
    fs::path resolvePath(const std::string& p) {
        if (p.empty()) return workdir();
        fs::path fp = fromU8(p);
        return fp.is_absolute() ? fp : (workdir() / fp);
    }

    // Did the client escape our root prison?
    bool withinRoot(const fs::path& p) {
        if (!g_rootLocked) return true;
        std::error_code ec;
        fs::path a = fs::weakly_canonical(p, ec);
        if (a.empty()) a = p.lexically_normal();
        fs::path rel = a.lexically_relative(g_rootLock);
        if (rel.empty()) return false;
        std::string s = toU8gen(rel);
        if (s == ".." || s.rfind("../", 0) == 0) return false; // escapes upward
        return true; // "." (equal) or below
    }

    std::string outsideRootMsg(const fs::path& p) {
        return "'" + toU8(p) + "' is outside the locked --root (" + toU8(g_rootLock) + ")";
    }

    // Directories we never bother descending into
    const std::set<std::string>& skipDirs() {
        static const std::set<std::string> s = { ".git", ".vs", ".idea", "build", "out", "x64", "Win32", "Debug", "Release", "node_modules", "target", "dist" };
        return s;
    }

    // Extensions we treat as binary, so text scans skip them
    bool isBinaryExt(const std::string& ext) {
        static const std::set<std::string> s = {
            ".exe", ".obj", ".pdb", ".ilk", ".idb", ".dll", ".lib", ".a", ".o",
            ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".pdf", ".zip",
            ".gz", ".7z", ".bin", ".class", ".jar", ".wav", ".mp3", ".mp4"
        };
        return s.count(ext) != 0;
    }

    // lowercase a copy of the string
    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    // Drop a trailing CR and clip long lines so results stay compact
    std::string tidy(const std::string& line, size_t cap = 200) {
        std::string s = line;
        if (!s.empty() && s.back() == '\r') s.pop_back();
        if (s.size() > cap) s = s.substr(0, cap) + " ...";
        return s;
    }

    // A cap on how big any one tool result can get. Whatever we return is re-fed
    // to the model on every later turn, so a bloated result costs tokens again and again
    // We want to feed it with a spoon, like a child, not like a cow
    std::string capOutput(std::string s, size_t maxBytes = 48000) {
        if (s.size() <= maxBytes) return s;
        s.resize(maxBytes);
        size_t nl = s.rfind('\n');
        if (nl != std::string::npos && nl > maxBytes / 2) s.resize(nl + 1);
        s += "... [output truncated at ~12k tokens -- narrow the query, lower maxResults, or read smaller ranges]\n";
        return s;
    }

    std::set<std::string> parseExts(const std::string& csv) {
        std::set<std::string> out;
        std::stringstream ss(csv);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            size_t b = tok.find_first_not_of(" \t");
            size_t e = tok.find_last_not_of(" \t");
            if (b == std::string::npos) continue;
            std::string t = tok.substr(b, e - b + 1);
            if (t.empty()) continue;
            if (t[0] != '.') t = "." + t;
            out.insert(lower(t));
        }
        return out;
    }


    // load_dir pulls a directory's files into RAM so repeated search/read/outline
    // over that tree don't keep hitting the disk.
    struct CacheEntry {
        std::string content;
        fs::file_time_type mtime;
        std::uintmax_t size = 0;
    };
    std::mutex g_cacheMx;
    std::unordered_map<std::string, CacheEntry> g_cache;
    std::set<std::string> g_loadedDirs;

    // A stable, disk-free key for a path: normalized, forward-slashed, and (on Windows) lowercased
    std::string cacheKey(const fs::path& p) {
        std::string k = toU8gen(p.lexically_normal());
#if defined(_WIN32)
        k = lower(k);
#endif
        return k;
    }

    bool readFile(const fs::path& path, std::string& out) {
        const std::string key = cacheKey(path);
        {
            std::lock_guard<std::mutex> lk(g_cacheMx);
            auto it = g_cache.find(key);
            if (it != g_cache.end()) {
                std::error_code ec;
                auto mt = fs::last_write_time(path, ec);
                auto sz = fs::file_size(path, ec);
                if (!ec && mt == it->second.mtime && sz == it->second.size) {
                    out = it->second.content; // served from memory, body not read
                    return true;
                }
                g_cache.erase(it); // changed or gone -> fall through to disk
            }
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    bool writeFile(const fs::path& path, const std::string& data) {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        f << data;
        f.close();

        const std::string key = cacheKey(path);
        std::lock_guard<std::mutex> lk(g_cacheMx);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            std::error_code ec;
            it->second.content = data;
            it->second.mtime = fs::last_write_time(path, ec);
            it->second.size = data.size();
        }
        return true;
    }

    std::vector<std::string> splitLines(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        std::istringstream in(s);
        while (std::getline(in, cur)) {
            if (!cur.empty() && cur.back() == '\r') {
                cur.pop_back();
            }
            out.push_back(cur);
        }
        return out;
    }

    std::vector<std::string> strArray(const rj::Value& a, const char* key) {
        std::vector<std::string> out;
        const rj::Value* v = args::get(a, key);
        if (v && v->IsArray()) {
            for (rj::SizeType i = 0; i < v->Size(); ++i) {
                if ((*v)[i].IsString()) {
                    out.push_back((*v)[i].GetString());
                }
            }
        }
        return out;
    }

    void parseRangeSpec(const std::string& s, std::string& path, long long& start, long long& count) {
        start = 1;
        count = 200;

        size_t colon = s.rfind(':');
        while (colon != std::string::npos) {
            std::string suf = s.substr(colon + 1);

            // The suffix has to be all digits, with at most one dash in the middle
            bool ok = !suf.empty() && suf[0] != '-';
            int dashes = 0;
            for (char c : suf) {
                if (c == '-') {
                    if (++dashes > 1) {
                        ok = false;
                        break;
                    }
                }
                else if (c < '0' || c > '9') {
                    ok = false;
                    break;
                }
            }

            if (ok) {
                path = s.substr(0, colon);
                size_t d = suf.find('-');
                if (d == std::string::npos) {
                    start = std::stoll(suf);
                    count = 200;
                }
                else {
                    long long a2 = std::stoll(suf.substr(0, d));
                    long long b2 = std::stoll(suf.substr(d + 1));
                    start = a2 < 1 ? 1 : a2;
                    count = b2 - start + 1;
                    if (count < 1) count = 1;
                }
                return;
            }

            // This colon wasn't a range separator so we back up and try the previous one.
            if (colon == 0) break;
            colon = s.rfind(':', colon - 1);
        }

        // Nothing looked like a range, so the whole thing is the path
        path = s;
    }

    void emitFileWindow(const std::string& displayPath, long long start, long long count, std::ostringstream& out) {
        if (start < 1) start = 1;
        if (count < 1) count = 1;
        if (count > 5000) count = 5000;

        std::string content;
        fs::path fp = resolvePath(displayPath);
        if (!withinRoot(fp)) {
            out << "== " << displayPath << " (outside --root) ==\n";
            return;
        }
        if (!readFile(fp, content)) {
            out << "== " << displayPath << " (cannot open) ==\n";
            return;
        }

        std::vector<std::string> lines = splitLines(content);
        long long last = start + count - 1;
        if (last > (long long)lines.size()) last = (long long)lines.size();

        out << "== " << displayPath << " (" << lines.size() << " lines) showing " << start << ".." << last << " ==\n";

        long long emitted = 0;
        for (long long i = start; i <= (long long)lines.size() && emitted < count; ++i, ++emitted) {
            out << i << ": " << tidy(lines[(size_t)i - 1], 1000) << "\n";
        }
    }

    // Translate a glob into a regex. '?' = one char, '*' = stay within a single path segment
    // '**' crosses '/'. A glob with a '/' in it gets matched against the path relative to the search root, otherwise just the filename
    // '**' collapses, so '**/test' still maches to a top level 'test'
    std::regex globToRegex(const std::string& glob) {
        std::string re = "^";
        for (size_t i = 0; i < glob.size(); ++i) {
            char c = glob[i];
            if (c == '*') {
                if (i + 1 < glob.size() && glob[i + 1] == '*') {
                    // '**': crosses directory separators
                    ++i;
                    if (i + 1 < glob.size() && glob[i + 1] == '/') {
                        ++i;
                        re += "(?:.*/)?";
                    }
                    else {
                        re += ".*";
                    }
                }
                else {
                    // single '*': stays within one segment
                    re += "[^/]*";
                }
            }
            else if (c == '?') {
                re += "[^/]";
            }
            else if (c == '.' || c == '(' || c == ')' || c == '+' || c == '|' ||
                c == '^' || c == '$' || c == '\\' || c == '{' || c == '}' ||
                c == '[' || c == ']') {
                // regex metacharacter, escape it
                re += '\\';
                re += c;
            }
            else {
                // everything else, including a literal '/'
                re += c;
            }
        }
        re += "$";
        return std::regex(re, std::regex::icase);
    }

    // search. grep across the tree and return "path:line: text" lines
    ToolResult toolSearch(const rj::Value& a, RequestContext& ctx) {
        // Accept one 'pattern' and/or a 'patterns' array. Everything is OR-matched in
        // a single pass, so several terms sweep the tree in one call.
        std::vector<std::string> pats;
        {
            std::string p0 = args::str(a, "pattern");
            if (!p0.empty()) pats.push_back(p0);
        }
        for (const std::string& p : strArray(a, "patterns")) {
            if (!p.empty()) pats.push_back(p);
        }
        if (pats.empty()) return ToolResult::error("search: 'pattern' or 'patterns' is required");

        std::string root = args::str(a, "path", ".");
        std::set<std::string> exts = parseExts(args::str(a, "exts"));
        bool useRegex = args::boolean(a, "regex", false);
        bool ignoreCase = args::boolean(a, "ignoreCase", false);
        long long before = args::integer(a, "before", 0);
        long long after = args::integer(a, "after", 0);
        long long maxResults = args::integer(a, "maxResults", 200);
        if (maxResults <= 0) maxResults = 200;

        // Compile the patterns up front: real regexes when regex=true, otherwise plain
        // substrings (lowercased if the search is case-insensitive).
        std::vector<std::regex> res;
        std::vector<std::string> needles;
        if (useRegex) {
            auto flags = std::regex::ECMAScript;
            if (ignoreCase) flags |= std::regex::icase;
            for (const std::string& p : pats) {
                try {
                    res.emplace_back(p, flags);
                }
                catch (const std::exception& ex) {
                    return ToolResult::error(std::string("search: bad regex '") + p + "': " + ex.what());
                }
            }
        }
        else {
            for (const std::string& p : pats) {
                needles.push_back(ignoreCase ? lower(p) : p);
            }
        }

        std::error_code ec;
        fs::path base = resolvePath(root);
        if (!withinRoot(base)) return ToolResult::error("search: " + outsideRootMsg(base));
        if (!fs::exists(base, ec)) return ToolResult::error("search: path not found: " + toU8(base));

        std::ostringstream out;
        long long hits = 0;
        bool truncated = false, cancelled = false;

        // Scan one file, appending matching lines (plus any context) to `out`. Returns
        // false once we hit the result cap, which stops the whole search.
        auto scanOne = [&](const fs::path& file, const std::string& rel) -> bool {
            std::string content;
            if (!readFile(file, content)) return true;
            std::vector<std::string> lines = splitLines(content);

            for (size_t i = 0; i < lines.size(); ++i) {
                bool match = false;
                if (useRegex) {
                    for (const std::regex& r : res) {
                        if (std::regex_search(lines[i], r)) {
                            match = true;
                            break;
                        }
                    }
                }
                else {
                    const std::string hay = ignoreCase ? lower(lines[i]) : lines[i];
                    for (const std::string& n : needles) {
                        if (hay.find(n) != std::string::npos) {
                            match = true;
                            break;
                        }
                    }
                }
                if (!match) continue;

                if (hits >= maxResults) {
                    truncated = true;
                    return false;
                }

                // Widen to the requested before/after context, clamped to the file.
                long long lo = (long long)i - before;
                if (lo < 0) lo = 0;
                long long hi = (long long)i + after;
                if (hi >= (long long)lines.size()) hi = (long long)lines.size() - 1;

                for (long long j = lo; j <= hi; ++j) {
                    char sep = (j == (long long)i) ? ':' : '-';
                    out << rel << sep << (j + 1) << sep << " " << tidy(lines[j]) << "\n";
                }
                if (before || after) out << "--\n";
                ++hits;
            }
            return true;
            };

        if (fs::is_regular_file(base, ec)) {
            // A single file was given, so this is just "grep this one file" - matching
            // lines only, no window. Handy for locating something in a known file.
            scanOne(base, toU8gen(base.filename()));
        }
        else {
            long long scanned = 0;
            fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
            for (; it != end && !ec; it.increment(ec)) {
                if (ctx.cancelled()) {   // client asked us to stop
                    cancelled = true;
                    break;
                }

                const fs::directory_entry& de = *it;
                if (de.is_directory(ec)) {
                    if (skipDirs().count(toU8(de.path().filename()))) it.disable_recursion_pending();
                    continue;
                }
                if (!de.is_regular_file(ec)) continue;

                // Ping the client every so often so a big scan doesn't look hung (and
                // clients that honour progress will keep the call open).
                if ((++scanned % 500) == 0) {
                    ctx.progress((double)scanned, -1.0,
                        "scanned " + std::to_string(scanned) + " files, " +
                        std::to_string(hits) + " matches");
                }

                // Filter by extension (or skip known-binary files when no exts were
                // given), and skip anything over 5 MB.
                std::string ext = lower(toU8(de.path().extension()));
                if (!exts.empty()) {
                    if (!exts.count(ext)) continue;
                }
                else if (isBinaryExt(ext)) {
                    continue;
                }
                if (de.file_size(ec) > 5 * 1024 * 1024) continue;

                std::string rel = toU8gen(fs::relative(de.path(), base, ec));
                if (rel.empty()) rel = toU8gen(de.path());
                if (!scanOne(de.path(), rel)) break;
            }
        }

        std::ostringstream head;
        head << hits << " match(es)";
        if (truncated) head << " (truncated at " << maxResults << " -- narrow the search)";
        if (cancelled) head << " (cancelled)";
        head << "\n" << out.str();
        return ToolResult::text(capOutput(head.str()));
    }

    // read_lines: return a numbered window of a file
    // args: path (req), start=1, count=200 - or a 'ranges' array for batch mode.
    ToolResult toolReadLines(const rj::Value& a) {
        // Batch mode: read several slices/files in one call. Fewer round-trips.
        std::vector<std::string> ranges = strArray(a, "ranges");
        if (!ranges.empty()) {
            std::ostringstream out;
            for (const std::string& spec : ranges) {
                std::string p;
                long long s, c;
                parseRangeSpec(spec, p, s, c);
                emitFileWindow(p, s, c, out);
            }
            return ToolResult::text(capOutput(out.str()));
        }

        std::string path = args::str(a, "path");
        if (path.empty()) return ToolResult::error("read_lines: 'path' or 'ranges' is required");

        // Start line comes from 'start', or its alias 'offset' - the built-in Read
        // tool calls it 'offset' and the model reaches for that out of habit, so we
        // accept either rather than ignoring it.
        long long start = args::has(a, "start") ? args::integer(a, "start", 1)
            : args::integer(a, "offset", 1);
        if (start < 1) start = 1;

        // Line count: 'end' (inclusive) wins, then 'count', then 'limit' (another
        // Read-ism), else 200. We used to silently drop end/offset/limit and always
        // hand back 200 lines from the top - all three are honoured now.
        long long count;
        if (args::has(a, "end")) {
            long long end = args::integer(a, "end", start);
            count = end - start + 1;
        }
        else if (args::has(a, "count")) {
            count = args::integer(a, "count", 200);
        }
        else {
            count = args::integer(a, "limit", 200);
        }
        if (count < 1) count = 1;
        if (count > 5000) count = 5000;

        std::string content;
        fs::path fp = resolvePath(path);
        if (!withinRoot(fp)) return ToolResult::error("read_lines: " + outsideRootMsg(fp));
        if (!readFile(fp, content)) return ToolResult::error("read_lines: cannot open: " + path);
        std::vector<std::string> lines = splitLines(content);

        std::ostringstream out;
        long long emitted = 0;
        for (long long i = start; i <= (long long)lines.size() && emitted < count; ++i, ++emitted) {
            out << i << ": " << tidy(lines[(size_t)i - 1], 1000) << "\n";
        }

        std::ostringstream head;
        head << path << " (" << lines.size() << " lines)  showing " << start << ".."
            << (start + emitted - (emitted ? 1 : 0));
        if (start + count - 1 < (long long)lines.size()) head << "  [more below]";
        head << "\n" << out.str();
        return ToolResult::text(capOutput(head.str()));
    }


    // outline. A rough "what is declared in this file" map. It is built for codebases
    // It is a heuristic and not a parser, in this case, it's better to overlist than miss something really important
    // return true on the usual declaration keywords, otherwise fall back to 
    // "looks like a function signature" pattern

    bool looksLikeDefinition(const std::string& raw) {
        // Ignore leading indentation, find the first real character.
        size_t b = raw.find_first_not_of(" \t");
        if (b == std::string::npos) return false;   // blank / whitespace line
        std::string s = raw.substr(b);

        // Drop a trailing line/block comment so we still get a match, even for heavy inline comments.
        // < int boo()  // this method scares you > We will still judge int boo here, ignoring the comment
        size_t cmt = std::min(s.find("//"), s.find("/*"));
        if (cmt != std::string::npos) s = s.substr(0, cmt);
        size_t e = s.find_last_not_of(" \t");        // re-trim the right side after cutting the comment
        if (e == std::string::npos) return false;    // line was ONLY a comment
        s = s.substr(0, e + 1);

        // These are impostor functions, we want to skip.
        // We always check controls before everything callable: lines like "for (...)" or
        // "catch (...)" look like a function further down. Vetoing them here stops that
        static const std::regex ctrl(
            R"(^(if|else|elif|for|foreach|while|do|switch|case|default|return|break|continue|goto|try|catch|finally|throw|synchronized|lock|with)\b)");
        if (std::regex_search(s, ctrl)) return false;

        // The obvious declaration keywords across a handful of languages.
        // Reads as: [visibility] [modifier] [template<...>] KEYWORD
        //   - visibility = public / private / pub / ... (optional)
        //   - modifier   = static / final / abstract / sealed (optional)
        //   - KEYWORD    = class, struct, enum, namespace, record, fn, def, ...
        // If a line starts this way it's a type/function declaration - accept it.
        static const std::regex kw(
            R"(^(export\s+|pub\s+|pub\(crate\)\s+|public\s+|private\s+|protected\s+|internal\s+)?(static\s+|final\s+|abstract\s+|sealed\s+)?(template\s*<.*>\s*)?(class|struct|union|enum|namespace|interface|trait|impl|module|concept|record|protocol|extension|object|actor|typedef|using\s+\w+\s*=|type\s+\w|def|async\s+def|fn|func|function)\b)");
        if (std::regex_search(s, kw)) return true;

        // Looks like a function signature: <return type> name(...) [quals] { or ;
        // Reads as: TYPE  NAME ( ARGS )  [const/noexcept/override/final/=0/-> type]  { or ;
        //   - TYPE  = the leading part (a return type, may carry ::, <>, *, &)
        //   - NAME  = a normal identifier, an "operator==" style name, or ~Destructor
        //   - the line MUST end in { (a definition) or ; (a prototype)
        // 
        // Looks like a function signature, reads as: TYPE NAME ( ARGS )
        // The leading return type is what separates a real declaration ("int boo(x);")
        // from a bare call ("boo(x);"), a call has no type in front, so it won't match
        static const std::regex func(
            R"(^[A-Za-z_][A-Za-z_0-9:<>,&\*~\s]*\b(operator\s*[^\s(]+|~?[A-Za-z_]\w*)\s*\([^;=]*\)\s*(const\s*)?(noexcept\s*)?(override\s*)?(final\s*)?(=\s*0\s*)?(->\s*[^;{]+)?[{;]\s*$)");
        if (std::regex_search(s, func)) return true;

        // Same idea as above but with NO return type in front, so it's easy to confuse with
        // a plain call. To stay safe we only trust it when the line actually opens a body with '{'
        static const std::regex funcDef(
            R"(^~?[A-Za-z_]\w*\s*\([^;=]*\)\s*(const\s*)?(noexcept\s*)?(:\s*[^{]+)?\{\s*$)");
        return std::regex_search(s, funcDef);
    }

    ToolResult toolOutline(const rj::Value& a) {
        // Take one path or multiple paths so we can outline several files at once
        std::vector<std::string> paths = strArray(a, "paths");
        {
            std::string p0 = args::str(a, "path");
            if (!p0.empty()) paths.insert(paths.begin(), p0);
        }
        if (paths.empty()) {
            return ToolResult::error("outline: 'path' or 'paths' is required");
        }

        std::ostringstream out;
        for (const std::string& path : paths) {
            std::string content;
            fs::path fp = resolvePath(path);
            if (!withinRoot(fp)) {
                out << path << " (outside --root)\n";
                continue;
            }
            if (!readFile(fp, content)) {
                out << path << " (cannot open)\n";
                continue;
            }

            std::vector<std::string> lines = splitLines(content);
            long long found = 0;
            std::ostringstream body;
            for (size_t i = 0; i < lines.size(); ++i) {
                if (looksLikeDefinition(lines[i])) {
                    body << (i + 1) << ": " << tidy(lines[i]) << "\n";
                    ++found;
                }
            }

            out << path << " (" << lines.size() << " lines, " << found << " declarations - heuristic map)\n" << body.str();
            if (paths.size() > 1) {
                out << "\n";
            }
        }
        return ToolResult::text(capOutput(out.str()));
    }

    // find files. List paths matching a glob, names only, not file contents
    // args: glob (like "*.cs"), path="/project/test", maxResults="300"
    ToolResult toolFindFiles(const rj::Value& a) {
        std::string glob = args::str(a, "glob");
        if (glob.empty()) return ToolResult::error("find_files: 'glob' is required");
        std::string root = args::str(a, "path", ".");
        long long maxResults = args::integer(a, "maxResults", 500);
        if (maxResults <= 0) maxResults = 500;

        std::regex re;
        try {
            re = globToRegex(glob);
        }
        catch (const std::exception& ex) {
            return ToolResult::error(std::string("find_files: bad glob: ") + ex.what());
        }

        // A glob with a '/' in it (including '**/') matches the path relative to the
        // root. A plain one matches just the filename, anywhere in the tree
        bool pathGlob = glob.find('/') != std::string::npos;

        std::error_code ec;
        fs::path base = resolvePath(root);
        if (!withinRoot(base)) return ToolResult::error("find_files: " + outsideRootMsg(base));
        if (!fs::exists(base, ec)) return ToolResult::error("find_files: path not found: " + toU8(base));

        std::ostringstream out;
        long long n = 0;
        bool truncated = false;
        fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end && !ec; it.increment(ec)) {
            const fs::directory_entry& de = *it;
            if (de.is_directory(ec)) {
                if (skipDirs().count(toU8(de.path().filename()))) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!de.is_regular_file(ec)) continue;

            std::string cand = pathGlob ? toU8gen(fs::relative(de.path(), base, ec)) : toU8(de.path().filename());
            if (!std::regex_match(cand, re)) continue;

            if (n >= maxResults) {
                truncated = true;
                break;
            }
            std::string rel = toU8gen(fs::relative(de.path(), base, ec));
            out << (rel.empty() ? toU8gen(de.path()) : rel) << "\n";
            ++n;
        }

        std::ostringstream head;
        head << n << " file(s)";
        if (truncated) {
            head << " (truncated at " << maxResults << ")";
        }
        head << "\n" << out.str();
        return ToolResult::text(capOutput(head.str()));
    }

    // list directory, show the immediate children of a directory, not recursive, recursive listing can be done by find files 
    // Lets the model explore layout one level at a time. Dirs come first with a trailing '/', then files
    ToolResult toolListDir(const rj::Value& a) {
        std::string p = args::str(a, "path", ".");
        bool showHidden = args::boolean(a, "showHidden", false);

        std::error_code ec;
        fs::path base = resolvePath(p);
        if (!withinRoot(base)) return ToolResult::error("list_dir: " + outsideRootMsg(base));
        if (!fs::exists(base, ec)) return ToolResult::error("list_dir: path not found: " + toU8(base));
        if (!fs::is_directory(base, ec)) return ToolResult::error("list_dir: not a directory: " + toU8(base));

        std::vector<std::string> dirs, files;
        fs::directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end && !ec; it.increment(ec)) {
            const fs::directory_entry& de = *it;
            std::string name = toU8(de.path().filename());
            if (!showHidden && !name.empty() && name[0] == '.') continue;
            if (de.is_directory(ec)) {
                dirs.push_back(name + "/");
            }
            else {
                files.push_back(name);
            }
        }
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end());

        std::ostringstream out;
        out << toU8gen(base) << "  (" << dirs.size() << " dir(s), " << files.size() << " file(s))\n";
        for (const auto& d : dirs)  out << d << "\n";
        for (const auto& f : files) out << f << "\n";
        return ToolResult::text(capOutput(out.str()));
    }

    // create_dir: make a directory, parents and all (like mkdir -p)
    // args: path (req)
    ToolResult toolCreateDir(const rj::Value& a) {
        std::string path = args::str(a, "path");
        if (path.empty()) return ToolResult::error("create_dir: 'path' is required");
        fs::path fp = resolvePath(path);
        if (!withinRoot(fp)) return ToolResult::error("create_dir: " + outsideRootMsg(fp));

        std::error_code ec;
        if (fs::exists(fp, ec)) {
            if (fs::is_directory(fp, ec)) return ToolResult::text("dir already exists: " + toU8(fp));
            return ToolResult::error("create_dir: path exists and is not a directory: " + toU8(fp));
        }
        fs::create_directories(fp, ec);
        if (ec) return ToolResult::error("create_dir: cannot create " + toU8(fp) + ": " + ec.message());
        return ToolResult::text("created dir " + toU8(fp));
    }

    // createfile. write a new file, creating any missing parent dirs on the way
    // args: path (req), content="", overwrite=false
    ToolResult toolCreateFile(const rj::Value& a) {
        std::string path = args::str(a, "path");
        if (path.empty()) return ToolResult::error("create_file: 'path' is required");
        std::string content = args::str(a, "content");
        bool overwrite = args::boolean(a, "overwrite", false);
        fs::path fp = resolvePath(path);
        if (!withinRoot(fp)) return ToolResult::error("create_file: " + outsideRootMsg(fp));

        std::error_code ec;
        if (fs::exists(fp, ec) && !overwrite) {
            return ToolResult::error("create_file: already exists (pass overwrite=true to replace): " + toU8(fp));
        }

        if (fp.has_parent_path()) {
            fs::create_directories(fp.parent_path(), ec);
            if (ec) {
                return ToolResult::error("create_file: cannot create parent dir: " + ec.message());
            }
        }
        if (!writeFile(fp, content)) {
            return ToolResult::error("create_file: cannot write: " + toU8(fp));
        }

        std::ostringstream head;
        head << (overwrite ? "wrote " : "created ") << toU8(fp) << " (" << content.size() << " bytes)";
        return ToolResult::text(capOutput(head.str()));
    }

    // apply_edit: replace an exact string in a file, only returns a confirmation whether replace was ok or not
    // don't need to feed everything back to the client.
    // args: path (req), find (req), replace (req), all=false
    ToolResult toolApplyEdit(const rj::Value& a) {
        std::string path = args::str(a, "path");
        std::string find = args::str(a, "find");
        if (path.empty() || find.empty()) {
            return ToolResult::error("apply_edit: 'path' and 'find' are required");
        }
        if (!args::has(a, "replace")) {
            return ToolResult::error("apply_edit: 'replace' is required");
        }
        std::string repl = args::str(a, "replace");
        bool all = args::boolean(a, "all", false);

        fs::path fp = resolvePath(path);
        if (!withinRoot(fp)) return ToolResult::error("apply_edit: " + outsideRootMsg(fp));
        std::string content;
        if (!readFile(fp, content)) return ToolResult::error("apply_edit: cannot open: " + path);

        // Be understanding and forgiving about newlines
        // AI model usually send '\n', but the files may be CRLF, a raw compare would fail
        // retype find/replace to whatever line ending the file already uses, it also gets to keep its own line-ending style this way
        {
            bool fileCrlf = content.find("\r\n") != std::string::npos;
            auto normNl = [fileCrlf](const std::string& t) {
                // First strip every CR so we're on plain LF
                std::string s;
                s.reserve(t.size());
                for (char c : t) {
                    if (c != '\r') s += c;
                }
                if (!fileCrlf) return s;

                // File is CRLF, so put a CR back in front of each LF
                std::string o;
                o.reserve(s.size() + s.size() / 8);
                for (char c : s) {
                    if (c == '\n') o += '\r';
                    o += c;
                }
                return o;
                };
            find = normNl(find);
            repl = normNl(repl);
        }

        // Walk the file, copying it through and swapping in the replacement at each hit, stop after the first one unless all=true
        size_t count = 0, pos = 0;
        std::string result;
        result.reserve(content.size());
        while (true) {
            size_t at = content.find(find, pos);
            if (at == std::string::npos) {
                result.append(content, pos, std::string::npos);
                break;
            }
            result.append(content, pos, at - pos);
            result += repl;
            pos = at + find.size();
            ++count;
            if (!all) {
                result.append(content, pos, std::string::npos);
                break;
            }
        }
        if (count == 0) {
            return ToolResult::error("apply_edit: 'find' text not present in " + path);
        }
        if (!writeFile(fp, result)) {
            return ToolResult::error("apply_edit: cannot write: " + path);
        }

        std::ostringstream head;
        head << "edited " << path << " (" << count << " replacement" << (count == 1 ? "" : "s") << ")";
        return ToolResult::text(capOutput(head.str()));
    }


    // load_dir reads a directory tree into g_cache on a BACKGROUND thread so the
    // client can fire it and move on. It returns a load-job id right away so progress
    // and completion show up in cache_status. Reads are served from RAM the moment
    // each file lands. A memory budget bounds how much it pulls in
    struct LoadJob {
        long long id = 0;
        std::string dir;
        std::atomic<size_t> loaded{ 0 };
        std::atomic<std::uintmax_t> bytes{ 0 };
        std::atomic<size_t> skippedBig{ 0 };
        std::atomic<size_t> skippedBin{ 0 };
        std::atomic<size_t> failed{ 0 };
        std::atomic<bool> done{ false };
        std::atomic<bool> capped{ false }; // stopped early on the memory budget
    };

    std::mutex g_loadJobsMx;
    std::map<long long, std::shared_ptr<LoadJob>> g_loadJobs;
    std::atomic<long long> g_loadJobSeq{ 1 };

    // The actual walk. Runs on a detached worker thread
    void runLoadJob(std::shared_ptr<LoadJob> job, fs::path base,
        std::set<std::string> exts, std::uintmax_t maxFileBytes,
        std::uintmax_t memBudget) {
        std::error_code ec;
        std::uintmax_t loadedBytes = 0;
        fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const fs::path& p = it->path();
            if (it->is_directory(ec)) {
                if (skipDirs().count(toU8(p.filename()))) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::string ext = lower(toU8(p.extension()));
            if (isBinaryExt(ext)) { job->skippedBin++; continue; }
            if (!exts.empty() && !exts.count(ext)) continue;
            std::uintmax_t sz = it->file_size(ec);
            if (ec) { ec.clear(); job->failed++; continue; }
            if (sz > maxFileBytes) { job->skippedBig++; continue; }

            std::string content;
            {
                std::ifstream f(p, std::ios::binary);
                if (!f) { job->failed++; continue; }
                std::ostringstream ss; ss << f.rdbuf(); content = ss.str();
            }
            CacheEntry e;
            e.size = content.size();
            e.mtime = fs::last_write_time(p, ec);
            e.content = std::move(content);
            {
                std::lock_guard<std::mutex> lk(g_cacheMx);
                g_cache[cacheKey(p)] = std::move(e);
            }
            loadedBytes += sz;
            job->loaded++;
            job->bytes.store(loadedBytes);

            if (loadedBytes >= memBudget) { job->capped = true; break; }
        }
        {
            std::lock_guard<std::mutex> lk(g_cacheMx);
            g_loadedDirs.insert(cacheKey(base));
        }
        job->done = true;
    }

    // load_dir: kick off a background load
    // args: path=".", memBudgetMB=256, maxFileSizeKB=2048, exts="", setRoot=false
    ToolResult toolLoadDir(const rj::Value& a) {
        std::string path = args::str(a, "path");
        fs::path base = resolvePath(path);
        std::error_code ec;
        if (!withinRoot(base)) return ToolResult::error("load_dir: " + outsideRootMsg(base));
        if (!fs::exists(base, ec)) return ToolResult::error("load_dir: path not found: " + toU8(base));
        if (!fs::is_directory(base, ec)) return ToolResult::error("load_dir: not a directory: " + toU8(base));

        long long budgetMB = args::integer(a, "memBudgetMB", 256);
        if (budgetMB < 1) budgetMB = 256;
        long long maxKB = args::integer(a, "maxFileSizeKB", 2048);
        if (maxKB < 1) maxKB = 2048;
        std::set<std::string> exts = parseExts(args::str(a, "exts"));
        bool setRoot = args::boolean(a, "setRoot", false);

        std::string rootNote;
        if (setRoot) {
            fs::path canon = fs::weakly_canonical(base, ec);
            fs::path target = canon.empty() ? base : canon;
            if (!withinRoot(target)) {
                return ToolResult::error("load_dir: setRoot target '" + toU8(target) + "' is outside the locked --root (" + toU8(g_rootLock) + ")");
            }
            setWorkdir(target);
            rootNote = "\nworkdir set to " + toU8(workdir());
        }

        auto job = std::make_shared<LoadJob>();
        job->id = g_loadJobSeq++;
        job->dir = toU8(base);
        {
            std::lock_guard<std::mutex> lk(g_loadJobsMx);
            g_loadJobs[job->id] = job;
        }

        std::thread(runLoadJob, job, base, exts,
            (std::uintmax_t)maxKB * 1024,
            (std::uintmax_t)budgetMB * 1024 * 1024).detach();

        std::ostringstream out;
        out << "load started for " << toU8(base) << " (load job " << job->id << ")\n"
            << "Files are streaming into memory in the background, up to a " << budgetMB
            << " MB budget. Reads hit RAM the moment each file lands.\n"
            << "Check progress with cache_status." << rootNote;
        return ToolResult::text(capOutput(out.str()));
    }

    // cache_clear: free cached files. No path -> clear everything; a path -> clear
    // just the entries under that directory
    ToolResult toolCacheClear(const rj::Value& a) {
        std::string path = args::str(a, "path");
        std::lock_guard<std::mutex> lk(g_cacheMx);
        if (path.empty()) {
            size_t n = g_cache.size();
            g_cache.clear();
            g_loadedDirs.clear();
            return ToolResult::text("cleared cache: freed " + std::to_string(n) + " file(s)");
        }
        std::string prefix = cacheKey(resolvePath(path));
        size_t removed = 0;
        for (auto it = g_cache.begin(); it != g_cache.end();) {
            if (it->first == prefix || it->first.rfind(prefix + "/", 0) == 0) {
                it = g_cache.erase(it);
                ++removed;
            }
            else {
                ++it;
            }
        }
        g_loadedDirs.erase(prefix);
        return ToolResult::text("cleared " + std::to_string(removed) + " cached file(s) under " + path);
    }

    // cache_status: totals held in memory plus any load jobs and their progress
    ToolResult toolCacheStatus(const rj::Value&) {
        std::ostringstream out;
        {
            std::lock_guard<std::mutex> lk(g_cacheMx);
            std::uintmax_t bytes = 0;
            for (const auto& kv : g_cache) bytes += kv.second.content.size();
            out << g_cache.size() << " file(s) in memory, " << (bytes / 1024) << " KB\n";
        }
        std::lock_guard<std::mutex> lk(g_loadJobsMx);
        if (g_loadJobs.empty()) {
            out << "no load jobs";
            return ToolResult::text(capOutput(out.str()));
        }
        out << "load jobs:\n";
        for (const auto& kv : g_loadJobs) {
            const LoadJob& j = *kv.second;
            out << "  job " << j.id << ": "
                << (j.done ? (j.capped ? "DONE (hit budget)" : "DONE") : "RUNNING")
                << ", " << j.loaded.load() << " file(s), " << (j.bytes.load() / 1024) << " KB";
            if (j.skippedBig) out << ", " << j.skippedBig.load() << " too big";
            if (j.skippedBin) out << ", " << j.skippedBin.load() << " binary";
            if (j.failed)     out << ", " << j.failed.load() << " failed";
            out << "  <- " << j.dir << "\n";
        }
        return ToolResult::text(capOutput(out.str()));
    }


    // run/job bookkeeping, all backed by files on disk so they outlive us (the server)
    // In a lot of cases, depending how this server is used, a client can decide to restart it's child mcp server
    // This is not in our control, so we run keep track of the commands/logs/results on disk so they can outlive us
    /*
    * This is what each file means:
    * .bat, the wrapper that runs the command, using a script is easier than dealing with cmd's rules for nested quotes
    * .log, the stdout/output of the ran command
    * .exit, this is written last, it only holds the exit code, later we can determine if a command failed or succeeded based on the presence of this file (or it's content)
    */

    std::mutex g_jobMx;

    fs::path jobsDir() {
        std::error_code ec;
        fs::path d = fs::temp_directory_path(ec) / "fs-mcp-jobs";
        fs::create_directories(d, ec);
        return d;
    }

    // Each job owns three files, keyed by its id, look at the comments above this one
    fs::path jobLog(long long id) {
        return jobsDir() / ("job-" + std::to_string(id) + ".log");
    }
    fs::path jobExit(long long id) {
        return jobsDir() / ("job-" + std::to_string(id) + ".exit");
    }
    fs::path jobBat(long long id) {
        return jobsDir() / ("job-" + std::to_string(id) + ".bat");
    }

    long long nextJobId() {
        std::lock_guard<std::mutex> lk(g_jobMx);
        long long mx = 0;
        std::error_code ec;
        for (fs::directory_iterator it(jobsDir(), ec), end; it != end && !ec; it.increment(ec)) {
            std::string fn = toU8(it->path().filename());
            if (fn.rfind("job-", 0) != 0) continue;
            size_t dot = fn.find('.', 4);
            if (dot == std::string::npos) continue;
            try {
                long long v = std::stoll(fn.substr(4, dot - 4));
                if (v > mx) mx = v;
            }
            catch (...) {
                // not a number we care, skip it
            }
        }
        return mx + 1;
    }

    // A snapshot of a job, read straight from its files
    struct JobView {
        bool exists = false;
        bool done = false;
        int exit = 0;
        std::string output;
    };

    JobView readJobFiles(long long id) {
        JobView v;
        bool haveLog = readFile(jobLog(id), v.output);
        std::string exitStr;
        bool haveExit = readFile(jobExit(id), exitStr);
        v.exists = haveLog || haveExit;

        if (haveExit) {
            size_t b = exitStr.find_first_not_of(" \t\r\n");
            if (b != std::string::npos) {
                v.done = true;
                try {
                    v.exit = std::stoi(exitStr.substr(b));
                }
                catch (...) {
                    v.exit = 0;
                }
            }
        }
        return v;
    }

    // Foreground run, block until the command finishes, get its output and exit code
    // and write the same job files a background run would so read_log can pull it by id later
    void runForeground(const std::string& cmd, long long id, std::string& output, int& code) {
        std::string base = toU8(workdir());
#if defined(_WIN32)
        std::string full = "cd /d \"" + base + "\" && " + cmd + " 2>&1";
#else
        std::string full = "cd \"" + base + "\" && " + cmd + " 2>&1";
#endif

        output.clear();
        FILE* p = MCP_POPEN(full.c_str(), "r");
        if (!p) {
            code = -1;
            output = "(failed to start command)";
        }
        else {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) {
                output.append(buf, n);
            }
            code = MCP_PCLOSE(p);
        }

        writeFile(jobLog(id), output);
        writeFile(jobExit(id), std::to_string(code) + "\n");
    }

    // Background run, start two detached OS processes that outlive us
    // 1. The command itself, write the .log so the client can read it
    // 2. A visible console window that tails the .log at real time so the user can watch it
    //    By default, it outlives the client or us, the server, so the user can see (better to have another window opened than debugging what happened)

    // We reroute through a tiny .bat to dodge cmd's nested-quote nightmare and to get a real %errorlevel% back out
    bool launchDetached(const std::string& cmd, long long id, std::string& err) {
        std::string wd = toU8(workdir());
        std::string log = toU8(jobLog(id));
        std::string done = toU8(jobExit(id));
        std::string sid = std::to_string(id);
        writeFile(jobLog(id), std::string()); // pre-create the log so the viewer can attach right away
#if defined(_WIN32)
        std::string script =
            "@echo off\r\n"
            "cd /d \"" + wd + "\"\r\n"
            + cmd + " > \"" + log + "\" 2>&1\r\n"
            "echo %errorlevel%> \"" + done + "\"\r\n"
            "echo [job " + sid + " finished, exit %errorlevel%]>> \"" + log + "\"\r\n";
        if (!writeFile(jobBat(id), script)) {
            err = "cannot write job script";
            return false;
        }
        // 1) the command: detached and headless, its stdio pointed at nul so it never touches the MCP pipe
        std::string build = "start \"\" /b cmd /c \"" + toU8(jobBat(id)) + "\" >nul 2>nul";
        std::system(build.c_str());
        // 2) the viewer: a real window titled with the job, tailing the log live
        std::string view =
            "start \"job " + sid + ": " + cmd + "\" powershell -NoProfile -Command "
            "\"Get-Content -LiteralPath '" + log + "' -Wait -Tail 5000\"";
        std::system(view.c_str());
#else
        std::string script =
            "#!/bin/sh\n"
            "cd \"" + wd + "\"\n"
            + cmd + " > \"" + log + "\" 2>&1\n"
            "echo $? > \"" + done + "\"\n"
            "echo \"[job " + sid + " finished, exit $?]\" >> \"" + log + "\"\n";
        if (!writeFile(jobBat(id), script)) {
            err = "cannot write job script";
            return false;
        }
        std::system(("sh \"" + toU8(jobBat(id)) + "\" >/dev/null 2>&1 &").c_str());
        std::system(("x-terminal-emulator -e sh -c 'tail -f \"" + log + "\"' >/dev/null 2>&1 &").c_str());
#endif
        return true;
    }

}

int main(int argc, char** argv) {
    Server s("fs-mcp-server", "1.0.0");
    loadPersistedWorkdir(); // if workdir exists, pick it up
    bool allowExec = true;


    // Set the root path for the server
    {
        std::string rootArg;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--no-exec") {
                allowExec = false;
            }
            if (a == "--root" && i + 1 < argc) {
                rootArg = argv[++i];
            }
        }
        if (!rootArg.empty()) {
            std::error_code ec;
            fs::path want = fromU8(rootArg);
            if (fs::is_directory(want, ec)) {
                fs::path canon = fs::weakly_canonical(want, ec);
                fs::path rootp = canon.empty() ? want : canon;
                g_rootLock = rootp; // this tree is now the boundary for the session
                g_rootLocked = true;
                setWorkdir(rootp);
                // diagnostics go to stderr, nothing from here should go to stdout, it is reserved for the JSON-RPC channel
                std::fprintf(stderr, "[fs-mcp] root locked to %s\n", toU8(workdir()).c_str());
            }
            else {
                std::fprintf(stderr, "[fs-mcp] --root '%s' is not a directory; ignoring\n", rootArg.c_str());
            }
        }
    }

    // A persistent memory at the beginning of each session
    s.setInstructions(
        "FIRST STEP: call set_workdir with your project root (e.g. the repo you "
        "are working in). All relative path arguments to search / find_files / "
        "list_dir / read_lines / outline / apply_edit / create_file / create_dir, "
        "and every 'run' command, resolve against that workdir. "
        "Discover layout with list_dir (one level) and find_files (recursive glob). "
        "Working in one directory a lot? Call load_dir on it once (optionally "
        "setRoot=true) to cache it in memory in the background, so repeated "
        "search / read_lines / outline are faster.");

    // search -> pattern across files
    {
        Tool t("search", "Find text/regex across files, returns 'path:line: text', not whole files. BATCH: pass several terms in 'patterns' to scan the tree ONCE (OR-matched) instead of calling search repeatedly.");
        t.addParameter("pattern", PropertyType::String, "Text or regex to find (or use 'patterns' for several)", false);
        {
            ToolParameter p("patterns", PropertyType::Array, "Batch: array of terms, OR-matched in a single scan. Prefer this over multiple search calls.", false);
            p.setItemType("string");
            t.addParameter(p);
        }
        t.addParameter("path", PropertyType::String, "Root dir, OR a single file to grep just that file (default '.'). To LOCATE inside a known file, search it here, it returns only matching lines, far leaner than read_lines.", false);
        t.addParameter("exts", PropertyType::String, "Comma list e.g. 'cpp,h' (default: all text files)", false);
        t.addParameter("regex", PropertyType::Boolean, "Treat pattern as regex", false);
        t.addParameter("ignoreCase", PropertyType::Boolean, "Case-insensitive", false);
        t.addParameter("before", PropertyType::Integer, "Context lines before each match", false);
        t.addParameter("after", PropertyType::Integer, "Context lines after each match", false);
        t.addParameter("maxResults", PropertyType::Integer, "Cap matches (default 200)", false);
        s.addToolCtx(t, toolSearch);
    }
    // read_lines -> a numbered window of one file
    {
        Tool t("read_lines", "Read numbered line windows. BATCH: pass 'ranges' to read MANY slices/files in ONE call -- far fewer turns (and tokens) than repeated single reads.");
        {
            ToolParameter p("ranges", PropertyType::Array, "Batch: array of 'path:start-end' (also 'path:start' for a default window, or 'path' for the whole file). Read everything you need in one call. Overrides the single-file params below.", false);
            p.setItemType("string");
            t.addParameter(p);
        }
        t.addParameter("path", PropertyType::String, "Single-file mode: file path", false);
        t.addParameter("start", PropertyType::Integer, "1-based start line (alias: offset; default 1)", false);
        t.addParameter("offset", PropertyType::Integer, "Alias for start", false);
        t.addParameter("end", PropertyType::Integer, "1-based inclusive end line (overrides count/limit if given)", false);
        t.addParameter("count", PropertyType::Integer, "How many lines (alias: limit; default 200; ignored if end is given)", false);
        t.addParameter("limit", PropertyType::Integer, "Alias for count", false);
        s.addTool(t, toolReadLines);
    }
    // outline -> compact declaration map of a file
    {
        Tool t("outline", "Compact map of a file's declarations (classes/functions/etc.) with line numbers, no bodies. BATCH: pass 'paths' to map several files at once.");
        t.addParameter("path", PropertyType::String, "Single file path (or use 'paths')", false);
        {
            ToolParameter p("paths", PropertyType::Array, "Batch: array of file paths to outline in one call.", false);
            p.setItemType("string");
            t.addParameter(p);
        }
        s.addTool(t, toolOutline);
    }
    // find_files -> list paths matching a glob, no contents
    {
        Tool t("find_files", "List file paths matching a glob (e.g. '*.cpp', or a path glob like 'code/**/CSMS*'). Paths only, no contents.");
        t.addParameter("glob", PropertyType::String, "Glob: '*' (one segment), '**' (crosses '/'), '?'. No '/' = match filename anywhere, with '/' = match path relative to 'path'.");
        t.addParameter("path", PropertyType::String, "Root dir (default '.')", false);
        t.addParameter("maxResults", PropertyType::Integer, "Cap results (default 500)", false);
        s.addTool(t, toolFindFiles);
    }
    // list_dir -> immediate/1st level children of one directory
    {
        Tool t("list_dir", "List the immediate contents of a directory (dirs first, suffixed '/', then files), non-recursive -- use to discover layout one level at a time.");
        t.addParameter("path", PropertyType::String, "Dir to list (default '.')", false);
        t.addParameter("showHidden", PropertyType::Boolean, "Include dot-entries (default false)", false);
        s.addTool(t, toolListDir);
    }
    // apply_edit -> targeted string replace
    {
        Tool t("apply_edit", "Replace exact text in a file, returns a small confirmation instead of the whole file.");
        t.addParameter("path", PropertyType::String, "File path");
        t.addParameter("find", PropertyType::String, "Exact text to replace");
        t.addParameter("replace", PropertyType::String, "Replacement text");
        t.addParameter("all", PropertyType::Boolean, "Replace all occurrences (default: first only)", false);
        s.addTool(t, toolApplyEdit);
    }
    // create_dir -> "mkdir -p" a directory
    {
        Tool t("create_dir", "Create a directory (and any missing parents, like 'mkdir -p').");
        t.addParameter("path", PropertyType::String, "Dir to create (relative to workdir or absolute)");
        s.addTool(t, toolCreateDir);
    }
    // create_file -> write a new file, creating missing parent dirs
    {
        Tool t("create_file", "Create a new file (missing parent dirs are created). Refuses to clobber unless overwrite=true.");
        t.addParameter("path", PropertyType::String, "File to create (relative to workdir or absolute)");
        t.addParameter("content", PropertyType::String, "Initial file contents (default empty)", false);
        t.addParameter("overwrite", PropertyType::Boolean, "Replace if it already exists (default false)", false);
        s.addTool(t, toolCreateFile);
    }

    // don't expose the tools at all if the user does not want command execution
    if (allowExec) {
        // run -> execute a command
        {
            Tool t("run", "Run a shell command (e.g. git) in the session workdir. Foreground returns exit code + last lines + a log_id. For anything slow, pass background:true -- it opens a visible window you can watch, returns a job_id at once, and keeps running even if this call is cancelled or the server restarts. Wait for it with 'job_wait'.");
            t.addParameter("command", PropertyType::String, "Command line to execute (runs in the workdir set by set_workdir)");
            t.addParameter("tail", PropertyType::Integer, "How many trailing lines to return (default 20)", false);
            t.addParameter("background", PropertyType::Boolean, "Launch detached (visible window + on-disk log) and return a job_id at once. Use for long/slow commands. Default false.", false);
            s.addTool(t, [](const rj::Value& a) {
                std::string cmd = args::str(a, "command");
                if (cmd.empty()) return ToolResult::error("run: 'command' is required");
                long long tailN = args::integer(a, "tail", 20);
                if (tailN < 0) tailN = 20;
                bool background = args::boolean(a, "background", false);

                long long id = nextJobId();

                if (background) {
                    std::string err;
                    if (!launchDetached(cmd, id, err)) return ToolResult::error("run: " + err);
                    std::ostringstream out;
                    out << "started background job " << id << " (job_id " << id << ", log_id " << id << ")\n"
                        << "A window opened so you can watch it live.\n"
                        << "Wait for it:  job_wait id=" << id << "   Status:  job id=" << id
                        << "   Full log:  read_log log_id=" << id << "\n"
                        << "Survives a server/Claude restart (state is on disk).";
                    return ToolResult::text(capOutput(out.str()));
                }

                std::string output;
                int code;
                runForeground(cmd, id, output, code);
                std::vector<std::string> lines = splitLines(output);
                std::ostringstream out;
                out << "exit " << code << ", " << lines.size() << " lines (log_id " << id << ")\n";
                long long from = (long long)lines.size() - tailN;
                if (from < 0) from = 0;
                for (long long i = from; i < (long long)lines.size(); ++i) {
                    out << lines[(size_t)i] << "\n";
                }
                return ToolResult::text(capOutput(out.str()));
                });
        }
        // job -> non-blocking status snapshot of a background job
        {
            Tool t("job", "Check a background 'run' job: RUNNING or DONE + exit code, plus the last lines of its output. Non-blocking snapshot, to wait, use job_wait.");
            t.addParameter("id", PropertyType::Integer, "job_id returned by run(background:true)");
            t.addParameter("tail", PropertyType::Integer, "How many trailing lines to return (default 20)", false);
            s.addTool(t, [](const rj::Value& a) {
                long long id = args::integer(a, "id", 0);
                long long tailN = args::integer(a, "tail", 20);
                if (tailN < 0) tailN = 20;
                JobView v = readJobFiles(id);
                if (!v.exists) return ToolResult::error("job: unknown id " + std::to_string(id));

                std::vector<std::string> lines = splitLines(v.output);
                std::ostringstream out;
                if (v.done) out << "job " << id << ": DONE, exit " << v.exit;
                else        out << "job " << id << ": RUNNING";
                out << ", " << lines.size() << " lines so far (log_id " << id << ")\n";
                long long from = (long long)lines.size() - tailN;
                if (from < 0) from = 0;
                for (long long i = from; i < (long long)lines.size(); ++i) {
                    out << lines[(size_t)i] << "\n";
                }
                return ToolResult::text(capOutput(out.str()));
                });
        }
        // job_wait -> long-poll a job's files until it finishes (or times out)
        // The default timeout is between 45-50 seconds, that's how long usually client wait before restarting an stdio mcp server 
        {
            Tool t("job_wait", "Wait for a background 'run' job to finish, then return its exit code + tail. Polls the job's on-disk state and returns THE MOMENT it completes (or, if still running after 'timeout' seconds, a RUNNING status -- just call again). Use this instead of sleeping.");
            t.addParameter("id", PropertyType::Integer, "job_id returned by run(background:true)");
            t.addParameter("timeout", PropertyType::Integer, "Max seconds to block (default 45, max 50 -- kept under the client timeout so it never forces a server restart).", false);
            t.addParameter("tail", PropertyType::Integer, "How many trailing lines to return (default 20)", false);
            s.addTool(t, [](const rj::Value& a) {
                long long id = args::integer(a, "id", 0);
                long long timeoutS = args::integer(a, "timeout", 45);
                long long tailN = args::integer(a, "tail", 20);
                if (timeoutS < 1) timeoutS = 1;
                if (timeoutS > 50) timeoutS = 50;
                if (tailN < 0) tailN = 20;

                JobView v = readJobFiles(id);
                if (!v.exists) {
                    return ToolResult::error("job_wait: unknown id " + std::to_string(id));
                }
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutS);
                while (!v.done && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    v = readJobFiles(id);
                }

                std::vector<std::string> lines = splitLines(v.output);
                std::ostringstream out;
                if (v.done) {
                    out << "job " << id << ": DONE, exit " << v.exit;
                }
                else {
                    out << "job " << id << ": RUNNING (still going after " << timeoutS << "s -- call job_wait again)";
                }
                out << ", " << lines.size() << " lines so far (log_id " << id << ")\n";
                long long from = (long long)lines.size() - tailN;
                if (from < 0) from = 0;
                for (long long i = from; i < (long long)lines.size(); ++i) {
                    out << lines[(size_t)i] << "\n";
                }
                return ToolResult::text(capOutput(out.str()));
                });
        }
        // read_log -> fetch a window of a run/job log by its id (reads the log file)
        {
            Tool t("read_log", "Read a numbered window of a captured run/job log by its log_id.");
            t.addParameter("log_id", PropertyType::Integer, "id returned by run or job");
            t.addParameter("start", PropertyType::Integer, "1-based start line (default 1)", false);
            t.addParameter("count", PropertyType::Integer, "How many lines (default 200)", false);
            s.addTool(t, [](const rj::Value& a) {
                long long id = args::integer(a, "log_id", 0);
                JobView v = readJobFiles(id);
                if (!v.exists) return ToolResult::error("read_log: unknown log_id");

                long long start = args::integer(a, "start", 1);
                long long count = args::integer(a, "count", 200);
                if (start < 1) start = 1;
                if (count < 1) count = 200;

                std::vector<std::string> lines = splitLines(v.output);
                std::ostringstream out;
                out << "log " << id << " (" << lines.size() << " lines)  showing " << start << "..\n";
                long long emitted = 0;
                for (long long i = start; i <= (long long)lines.size() && emitted < count; ++i, ++emitted) {
                    out << i << ": " << tidy(lines[(size_t)i - 1], 1000) << "\n";
                }
                return ToolResult::text(capOutput(out.str()));
                });
        }
    }

    // set_workdir -> set the session base dir so all path args (and run) can be short
    {
        Tool t("set_workdir", "Set the session working directory. All relative path args (search/find_files/read_lines/outline/apply_edit) and run commands resolve against it. Call with no path to just report the current one.");
        t.addParameter("path", PropertyType::String, "New base dir (absolute, or relative to the current one). Omit to query.", false);
        s.addTool(t, [](const rj::Value& a) {
            std::string p = args::str(a, "path");
            if (!p.empty()) {
                fs::path want = resolvePath(p);
                std::error_code ec;
                if (!fs::is_directory(want, ec)) {
                    return ToolResult::error("set_workdir: not a directory: " + toU8(want));
                }
                fs::path canon = fs::weakly_canonical(want, ec);
                fs::path target = canon.empty() ? want : canon;
                if (!withinRoot(target)) {
                    return ToolResult::error("set_workdir: '" + toU8(target) + "' is outside the locked --root (" + toU8(g_rootLock) + "); staying at " + toU8(workdir()));
                }
                setWorkdir(target);
            }
            return ToolResult::text("workdir: " + toU8(workdir()));
            });
    }

    // load_dir -> cache a directory tree in memory (background) for faster repeated access
    {
        Tool t("load_dir", "Load a directory's files into memory in the BACKGROUND so later search / read_lines / outline over that tree are served from RAM instead of disk. Returns a load-job id at once -- watch it with cache_status. Reads stay correct: a cached file is re-checked by mtime+size and refreshed if it changed on disk. Point it at the directory you're working in.");
        t.addParameter("path", PropertyType::String, "Directory to load (relative to workdir or absolute; default '.')", false);
        t.addParameter("memBudgetMB", PropertyType::Integer, "Stop after caching roughly this many MB (default 256)", false);
        t.addParameter("maxFileSizeKB", PropertyType::Integer, "Skip any single file larger than this many KB (default 2048)", false);
        t.addParameter("exts", PropertyType::String, "Only load these extensions, comma list e.g. 'cpp,h' (default: all text files)", false);
        t.addParameter("setRoot", PropertyType::Boolean, "Also make this directory the session root (workdir), so later relative paths resolve here (default false)", false);
        s.addTool(t, toolLoadDir);
    }
    // cache_clear -> free cached files
    {
        Tool t("cache_clear", "Free files loaded by load_dir. With no path, clears the whole cache; with a path, clears just the files under that directory.");
        t.addParameter("path", PropertyType::String, "Directory to drop from the cache (default: clear everything)", false);
        s.addTool(t, toolCacheClear);
    }
    // cache_status -> what's held in memory + load-job progress
    {
        Tool t("cache_status", "Show how many files (and how much memory) are cached, plus any load_dir jobs and their progress.");
        s.addTool(t, toolCacheStatus);
    }

    return s.run();   // hand off to the server loop (talks over stdio)
}

