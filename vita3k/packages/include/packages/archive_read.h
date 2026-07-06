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

// Read a game's display metadata + icon from an archive without launching it, for the ROM library.

#pragma once

#include <util/fs.h>

#include <cstdint>
#include <string>
#include <vector>

struct ArchiveGameInfo {
    bool ok = false;
    std::string title_id;
    std::string content_id;
    std::string title;
    std::string stitle;
    std::string category;
    std::vector<uint8_t> icon0; // icon0.png bytes; empty if unavailable (e.g. undecrypted pkg)
};

// Reads a game's metadata (+ icon0.png where cheaply available) from a .zip/.7z/.pkg — one holding a
// decrypted app tree, or a raw pkg. `sys_lang` selects the localized title. ok=false on failure.
ArchiveGameInfo read_archive_game_info(const fs::path &archive_path, int sys_lang);
