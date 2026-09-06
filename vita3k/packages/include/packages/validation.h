// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

#include <emuenv/state.h>

#include <string>
#include <vector>

struct ValidationContentItem {
    std::string source_path;
    std::string content_id;
    std::string category;
    std::string app_version;
};

struct ValidationContentInventory {
    std::string title_id;

    // False when a source explicitly belonging to this TITLEID (for example PCSE00001.zip) cannot
    // be inspected. In that case the validator must not claim that all expected content was mounted.
    bool inventory_complete = true;

    // Update PKGs that the normal NoInstall resolver would consider for this TITLEID. If several
    // exist, selected_update_* mirrors the resolver's highest-app_version choice.
    std::vector<ValidationContentItem> update_candidates;
    std::string selected_update_source;
    std::string selected_update_content_id;
    std::string selected_update_version;

    // DLC expected from the configured DLC folder. Package-backed DLC carries its real content id;
    // already-decrypted addcont trees contribute the mounted content-directory name.
    std::vector<ValidationContentItem> dlc_packages;
    std::vector<std::string> decrypted_dlc_content_ids;

    // External RIF content ids available from <license_folder>/<TITLEID>/ or license.zip. These are
    // informative rather than independently fatal: self-contained NoNpDrm content can supply work.bin.
    std::vector<std::string> external_license_content_ids;

    // Inventory/read problems. The caller surfaces them in the validation report; targeted content
    // failures also set inventory_complete=false.
    std::vector<std::string> warnings;
};

// Independently inventories the configured Updates, DLCs and License folders for one TITLEID without
// decrypting packages. This is intentionally separate from mount_pkg_for_play: the validator compares
// what the launch path produced against what this scanner says should have been consumed.
ValidationContentInventory inventory_validation_content(const EmuEnvState &emuenv, const std::string &title_id);

// Unique addcont directory ids expected from both package-backed and already-decrypted DLC sources.
// Package content IDs are normalized the same way install_pkg names mounted DLC directories.
std::vector<std::string> validation_expected_dlc_content_ids(const ValidationContentInventory &inventory);
