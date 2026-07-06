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

// Minimal 7z (LZMA SDK) reader used by the play-without-install path. Mirrors the zip helpers in
// pkg.cpp so mount_pkg_for_play can treat .zip and .7z uniformly.

#pragma once

#include <util/fs.h>

#include <cstdint>
#include <string>
#include <vector>

// True if the 7z contains a decrypted game tree (has an sce_sys/param.sfo entry) rather than a .pkg.
bool sevenz_has_decrypted_game(const fs::path &archive_path);

// Extract every member of a 7z into dst_dir (preserving its tree). error_out set on failure.
bool extract_7z_to_dir(const fs::path &archive_path, const fs::path &dst_dir, std::string &error_out);

// Extract the first .pkg member of a 7z to out_pkg. error_out set on failure / no .pkg.
bool extract_pkg_from_7z(const fs::path &archive_path, const fs::path &out_pkg, std::string &error_out);

// Read the first member whose lowercased name ends with `suffix_lower` into `out` (in memory).
// Returns false if the archive can't be opened or no such member exists. Used for metadata scans.
bool read_7z_entry(const fs::path &archive_path, const std::string &suffix_lower, std::vector<uint8_t> &out);
