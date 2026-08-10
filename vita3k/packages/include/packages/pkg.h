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

/**
 * @file pkg.h
 * @brief PlayStation Vita software package (`.pkg`) handling
 */

#pragma once

#include <cstdint>
#include <emuenv/state.h>
#include <string>
#include <vector>

// Credits to mmozeiko https://github.com/mmozeiko/pkg2zip

const uint8_t pkg_vita_2[] = { 0xe3, 0x1a, 0x70, 0xc9, 0xce, 0x1d, 0xd7, 0x2b, 0xf3, 0xc0, 0x62, 0x29, 0x63, 0xf2, 0xec, 0xcb };
const uint8_t pkg_vita_3[] = { 0x42, 0x3a, 0xca, 0x3a, 0x2b, 0xd5, 0x64, 0x9f, 0x96, 0x86, 0xab, 0xad, 0x6f, 0xd8, 0x80, 0x1f };
const uint8_t pkg_vita_4[] = { 0xaf, 0x07, 0xfd, 0x59, 0x65, 0x25, 0x27, 0xba, 0xf1, 0x33, 0x89, 0x66, 0x8b, 0x17, 0xd9, 0xea };

enum class PkgType {
    PKG_TYPE_VITA_APP = 0,
    PKG_TYPE_VITA_DLC = 1,
    PKG_TYPE_VITA_PATCH = 2,
    PKG_TYPE_VITA_THEME = 3
};

struct PkgHeader {
    uint32_t magic;
    uint16_t revision;
    uint16_t type;
    uint32_t info_offset;
    uint32_t info_count;
    uint32_t header_size;
    uint32_t file_count;
    uint64_t total_size;
    uint64_t data_offset;
    uint64_t data_size;
    char content_id[0x30];
    uint8_t digest[0x10];
    uint8_t pkg_data_iv[0x10];
    uint8_t pkg_signatures[0x40];
};

struct PkgExtHeader {
    uint32_t magic;
    uint32_t unknown_01;
    uint32_t header_size;
    uint32_t data_size;
    uint32_t data_offset;
    uint32_t data_type;
    uint64_t pkg_data_size;

    uint32_t padding_01;
    uint32_t data_type2;
    uint32_t unknown_02;
    uint32_t padding_02;
    uint64_t padding_03;
    uint64_t padding_04;
};

struct PkgEntry {
    uint32_t name_offset;
    uint32_t name_size;
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t type;
    uint32_t padding;
};

// Installs a pkg into ux0. If `temp_root` is non-empty, instead decrypts a base-game pkg into
// `temp_root/app` (no ux0/app install, copy_path skipped) for play-without-install; the license rif
// still goes to the real ux0/license. When no zRIF is supplied, a self-contained NoNpDrm pkg's
// sce_sys/package/work.bin is used to derive one.
bool install_pkg(const fs::path &pkg_path, EmuEnvState &emuenv, std::string &p_zRIF, const std::function<void(float)> &progress_callback = nullptr, const fs::path &temp_root = {});
std::string find_pkg_zrif(const fs::path &pkg_path, const fs::path &vita_fs_path);

// Reads the metadata param.sfo out of a pkg's info section (no decryption). For a games-list scan:
// gives title/title_id/category cheaply. Returns false if not a valid pkg or it has no sfo.
bool read_pkg_param_sfo(const fs::path &pkg_path, std::vector<uint8_t> &sfo_out);
bool decrypt_install_nonpdrm(EmuEnvState &emuenv, const fs::path &drmlicpath, const fs::path &title_path, const std::function<void(float)> &progress_callback = nullptr);

// Decrypts a self-contained (NoNpDrm) base-game pkg to a private temp dir, drops its license rif on
// the real ux0/license, and mounts the temp tree read-only (io.mount) so it can be booted with no
// permanent install. Returns the title id to boot (run_app_path), or "" on failure (error_out set).
// The temp dir is owned by the mount and deleted in io_deinit when the game stops.
std::string mount_pkg_for_play(EmuEnvState &emuenv, const fs::path &pkg_path, std::string &error_out);
