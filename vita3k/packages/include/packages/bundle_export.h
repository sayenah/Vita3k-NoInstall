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

#include <string>
#include <vector>

struct EmuEnvState;

struct BundleExportResult {
    std::string title_id;
    std::string app_version; // effective version of the exported app/ tree (update merged)
    std::vector<std::string> dlc_ids; // addcont folders in the bundle
    std::vector<std::string> licenses; // rif file names in the bundle
};

// Converts one game into a single self-contained .zip: the decrypted app/ tree with its latest
// update merged in, every DLC decrypted under addcont/<id>/, and the game's licenses under
// license/<TITLEID>/. Inputs are resolved exactly like play-without-install (prepare_game_tree), so
// the configured Updates/DLCs/License folders are used. Never overwrites `output_zip`; the zip is
// written to "<output_zip>.partial" and renamed only once complete. Returns false with error_out set
// on failure.
bool export_game_bundle(EmuEnvState &emuenv, const fs::path &input_path, const fs::path &output_zip, BundleExportResult &result, std::string &error_out);
