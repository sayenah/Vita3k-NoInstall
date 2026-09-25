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

#include <packages/bundle_export.h>

#include <miniz.h>

#include <config/state.h>
#include <emuenv/state.h>
#include <packages/pkg.h>
#include <packages/sfo.h>
#include <packages/validation.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <algorithm>
#include <ctime>

namespace {

struct ZipEntry {
    std::string name; // path inside the zip, '/'-separated
    fs::path source;
};

// Every file under root as a zip entry, manifest first then sorted by name (stable, readable zips).
std::vector<ZipEntry> collect_entries(const fs::path &root) {
    std::vector<ZipEntry> entries;
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!fs::is_regular_file(it->path(), ec))
            continue;
        const fs::path rel = it->path().lexically_relative(root);
        entries.push_back({ fs_utils::path_to_utf8(rel.generic_path()), it->path() });
    }
    std::sort(entries.begin(), entries.end(), [](const ZipEntry &a, const ZipEntry &b) {
        const bool a_manifest = a.name == "vita3k_bundle.json";
        const bool b_manifest = b.name == "vita3k_bundle.json";
        if (a_manifest != b_manifest)
            return a_manifest;
        return a.name < b.name;
    });
    return entries;
}

bool write_zip(const fs::path &root, const fs::path &zip_path, std::string &error_out) {
    FILE *out = FOPEN(zip_path.c_str(), "wb");
    if (!out) {
        error_out = "cannot create " + fs_utils::path_to_utf8(zip_path);
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_cfile(&zip, out, 0)) {
        fclose(out);
        error_out = "cannot start zip writer";
        return false;
    }

    bool ok = true;
    const MZ_TIME_T now = std::time(nullptr);
    for (const auto &entry : collect_entries(root)) {
        FILE *src = FOPEN(entry.source.c_str(), "rb");
        boost::system::error_code ec;
        const auto size = fs::file_size(entry.source, ec);
        if (!src || ec) {
            if (src)
                fclose(src);
            error_out = "cannot read " + entry.name;
            ok = false;
            break;
        }
        const bool added = mz_zip_writer_add_cfile(&zip, entry.name.c_str(), src, size, &now, nullptr, 0,
            MZ_DEFAULT_LEVEL, nullptr, 0, nullptr, 0);
        fclose(src);
        if (!added) {
            error_out = "cannot add " + entry.name + " to the zip: " + mz_zip_get_error_string(mz_zip_get_last_error(&zip));
            ok = false;
            break;
        }
    }

    if (ok && !mz_zip_writer_finalize_archive(&zip)) {
        error_out = std::string("cannot finalize the zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip));
        ok = false;
    }
    mz_zip_writer_end(&zip);
    if (fclose(out) != 0 && ok) {
        error_out = "cannot finish writing " + fs_utils::path_to_utf8(zip_path);
        ok = false;
    }
    return ok;
}

// Copy the game's rifs (game + update + DLC share one title id) from the real ux0/license into the
// bundle, where playing it copies them back (any *.rif inside a decrypted-game archive is applied).
std::vector<std::string> bundle_licenses(EmuEnvState &emuenv, const std::string &title_id, const fs::path &temp_root) {
    std::vector<std::string> copied;
    boost::system::error_code ec;
    const fs::path src_dir = emuenv.vita_fs_path / "ux0/license" / title_id;
    const fs::path dst_dir = temp_root / "license" / title_id;
    for (fs::directory_iterator it(src_dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!fs::is_regular_file(it->path(), ec) || string_utils::tolower(it->path().extension().string()) != ".rif")
            continue;
        fs::create_directories(dst_dir, ec);
        fs::copy_file(it->path(), dst_dir / it->path().filename(), fs::copy_options::overwrite_existing, ec);
        if (!ec)
            copied.push_back(it->path().filename().string());
        ec.clear();
    }
    std::sort(copied.begin(), copied.end());
    return copied;
}

} // namespace

bool export_game_bundle(EmuEnvState &emuenv, const fs::path &input_path, const fs::path &output_zip, BundleExportResult &result, std::string &error_out) {
    boost::system::error_code ec;
    if (fs::exists(output_zip, ec)) {
        error_out = "output already exists: " + fs_utils::path_to_utf8(output_zip);
        return false;
    }
    if (fs::equivalent(input_path, output_zip, ec)) {
        error_out = "output must differ from the input";
        return false;
    }
    ec.clear();
    // Without side folders there is nothing to combine, and a base-only zip would look complete.
    if (emuenv.cfg.updates_folder.empty() && emuenv.cfg.dlc_folder.empty() && emuenv.cfg.license_folder.empty()) {
        error_out = "no Updates, DLCs or License folder is configured, so there is nothing to combine";
        return false;
    }

    const fs::path temp_root = emuenv.cache_path / "pkgexport";
    result.title_id = prepare_game_tree(emuenv, input_path, temp_root, error_out);
    if (result.title_id.empty())
        return false;

    const auto cleanup = [&]() { fs::remove_all(temp_root, ec); };

    std::vector<uint8_t> sfo_buf;
    if (!fs_utils::read_data(temp_root / "app/sce_sys/param.sfo", sfo_buf)) {
        error_out = "prepared game has no app/sce_sys/param.sfo";
        cleanup();
        return false;
    }
    sfo::SfoAppInfo info;
    sfo::get_param_info(info, sfo_buf, emuenv.cfg.sys_lang);
    result.app_version = info.app_version;

    for (fs::directory_iterator it(temp_root / "addcont", ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (fs::is_directory(it->path(), ec)) {
            if (fs::is_directory(it->path() / "sce_pfs", ec)) {
                error_out = "DLC " + it->path().filename().string() + " could not be decrypted (missing license, or the dump was altered)";
                cleanup();
                return false;
            }
            result.dlc_ids.push_back(it->path().filename().string());
        }
    }
    ec.clear();
    std::sort(result.dlc_ids.begin(), result.dlc_ids.end());

    // The bundle replaces the loose update/DLC files, so it must provably contain them: check the
    // prepared tree against the validator's independent inventory of what should have been consumed.
    const auto inventory = inventory_validation_content(emuenv, result.title_id);
    if (!inventory.inventory_complete) {
        error_out = "could not inspect all of this game's update/DLC sources";
        for (const auto &w : inventory.warnings)
            error_out += "; " + w;
        cleanup();
        return false;
    }
    if (!inventory.selected_update_version.empty() && inventory.selected_update_version != result.app_version) {
        error_out = "update " + inventory.selected_update_version + " was not applied (app is v" + result.app_version + ")";
        cleanup();
        return false;
    }
    std::vector<std::string> missing;
    for (const auto &id : validation_expected_dlc_content_ids(inventory)) {
        if (std::find(result.dlc_ids.begin(), result.dlc_ids.end(), id) == result.dlc_ids.end())
            missing.push_back(id);
    }
    if (!missing.empty()) {
        error_out = "DLC not included:";
        for (const auto &id : missing)
            error_out += " " + id;
        cleanup();
        return false;
    }

    result.licenses = bundle_licenses(emuenv, result.title_id, temp_root);

    if (!output_zip.parent_path().empty())
        fs::create_directories(output_zip.parent_path(), ec);
    const fs::path partial = fs_utils::path_concat(output_zip, ".partial");
    fs::remove(partial, ec);
    if (!write_zip(temp_root, partial, error_out)) {
        fs::remove(partial, ec);
        cleanup();
        return false;
    }
    cleanup();

    fs::rename(partial, output_zip, ec);
    if (ec) {
        error_out = "cannot move the finished zip into place: " + ec.message();
        fs::remove(partial, ec);
        return false;
    }

    LOG_INFO("Exported [{}] v{} with {} DLC and {} license(s) to {}", result.title_id, result.app_version,
        result.dlc_ids.size(), result.licenses.size(), fs_utils::path_to_utf8(output_zip));
    return true;
}
