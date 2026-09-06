// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <packages/validation.h>

#include <miniz.h>

#include <config/state.h>
#include <packages/archive_7z.h>
#include <packages/pkg.h>
#include <packages/sfo.h>
#include <util/bytes.h>
#include <util/fs.h>
#include <util/string_utils.h>

#include <algorithm>
#include <cstdio>
#include <set>

namespace {

struct PkgMetadata {
    bool header_ok = false;
    bool sfo_ok = false;
    std::string content_id;
    std::string title_id;
    std::string category;
    std::string app_version;
};

std::string fixed_c_string(const char *begin, std::size_t size) {
    const char *end = std::find(begin, begin + size, '\0');
    return std::string(begin, end);
}

PkgMetadata inspect_pkg(const fs::path &path, int sys_lang) {
    PkgMetadata out;

    FILE *fp = FOPEN(path.c_str(), "rb");
    if (!fp)
        return out;

    PkgHeader header{};
    const std::size_t bytes = fread(&header, 1, sizeof(header), fp);
    fclose(fp);
    if (bytes != sizeof(header) || byte_swap(header.magic) != 0x7F504B47)
        return out;

    out.header_ok = true;
    out.content_id = fixed_c_string(header.content_id, sizeof(header.content_id));
    if (out.content_id.size() >= 16)
        out.title_id = out.content_id.substr(7, 9);

    std::vector<uint8_t> sfo_bytes;
    if (read_pkg_param_sfo(path, sfo_bytes)) {
        sfo::SfoAppInfo info;
        sfo::get_param_info(info, sfo_bytes, sys_lang);
        out.sfo_ok = true;
        out.category = info.app_category;
        out.app_version = info.app_version;
        if (out.title_id.empty())
            out.title_id = info.app_title_id;
        if (out.content_id.empty())
            out.content_id = info.app_content_id;
    }

    return out;
}

bool extract_zip_to_dir(const fs::path &zip_path, const fs::path &dst_dir, std::string &error_out) {
    FILE *fp = FOPEN(zip_path.c_str(), "rb");
    if (!fp) {
        error_out = "cannot open zip";
        return false;
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        fclose(fp);
        error_out = "invalid zip";
        return false;
    }

    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            continue;

        const fs::path relative = fs_utils::utf8_to_path(stat.m_filename);
        // Validation is read-only and must never let an archive escape its scratch tree.
        if (relative.is_absolute() || fs_utils::path_to_utf8(relative).find("..") != std::string::npos) {
            error_out = std::string("unsafe member path: ") + stat.m_filename;
            ok = false;
            break;
        }

        const fs::path out_path = dst_dir / relative;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(out_path);
            continue;
        }

        fs::create_directories(out_path.parent_path());
        if (!mz_zip_reader_extract_to_file(&zip, i, fs_utils::path_to_utf8(out_path).c_str(), 0)) {
            error_out = std::string("failed extracting member: ") + stat.m_filename;
            ok = false;
            break;
        }
    }

    mz_zip_reader_end(&zip);
    fclose(fp);
    return ok;
}

void collect_decrypted_addcont_ids(const fs::path &root, const std::string &title_id, std::set<std::string> &out) {
    boost::system::error_code ec;

    const auto collect_from = [&](const fs::path &addcont_dir) {
        boost::system::error_code local_ec;
        if (!fs::is_directory(addcont_dir, local_ec))
            return;

        fs::path base = addcont_dir;
        if (fs::is_directory(addcont_dir / title_id, local_ec))
            base = addcont_dir / title_id;

        for (fs::directory_iterator it(base, local_ec), end; it != end; it.increment(local_ec)) {
            if (local_ec) {
                local_ec.clear();
                continue;
            }
            if (fs::is_directory(it->path(), local_ec))
                out.insert(fs_utils::path_to_utf8(it->path().filename()));
        }
    };

    collect_from(root / "addcont");
    if (!fs::is_directory(root, ec))
        return;

    for (fs::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (fs::is_directory(it->path(), ec))
            collect_from(it->path() / "addcont");
    }
}

void collect_pkg_candidates(const fs::path &root, const fs::path &source_override, const std::string &title_id,
    int sys_lang, const std::string &wanted_category, bool match_all, std::set<std::string> &seen_paths,
    std::vector<ValidationContentItem> &out, std::vector<std::string> &warnings, bool *inventory_complete) {
    boost::system::error_code ec;
    if (!fs::is_directory(root, ec))
        return;

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const fs::path path = it->path();
        if (!fs::is_regular_file(path, ec) || string_utils::tolower(path.extension().string()) != ".pkg")
            continue;

        const std::string path_key = fs_utils::path_to_utf8(path);
        if (!seen_paths.insert(path_key).second)
            continue;

        const PkgMetadata meta = inspect_pkg(path, sys_lang);
        if (!meta.header_ok) {
            warnings.push_back("Unreadable PKG header: " + path_key);
            if (inventory_complete)
                *inventory_complete = false;
            continue;
        }
        if (!match_all && meta.title_id != title_id)
            continue;

        // Mirror mount_pkg_for_play's behavior: if the SFO is readable, wrong-category content is
        // skipped. If the SFO is unreadable the launch resolver still attempts it, so inventory it.
        if (meta.sfo_ok && meta.category != wanted_category)
            continue;

        ValidationContentItem item;
        if (!source_override.empty())
            item.source_path = fs_utils::path_to_utf8(source_override) + "::" + fs_utils::path_to_utf8(path.filename());
        else
            item.source_path = path_key;
        item.content_id = meta.content_id;
        item.category = meta.category;
        item.app_version = meta.app_version;
        out.push_back(std::move(item));
    }
}

void inventory_container(const fs::path &archive_path, const fs::path &scratch, const std::string &title_id,
    int sys_lang, const std::string &category, std::vector<ValidationContentItem> &pkg_out,
    std::set<std::string> *decrypted_dlc_ids, std::vector<std::string> &warnings, bool &inventory_complete) {
    boost::system::error_code ec;
    fs::remove_all(scratch, ec);
    if (ec) {
        warnings.push_back("Could not clear inventory scratch path " + fs_utils::path_to_utf8(scratch) + ": " + ec.message());
        inventory_complete = false;
        return;
    }
    fs::create_directories(scratch, ec);
    if (ec) {
        warnings.push_back("Could not create inventory scratch path " + fs_utils::path_to_utf8(scratch) + ": " + ec.message());
        inventory_complete = false;
        return;
    }

    std::string error;
    const std::string ext = string_utils::tolower(archive_path.extension().string());
    const bool ok = (ext == ".7z")
        ? extract_7z_to_dir(archive_path, scratch, error)
        : extract_zip_to_dir(archive_path, scratch, error);

    if (!ok) {
        warnings.push_back("Could not inspect " + fs_utils::path_to_utf8(archive_path) + ": " + error);
        inventory_complete = false;
        fs::remove_all(scratch, ec);
        return;
    }

    std::set<std::string> seen;
    collect_pkg_candidates(scratch, archive_path, title_id, sys_lang, category, true, seen, pkg_out, warnings, &inventory_complete);
    if (decrypted_dlc_ids)
        collect_decrypted_addcont_ids(scratch, title_id, *decrypted_dlc_ids);

    fs::remove_all(scratch, ec);
}

void collect_license_ids_from_zip(const fs::path &zip_path, const std::string &title_id, std::set<std::string> &out,
    std::vector<std::string> &warnings) {
    FILE *fp = FOPEN(zip_path.c_str(), "rb");
    if (!fp) {
        warnings.push_back("Could not open license.zip");
        return;
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        fclose(fp);
        warnings.push_back("Invalid license.zip");
        return;
    }

    const std::string prefix = title_id + "/";
    const std::string needle = "/" + title_id + "/";
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || mz_zip_reader_is_file_a_directory(&zip, i))
            continue;

        const std::string name = stat.m_filename;
        const std::string lower = string_utils::tolower(name);
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".rif") != 0)
            continue;
        if (name.rfind(prefix, 0) != 0 && name.find(needle) == std::string::npos)
            continue;

        out.insert(fs_utils::path_to_utf8(fs_utils::utf8_to_path(name).stem()));
    }

    mz_zip_reader_end(&zip);
    fclose(fp);
}

} // namespace

ValidationContentInventory inventory_validation_content(const EmuEnvState &emuenv, const std::string &title_id) {
    ValidationContentInventory inventory;
    inventory.title_id = title_id;

    const fs::path scratch_root = emuenv.cache_path / "validation_inventory" / title_id;
    boost::system::error_code ec;
    fs::remove_all(scratch_root, ec);

    // Updates: mirror <TITLEID>/, <TITLEID>.zip/.7z and loose-PKG discovery. Path de-duplication lets
    // us scan the whole root after the title folder without double-counting the same physical PKG.
    if (!emuenv.cfg.updates_folder.empty()) {
        const fs::path root = fs_utils::utf8_to_path(emuenv.cfg.updates_folder);
        if (fs::is_directory(root, ec)) {
            std::set<std::string> seen;
            collect_pkg_candidates(root / title_id, {}, title_id, emuenv.cfg.sys_lang, "gp", true, seen,
                inventory.update_candidates, inventory.warnings, &inventory.inventory_complete);

            for (const char *ext : { ".zip", ".7z" }) {
                const fs::path archive = root / (title_id + ext);
                if (fs::is_regular_file(archive, ec)) {
                    inventory_container(archive, scratch_root / (std::string("updates") + ext), title_id,
                        emuenv.cfg.sys_lang, "gp", inventory.update_candidates, nullptr, inventory.warnings,
                        inventory.inventory_complete);
                }
            }

            // Loose packages outside the targeted title directory cannot be assigned to this game if
            // their header is unreadable, so those are warnings but do not make this title incomplete.
            collect_pkg_candidates(root, {}, title_id, emuenv.cfg.sys_lang, "gp", false, seen,
                inventory.update_candidates, inventory.warnings, nullptr);
        }
    }

    // Mirror the launch resolver: lexicographically highest app_version wins; an unreadable/empty
    // version can be selected only until a real higher version is encountered.
    for (const auto &candidate : inventory.update_candidates) {
        if (inventory.selected_update_source.empty() || candidate.app_version > inventory.selected_update_version) {
            inventory.selected_update_source = candidate.source_path;
            inventory.selected_update_content_id = candidate.content_id;
            inventory.selected_update_version = candidate.app_version;
        }
    }

    // DLCs: inventory package-backed DLC plus already-decrypted addcont trees from every recognized
    // side-folder layout. The launch archive itself may also carry addcont; those will appear later as
    // mounted extras and are intentionally not treated as missing side-folder content.
    std::set<std::string> decrypted_dlc_ids;
    if (!emuenv.cfg.dlc_folder.empty()) {
        const fs::path root = fs_utils::utf8_to_path(emuenv.cfg.dlc_folder);
        if (fs::is_directory(root, ec)) {
            std::set<std::string> seen;
            const fs::path title_dir = root / title_id;
            collect_pkg_candidates(title_dir, {}, title_id, emuenv.cfg.sys_lang, "ac", true, seen,
                inventory.dlc_packages, inventory.warnings, &inventory.inventory_complete);
            collect_decrypted_addcont_ids(title_dir, title_id, decrypted_dlc_ids);

            for (const char *ext : { ".zip", ".7z" }) {
                const fs::path archive = root / (title_id + ext);
                if (fs::is_regular_file(archive, ec)) {
                    inventory_container(archive, scratch_root / (std::string("dlc") + ext), title_id,
                        emuenv.cfg.sys_lang, "ac", inventory.dlc_packages, &decrypted_dlc_ids, inventory.warnings,
                        inventory.inventory_complete);
                }
            }

            collect_pkg_candidates(root, {}, title_id, emuenv.cfg.sys_lang, "ac", false, seen,
                inventory.dlc_packages, inventory.warnings, nullptr);
        }
    }
    inventory.decrypted_dlc_content_ids.assign(decrypted_dlc_ids.begin(), decrypted_dlc_ids.end());

    // Licenses: mirror the exact two layouts used by mount_licenses_for_game.
    std::set<std::string> license_ids;
    if (!emuenv.cfg.license_folder.empty()) {
        const fs::path root = fs_utils::utf8_to_path(emuenv.cfg.license_folder);
        if (fs::is_directory(root, ec)) {
            const fs::path title_dir = root / title_id;
            if (fs::is_directory(title_dir, ec)) {
                for (fs::directory_iterator it(title_dir, ec), end; it != end; it.increment(ec)) {
                    if (ec) {
                        ec.clear();
                        continue;
                    }
                    if (fs::is_regular_file(it->path(), ec)
                        && string_utils::tolower(it->path().extension().string()) == ".rif") {
                        license_ids.insert(fs_utils::path_to_utf8(it->path().stem()));
                    }
                }
            }

            const fs::path license_zip = root / "license.zip";
            if (fs::is_regular_file(license_zip, ec))
                collect_license_ids_from_zip(license_zip, title_id, license_ids, inventory.warnings);
        }
    }
    inventory.external_license_content_ids.assign(license_ids.begin(), license_ids.end());

    fs::remove_all(scratch_root, ec);
    return inventory;
}

std::vector<std::string> validation_expected_dlc_content_ids(const ValidationContentInventory &inventory) {
    std::set<std::string> ids(inventory.decrypted_dlc_content_ids.begin(), inventory.decrypted_dlc_content_ids.end());
    for (const auto &item : inventory.dlc_packages) {
        if (!item.content_id.empty()) {
            // install_pkg uses the content-specific suffix as the addcont directory name. Compare
            // against that mounted identity while retaining the full PSN content ID in dlc_packages.
            ids.insert(item.content_id.size() > 20 ? item.content_id.substr(20) : item.content_id);
        }
    }
    return { ids.begin(), ids.end() };
}
