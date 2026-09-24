#include <updater/functions.h>

#include <config/version.h>
#include <fmt/format.h>

namespace updater {

std::string release_api_url() {
    return "https://api.github.com/repos/sayenah/Vita3k-NoInstall/releases/tags/latest";
}

std::string release_page_url() {
    return "https://github.com/sayenah/Vita3k-NoInstall/releases/tag/latest";
}

std::string display_version(const UpdateInfo &info) {
    if (!info.version.empty())
        return fmt::format("{} ({})", info.version, info.build_number);

    return fmt::format("Build {}", info.build_number);
}

std::string current_display_version() {
    return fmt::format("{} ({})", app_version, app_number);
}

bool is_official_build() {
    return ::is_official_build;
}

} // namespace updater
