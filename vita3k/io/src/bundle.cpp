// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <io/bundle.h>
#include <io/state.h>

#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <cstdio>

namespace bundle {

// ---------------------------------------------------------------------------
// Directory backend (P0): a plain decrypted folder on disk.
// key -> <root>/<key>, with a case-insensitive component fallback so games that request
// wrong-case paths on a case-sensitive host FS still resolve (mirrors io.cpp behaviour).
// ---------------------------------------------------------------------------

namespace {

// Resolve a forward-slash bundle key to an actual host path under `root`, walking components with a
// case-insensitive fallback. Returns an empty path if nothing matches. Never escapes `root`.
fs::path resolve_icase(const fs::path &root, const std::string &key) {
    const fs::path exact = root / fs_utils::utf8_to_path(key);
    if (fs::exists(exact))
        return exact;

    fs::path cur = root;
    size_t start = 0;
    while (start < key.size()) {
        const size_t slash = key.find('/', start);
        const std::string comp = (slash == std::string::npos) ? key.substr(start) : key.substr(start, slash - start);
        start = (slash == std::string::npos) ? key.size() : slash + 1;
        if (comp.empty() || comp == ".")
            continue;
        if (comp == "..") // never traverse upward out of the bundle
            return {};

        const fs::path next = cur / fs_utils::utf8_to_path(comp);
        if (fs::exists(next)) {
            cur = next;
            continue;
        }

        bool found = false;
        boost::system::error_code ec;
        if (fs::is_directory(cur, ec)) {
            const std::string lc = string_utils::tolower(comp);
            for (const auto &entry : fs::directory_iterator(cur)) {
                if (string_utils::tolower(fs_utils::path_to_utf8(entry.path().filename())) == lc) {
                    cur = entry.path();
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            return {};
    }
    return cur;
}

class DirEntryReader : public EntryReader {
    mutable fs::ifstream stream;
    uint64_t m_size;

public:
    DirEntryReader(const fs::path &path, uint64_t size)
        : stream(path, std::ios::in | std::ios::binary)
        , m_size(size) {}

    bool good() const { return static_cast<bool>(stream); }

    int64_t pread(void *buf, uint64_t offset, uint64_t len) override {
        if (!stream || len == 0)
            return stream ? 0 : -1;
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream)
            return -1;
        stream.read(static_cast<char *>(buf), static_cast<std::streamsize>(len));
        // read() sets failbit on a short read at EOF; that is not an error for us.
        return static_cast<int64_t>(stream.gcount());
    }

    uint64_t size() const override { return m_size; }
};

class DirBackend : public Backend {
    fs::path m_root;

public:
    explicit DirBackend(fs::path root)
        : m_root(std::move(root)) {}

    bool exists(const std::string &key) const override {
        return !resolve_icase(m_root, key).empty();
    }

    std::optional<Stat> stat(const std::string &key) const override {
        const fs::path p = resolve_icase(m_root, key);
        if (p.empty())
            return std::nullopt;
        boost::system::error_code ec;
        Stat s;
        s.is_dir = fs::is_directory(p, ec);
        s.size = s.is_dir ? 0 : static_cast<uint64_t>(fs::file_size(p, ec));
        return s;
    }

    std::vector<Dirent> list_dir(const std::string &key) const override {
        std::vector<Dirent> out;
        const fs::path p = resolve_icase(m_root, key);
        boost::system::error_code ec;
        if (p.empty() || !fs::is_directory(p, ec))
            return out;
        for (const auto &entry : fs::directory_iterator(p)) {
            Dirent d;
            d.name = fs_utils::path_to_utf8(entry.path().filename());
            d.is_dir = fs::is_directory(entry.path(), ec);
            out.push_back(std::move(d));
        }
        return out;
    }

    std::shared_ptr<EntryReader> open(const std::string &key) const override {
        const fs::path p = resolve_icase(m_root, key);
        boost::system::error_code ec;
        if (p.empty() || fs::is_directory(p, ec))
            return nullptr;
        const auto size = static_cast<uint64_t>(fs::file_size(p, ec));
        auto reader = std::make_shared<DirEntryReader>(p, size);
        if (!reader->good())
            return nullptr;
        return reader;
    }

    bool read_whole(const std::string &key, std::vector<uint8_t> &out) const override {
        const fs::path p = resolve_icase(m_root, key);
        boost::system::error_code ec;
        if (p.empty() || fs::is_directory(p, ec))
            return false;
        return fs_utils::read_data(p, out);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Manifest reader. Minimal, tolerant reader for the flat fixed-schema vita3k_bundle.json we emit.
// NOT a general JSON parser: assumes no escape sequences inside the (alnum) string values. Adequate
// because we control the emitter; revisit if we ever vendor a JSON lib.
// ---------------------------------------------------------------------------

namespace {

std::optional<std::string> find_string(const std::string &s, const std::string &key) {
    const std::string pat = "\"" + key + "\"";
    const size_t k = s.find(pat);
    if (k == std::string::npos)
        return std::nullopt;
    const size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos)
        return std::nullopt;
    const size_t q1 = s.find('"', colon + 1);
    if (q1 == std::string::npos)
        return std::nullopt;
    const size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos)
        return std::nullopt;
    return s.substr(q1 + 1, q2 - q1 - 1);
}

// Raw scalar token (number / bool) after "key": up to the next , } ] or whitespace.
std::optional<std::string> find_scalar(const std::string &s, const std::string &key) {
    const std::string pat = "\"" + key + "\"";
    const size_t k = s.find(pat);
    if (k == std::string::npos)
        return std::nullopt;
    size_t p = s.find(':', k + pat.size());
    if (p == std::string::npos)
        return std::nullopt;
    ++p;
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])))
        ++p;
    const size_t begin = p;
    while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']' && !std::isspace(static_cast<unsigned char>(s[p])))
        ++p;
    if (p == begin)
        return std::nullopt;
    return s.substr(begin, p - begin);
}

std::vector<std::string> find_string_array(const std::string &s, const std::string &key) {
    std::vector<std::string> out;
    const std::string pat = "\"" + key + "\"";
    const size_t k = s.find(pat);
    if (k == std::string::npos)
        return out;
    const size_t open = s.find('[', k + pat.size());
    if (open == std::string::npos)
        return out;
    const size_t close = s.find(']', open + 1);
    if (close == std::string::npos)
        return out;
    size_t p = open + 1;
    while (p < close) {
        const size_t q1 = s.find('"', p);
        if (q1 == std::string::npos || q1 >= close)
            break;
        const size_t q2 = s.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > close)
            break;
        out.push_back(s.substr(q1 + 1, q2 - q1 - 1));
        p = q2 + 1;
    }
    return out;
}

} // namespace

bool parse_manifest(const std::vector<uint8_t> &json_bytes, Manifest &out, std::string &error_out) {
    const std::string s(json_bytes.begin(), json_bytes.end());

    const auto title_id = find_string(s, "title_id");
    if (!title_id || title_id->empty()) {
        error_out = "manifest missing title_id";
        return false;
    }

    const auto version = find_scalar(s, "version");
    out.version = version ? std::atoi(version->c_str()) : 0;
    out.title_id = *title_id;
    out.content_id = find_string(s, "content_id").value_or("");
    out.category = find_string(s, "category").value_or("gd");
    const auto has_patch = find_scalar(s, "has_patch");
    out.has_patch = has_patch && (*has_patch == "true" || *has_patch == "1");
    out.dlc = find_string_array(s, "dlc");
    return true;
}

std::shared_ptr<Backend> open_directory_backend(const fs::path &root, Manifest &manifest_out, std::string &error_out) {
    boost::system::error_code ec;
    if (!fs::is_directory(root, ec)) {
        error_out = "bundle root is not a directory: " + fs_utils::path_to_utf8(root);
        return nullptr;
    }

    std::vector<uint8_t> json_bytes;
    if (!fs_utils::read_data(root / "vita3k_bundle.json", json_bytes)) {
        error_out = "missing or unreadable vita3k_bundle.json";
        return nullptr;
    }
    if (!parse_manifest(json_bytes, manifest_out, error_out))
        return nullptr;
    if (manifest_out.version != 1) {
        error_out = "unsupported bundle version " + std::to_string(manifest_out.version);
        return nullptr;
    }

    return std::make_shared<DirBackend>(root);
}

// ---------------------------------------------------------------------------
// Mount + routing helpers.
// ---------------------------------------------------------------------------

std::shared_ptr<BundleMount> make_mount(std::shared_ptr<Backend> backend, const Manifest &manifest) {
    auto mount = std::make_shared<BundleMount>();
    mount->backend = std::move(backend);
    mount->manifest = manifest;
    mount->app_prefix = "app/" + manifest.title_id;
    mount->addcont_prefix = "addcont/" + manifest.title_id;
    return mount;
}

std::optional<bool> try_read_ux0_file(const IOState &io, const fs::path &ux0_rel, std::vector<uint8_t> &out) {
    if (!io.mount)
        return std::nullopt;
    const auto key = io.mount->map_ux0_path(ux0_rel.generic_string());
    if (!key)
        return std::nullopt;
    return io.mount->backend->read_whole(*key, out);
}

std::optional<bool> try_exists_ux0(const IOState &io, const fs::path &ux0_rel) {
    if (!io.mount)
        return std::nullopt;
    const auto key = io.mount->map_ux0_path(ux0_rel.generic_string());
    if (!key)
        return std::nullopt;
    const auto st = io.mount->backend->stat(*key);
    if (!st)
        return false;
    if (st->is_dir)
        return !io.mount->backend->list_dir(*key).empty();
    return true;
}

} // namespace bundle

// ---------------------------------------------------------------------------
// BundleMount::map_ux0_path
// ---------------------------------------------------------------------------

std::optional<std::string> BundleMount::map_ux0_path(std::string_view ux0_rel) const {
    const auto under = [&](const std::string &prefix, const char *root) -> std::optional<std::string> {
        if (ux0_rel == prefix)
            return std::string(root);
        if (ux0_rel.size() > prefix.size() && ux0_rel.compare(0, prefix.size(), prefix) == 0 && ux0_rel[prefix.size()] == '/')
            return std::string(root) + std::string(ux0_rel.substr(prefix.size()));
        return std::nullopt;
    };

    if (auto k = under(app_prefix, "app"))
        return k;
    if (auto k = under(addcont_prefix, "addcont"))
        return k;
    return std::nullopt;
}
