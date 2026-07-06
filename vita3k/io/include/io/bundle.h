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

// Game Bundle mount (read-only). See docs/game-bundle/brief.md.
//
// A "bundle" is a self-contained, already-decrypted game (base + merged update + DLC) plus a
// manifest. v1 backends: a plain on-disk directory (P0) and a STORE-mode zip (P1). Both expose the
// same read-only interface; the io op-surface serves app0:/addcont0: reads from the mounted bundle
// instead of ux0/app|addcont on the host FS. Saves/licenses always stay on the real FS.

#pragma once

#include <util/fs.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct IOState;

namespace bundle {

struct Stat {
    uint64_t size = 0;
    bool is_dir = false;
};

struct Dirent {
    std::string name;
    bool is_dir = false;
};

struct Manifest {
    int version = 0;
    std::string title_id;
    std::string content_id;
    std::string category; // "gd" for a normal game
    bool has_patch = false;
    std::vector<std::string> dlc; // DLC content IDs
};

// One opened file entry, owned by a bundle-backed FileStats fd. Backends implement random-access
// reads; the fd layer keeps the cursor.
class EntryReader {
public:
    virtual ~EntryReader() = default;
    // Read up to `len` bytes at absolute `offset`. Returns bytes read (>= 0), or -1 on error.
    virtual int64_t pread(void *buf, uint64_t offset, uint64_t len) = 0;
    virtual uint64_t size() const = 0;
};

// One opened directory listing, owned by a bundle-backed DirStats fd.
class DirReader {
public:
    std::vector<Dirent> entries;
    size_t cursor = 0;
};

// Read-only view over a decrypted game's tree. Keys are forward-slash, relative to the bundle root
// (e.g. "app/eboot.bin", "app/sce_sys/param.sfo", "addcont/<CONTENTID>/data.bin").
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool exists(const std::string &key) const = 0;
    virtual std::optional<Stat> stat(const std::string &key) const = 0;
    virtual std::vector<Dirent> list_dir(const std::string &key) const = 0;
    virtual std::shared_ptr<EntryReader> open(const std::string &key) const = 0;
    virtual bool read_whole(const std::string &key, std::vector<uint8_t> &out) const = 0;
};

// P0 backend: a plain decrypted folder containing vita3k_bundle.json, app/, addcont/, license/.
// Returns nullptr and sets `error_out` on failure (missing/invalid manifest, unreadable root).
std::shared_ptr<Backend> open_directory_backend(const fs::path &root, Manifest &manifest_out, std::string &error_out);

// Minimal reader for the flat, fixed-schema vita3k_bundle.json we emit. NOT a general JSON parser.
bool parse_manifest(const std::vector<uint8_t> &json_bytes, Manifest &out, std::string &error_out);

} // namespace bundle

// Active mount for the currently running title. Stored in IOState as shared_ptr<BundleMount>, set at
// boot and cleared in io_deinit. Single mount per emulated FS (Vita3K runs one app at a time).
struct BundleMount {
    std::shared_ptr<bundle::Backend> backend;
    bundle::Manifest manifest;
    std::string app_prefix; // "app/" + title_id     (matches translated app0: paths)
    std::string addcont_prefix; // "addcont/" + title_id (matches translated addcont0: paths)

    // If non-empty, a temporary directory this mount owns (e.g. a pkg decrypted for play-without-
    // install). io_deinit deletes it when the mount is cleared, so nothing persists after play.
    fs::path owned_temp;

    // Map a ux0-relative path (post-translate_path, forward-slash) to a bundle key. nullopt if the
    // path is not under a virtualized root (savedata, other devices, other titles).
    std::optional<std::string> map_ux0_path(std::string_view ux0_rel) const;
};

namespace bundle {

// Build a mount from a backend + manifest (fills the app/addcont prefixes from the title id).
std::shared_ptr<BundleMount> make_mount(std::shared_ptr<Backend> backend, const Manifest &manifest);

// Whole-file read for a ux0-relative path, if covered by io.mount.
//   returns nullopt   -> not mounted / not covered: caller should fall back to host FS
//   returns true/false -> handled by the bundle (success flag)
std::optional<bool> try_read_ux0_file(const IOState &io, const fs::path &ux0_rel, std::vector<uint8_t> &out);

// Existence check for a ux0-relative path, if covered by io.mount. For directories, "exists" means
// exists AND non-empty (matches is_addcont_exist semantics); for files, exists.
//   returns nullopt   -> not mounted / not covered: caller should fall back to host FS
//   returns true/false -> handled by the bundle
std::optional<bool> try_exists_ux0(const IOState &io, const fs::path &ux0_rel);

} // namespace bundle
