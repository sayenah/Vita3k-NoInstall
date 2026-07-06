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

#include <packages/archive_7z.h>

#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>

// LZMA SDK (public domain). Headers carry their own extern "C" guards.
#include <7z.h>
#include <7zAlloc.h>
#include <7zCrc.h>
#include <7zFile.h>
#include <7zTypes.h>

#include <algorithm>
#include <vector>

namespace {

constexpr size_t k_input_buf_size = size_t(1) << 18;

// The 7z decoder needs the CRC table built once before opening any archive.
void ensure_crc_table() {
    static const bool initialized = [] {
        CrcGenerateTable();
        return true;
    }();
    (void)initialized;
}

std::string utf16_to_utf8(const UInt16 *s, size_t len) {
    std::string out;
    for (size_t i = 0; i < len; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len) { // high surrogate
            const uint32_t lo = s[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// RAII wrapper over an opened 7z archive with the LZMA SDK C API.
class SevenZReader {
    ISzAlloc alloc_main{ SzAlloc, SzFree };
    ISzAlloc alloc_temp{ SzAllocTemp, SzFreeTemp };
    CFileInStream archive_stream{};
    CLookToRead2 look_stream{};
    CSzArEx db{};
    bool opened = false;

    // Extraction block cache (reused across files in the same folder).
    UInt32 block_index = 0xFFFFFFFF;
    Byte *out_buffer = nullptr;
    size_t out_buffer_size = 0;

public:
    ~SevenZReader() {
        if (opened) {
            ISzAlloc_Free(&alloc_main, out_buffer);
            SzArEx_Free(&db, &alloc_main);
            ISzAlloc_Free(&alloc_main, look_stream.buf);
            File_Close(&archive_stream.file);
        }
    }

    bool open(const fs::path &path) {
        ensure_crc_table();

#ifdef _WIN32
        const WRes wres = InFile_OpenW(&archive_stream.file, path.c_str());
#else
        const WRes wres = InFile_Open(&archive_stream.file, path.string().c_str());
#endif
        if (wres != 0)
            return false;

        FileInStream_CreateVTable(&archive_stream);
        LookToRead2_CreateVTable(&look_stream, False);
        look_stream.buf = static_cast<Byte *>(ISzAlloc_Alloc(&alloc_main, k_input_buf_size));
        if (!look_stream.buf) {
            File_Close(&archive_stream.file);
            return false;
        }
        look_stream.bufSize = k_input_buf_size;
        look_stream.realStream = &archive_stream.vt;
        LookToRead2_Init(&look_stream);

        SzArEx_Init(&db);
        if (SzArEx_Open(&db, &look_stream.vt, &alloc_main, &alloc_temp) != SZ_OK) {
            SzArEx_Free(&db, &alloc_main);
            ISzAlloc_Free(&alloc_main, look_stream.buf);
            File_Close(&archive_stream.file);
            return false;
        }
        opened = true;
        return true;
    }

    UInt32 num_files() const { return db.NumFiles; }
    bool is_dir(UInt32 i) const { return SzArEx_IsDir(&db, i) != 0; }

    std::string name(UInt32 i) {
        const size_t len16 = SzArEx_GetFileNameUtf16(&db, i, nullptr); // count incl. null terminator
        if (len16 == 0)
            return {};
        std::vector<UInt16> buf(len16);
        SzArEx_GetFileNameUtf16(&db, i, buf.data());
        std::string name = utf16_to_utf8(buf.data(), len16 - 1);
        std::replace(name.begin(), name.end(), '\\', '/');
        return name;
    }

    // Extract file `i`; on success returns a pointer + size into the internal block buffer.
    bool extract(UInt32 i, const Byte **data, size_t *size) {
        size_t offset = 0;
        size_t processed = 0;
        const SRes res = SzArEx_Extract(&db, &look_stream.vt, i, &block_index,
            &out_buffer, &out_buffer_size, &offset, &processed, &alloc_main, &alloc_temp);
        if (res != SZ_OK)
            return false;
        *data = out_buffer + offset;
        *size = processed;
        return true;
    }
};

bool write_file(const fs::path &out_path, const Byte *data, size_t size) {
    boost::system::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);
    fs::ofstream f(out_path, std::ios::out | std::ios::binary);
    if (!f)
        return false;
    if (size)
        f.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

} // namespace

bool sevenz_has_decrypted_game(const fs::path &archive_path) {
    SevenZReader r;
    if (!r.open(archive_path))
        return false;
    for (UInt32 i = 0; i < r.num_files(); i++) {
        if (r.is_dir(i))
            continue;
        const std::string name = string_utils::tolower(r.name(i));
        if (name.size() >= 17 && name.compare(name.size() - 17, 17, "sce_sys/param.sfo") == 0)
            return true;
    }
    return false;
}

bool extract_7z_to_dir(const fs::path &archive_path, const fs::path &dst_dir, std::string &error_out) {
    SevenZReader r;
    if (!r.open(archive_path)) {
        error_out = "cannot open 7z archive";
        return false;
    }
    for (UInt32 i = 0; i < r.num_files(); i++) {
        const std::string name = r.name(i);
        const fs::path out_path = dst_dir / fs_utils::utf8_to_path(name);
        if (r.is_dir(i)) {
            boost::system::error_code ec;
            fs::create_directories(out_path, ec);
            continue;
        }
        const Byte *data = nullptr;
        size_t size = 0;
        if (!r.extract(i, &data, &size) || !write_file(out_path, data, size)) {
            error_out = "failed to extract " + name + " from the 7z";
            return false;
        }
    }
    return true;
}

bool extract_pkg_from_7z(const fs::path &archive_path, const fs::path &out_pkg, std::string &error_out) {
    SevenZReader r;
    if (!r.open(archive_path)) {
        error_out = "cannot open 7z archive";
        return false;
    }
    for (UInt32 i = 0; i < r.num_files(); i++) {
        if (r.is_dir(i))
            continue;
        const std::string lname = string_utils::tolower(r.name(i));
        if (lname.size() >= 4 && lname.compare(lname.size() - 4, 4, ".pkg") == 0) {
            const Byte *data = nullptr;
            size_t size = 0;
            if (!r.extract(i, &data, &size) || !write_file(out_pkg, data, size)) {
                error_out = "failed to extract .pkg from the 7z";
                return false;
            }
            return true;
        }
    }
    error_out = "no .pkg found inside the 7z";
    return false;
}
