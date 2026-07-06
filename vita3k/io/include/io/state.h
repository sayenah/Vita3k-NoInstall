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

#pragma once

#include <io/filesystem.h>
#include <io/types.h>
#include <io/util.h>

#include <map>
#include <unordered_map>

// Game Bundle mount types (see io/bundle.h). Forward-declared and held behind shared_ptr so state.h
// stays free of a circular include; bundle.h includes state.h, not the reverse.
namespace bundle {
class EntryReader;
class DirReader;
} // namespace bundle
struct BundleMount;

// Class for all needed information to access files on Vita3K.
class FileStats : public VitaStats {
    // Shared file pointer
    FilePtr wrapped_file;

    // When set, this fd is served read-only from a mounted Game Bundle instead of a host file;
    // wrapped_file is then null and read/seek/tell operate on this reader + cursor.
    std::shared_ptr<bundle::EntryReader> bundle_reader;
    mutable SceOff bundle_cursor = 0;

public:
    // Constructor used for files
    // Based on https://codereview.stackexchange.com/questions/4679/
    explicit FileStats(const char *vita, const std::string &t, const fs::path &file, const int open) {
        wrapped_file = create_shared_file(file, open);

        file_info.vita_loc = vita;
        file_info.translated = t;
        file_info.sys_loc = file;
        file_info.open_mode = open;
        file_info.file_mode = SCE_SO_IFREG | SCE_SO_IROTH;
        file_info.access_mode = SCE_S_IFREG;
    }

    // Constructor used for read-only files served from a mounted Game Bundle (no host file).
    explicit FileStats(const char *vita, const std::string &t, std::shared_ptr<bundle::EntryReader> reader) {
        bundle_reader = std::move(reader);

        file_info.vita_loc = vita;
        file_info.translated = t;
        file_info.open_mode = SCE_O_RDONLY;
        file_info.file_mode = SCE_SO_IFREG | SCE_SO_IROTH;
        file_info.access_mode = SCE_S_IFREG;
    }

    bool is_bundle_file() const {
        return static_cast<bool>(bundle_reader);
    }

    const std::shared_ptr<bundle::EntryReader> &get_bundle_reader() const {
        return bundle_reader;
    }

    bool is_regular_file() const {
        return file_info.file_mode & SCE_SO_IFREG;
    }

    // Check if the file is writable
    bool can_write_file() const {
        if (!is_regular_file())
            return false;

        return can_write(file_info.open_mode);
    }

    // File operations
    FILE *get_file_pointer() const {
        return wrapped_file.get();
    }

    // File functions
    SceOff read(void *input_data, int element_size, SceSize element_count) const;
    SceOff write(const void *data, SceSize size, int count) const;
    int truncate(const SceSize size) const;
    bool seek(SceOff offset, SceIoSeekMode seek_mode) const;
    SceOff tell() const;
};

// Class for implementing Directory structure; path names are wide for Windows, normal for else
class DirStats : public VitaStats {
    // Shared directory pointer
    DirPtr dir_ptr;

    // When set, this dir fd is served from a mounted Game Bundle instead of the host FS.
    std::shared_ptr<bundle::DirReader> bundle_dir;

public:
    DirStats(const char *vita, const std::string &t, const fs::path &file, DirPtr ptr) {
        dir_ptr = std::move(ptr);

        file_info.vita_loc = vita;
        file_info.translated = t;
        file_info.sys_loc = file;
        file_info.open_mode = SCE_O_RDONLY;
        file_info.file_mode = SCE_SO_IFDIR | SCE_SO_IROTH;
        file_info.access_mode = SCE_S_IFDIR | SCE_S_IRUSR;
    }

    // Constructor used for directories served from a mounted Game Bundle (no host dir).
    DirStats(const char *vita, const std::string &t, std::shared_ptr<bundle::DirReader> dir) {
        bundle_dir = std::move(dir);

        file_info.vita_loc = vita;
        file_info.translated = t;
        file_info.open_mode = SCE_O_RDONLY;
        file_info.file_mode = SCE_SO_IFDIR | SCE_SO_IROTH;
        file_info.access_mode = SCE_S_IFDIR | SCE_S_IRUSR;
    }

    auto get_dir_ptr() const {
        return get_system_dir_ptr(dir_ptr);
    }

    bool is_bundle_dir() const {
        return static_cast<bool>(bundle_dir);
    }

    const std::shared_ptr<bundle::DirReader> &get_bundle_dir() const {
        return bundle_dir;
    }

    bool is_directory() const {
        return file_info.file_mode & SCE_SO_IFDIR;
    }
};

typedef std::map<SceUID, TtyType> TtyFiles;
typedef std::map<SceUID, FileStats> StdFiles;
typedef std::map<SceUID, DirStats> DirEntries;

struct IOState {
    struct DevicePaths {
        std::string app0;
        std::string savedata0;
        std::string addcont0;
    } device_paths;

    std::string addcont;
    std::string content_id;
    std::string savedata;
    std::string title_id;
    std::string app_path;

    std::string user_id;
    std::string user_name;

    bool redirect_stdio;

    SceUID next_fd = 0;
    TtyFiles tty_files;
    StdFiles std_files;
    DirEntries dir_entries;

    std::unordered_map<std::string, std::string> cachemap;
    bool case_isens_find_enabled = false;

    std::mutex overlay_mutex;
    SceUID next_overlay_id = 1;
    // overlay in the order they should be applied
    std::vector<FiosOverlay> overlays;

    // Active read-only Game Bundle mount for the running title (see io/bundle.h). Null when the
    // running app is a normal installed app. Set at boot, cleared in io_deinit.
    std::shared_ptr<BundleMount> mount;
};
