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

#include <packages/archive_read.h>

#include <packages/archive_7z.h>
#include <packages/pkg.h>
#include <packages/sfo.h>

#include <util/fs.h>
#include <util/string_utils.h>

#include <miniz.h>

namespace {

// Read the first zip member whose lowercased name ends with `suffix_lower` into `out` (in memory).
bool read_zip_entry(const fs::path &zip_path, const std::string &suffix_lower, std::vector<uint8_t> &out) {
    FILE *fp = FOPEN(zip_path.c_str(), "rb");
    if (!fp)
        return false;
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        fclose(fp);
        return false;
    }
    bool found = false;
    const mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        const std::string name = string_utils::tolower(stat.m_filename);
        if (name.size() >= suffix_lower.size() && name.compare(name.size() - suffix_lower.size(), suffix_lower.size(), suffix_lower) == 0) {
            size_t sz = 0;
            void *p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
            if (p) {
                const uint8_t *bytes = static_cast<const uint8_t *>(p);
                out.assign(bytes, bytes + sz);
                mz_free(p);
                found = true;
            }
            break;
        }
    }
    mz_zip_reader_end(&zip);
    fclose(fp);
    return found;
}

} // namespace

ArchiveGameInfo read_archive_game_info(const fs::path &archive_path, int sys_lang) {
    ArchiveGameInfo info;
    const std::string ext = string_utils::tolower(archive_path.extension().string());

    std::vector<uint8_t> sfo_buf;
    if (ext == ".zip") {
        if (!read_zip_entry(archive_path, "sce_sys/param.sfo", sfo_buf))
            return info;
        read_zip_entry(archive_path, "sce_sys/icon0.png", info.icon0);
    } else if (ext == ".7z") {
        if (!read_7z_entry(archive_path, "sce_sys/param.sfo", sfo_buf))
            return info;
        read_7z_entry(archive_path, "sce_sys/icon0.png", info.icon0);
    } else if (ext == ".pkg") {
        // A raw pkg's icon0 lives inside the PFS layer (needs decrypt), so skip it here (default icon).
        if (!read_pkg_param_sfo(archive_path, sfo_buf))
            return info;
    } else {
        return info;
    }

    sfo::SfoAppInfo sfo_info;
    sfo::get_param_info(sfo_info, sfo_buf, sys_lang);
    info.title_id = sfo_info.app_title_id;
    info.content_id = sfo_info.app_content_id;
    info.title = sfo_info.app_title;
    info.stitle = sfo_info.app_short_title;
    info.category = sfo_info.app_category;
    info.ok = !info.title_id.empty();
    return info;
}
