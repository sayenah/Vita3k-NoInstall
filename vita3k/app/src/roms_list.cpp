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

namespace app {

std::vector<std::string> scan_roms(EmuEnvState &emuenv) {
    std::vector<std::string> failures;

    const std::string &folder = emuenv.cfg.roms_folder;
    boost::system::error_code ec;
    const fs::path folder_path = fs_utils::utf8_to_path(folder);
    if (folder.empty() || !fs::is_directory(folder_path, ec)) {
        // No folder configured: just drop any stale ROM rows so the list reflects reality.
        std::lock_guard<std::mutex> lock(emuenv.app.apps_list.mutex);
        std::erase_if(emuenv.app.apps_list.apps, [](const AppEntry &a) { return !a.archive_path.empty(); });
        return failures;
    }

    std::vector<AppEntry> roms;
    for (const auto &entry : fs::directory_iterator(folder_path, ec)) {
        if (ec)
            break;
        boost::system::error_code fec;
        if (!fs::is_regular_file(entry.path(), fec))
            continue;
        const std::string ext = string_utils::tolower(entry.path().extension().string());
        if (ext != ".zip" && ext != ".7z" && ext != ".pkg")
            continue;

        const ArchiveGameInfo info = read_archive_game_info(entry.path(), emuenv.cfg.sys_lang);
        if (!info.ok) {
            failures.push_back(fs_utils::path_to_utf8(entry.path().filename()));
            continue;
        }

        // Cache the icon0 to a file the games-list renderer can load by path.
        std::string icon_path;
        if (!info.icon0.empty()) {
            const fs::path icon_dir = emuenv.cache_path / "roms" / info.title_id;
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
    }

    {
        std::lock_guard<std::mutex> lock(emuenv.app.apps_list.mutex);
        auto &apps = emuenv.app.apps_list.apps;
        std::erase_if(apps, [](const AppEntry &a) { return !a.archive_path.empty(); }); // drop prior ROM rows
        for (auto &rom : roms)
            apps.push_back(std::move(rom));
    }

    LOG_INFO("ROMs folder scan: {} playable, {} unreadable", roms.size(), failures.size());
    return failures;
}

} // namespace app
