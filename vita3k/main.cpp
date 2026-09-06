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

#include "archive.h"
#include "interface.h"

#include <app/functions.h>
#include <config/functions.h>
#include <config/version.h>
#include <emuenv/state.h>
#include <gui-qt/gui_language.h>
#include <gui-qt/gui_settings.h>
#include <gui-qt/log_widget.h>
#include <gui-qt/main_window.h>
#include <gui-qt/persistent_settings.h>
#include <include/cpu.h>
#include <include/environment.h>
#include <io/bundle.h>
#include <io/state.h>
#include <modules/module_parent.h>
#include <packages/functions.h>
#include <packages/license.h>
#include <packages/pkg.h>
#include <packages/sfo.h>
#include <shader/spirv_recompiler.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <QApplication>
#include <QMessageBox>
#include <QMetaObject>
#include <QTimer>

#if USE_DISCORD
#include <app/discord.h>
#endif

#ifdef _WIN32
#include <combaseapi.h>
#define SDL_MAIN_HANDLED
#endif

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

#include "gui-qt/qt_utils.h"

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_main.h>

#include <chrono>
#include <cstdlib>
#include <optional>

namespace {

int validation_env_int(const char *name, int fallback, int minimum, int maximum) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;

    const int parsed = std::atoi(value);
    if (parsed < minimum || parsed > maximum)
        return fallback;
    return parsed;
}

std::string json_escape(std::string value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

struct ValidationPreparationSnapshot {
    std::string source_path;
    std::string title_id;
    std::string effective_app_version;
    std::size_t mounted_dlc = 0;
    std::size_t license_rifs = 0;
};

ValidationPreparationSnapshot snapshot_prepared_game(const EmuEnvState &emuenv, const fs::path &source_path, const std::string &title_id) {
    ValidationPreparationSnapshot snapshot;
    snapshot.source_path = fs_utils::path_to_utf8(source_path);
    snapshot.title_id = title_id;

    const fs::path temp_root = emuenv.cache_path / "pkgplay";
    std::vector<uint8_t> sfo_bytes;
    if (fs_utils::read_data(temp_root / "app/sce_sys/param.sfo", sfo_bytes)) {
        sfo::SfoAppInfo info;
        sfo::get_param_info(info, sfo_bytes, emuenv.cfg.sys_lang);
        snapshot.effective_app_version = info.app_version;
    }

    boost::system::error_code ec;
    const fs::path addcont_root = temp_root / "addcont";
    if (fs::is_directory(addcont_root, ec)) {
        for (fs::directory_iterator it(addcont_root, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (fs::is_directory(it->path(), ec))
                ++snapshot.mounted_dlc;
        }
    }

    ec.clear();
    const fs::path license_root = emuenv.vita_fs_path / "ux0/license" / title_id;
    if (fs::is_directory(license_root, ec)) {
        for (fs::recursive_directory_iterator it(license_root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!fs::is_regular_file(it->path(), ec))
                continue;
            if (string_utils::tolower(it->path().extension().string()) == ".rif")
                ++snapshot.license_rifs;
        }
    }

    return snapshot;
}

void write_validation_result(const fs::path &path, const ValidationPreparationSnapshot &snapshot,
    bool boot_started, bool frames_observed, bool stable_runtime_complete,
    int requested_runtime_seconds, int boot_timeout_seconds, int observed_runtime_ms,
    const std::string &reason) {
    boost::system::error_code ec;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path(), ec);

    fs::ofstream out(path, std::ios::out | std::ios::binary);
    if (!out) {
        LOG_ERROR("VALIDATION: could not write result file {}", fs_utils::path_to_utf8(path));
        return;
    }

    const bool pass = boot_started && frames_observed && stable_runtime_complete;
    out << "{\n"
        << "  \"status\": \"" << (pass ? "pass" : "fail") << "\",\n"
        << "  \"reason\": \"" << json_escape(reason) << "\",\n"
        << "  \"source_path\": \"" << json_escape(snapshot.source_path) << "\",\n"
        << "  \"title_id\": \"" << json_escape(snapshot.title_id) << "\",\n"
        << "  \"effective_app_version\": \"" << json_escape(snapshot.effective_app_version) << "\",\n"
        << "  \"mounted_dlc\": " << snapshot.mounted_dlc << ",\n"
        << "  \"license_rifs\": " << snapshot.license_rifs << ",\n"
        << "  \"boot_started\": " << (boot_started ? "true" : "false") << ",\n"
        << "  \"frames_observed\": " << (frames_observed ? "true" : "false") << ",\n"
        << "  \"stable_runtime_complete\": " << (stable_runtime_complete ? "true" : "false") << ",\n"
        << "  \"requested_runtime_seconds\": " << requested_runtime_seconds << ",\n"
        << "  \"boot_timeout_seconds\": " << boot_timeout_seconds << ",\n"
        << "  \"observed_runtime_ms\": " << observed_runtime_ms << "\n"
        << "}\n";
    out.close();

    LOG_INFO("VALIDATION: {} [{}] result={}", pass ? "PASS" : "FAIL", snapshot.title_id, fs_utils::path_to_utf8(path));
}

} // namespace

int main(int argc, char *argv[]) {
#ifdef __APPLE__
    qputenv("QT_MTL_NO_TRANSACTION", "1");
    qputenv("QT_MAC_NO_CONTAINER_LAYER", "1");
#endif

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Vita3K"));
    QCoreApplication::setApplicationName(QStringLiteral("Vita3K"));

#ifdef TRACY_ENABLE
    ZoneScoped; // Tracy - Track main function scope
#endif

    Root root_paths;
    bool portable = app::init_paths(root_paths);

    if (!fs::exists(root_paths.get_vita_fs_path())) {
        fs::create_directories(root_paths.get_vita_fs_path());
    }

    LogWidget::register_callback();
    if (logging::init(root_paths, true) != Success) {
        return InitConfigFailed;
    }

    // Check admin privs before init starts to avoid creating of file as other user by accident
    bool admin_priv = false;
#ifdef _WIN32
    // https://stackoverflow.com/questions/8046097/how-to-check-if-a-process-has-the-administrative-rights
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize)) {
            admin_priv = Elevation.TokenIsElevated;
        }
    }
    if (hToken) {
        CloseHandle(hToken);
    }
#else
    auto uid = getuid();
    auto euid = geteuid();

    // if either effective uid or uid is the one of the root user assume running as root.
    // else if euid and uid are different then permissions errors can happen if its running
    // as a completely different user than the uid/euid
    if (uid == 0 || euid == 0 || uid != euid)
        admin_priv = true;
#endif

    Config cfg{};
    EmuEnvState emuenv;
    const auto config_err = config::init_config(cfg, argc, argv, root_paths, portable);

    fs::create_directories(cfg.get_vita_fs_path());

    if (config_err != Success) {
        if (config_err == QuitRequested) {
            if (cfg.recompile_shader_path.has_value()) {
                LOG_INFO("Recompiling {}", *cfg.recompile_shader_path);
                shader::convert_gxp_to_glsl_from_filepath(*cfg.recompile_shader_path);
            }
            if (cfg.delete_title_id.has_value()) {
                LOG_INFO("Deleting title id {}", *cfg.delete_title_id);
                fs::remove_all(cfg.get_vita_fs_path() / "ux0/app" / *cfg.delete_title_id);
                fs::remove_all(cfg.get_vita_fs_path() / "ux0/addcont" / *cfg.delete_title_id);
                fs::remove_all(cfg.get_vita_fs_path() / "ux0/user/00/savedata" / *cfg.delete_title_id);
                fs::remove_all(root_paths.get_cache_path() / "shaders" / *cfg.delete_title_id);
            }
            if (cfg.pup_path.has_value()) {
                LOG_INFO("Installing firmware file {}", *cfg.pup_path, [](uint32_t progress) {
                    LOG_INFO("Firmware installation progress: {}%", progress);
                });
            }
            if (cfg.pkg_path.has_value() && cfg.pkg_zrif.has_value()) {
                LOG_INFO("Installing pkg from {} ", *cfg.pkg_path);
                emuenv.cache_path = root_paths.get_cache_path().generic_path();
                emuenv.vita_fs_path = cfg.get_vita_fs_path();
                auto pkg_path = fs_utils::utf8_to_path(*cfg.pkg_path);
                install_pkg(pkg_path, emuenv, *cfg.pkg_zrif, [](float) {});
            }
            return Success;
        }
        LOG_ERROR("Failed to initialise config");
        return InitConfigFailed;
    }

    gui::i18n::apply_ui_language(app, cfg.user_lang, emuenv.static_assets_path);

#ifdef _WIN32
    {
        auto res = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        LOG_ERROR_IF(res == S_FALSE, "Failed to initialize COM Library");
    }
#endif

    if (cfg.console) {
        if (logging::init(root_paths, false) != Success)
            return InitConfigFailed;
    } else {
        std::atexit(SDL_Quit);

        // Joystick events on background thread
        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
        // Enable HIDAPI rumble for DS4/DS
        SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
        // Enable Switch controller
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");

        if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_SENSOR | SDL_INIT_CAMERA)) {
            LOG_ERROR("SDL initialisation failed: {}", SDL_GetError());
            QMessageBox::critical(nullptr, "Error", "SDL initialisation failed.");
            return SDLInitFailed;
        }
    }

    LOG_INFO("{}", window_title);
    LOG_INFO("OS: {}", CppCommon::Environment::OSVersion());
    LOG_INFO("CPU: {} | {} Threads | {} GHz", CppCommon::CPU::Architecture(), CppCommon::CPU::LogicalCores(), static_cast<float>(CppCommon::CPU::ClockSpeed()) / 1000.f);
    LOG_INFO("Available ram memory: {} MiB", SDL_GetSystemRAM());

    app::AppRunType run_type = app::AppRunType::Unknown;
    if (cfg.run_app_path)
        run_type = app::AppRunType::Extracted;

    if (!app::init(emuenv, cfg, root_paths)) {
        QMessageBox::critical(nullptr, "Error", "Emulated environment initialization failed.");
        return 1;
    }

    if (emuenv.cfg.controller_binds.empty() || (emuenv.cfg.controller_binds.size() != 15)
        || emuenv.cfg.controller_axis_binds.empty() || (emuenv.cfg.controller_axis_binds.size() != 6))
        app::reset_controller_binding(emuenv);

    init_libraries(emuenv);

    if (!app::init_apps_list(emuenv)) {
        LOG_ERROR("Failed to initialize apps list.");
        return 1;
    }

    app::load_users(emuenv);

    if (cfg.content_path.has_value()) {
        const auto extension = string_utils::tolower(cfg.content_path->extension().string());
        const auto is_archive = (extension == ".vpk") || (extension == ".zip");
        const auto is_rif = (extension == ".rif") || (cfg.content_path->filename() == "work.bin");
        const auto is_directory = fs::is_directory(*cfg.content_path);

        std::string boot_title_id;

        if (is_archive) {
            LOG_INFO("Installing archive from CLI: {}", cfg.content_path->string());
            std::vector<ContentInfo> contents_info = install_archive(emuenv, *cfg.content_path);
            const auto content_index = std::find_if(contents_info.begin(), contents_info.end(), [](const ContentInfo &c) {
                return c.category == "gd";
            });
            if (content_index != contents_info.end() && content_index->state)
                boot_title_id = content_index->title_id;
        } else if (is_directory) {
            LOG_INFO("Installing contents from CLI: {}", cfg.content_path->string());
            if (install_contents(emuenv, *cfg.content_path) == 1 && emuenv.app_info.app_category == "gd")
                boot_title_id = emuenv.app_info.app_title_id;
        } else if (is_rif) {
            LOG_INFO("Installing license from CLI: {}", cfg.content_path->string());
            copy_license(emuenv, *cfg.content_path);
        } else {
            LOG_ERROR("File: [{}] is not a supported content type.", cfg.content_path->string());
        }

        cfg.content_path.reset();

        if (!boot_title_id.empty()) {
            cfg.run_app_path = boot_title_id;
            LOG_INFO("Content installed, will auto-boot: {}", boot_title_id);
        }

        if (!app::init_apps_list(emuenv)) {
            LOG_ERROR("Failed to refresh apps list after content install.");
        }
    }

    // Frontends (ES-DE etc.) don't need to pick the right flag: --bundle pointed at an archive FILE
    // behaves like --play-pkg, and --play-pkg pointed at a DIRECTORY behaves like --bundle.
    if (emuenv.cfg.bundle_path.has_value() && !emuenv.cfg.play_pkg_path.has_value() && fs::is_regular_file(*emuenv.cfg.bundle_path)) {
        emuenv.cfg.play_pkg_path = emuenv.cfg.bundle_path;
        emuenv.cfg.bundle_path.reset();
    } else if (emuenv.cfg.play_pkg_path.has_value() && !emuenv.cfg.bundle_path.has_value() && fs::is_directory(*emuenv.cfg.play_pkg_path)) {
        emuenv.cfg.bundle_path = emuenv.cfg.play_pkg_path;
        emuenv.cfg.play_pkg_path.reset();
    }

    const bool validation_mode = std::getenv("VITA3K_VALIDATE_GAME") != nullptr;
    const int validation_runtime_seconds = validation_env_int("VITA3K_VALIDATE_RUNTIME", 10, 1, 600);
    const int validation_boot_timeout_seconds = validation_env_int("VITA3K_VALIDATE_BOOT_TIMEOUT", 60, 5, 1800);
    const char *validation_result_env = std::getenv("VITA3K_VALIDATE_RESULT");
    fs::path validation_result_path;
    if (validation_result_env && *validation_result_env)
        validation_result_path = fs_utils::utf8_to_path(validation_result_env);
    else
        validation_result_path = emuenv.cache_path / "validation-result.json";
    ValidationPreparationSnapshot validation_snapshot;
    const std::string validation_original_user_id = emuenv.cfg.user_id;
    std::string validation_user_id;

    // Dev/testing (P0): mount a Game Bundle directory and boot it directly, with no install into
    // ux0/app. A synthetic apps-list entry lets the normal boot path (set_app_info -> load_app)
    // resolve to the mounted bundle; the mount serves app0:/addcont0: reads (see io/bundle.h).
    // NB: app::init above moved the local `cfg` into emuenv.cfg, so read the bundle path and set
    // run_app_path on emuenv.cfg — that is the live config MainWindow boots from.
    if (emuenv.cfg.bundle_path.has_value()) {
        bundle::Manifest manifest;
        std::string bundle_error;
        auto backend = bundle::open_directory_backend(*emuenv.cfg.bundle_path, manifest, bundle_error);
        if (!backend) {
            LOG_CRITICAL("Failed to mount Game Bundle at {}: {}", emuenv.cfg.bundle_path->string(), bundle_error);
            return 1;
        }
        emuenv.io.mount = bundle::make_mount(backend, manifest);

        app::AppEntry entry;
        entry.title_id = manifest.title_id;
        entry.path = manifest.title_id;
        entry.addcont = manifest.title_id;
        entry.savedata = manifest.title_id;
        entry.content_id = manifest.content_id;
        entry.category = manifest.category.empty() ? "gd" : manifest.category;
        entry.title = manifest.title_id; // real title is loaded from the bundle's param.sfo at boot
        entry.stitle = manifest.title_id;
        entry.app_ver = "N/A";
        entry.parental_level = "N/A";
        {
            std::lock_guard<std::mutex> lock(emuenv.app.apps_list.mutex);
            auto &apps = emuenv.app.apps_list.apps;
            std::erase_if(apps, [&](const app::AppEntry &a) { return a.path == entry.path; });
            apps.push_back(entry);
        }
        emuenv.cfg.run_app_path = manifest.title_id;
        LOG_INFO("Mounted Game Bundle [{}] from {}; booting directly", manifest.title_id, emuenv.cfg.bundle_path->string());
    }

    // Play a self-contained NoNpDrm pkg with no permanent install: decrypt to temp, mount, boot;
    // the temp tree is deleted when the game stops (io_deinit).
    if (emuenv.cfg.play_pkg_path.has_value()) {
        const fs::path source_path = *emuenv.cfg.play_pkg_path;
        std::string play_error;
        const std::string title_id = mount_pkg_for_play(emuenv, source_path, play_error);
        if (title_id.empty()) {
            LOG_CRITICAL("Failed to play pkg {}: {}", source_path.string(), play_error);
            if (validation_mode) {
                validation_snapshot.source_path = fs_utils::path_to_utf8(source_path);
                write_validation_result(validation_result_path, validation_snapshot, false, false, false,
                    validation_runtime_seconds, validation_boot_timeout_seconds, 0, "preparation_failed: " + play_error);
            }
            return 1;
        }
        emuenv.cfg.run_app_path = title_id;
        LOG_INFO("Playing pkg [{}] without install; booting directly", title_id);
        if (validation_mode)
            validation_snapshot = snapshot_prepared_game(emuenv, source_path, title_id);
    }

    // Validation boots use an isolated throw-away Vita user. Saves, trophies and play history created
    // by the short automated boot therefore never touch the user's real profile. The temporary user
    // is deleted and the original active user restored after the validation event loop exits.
    if (validation_mode) {
        validation_user_id = app::create_user(emuenv, "Vita3K Validator");
        if (validation_user_id.empty() || !app::activate_user(emuenv, validation_user_id)) {
            write_validation_result(validation_result_path, validation_snapshot, false, false, false,
                validation_runtime_seconds, validation_boot_timeout_seconds, 0, "could_not_create_isolated_validation_user");
            return 1;
        }
        emuenv.cfg.user_id = validation_user_id;
        LOG_INFO("VALIDATION: using isolated temporary user [{}]", validation_user_id);
    }

    const QString gui_configs_dir = gui::utils::to_qt_path(emuenv.config_path / "gui-configs");
    auto gui_settings = std::make_shared<GuiSettings>(gui_configs_dir);
    auto persistent_settings = std::make_shared<PersistentSettings>(gui_configs_dir);

    MainWindow mainwindow(emuenv, gui_settings, persistent_settings, admin_priv);

    bool validation_pass = false;
    if (validation_mode) {
        LOG_INFO("VALIDATION: armed runtime={}s boot-timeout={}s result={}",
            validation_runtime_seconds, validation_boot_timeout_seconds, fs_utils::path_to_utf8(validation_result_path));

        const auto validation_started = std::chrono::steady_clock::now();
        auto first_frame_at = validation_started;
        auto last_render_activity_at = validation_started;
        bool boot_started = false;
        bool frames_observed = false;
        bool first_frame_recorded = false;

        auto *validation_timer = new QTimer(&mainwindow);
        validation_timer->setInterval(250);
        QObject::connect(validation_timer, &QTimer::timeout, &mainwindow, [&]() {
            if (QWidget *modal = QApplication::activeModalWidget()) {
                if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                    LOG_WARN("VALIDATION: closing modal dialog: {}", box->text().toStdString());
                    box->done(QMessageBox::Ok);
                }
            }

            if (emuenv.main_thread_id != 0)
                boot_started = true;

            const auto now = std::chrono::steady_clock::now();
            if (emuenv.frame_count > 0) {
                last_render_activity_at = now;
                if (!frames_observed) {
                    frames_observed = true;
                    first_frame_recorded = true;
                    first_frame_at = now;
                    LOG_INFO("VALIDATION: first rendered frames observed for [{}]", validation_snapshot.title_id);
                }
            }

            const int total_elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - validation_started).count());
            const int stable_elapsed_ms = first_frame_recorded
                ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - first_frame_at).count())
                : 0;
            const int render_idle_ms = frames_observed
                ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render_activity_at).count())
                : total_elapsed_ms;

            // A single rendered frame is not enough. At the end of the requested window, require
            // evidence that frames were still arriving recently; otherwise keep observing until the
            // boot timeout in case rendering recovers.
            constexpr int max_render_idle_ms = 2000;
            const bool rendering_is_current = frames_observed && render_idle_ms <= max_render_idle_ms;
            const bool stable_runtime_complete = rendering_is_current && stable_elapsed_ms >= validation_runtime_seconds * 1000;
            const bool timed_out = total_elapsed_ms >= validation_boot_timeout_seconds * 1000;
            if (!stable_runtime_complete && !timed_out)
                return;

            validation_pass = boot_started && frames_observed && stable_runtime_complete;
            std::string reason;
            if (validation_pass)
                reason = "booted_and_sustained_rendering";
            else if (frames_observed)
                reason = "rendering_stalled_before_validation_completed";
            else if (boot_started)
                reason = "boot_started_but_no_rendered_frames_before_timeout";
            else
                reason = "boot_did_not_start_before_timeout";

            write_validation_result(validation_result_path, validation_snapshot, boot_started, frames_observed,
                stable_runtime_complete, validation_runtime_seconds, validation_boot_timeout_seconds,
                stable_elapsed_ms, reason);

            validation_timer->stop();
            QMetaObject::invokeMethod(&mainwindow, "on_stop_triggered", Qt::DirectConnection);
            QTimer::singleShot(250, &app, &QCoreApplication::quit);
        });
        validation_timer->start();
    }

    mainwindow.show();
    if (validation_mode || mainwindow.prompt_startup_warnings())
        app.exec();

    if (validation_mode && !validation_user_id.empty()) {
        app::delete_user(emuenv, validation_user_id);
        emuenv.cfg.user_id = validation_original_user_id;
        if (!validation_original_user_id.empty())
            app::activate_user(emuenv, validation_original_user_id);
        config::serialize_config(emuenv.cfg, emuenv.cfg.config_path);
        LOG_INFO("VALIDATION: removed temporary user and restored [{}]", validation_original_user_id);
    }

#ifdef _WIN32
    CoUninitialize();
#endif

    if (validation_mode && !validation_pass)
        return 2;
    return Success;
}
