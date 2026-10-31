// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <compressapi.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace usbmirror::bundle {
using Bytes = std::vector<BYTE>;
using Digest = std::array<BYTE,32>;
constexpr std::size_t MaximumBytes = 64 * 1024 * 1024;
inline void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    Handle(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value,INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if(this!=&other){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=std::exchange(other.value,INVALID_HANDLE_VALUE);}return *this;
    }
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
inline Digest hash(const BYTE* data, std::size_t size) {
    HCRYPTPROV provider{}; HCRYPTHASH context{};
    require(CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT),"Windows could not initialize the file check.");
    struct Cleanup { HCRYPTPROV p; HCRYPTHASH& h; ~Cleanup() { if (h) CryptDestroyHash(h); CryptReleaseContext(p,0); } } cleanup{provider,context};
    require(CryptCreateHash(provider,CALG_SHA_256,0,0,&context),"Windows could not initialize SHA-256.");
    require(size <= MAXDWORD && CryptHashData(context,data,static_cast<DWORD>(size),0),"Windows could not check the bundled data.");
    Digest result{}; DWORD length = static_cast<DWORD>(result.size());
    require(CryptGetHashParam(context,HP_HASHVAL,result.data(),&length,0) && length == result.size(),"Windows could not finish the file check.");
    return result;
}
inline std::wstring hex(const Digest& value) {
    constexpr wchar_t digits[] = L"0123456789abcdef"; std::wstring result;
    for (auto byte : value) { result += digits[byte >> 4]; result += digits[byte & 15]; } return result;
}
inline std::wstring uppercase(std::wstring value) { for (auto& c : value) if (c >= L'a' && c <= L'z') c -= L'a'-L'A'; return value; }
inline bool safe_name(const std::wstring& name) {
    if (name.empty() || name.size() > 180 || name.front() == L'/' || name.front() == L'\\') return false;
    std::wstring segment;
    const auto valid_segment = [](const std::wstring& item) {
        if (item.empty() || item == L"." || item == L".." || item.back() == L'.') return false;
        const auto stem = uppercase(item.substr(0,item.find(L'.')));
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") return false;
        if (stem.size() == 4 && (stem.substr(0,3) == L"COM" || stem.substr(0,3) == L"LPT") && stem[3] >= L'1' && stem[3] <= L'9') return false;
        return true;
    };
    for (auto c : name) {
        if (c == L'/' || c == L'\\') { if (!valid_segment(segment)) return false; segment.clear(); continue; }
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_')) return false;
        segment += c;
    } return valid_segment(segment);
}
inline std::wstring normalized(std::wstring value) { std::replace(value.begin(),value.end(),L'/',L'\\'); return value; }
inline void append_u32(Bytes& bytes, std::uint32_t value) { for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<BYTE>(value>>(i*8))); }
struct Reader {
    const BYTE* data; std::size_t size, offset{};
    const BYTE* take(std::size_t count) { require(count <= size-offset,"The bundled files are incomplete."); auto* result=data+offset; offset+=count; return result; }
    std::uint32_t u32() { const auto* p=take(4); return std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8) | (std::uint32_t(p[2])<<16) | (std::uint32_t(p[3])<<24); }
};
struct File { std::wstring name; Digest digest; const BYTE* data; std::uint32_t size; };
inline std::vector<File> parse(const Bytes& bytes) {
    Reader reader{bytes.data(),bytes.size()}; require(reader.u32() == 0x31424d55,"This file is not a USB Mirror bundle.");
    const auto count=reader.u32(); require(count && count<=64,"The bundle has an invalid number of files.");
    std::set<std::wstring> names; std::vector<File> files;
    for (unsigned i=0;i<count;++i) {
        const auto length=reader.u32(); require(length && length<=180,"A bundled filename is invalid.");
        const auto* name=reader.take(length); std::wstring relative(name,name+length);
        require(safe_name(relative),"A bundled file has an unsafe path."); relative=normalized(relative);
        require(names.insert(uppercase(relative)).second,"The bundle contains duplicate filenames.");
        const auto size=reader.u32(); require(size<=MaximumBytes,"A bundled file is too large.");
        Digest expected{}; memcpy(expected.data(),reader.take(expected.size()),expected.size()); const auto* contents=reader.take(size);
        require(hash(contents,size)==expected,"A bundled file failed its integrity check."); files.push_back({relative,expected,contents,size});
    }
    require(reader.offset==reader.size,"The bundle contains unexpected data."); return files;
}
inline Bytes compress(const Bytes& bytes) {
    COMPRESSOR_HANDLE compressor{}; require(CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&compressor),"Windows compression is unavailable.");
    struct Cleanup { COMPRESSOR_HANDLE h; ~Cleanup() { CloseCompressor(h); } } cleanup{compressor};
    SIZE_T needed{}; Compress(compressor,bytes.data(),bytes.size(),nullptr,0,&needed);
    require(GetLastError()==ERROR_INSUFFICIENT_BUFFER && needed && needed<=MaximumBytes,"Windows could not size the compressed bundle.");
    Bytes result(needed); require(Compress(compressor,bytes.data(),bytes.size(),result.data(),result.size(),&needed),"Windows could not compress the bundle."); result.resize(needed); return result;
}
inline Bytes pack(const Bytes& archive) {
    require(!archive.empty() && archive.size()<=MaximumBytes,"The runtime package is too large.");
    const auto compressed=compress(archive); Bytes result;
    append_u32(result,0x31584d55); append_u32(result,static_cast<std::uint32_t>(archive.size()));
    const auto digest=hash(archive.data(),archive.size()); result.insert(result.end(),digest.begin(),digest.end());
    append_u32(result,static_cast<std::uint32_t>(compressed.size())); result.insert(result.end(),compressed.begin(),compressed.end()); return result;
}
struct Archive {
    Bytes bytes; std::vector<File> files; Digest digest{};
    Archive()=default; Archive(const Archive&)=delete; Archive(Archive&&)=default;
};
inline Archive unpack(const BYTE* data,std::size_t size) {
    Reader reader{data,size}; require(reader.u32()==0x31584d55,"The embedded package is invalid."); const auto length=reader.u32();
    require(length && length<=MaximumBytes,"The embedded package has an invalid size.");
    Digest expected{}; memcpy(expected.data(),reader.take(expected.size()),expected.size()); const auto compressed_size=reader.u32();
    require(compressed_size && compressed_size<=MaximumBytes,"The compressed package is invalid."); const auto* compressed=reader.take(compressed_size);
    require(reader.offset==reader.size,"The embedded package contains unexpected data.");
    DECOMPRESSOR_HANDLE decompressor{}; require(CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&decompressor),"Windows decompression is unavailable.");
    struct Cleanup { DECOMPRESSOR_HANDLE h; ~Cleanup() { CloseDecompressor(h); } } cleanup{decompressor};
    Archive archive; archive.bytes.resize(length); SIZE_T written{};
    require(Decompress(decompressor,compressed,compressed_size,archive.bytes.data(),archive.bytes.size(),&written) && written==length,"Windows could not unpack the application files.");
    archive.digest=hash(archive.bytes.data(),archive.bytes.size()); require(archive.digest==expected,"The embedded package failed its integrity check.");
    archive.files=parse(archive.bytes); return archive;
}
inline Bytes read(HANDLE file) {
    LARGE_INTEGER length{}; require(GetFileSizeEx(file,&length) && length.QuadPart>=0 && length.QuadPart<=MaximumBytes,"An application file has an invalid size.");
    Bytes bytes(static_cast<std::size_t>(length.QuadPart)); DWORD count{};
    require(ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr) && count==bytes.size(),"Windows could not read an application file."); return bytes;
}
inline Bytes read(const std::filesystem::path& path) {
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    require(file.value!=INVALID_HANDLE_VALUE,"A file required to build the bundle is missing."); return read(file.value);
}
inline std::wstring quote(const std::wstring& argument) {
    std::wstring value=L"\""; unsigned slashes{};
    for (auto c:argument) {
        if (c==L'\\') { ++slashes; continue; }
        if (c==L'\"') { value.append(slashes*2+1,L'\\'); value+=c; }
        else { value.append(slashes,L'\\'); value+=c; }
        slashes=0;
    }
    value.append(slashes*2,L'\\'); value+=L'\"'; return value;
}
}
