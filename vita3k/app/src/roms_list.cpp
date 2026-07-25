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

#include <app/functions.h>

#include <config/state.h>
#include <emuenv/state.h>
#include <packages/archive_read.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <algorithm>
#include <set>
#include <unordered_map>

namespace app {

std::vector<std::string> &roms_folders(Config &cfg) {
    // Legacy migration: the original single "roms-folder" setting becomes the first entry of
    // "roms-folders". In-memory only — any later config save (adding/removing a folder, changing a
    // setting) persists the migrated shape, after which the legacy key stays empty.
    if (!cfg.roms_folder.empty()) {
        if (std::find(cfg.roms_folders.begin(), cfg.roms_folders.end(), cfg.roms_folder) == cfg.roms_folders.end())
            cfg.roms_folders.insert(cfg.roms_folders.begin(), cfg.roms_folder);
        cfg.roms_folder.clear();
    }
    return cfg.roms_folders;
}

std::vector<std::string> scan_roms(EmuEnvState &emuenv) {
    std::vector<std::string> failures;

    boost::system::error_code ec;
    std::vector<fs::path> roots;
    for (const auto &folder : roms_folders(emuenv.cfg)) {
        const fs::path p = fs_utils::utf8_to_path(folder);
        if (!folder.empty() && fs::is_directory(p, ec) && std::find(roots.begin(), roots.end(), p) == roots.end())
            roots.push_back(p);
    }
    if (roots.empty()) {
        // No usable folder configured: just drop any stale ROM rows so the list reflects reality.
        std::lock_guard<std::mutex> lock(emuenv.app.apps_list.mutex);
        std::erase_if(emuenv.app.apps_list.apps, [](const AppEntry &a) { return !a.archive_path.empty(); });
        return failures;
    }

    const bool multi_root = roots.size() > 1;
    std::vector<AppEntry> roms;
    std::vector<fs::path> rom_roots; // parallel to `roms`: which root each row came from
    std::set<std::string> seen_paths; // overlapping/nested roots must not produce duplicate rows

    for (const auto &folder_path : roots) {
        // Walk each folder recursively so games nested in sub-folders (e.g. "Base Set/",
        // "Translations/") are found too, not just archives sitting directly in the chosen folder.
        // skip_permission_denied steps over unreadable sub-dirs instead of aborting; directory
        // symlinks are not followed (boost default), so there are no symlink loops.
        for (const auto &entry : fs::recursive_directory_iterator(folder_path, fs::directory_options::skip_permission_denied, ec)) {
            if (ec)
                break;
            boost::system::error_code fec;
            if (!fs::is_regular_file(entry.path(), fec))
                continue;
            const std::string ext = string_utils::tolower(entry.path().extension().string());
            if (ext != ".zip" && ext != ".7z" && ext != ".pkg")
                continue;
            if (!seen_paths.insert(fs_utils::path_to_utf8(entry.path())).second)
                continue; // already found under an overlapping root

            // Label archives by their path relative to their folder ("Translations/game.zip") so
            // nested files and same-named files in different sub-folders stay distinguishable; with
            // several roots, prefix the root's name so the user can tell which folder it came from.
            std::string rel_label = fs_utils::path_to_utf8(entry.path().lexically_relative(folder_path));
            if (multi_root)
                rel_label = fs_utils::path_to_utf8(folder_path.filename()) + "/" + rel_label;

            const ArchiveGameInfo info = read_archive_game_info(entry.path(), emuenv.cfg.sys_lang);
            if (!info.ok) {
                failures.push_back(rel_label);
                continue;
            }

            // Cache icon0 to a file the games-list renderer can load by path. Key the cache dir on the
            // archive path, not just the title id, so two archives that share a title id -- a base game
            // and its translation in sibling folders -- don't overwrite each other's icon.
            std::string icon_path;
            if (!info.icon0.empty()) {
                const std::string archive_key = std::to_string(std::hash<std::string>{}(fs_utils::path_to_utf8(entry.path())));
                const fs::path icon_dir = emuenv.cache_path / "roms" / (info.title_id + "_" + archive_key);
                fs::create_directories(icon_dir, ec);
                const fs::path icon_file = icon_dir / "icon0.png";
                fs::ofstream icon_out(icon_file, std::ios::out | std::ios::binary);
                if (icon_out)
                    icon_out.write(reinterpret_cast<const char *>(info.icon0.data()), static_cast<std::streamsize>(info.icon0.size()));
                icon_out.close();
                icon_path = fs_utils::path_to_utf8(icon_file);
            }

            AppEntry app;
            app.title_id = info.title_id;
            app.title = info.title.empty() ? info.title_id : info.title;
            app.stitle = info.stitle.empty() ? app.title : info.stitle;
            app.category = info.category.empty() ? "gd" : info.category;
            app.content_id = info.content_id;
            app.addcont = info.title_id;
            app.savedata = info.title_id;
            app.app_ver = "N/A";
            app.parental_level = "N/A";
            // The row key is the archive path (unique, avoids colliding with an installed title id); the
            // launch routes through mount_pkg_for_play(archive_path) (see MainWindow::on_app_selected).
            app.path = fs_utils::path_to_utf8(entry.path());
            app.icon_path = icon_path;
            app.archive_path = app.path;
            roms.push_back(std::move(app));
            rom_roots.push_back(folder_path);
        }
        ec.clear();
    }

    // Disambiguate rows that would otherwise show the same title -- e.g. a base game and its
    // translation, which share a title id. Only annotate the duplicates (uniques keep a clean name):
    // append the sub-folder each came from, relative to its ROMs folder, so "Gravity Rush" becomes
    // "Gravity Rush (Base Set)" vs "Gravity Rush (Translations)". With several roots the root's name
    // is included too, so duplicates across folders read "Gravity Rush (SD Card/Base Set)".
    {
        std::unordered_map<std::string, int> title_counts;
        for (const auto &rom : roms)
            ++title_counts[rom.title];
        for (size_t i = 0; i < roms.size(); ++i) {
            auto &rom = roms[i];
            if (title_counts[rom.title] < 2)
                continue;
            const fs::path arch = fs_utils::utf8_to_path(rom.archive_path);
            const fs::path &root = rom_roots[i];
            std::string where = fs_utils::path_to_utf8(arch.parent_path().lexically_relative(root));
            if (where.empty() || where == ".")
                where = multi_root ? fs_utils::path_to_utf8(root.filename())
                                   : fs_utils::path_to_utf8(arch.stem()); // single root, at its top level: fall back to the file name
            else if (multi_root)
                where = fs_utils::path_to_utf8(root.filename()) + "/" + where;
            rom.title += " (" + where + ")";
        }
    }

    {
        std::lock_guard<std::mutex> lock(emuenv.app.apps_list.mutex);
        auto &apps = emuenv.app.apps_list.apps;
        std::erase_if(apps, [](const AppEntry &a) { return !a.archive_path.empty(); }); // drop prior ROM rows
        for (auto &rom : roms)
            apps.push_back(std::move(rom));
    }

    LOG_INFO("ROMs scan: {} folder(s), {} playable, {} unreadable", roots.size(), roms.size(), failures.size());
    return failures;
}

} // namespace app
