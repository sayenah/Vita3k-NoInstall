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
 * @file pkg.cpp
 * @brief PlayStation Vita software package (`.pkg`) handling
 */

#include <F00DKeyEncryptorFactory.h>
#include <PsvPfsParserConfig.h>
#include <Utils.h>
#include <miniz.h>
#include <openssl/evp.h>
#include <rif2zrif.h>

#include <io/bundle.h>
#include <io/functions.h>

#include <config/state.h>
#include <emuenv/state.h>

#include <packages/archive_7z.h>
#include <packages/functions.h>
#include <packages/license.h>
#include <packages/pkg.h>
#include <packages/sce_types.h>
#include <packages/sfo.h>

#include <util/bytes.h>
#include <util/log.h>
#include <util/string_utils.h>

// Credits to mmozeiko https://github.com/mmozeiko/pkg2zip

static void ctr_init(uint8_t *counter, uint8_t *iv, uint64_t n) {
    for (int i = 15; i >= 0; i--) {
        n = n + iv[i];
        counter[i] = (uint8_t)n;
        n >>= 8;
    }
}

static int execute(std::string &zrif, fs::path &title_src, fs::path &title_dst, F00DEncryptorTypes type, std::string &f00d_arg, PfsProgressCallback progress = nullptr) {
    std::string title_src_str = title_src.string();
    std::string title_dst_str = title_dst.string();
    return execute(zrif, title_src_str, title_dst_str, type, f00d_arg, progress);
}

bool decrypt_install_nonpdrm(EmuEnvState &emuenv, const fs::path &drmlicpath, const fs::path &title_path, const std::function<void(float)> &progress_callback) {
    fs::path title_id_src = title_path;
    fs::path title_id_dst = fs_utils::path_concat(title_path, "_dec");
    fs::ifstream binfile(drmlicpath, std::ios::in | std::ios::binary | std::ios::ate);
    std::string zRIF = rif2zrif(binfile);
    F00DEncryptorTypes f00d_enc_type = F00DEncryptorTypes::native;
    std::string f00d_arg = std::string();

    PfsProgressCallback pfs_progress = nullptr;
    if (progress_callback) {
        pfs_progress = [&progress_callback](std::uint64_t processed, std::uint64_t total, const std::string &) {
            progress_callback(total ? static_cast<float>(processed) / static_cast<float>(total) : 1.f);
        };
    }

    if ((execute(zRIF, title_id_src, title_id_dst, f00d_enc_type, f00d_arg, pfs_progress) < 0) && (title_path.string().find("theme") == std::string::npos))
        return false;

    if (!emuenv.app_info.app_category.contains("gp"))
        copy_license(emuenv, drmlicpath);

    fs::remove_all(title_id_src);
    fs::rename(title_id_dst, title_id_src);

    return true;
}

bool install_pkg(const fs::path &pkg_path, EmuEnvState &emuenv, std::string &p_zRIF, const std::function<void(float)> &progress_callback, const fs::path &temp_root) {
    const bool temp_mode = !temp_root.empty();
    FILE *infile = FOPEN(pkg_path.c_str(), "rb");
    if (!infile) {
        LOG_CRITICAL("Failed to load pkg file in path: {}", fs_utils::path_to_utf8(pkg_path));
        return false;
    }

    fseek(infile, 0, SEEK_END);
    const uint64_t pkg_size = ftell(infile);

    PkgHeader pkg_header;
    PkgExtHeader ext_header;
    fseek(infile, 0, SEEK_SET);
    fread(reinterpret_cast<void *>(&pkg_header), sizeof(PkgHeader), 1, infile);
    fseek(infile, sizeof(PkgHeader), SEEK_SET);
    fread(reinterpret_cast<char *>(&ext_header), sizeof(PkgExtHeader), 1, infile);

    progress_callback(0);

    if (byte_swap(pkg_header.magic) != 0x7F504b47 && byte_swap(ext_header.magic) != 0x7F657874) {
        LOG_ERROR("Not a valid pkg file!");
        return false;
    }

    if (pkg_size < byte_swap(pkg_header.total_size)) {
        LOG_ERROR("The pkg file is too small");
        return false;
    }

    if (pkg_size < byte_swap(pkg_header.data_offset) + byte_swap(pkg_header.file_count) * 32) {
        LOG_ERROR("The pkg file is too small");
        return false;
    }

    uint32_t info_offset = byte_swap(pkg_header.info_offset);
    uint32_t content_type = 0;
    uint32_t sfo_offset = 0;
    uint32_t sfo_size = 0;
    uint32_t items_offset = 0;

    for (uint32_t i = 0; i < byte_swap(pkg_header.info_count); i++) {
        uint32_t block[4];
        fseek(infile, info_offset, SEEK_SET);
        fread(block, sizeof(block), 1, infile);

        auto type = byte_swap(block[0]);
        auto size = byte_swap(block[1]);

        switch (type) {
        case 2:
            content_type = byte_swap(block[2]);
            break;
        case 13:
            items_offset = byte_swap(block[2]);
            break;
        case 14:
            sfo_offset = byte_swap(block[2]);
            sfo_size = byte_swap(block[3]);
            break;
        default:
            break;
        }

        info_offset += 2 * sizeof(uint32_t) + size;
    }

    PkgType type;

    switch (content_type) {
    case 0x15:
        type = PkgType::PKG_TYPE_VITA_APP;
        break;
    case 0x16:
        type = PkgType::PKG_TYPE_VITA_DLC;
        break;
    case 0x1F:
        type = PkgType::PKG_TYPE_VITA_THEME;
        break;
    default:
        LOG_ERROR("Unsupported content type: {}", content_type);
        return false;
        break;
    }

    auto key_type = byte_swap(ext_header.data_type2) & 7;

    uint8_t main_key[16];
    const uint8_t *pkg_vita_key = nullptr;
    switch (key_type) {
    case 2:
        pkg_vita_key = pkg_vita_2;
        break;
    case 3:
        pkg_vita_key = pkg_vita_3;
        break;
    case 4:
        pkg_vita_key = pkg_vita_4;
        break;
    default:
        LOG_ERROR("Unknown encryption key");
        return false;
        break;
    }

    EVP_CIPHER_CTX *cipher_ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER *cipher_CTR = EVP_CIPHER_fetch(nullptr, "AES-128-CTR", nullptr);
    EVP_CIPHER *cipher_ECB = EVP_CIPHER_fetch(nullptr, "AES-128-ECB", nullptr);
    int dec_len = 0;

    auto evp_cleanup = [&]() {
        EVP_CIPHER_CTX_free(cipher_ctx);
        EVP_CIPHER_free(cipher_CTR);
        EVP_CIPHER_free(cipher_ECB);
    };

    // get the main key
    EVP_EncryptInit_ex(cipher_ctx, cipher_ECB, nullptr, pkg_vita_key, nullptr);
    EVP_CIPHER_CTX_set_padding(cipher_ctx, 0);
    EVP_EncryptUpdate(cipher_ctx, main_key, &dec_len, pkg_header.pkg_data_iv, 0x10);
    EVP_EncryptFinal_ex(cipher_ctx, main_key + dec_len, &dec_len);

    std::vector<uint8_t> sfo_buffer(sfo_size);
    SfoFile sfo_file;
    fseek(infile, sfo_offset, SEEK_SET);
    fread(sfo_buffer.data(), sfo_buffer.size(), 1, infile);
    sfo::load(sfo_file, sfo_buffer);
    sfo::get_param_info(emuenv.app_info, sfo_buffer, emuenv.cfg.sys_lang);

    if (type == PkgType::PKG_TYPE_VITA_DLC)
        emuenv.app_info.app_content_id = emuenv.app_info.app_content_id.substr(20);

    if (type == PkgType::PKG_TYPE_VITA_APP && strcmp(emuenv.app_info.app_category.c_str(), "gp") == 0) {
        type = PkgType::PKG_TYPE_VITA_PATCH;
    }

    if (temp_mode && type != PkgType::PKG_TYPE_VITA_APP && type != PkgType::PKG_TYPE_VITA_DLC) {
        LOG_ERROR("Play-without-install supports base-game and DLC pkgs (got content type {})", content_type);
        return false;
    }

    // temp_mode decrypts the app tree into temp_root/app (mounted read-only, no ux0/app install).
    auto path{ temp_mode ? temp_root : emuenv.vita_fs_path / "ux0" };

    switch (type) {
    case PkgType::PKG_TYPE_VITA_APP:
        path /= temp_mode ? fs::path("app") : fs::path("app") / emuenv.app_info.app_title_id;
        if (fs::exists(path))
            fs::remove_all(path);
        emuenv.app_info.app_title += " (App)";
        break;
    case PkgType::PKG_TYPE_VITA_DLC:
        // Installed layout is addcont/<title>/<content>; the play-mount serves addcont0: from
        // temp_root/addcont/<content> (one game per mount, so the title-id level is redundant and,
        // more importantly, is stripped by BundleMount::map_ux0_path -- keep the temp tree matching).
        path /= temp_mode
            ? fs::path("addcont") / emuenv.app_info.app_content_id
            : fs::path("addcont") / emuenv.app_info.app_title_id / emuenv.app_info.app_content_id;
        emuenv.app_info.app_title += " (DLC)";
        break;
    case PkgType::PKG_TYPE_VITA_PATCH:
        path /= fs::path("patch") / emuenv.app_info.app_title_id;
        emuenv.app_info.app_title += " (Update)";
        break;
    case PkgType::PKG_TYPE_VITA_THEME:
        path /= fs::path("theme") / emuenv.app_info.app_content_id;
        emuenv.app_info.app_category = "theme";
        emuenv.app_info.app_title += " (Theme)";
        break;
    }

    auto decrypt_aes_ctr = [&](uint32_t offset, unsigned char *data, size_t size) {
        uint8_t counter[0x10];
        ctr_init(counter, pkg_header.pkg_data_iv, offset);
        EVP_DecryptInit_ex(cipher_ctx, cipher_CTR, nullptr, main_key, counter);
        EVP_CIPHER_CTX_set_padding(cipher_ctx, 0);
        EVP_DecryptUpdate(cipher_ctx, data, &dec_len, data, size);
        EVP_DecryptFinal_ex(cipher_ctx, data + dec_len, &dec_len);
    };

    std::vector<uint8_t> buffer(0x10000);
    for (uint32_t i = 0; i < byte_swap(pkg_header.file_count); i++) {
        PkgEntry entry;
        uint64_t file_offset = items_offset + i * 32;
        fseek(infile, byte_swap(pkg_header.data_offset) + file_offset, SEEK_SET);
        fread(&entry, sizeof(PkgEntry), 1, infile);

        decrypt_aes_ctr(file_offset / 16, reinterpret_cast<unsigned char *>(&entry), sizeof(PkgEntry));

        if (pkg_size < byte_swap(pkg_header.data_offset) + byte_swap(entry.name_offset) + byte_swap(entry.name_size) || pkg_size < byte_swap(pkg_header.data_offset) + byte_swap(entry.data_offset) + byte_swap(entry.data_size)) {
            LOG_ERROR("The pkg file size is too small, possibly corrupted");
            evp_cleanup();
            return false;
        }
        const auto file_count = (float)byte_swap(pkg_header.file_count);
        progress_callback(i / file_count * 100.f * 0.6f);
        std::vector<unsigned char> name(byte_swap(entry.name_size));
        fseek(infile, byte_swap(pkg_header.data_offset) + byte_swap(entry.name_offset), SEEK_SET);
        fread(name.data(), byte_swap(entry.name_size), 1, infile);

        decrypt_aes_ctr(byte_swap(entry.name_offset) / 16, name.data(), byte_swap(entry.name_size));

        auto string_name = std::string(name.begin(), name.end());
        LOG_INFO(string_name);

        if ((byte_swap(entry.type) & 0xFF) == 4 || (byte_swap(entry.type) & 0xFF) == 18) { // Directory
            fs::create_directories(path / string_name);
        } else { // File
            fs::ofstream outfile(path / string_name, std::ios::binary);

            auto offset = byte_swap(entry.data_offset);
            auto data_size = byte_swap(entry.data_size);

            uint8_t counter[0x10];
            ctr_init(counter, pkg_header.pkg_data_iv, offset / 16);
            EVP_DecryptInit_ex(cipher_ctx, cipher_CTR, nullptr, main_key, counter);
            EVP_CIPHER_CTX_set_padding(cipher_ctx, 0);

            fseek(infile, byte_swap(pkg_header.data_offset) + offset, SEEK_SET);
            while (data_size != 0) {
                size_t size = data_size < buffer.size() ? data_size : buffer.size();
                fread(buffer.data(), size, 1, infile);

                EVP_DecryptUpdate(cipher_ctx, buffer.data(), &dec_len, buffer.data(), size);

                outfile.write(reinterpret_cast<char *>(buffer.data()), dec_len);
                data_size -= size;
            }

            EVP_DecryptFinal_ex(cipher_ctx, buffer.data(), &dec_len);
            outfile.write(reinterpret_cast<char *>(buffer.data()), dec_len);
            outfile.close();
        }
    }
    fclose(infile);

    evp_cleanup();
    fs::path title_id_src = path;
    fs::path title_id_dst = fs_utils::path_concat(path, "_dec");
    std::string zRIF = p_zRIF;

    // Self-contained NoNpDrm dumps carry their license in sce_sys/package/work.bin. When no external
    // zRIF was supplied, derive one from work.bin so the PFS layer decrypts without a separate key.
    const auto workbin_path = path / "sce_sys/package/work.bin";
    if (zRIF.empty() && fs::exists(workbin_path)) {
        fs::ifstream binfile(workbin_path, std::ios::in | std::ios::binary | std::ios::ate);
        zRIF = rif2zrif(binfile);
    }

    // Original (non-NoNpDrm) pkgs carry no work.bin. Fall back to a rif the user pre-placed on
    // ux0/license (e.g. the whole tree built by tools/build-license-folder.py): find_pkg_zrif reads
    // the pkg's content id, loads that rif, and turns it back into a zRIF for the PFS decrypt.
    if (zRIF.empty())
        zRIF = find_pkg_zrif(pkg_path, emuenv.vita_fs_path);

    F00DEncryptorTypes f00d_enc_type = F00DEncryptorTypes::native;
    std::string f00d_arg = std::string();

    progress_callback(80);
    switch (type) {
    case PkgType::PKG_TYPE_VITA_APP:
    case PkgType::PKG_TYPE_VITA_PATCH:

        if (execute(zRIF, title_id_src, title_id_dst, f00d_enc_type, f00d_arg) < 0) {
            fs::remove_all(fs::path(title_id_src));
            fs::remove_all(fs::path(title_id_dst));
            return false;
        }
        fs::remove_all(title_id_src);
        fs::rename(title_id_dst, title_id_src);

        break;
    case PkgType::PKG_TYPE_VITA_DLC:

        if (execute(zRIF, title_id_src, title_id_dst, f00d_enc_type, f00d_arg) < 0) {
            fs::remove_all(fs::path(title_id_src));
            fs::remove_all(fs::path(title_id_dst));
            return false;
        }
        fs::remove_all(title_id_src);
        fs::rename(title_id_dst, title_id_src);
        if (temp_mode)
            create_license(emuenv, zRIF); // place the DLC rif on ux0/license so runtime DRM checks resolve
        return true;

    case PkgType::PKG_TYPE_VITA_THEME:

        // Theme don't have keystone file, need skip error
        execute(zRIF, title_id_src, title_id_dst, f00d_enc_type, f00d_arg);
        fs::remove_all(title_id_src);
        fs::rename(title_id_dst, title_id_src);
        return true;
        break;
    }

    // temp_mode keeps the decrypted tree in temp_root (no ux0/app install); copy_path would relocate
    // it into ux0/app, so skip it. The license (below) still goes to the real ux0/license.
    if (!temp_mode && !copy_path(title_id_src, emuenv.vita_fs_path, emuenv.app_info.app_title_id, emuenv.app_info.app_category))
        return false;

    create_license(emuenv, zRIF);

    progress_callback(100);
    return true;
}

// True if the zip contains a decrypted game tree (has an sce_sys/param.sfo entry) rather than a .pkg.
static bool zip_has_decrypted_game(const fs::path &zip_path) {
    FILE *fp = FOPEN(zip_path.c_str(), "rb");
    if (!fp)
        return false;
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        fclose(fp);
        return false;
    }
    bool found = false;
    const mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < num_files && !found; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            continue;
        const std::string name = string_utils::tolower(stat.m_filename);
        if (name.size() >= 17 && name.compare(name.size() - 17, 17, "sce_sys/param.sfo") == 0)
            found = true;
    }
    mz_zip_reader_end(&zip);
    fclose(fp);
    return found;
}

// After extracting a decrypted-game zip into temp_root, find the app tree (the dir holding
// sce_sys/param.sfo) wherever it sits (app/, app/<TITLEID>/, <TITLEID>/, …) and move it to
// temp_root/app so the directory-backend mount can serve it. Renames within temp are cheap.
static bool normalize_app_tree(const fs::path &temp_root, std::string &error_out) {
    boost::system::error_code ec;
    fs::path app_dir;
    for (fs::recursive_directory_iterator it(temp_root, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        const fs::path p = it->path();
        if (p.filename() == "param.sfo" && p.parent_path().filename() == "sce_sys") {
            app_dir = p.parent_path().parent_path();
            break;
        }
    }
    if (app_dir.empty()) {
        error_out = "no sce_sys/param.sfo found inside the zip";
        return false;
    }

    const fs::path canonical_app = temp_root / "app";
    if (fs::equivalent(app_dir, canonical_app, ec) && !ec)
        return true; // already at temp_root/app

    if (fs::equivalent(app_dir, temp_root, ec) && !ec) {
        error_out = "unexpected zip layout (game files at the zip root)";
        return false;
    }

    const fs::path stage = temp_root / "__app_stage";
    fs::remove_all(stage, ec);
    fs::rename(app_dir, stage, ec);
    if (ec) {
        error_out = "failed to relocate app tree";
        return false;
    }
    fs::remove_all(canonical_app, ec); // clear whatever held the app tree (e.g. app/<TITLEID> parent)
    fs::rename(stage, canonical_app, ec);
    if (ec) {
        error_out = "failed to place app tree";
        return false;
    }
    return true;
}

// Extract every member of a zip into dst_dir (preserving its tree). Returns false (error_out set) on
// any failure.
static bool extract_zip_to_dir(const fs::path &zip_path, const fs::path &dst_dir, std::string &error_out) {
    FILE *fp = FOPEN(zip_path.c_str(), "rb");
    if (!fp) {
        error_out = "cannot open zip file";
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        fclose(fp);
        error_out = "not a valid zip archive";
        return false;
    }
    bool ok = true;
    const mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            continue;
        const fs::path out_path = dst_dir / fs_utils::utf8_to_path(stat.m_filename);
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(out_path);
            continue;
        }
        fs::create_directories(out_path.parent_path());
        if (!mz_zip_reader_extract_to_file(&zip, i, fs_utils::path_to_utf8(out_path).c_str(), 0)) {
            error_out = std::string("failed to extract ") + stat.m_filename;
            ok = false;
            break;
        }
    }
    mz_zip_reader_end(&zip);
    fclose(fp);
    return ok;
}

// Extract the first .pkg member of a zip to `out_pkg`. Returns false (error_out set) if the zip can't
// be opened or contains no .pkg.
static bool extract_pkg_from_zip(const fs::path &zip_path, const fs::path &out_pkg, std::string &error_out) {
    FILE *fp = FOPEN(zip_path.c_str(), "rb");
    if (!fp) {
        error_out = "cannot open zip file";
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        fclose(fp);
        error_out = "not a valid zip archive";
        return false;
    }

    int pkg_index = -1;
    const mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        const std::string name = string_utils::tolower(stat.m_filename);
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".pkg") == 0) {
            pkg_index = static_cast<int>(i);
            break;
        }
    }

    if (pkg_index < 0) {
        mz_zip_reader_end(&zip);
        fclose(fp);
        error_out = "no .pkg found inside the zip";
        return false;
    }

    const bool ok = mz_zip_reader_extract_to_file(&zip, pkg_index, fs_utils::path_to_utf8(out_pkg).c_str(), 0);
    mz_zip_reader_end(&zip);
    fclose(fp);
    if (!ok)
        error_out = "failed to extract .pkg from the zip";
    return ok;
}

// ---- DLC (addcont0:) support for play-without-install ------------------------------------------
//
// DLC plays without install by decrypting/relocating it into temp_root/addcont/<contentid>, which the
// bundle mount serves as addcont0: (BundleMount::map_ux0_path strips the title-id level, so the temp
// uses <contentid> directly, no <title-id>/ level). Sources: an addcont/ folder inside the game
// archive, and the configured DLCs folder (a <TITLEID>/ folder, a <TITLEID>.zip/.7z, or loose .pkg
// files whose embedded title id matches the game).

// Title id embedded in a .pkg header's content id (chars 7..15), or "" if unreadable.
static std::string dlc_pkg_title_id(const fs::path &pkg_path) {
    FILE *f = FOPEN(pkg_path.c_str(), "rb");
    if (!f)
        return {};
    PkgHeader h{};
    const size_t n = fread(&h, 1, sizeof(PkgHeader), f);
    fclose(f);
    if (n < sizeof(PkgHeader))
        return {};
    const std::string cid(h.content_id);
    return cid.size() >= 16 ? cid.substr(7, 9) : std::string{};
}

// Decrypt one DLC .pkg into temp_root/addcont/<contentid> (install_pkg temp_mode). The zRIF is
// self-served from the pkg's work.bin or a pre-placed ux0/license rif.
static void decrypt_dlc_pkg(EmuEnvState &emuenv, const fs::path &pkg_path, const fs::path &temp_root) {
    // Only decrypt actual DLC. A base-game pkg shares the title id and, in temp mode, would decrypt
    // into temp_root/app and clobber the mounted game -- the pkg's own CATEGORY is "ac" for DLC.
    std::vector<uint8_t> sfo;
    if (read_pkg_param_sfo(pkg_path, sfo)) {
        sfo::SfoAppInfo info;
        sfo::get_param_info(info, sfo, emuenv.cfg.sys_lang);
        if (info.app_category != "ac") {
            LOG_INFO("DLC: skipping {} -- not DLC (category '{}')", fs_utils::path_to_utf8(pkg_path.filename()), info.app_category);
            return;
        }
    }
    std::string zrif;
    if (install_pkg(pkg_path, emuenv, zrif, [](float) {}, temp_root))
        LOG_INFO("DLC: mounted {}", fs_utils::path_to_utf8(pkg_path.filename()));
    else
        LOG_WARN("DLC: could not decrypt {} (needs a work.bin or a ux0/license rif)", fs_utils::path_to_utf8(pkg_path.filename()));
}

// Relocate already-decrypted DLC content (an addcont/ tree) into temp_root/addcont/<contentid>,
// collapsing a redundant <title_id>/ level if the tree uses the installed ux0 layout.
static void place_addcont_dir(const fs::path &addcont_dir, const fs::path &temp_root, const std::string &title_id) {
    boost::system::error_code ec;
    const fs::path dst_root = temp_root / "addcont";
    fs::create_directories(dst_root, ec);
    fs::path base = addcont_dir;
    if (fs::is_directory(addcont_dir / title_id, ec))
        base = addcont_dir / title_id;
    for (fs::directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!fs::is_directory(it->path(), ec))
            continue;
        const fs::path dst = dst_root / it->path().filename();
        if (fs::equivalent(it->path(), dst, ec) && !ec)
            continue; // already in place
        ec.clear();
        fs::remove_all(dst, ec);
        fs::rename(it->path(), dst, ec);
        if (ec) { // e.g. cross-device rename: fall back to a recursive copy
            ec.clear();
            fs::copy(it->path(), dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
    }
}

// Relocate any addcont/ trees at `root` or one level below it (covers "addcont/" and
// "<TITLEID>/addcont/"), skipping the game's own app tree.
static void collect_addcont_trees(const fs::path &root, const fs::path &temp_root, const std::string &title_id) {
    boost::system::error_code ec;
    const fs::path app_dir = temp_root / "app";
    const auto consider = [&](const fs::path &d) {
        if (fs::is_directory(d, ec))
            place_addcont_dir(d, temp_root, title_id);
    };
    consider(root / "addcont");
    for (fs::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::path p = it->path();
        if (!fs::is_directory(p, ec) || (fs::equivalent(p, app_dir, ec) && !ec)) {
            ec.clear();
            continue;
        }
        ec.clear();
        consider(p / "addcont");
    }
}

// Decrypt every .pkg under `dir`. match_all=false only decrypts pkgs whose embedded title id equals
// `title_id` (used for loose pkgs shared in one folder).
static void decrypt_dlc_pkgs_under(EmuEnvState &emuenv, const fs::path &dir, const fs::path &temp_root, const std::string &title_id, bool match_all) {
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::path p = it->path();
        if (!fs::is_regular_file(p, ec) || string_utils::tolower(p.extension().string()) != ".pkg")
            continue;
        if (match_all || dlc_pkg_title_id(p) == title_id)
            decrypt_dlc_pkg(emuenv, p, temp_root);
    }
}

// Bring in all of `title_id`'s DLC as addcont0: content under temp_root/addcont. Best-effort: warns
// and skips anything it can't process. install_pkg overwrites emuenv.app_info, so the caller must
// snapshot/restore the game's around this.
static void mount_dlc_for_game(EmuEnvState &emuenv, const fs::path &temp_root, const std::string &title_id) {
    boost::system::error_code ec;

    // (1) DLC shipped inside the game archive (an addcont/ folder extracted alongside app/).
    collect_addcont_trees(temp_root, temp_root, title_id);

    // (2) The configured DLCs folder.
    const std::string &folder = emuenv.cfg.dlc_folder;
    if (folder.empty())
        return;
    const fs::path dlc_root = fs_utils::utf8_to_path(folder);
    if (!fs::is_directory(dlc_root, ec))
        return;

    const fs::path t_dir = dlc_root / title_id;

    // (2a) A <TITLEID>/ folder holding this game's DLC (pkgs and/or decrypted addcont content).
    if (fs::is_directory(t_dir, ec)) {
        decrypt_dlc_pkgs_under(emuenv, t_dir, temp_root, title_id, /*match_all=*/true);
        collect_addcont_trees(t_dir, temp_root, title_id);
    }

    // (2b) A <TITLEID>.zip / .7z of this game's DLC.
    for (const char *ext : { ".zip", ".7z" }) {
        const fs::path t_arch = dlc_root / (title_id + ext);
        if (!fs::is_regular_file(t_arch, ec))
            continue;
        const fs::path scratch = temp_root / "__dlc";
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch, ec);
        std::string e;
        const bool ok = (std::string(ext) == ".7z") ? extract_7z_to_dir(t_arch, scratch, e) : extract_zip_to_dir(t_arch, scratch, e);
        if (ok) {
            decrypt_dlc_pkgs_under(emuenv, scratch, temp_root, title_id, /*match_all=*/true);
            collect_addcont_trees(scratch, temp_root, title_id);
        } else {
            LOG_WARN("DLC: could not extract {} ({})", fs_utils::path_to_utf8(t_arch.filename()), e);
        }
        fs::remove_all(scratch, ec);
    }

    // (2c) Loose .pkg files anywhere in the DLCs folder, matched to this game by embedded title id.
    const std::string t_dir_prefix = fs_utils::path_to_utf8(t_dir);
    for (fs::recursive_directory_iterator it(dlc_root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::path p = it->path();
        if (!fs::is_regular_file(p, ec) || string_utils::tolower(p.extension().string()) != ".pkg")
            continue;
        if (fs_utils::path_to_utf8(p).rfind(t_dir_prefix, 0) == 0)
            continue; // already handled inside the <TITLEID>/ folder
        if (dlc_pkg_title_id(p) == title_id)
            decrypt_dlc_pkg(emuenv, p, temp_root);
    }
}

std::string mount_pkg_for_play(EmuEnvState &emuenv, const fs::path &input_path, std::string &error_out) {
    boost::system::error_code ec;
    const fs::path temp_root = emuenv.cache_path / "pkgplay";
    fs::remove_all(temp_root, ec); // clear any stale temp (e.g. after a crash)
    fs::create_directories(temp_root, ec);

    const std::string ext = string_utils::tolower(input_path.extension().string());
    const bool is_zip = (ext == ".zip");
    const bool is_7z = (ext == ".7z");
    const bool is_archive = is_zip || is_7z;

    // Dispatch the archive helpers by container so .zip and .7z are handled uniformly.
    const auto arch_has_decrypted = [&](const fs::path &p) {
        return is_7z ? sevenz_has_decrypted_game(p) : zip_has_decrypted_game(p);
    };
    const auto arch_extract_all = [&](const fs::path &p, const fs::path &d, std::string &e) {
        return is_7z ? extract_7z_to_dir(p, d, e) : extract_zip_to_dir(p, d, e);
    };
    const auto arch_extract_pkg = [&](const fs::path &p, const fs::path &o, std::string &e) {
        return is_7z ? extract_pkg_from_7z(p, o, e) : extract_pkg_from_zip(p, o, e);
    };

    std::string title_id;
    std::string content_id;
    std::string category = "gd";

    if (is_archive && arch_has_decrypted(input_path)) {
        // An archive that already holds a decrypted game tree (no decryption needed): unpack it, then
        // move the app tree to temp_root/app regardless of how it was nested (app/, app/<TITLEID>/, …).
        if (!arch_extract_all(input_path, temp_root, error_out)) {
            fs::remove_all(temp_root, ec);
            return {};
        }
        if (!normalize_app_tree(temp_root, error_out)) {
            fs::remove_all(temp_root, ec);
            return {};
        }

        std::vector<uint8_t> mbytes;
        bundle::Manifest m;
        std::string berr;
        if (fs_utils::read_data(temp_root / "vita3k_bundle.json", mbytes) && bundle::parse_manifest(mbytes, m, berr) && !m.title_id.empty()) {
            title_id = m.title_id;
            content_id = m.content_id;
            if (!m.category.empty())
                category = m.category;
        } else {
            std::vector<uint8_t> sfo_buf;
            if (!fs_utils::read_data(temp_root / "app/sce_sys/param.sfo", sfo_buf)) {
                error_out = "zip has an app/ folder but no readable app/sce_sys/param.sfo";
                fs::remove_all(temp_root, ec);
                return {};
            }
            sfo::SfoAppInfo info;
            sfo::get_param_info(info, sfo_buf, emuenv.cfg.sys_lang);
            title_id = info.app_title_id;
            content_id = info.app_content_id;
            if (!info.app_category.empty())
                category = info.app_category;
        }

        // Place the license on the real ux0/license so boot can decrypt the game's own modules.
        // NoNpDrm dumps carry it in app/sce_sys/package/work.bin; also honor any bundled *.rif. Match
        // case-insensitively and recursively: a case-sensitive host (Android/Linux) misses a fixed
        // "work.bin" path when the dump stored a different case, so key off the lowercased filename/
        // extension instead of an exact path (this is why desktop, case-insensitive, found the license
        // but Android did not).
        for (const auto &entry : fs::recursive_directory_iterator(temp_root, ec)) {
            if (ec)
                break;
            if (!fs::is_regular_file(entry.path()))
                continue;
            const std::string fname = string_utils::tolower(entry.path().filename().string());
            const std::string fext = string_utils::tolower(entry.path().extension().string());
            if (fname == "work.bin" || fext == ".rif")
                copy_license(emuenv, entry.path());
        }
    } else {
        // A raw .pkg, or an archive containing one: decrypt into temp_root/app (rif -> ux0/license).
        fs::path pkg_path = input_path;
        fs::path extracted_pkg;
        if (is_archive) {
            extracted_pkg = temp_root / "_src.pkg";
            if (!arch_extract_pkg(input_path, extracted_pkg, error_out)) {
                fs::remove_all(temp_root, ec);
                return {};
            }
            pkg_path = extracted_pkg;
        }

        std::string zrif; // empty -> install_pkg derives it from the pkg's work.bin
        if (!install_pkg(pkg_path, emuenv, zrif, [](float) {}, temp_root)) {
            error_out = "failed to decrypt pkg (expected a self-contained NoNpDrm base-game pkg)";
            fs::remove_all(temp_root, ec);
            return {};
        }
        if (!extracted_pkg.empty())
            fs::remove(extracted_pkg, ec); // source pkg no longer needed once decrypted

        title_id = emuenv.app_info.app_title_id;
        content_id = emuenv.app_info.app_content_id;
        if (!emuenv.app_info.app_category.empty())
            category = emuenv.app_info.app_category;
    }

    if (title_id.empty()) {
        error_out = "could not determine the game's title id";
        fs::remove_all(temp_root, ec);
        return {};
    }

    // Bring in this game's DLC as addcont0: content (no install), from an addcont/ folder inside the
    // game archive and from the configured DLCs folder. install_pkg clobbers emuenv.app_info, so
    // snapshot/restore the game's around it -- boot needs the game's app_info, not a DLC's.
    {
        const auto saved_app_info = emuenv.app_info;
        mount_dlc_for_game(emuenv, temp_root, title_id);
        emuenv.app_info = saved_app_info;
    }

    // Ensure a manifest exists so the directory-backend mount can open temp_root.
    if (!fs::exists(temp_root / "vita3k_bundle.json")) {
        fs::ofstream mf(temp_root / "vita3k_bundle.json", std::ios::out | std::ios::binary);
        mf << "{\"version\":1,\"title_id\":\"" << title_id << "\",\"content_id\":\"" << content_id
           << "\",\"category\":\"" << category << "\",\"has_patch\":false,\"dlc\":[]}";
    }

    bundle::Manifest manifest;
    std::string berr;
    auto backend = bundle::open_directory_backend(temp_root, manifest, berr);
    if (!backend) {
        error_out = "failed to mount game: " + berr;
        fs::remove_all(temp_root, ec);
        return {};
    }

    auto mount = bundle::make_mount(backend, manifest);
    mount->owned_temp = temp_root;
    emuenv.io.mount = mount;

    LOG_INFO("Prepared [{}] for play-without-install from {}", title_id, fs_utils::path_to_utf8(input_path));
    return title_id;
}

bool read_pkg_param_sfo(const fs::path &pkg_path, std::vector<uint8_t> &sfo_out) {
    FILE *infile = FOPEN(pkg_path.c_str(), "rb");
    if (!infile)
        return false;

    PkgHeader pkg_header{};
    if (fread(&pkg_header, sizeof(PkgHeader), 1, infile) != 1 || byte_swap(pkg_header.magic) != 0x7F504b47) {
        fclose(infile);
        return false;
    }

    uint32_t info_offset = byte_swap(pkg_header.info_offset);
    uint32_t sfo_offset = 0;
    uint32_t sfo_size = 0;
    for (uint32_t i = 0; i < byte_swap(pkg_header.info_count); i++) {
        uint32_t block[4];
        fseek(infile, info_offset, SEEK_SET);
        if (fread(block, sizeof(block), 1, infile) != 1)
            break;
        if (byte_swap(block[0]) == 14) { // param.sfo record
            sfo_offset = byte_swap(block[2]);
            sfo_size = byte_swap(block[3]);
        }
        info_offset += 2 * sizeof(uint32_t) + byte_swap(block[1]);
    }

    if (sfo_size == 0) {
        fclose(infile);
        return false;
    }

    sfo_out.resize(sfo_size);
    fseek(infile, sfo_offset, SEEK_SET);
    const bool ok = fread(sfo_out.data(), sfo_size, 1, infile) == 1;
    fclose(infile);
    return ok;
}

std::string find_pkg_zrif(const fs::path &pkg_path, const fs::path &vita_fs_path) {
    FILE *infile = FOPEN(pkg_path.c_str(), "rb");
    if (!infile)
        return {};

    PkgHeader pkg_header{};
    fread(&pkg_header, sizeof(PkgHeader), 1, infile);
    fclose(infile);

    const std::string content_id(pkg_header.content_id);
    if (content_id.size() < 16)
        return {};

    const std::string title_id = content_id.substr(7, 9);
    const auto rif_path = vita_fs_path / "ux0/license" / title_id / (content_id + ".rif");

    if (!fs::exists(rif_path))
        return {};

    LOG_INFO("Found license file: {}", rif_path);
    fs::ifstream binfile(rif_path, std::ios::in | std::ios::binary | std::ios::ate);
    if (!binfile)
        return {};

    return rif2zrif(binfile);
}
