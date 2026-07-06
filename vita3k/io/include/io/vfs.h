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

#include <util/fs.h>
#include <util/types.h>

enum class VitaIoDevice : int;
struct IOState;

namespace vfs {

using FileBuffer = std::vector<SceUInt8>;

bool read_file(VitaIoDevice device, FileBuffer &buf, const fs::path &vita_fs_path, const fs::path &vfs_file_path);
// Reads a file from the running app's tree (app0:/ux0:app/<app_path>). Consults a mounted Game
// Bundle first (via io.mount), falling back to the host filesystem.
bool read_app_file(const IOState &io, FileBuffer &buf, const fs::path &vita_fs_path, const std::string &app_path, const fs::path &vfs_file_path);
SceSize get_directory_used_size(const VitaIoDevice device, const std::string &vfs_path, const fs::path &vita_fs_path);
} // namespace vfs
