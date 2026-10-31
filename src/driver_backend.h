// SPDX-License-Identifier: GPL-3.0-only
// Device selection and payload constants follow the upstream GPL driver manager; see SOURCE.md.
#pragma once
#include <setupapi.h>
#include <cfgmgr32.h>
#include <sddl.h>
#include <aclapi.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <string_view>
#include <utility>
#include "resource.h"

namespace usbmirror::driver {
namespace fs = std::filesystem;
inline void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE item = INVALID_HANDLE_VALUE) : value(item) {}
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = std::exchange(other.value, INVALID_HANDLE_VALUE); }
        return *this;
    }
    Handle(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Key {
    HKEY value{};
    explicit Key(const std::wstring& path, REGSAM access = KEY_READ) {
        const auto result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, access | KEY_WOW64_64KEY, &value);
        require(result == ERROR_SUCCESS, "Cannot open the selected device's registry state.");
    }
    ~Key() { if (value) RegCloseKey(value); }
};
inline bool equal(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
inline std::wstring uppercase(std::wstring value) { for (auto& c : value) c = static_cast<wchar_t>(towupper(c)); return value; }
inline bool allowed_id(const std::wstring& raw) {
    const auto value = uppercase(raw); constexpr std::wstring_view prefix = L"USB\\VID_05AC&PID_";
    if (value.size() < prefix.size() + 5 + 8 || value.size() > prefix.size() + 5 + 39 || value.compare(0, prefix.size(), prefix) != 0) return false;
    unsigned product{};
    for (std::size_t i = prefix.size(); i < prefix.size() + 4; ++i) {
        const auto c = value[i]; if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F'))) return false;
        product = product * 16 + (c <= L'9' ? c - L'0' : c - L'A' + 10);
    }
    if (value[prefix.size() + 4] != L'\\') return false;
    constexpr unsigned products[]{0x1290,0x1291,0x1292,0x1293,0x1294,0x1297,0x1299,0x129A,
        0x129C,0x129D,0x129E,0x129F,0x12A0,0x12A1,0x12A2,0x12A3,0x12A4,0x12A5,0x12A6,
        0x12A8,0x12A9,0x12AA,0x12AB,0x12AC};
    if (std::find(std::begin(products), std::end(products), product) == std::end(products)) return false;
    unsigned letters{};
    for (std::size_t i = prefix.size() + 5; i < value.size(); ++i) {
        const auto c = value[i]; const bool alphanumeric = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z');
        if (!alphanumeric && (c != L'-' || i == prefix.size() + 5)) return false;
        if (alphanumeric) ++letters;
    }
    return letters >= 8;
}
inline std::wstring device_path(const std::wstring& id) { return L"SYSTEM\\CurrentControlSet\\Enum\\" + id; }
struct Value {
    bool exists{}; DWORD type{}; std::vector<BYTE> bytes;
    bool operator==(const Value& other) const { return exists == other.exists && (!exists || (type == other.type && bytes == other.bytes)); }
};
inline Value read_value(const std::wstring& path, const wchar_t* name) {
    Key key(path); Value value; DWORD size{};
    auto result = RegQueryValueExW(key.value, name, nullptr, &value.type, nullptr, &size);
    if (result == ERROR_FILE_NOT_FOUND) return value;
    require(result == ERROR_SUCCESS && size <= 1024 * 1024, "Invalid registry value size.");
    value.bytes.resize(size); result = RegQueryValueExW(key.value, name, nullptr, &value.type, value.bytes.data(), &size);
    require(result == ERROR_SUCCESS, "Cannot read the selected device's driver state.");
    value.bytes.resize(size); value.exists = true; return value;
}
inline std::wstring string_value(const std::wstring& path, const wchar_t* name) {
    const auto value = read_value(path, name);
    if (!value.exists) return {};
    require((value.type == REG_SZ || value.type == REG_EXPAND_SZ) && value.bytes.size() % sizeof(wchar_t) == 0, "Unexpected registry string type.");
    std::wstring result(value.bytes.size() / sizeof(wchar_t), L'\0');
    if (!value.bytes.empty()) memcpy(result.data(), value.bytes.data(), value.bytes.size());
    while (!result.empty() && result.back() == L'\0') result.pop_back(); return result;
}
inline std::vector<std::wstring> filters(const Value& value) {
    if (!value.exists || value.bytes.empty()) return {};
    require(value.type == REG_MULTI_SZ && value.bytes.size() % sizeof(wchar_t) == 0, "Unexpected UpperFilters registry type.");
    std::wstring data(value.bytes.size() / sizeof(wchar_t), L'\0'); memcpy(data.data(), value.bytes.data(), value.bytes.size());
    require(!data.empty() && data.back() == L'\0', "Unterminated UpperFilters registry value.");
    std::vector<std::wstring> result;
    for (std::size_t i = 0; i < data.size() && data[i];) {
        const auto end = data.find(L'\0', i); require(end != std::wstring::npos, "Invalid filter name.");
        result.push_back(data.substr(i, end - i)); i = end + 1;
    }
    return result;
}
inline bool has_filter(const std::vector<std::wstring>& values) { return std::any_of(values.begin(), values.end(), [](const auto& v) { return equal(v, L"libusb0"); }); }
struct Device { std::wstring id, name, service; bool present{}, installed{}, healthy{}; };
inline std::vector<Device> devices() {
    const auto set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES);
    require(set != INVALID_HANDLE_VALUE, "Windows USB discovery failed.");
    struct Cleanup { HDEVINFO set; ~Cleanup() { SetupDiDestroyDeviceInfoList(set); } } cleanup{set};
    std::vector<Device> result;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA info{sizeof(info)};
        if (!SetupDiEnumDeviceInfo(set, index, &info)) { require(GetLastError() == ERROR_NO_MORE_ITEMS, "USB enumeration failed."); break; }
        wchar_t id[512]{}; if (!SetupDiGetDeviceInstanceIdW(set, &info, id, 512, nullptr) || !allowed_id(id)) continue;
        Device device; device.id = id; const auto path = device_path(device.id);
        device.service = string_value(path, L"Service"); device.name = string_value(path, L"FriendlyName");
        if (device.name.empty()) device.name = string_value(path, L"DeviceDesc");
        const auto separator = device.name.find_last_of(L';'); if (separator != std::wstring::npos) device.name.erase(0, separator + 1);
        if (device.name.empty()) device.name = L"Apple mobile device";
        DEVINST node{}; auto mutable_id = device.id;
        device.present = CM_Locate_DevNodeW(&node, mutable_id.data(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
        ULONG state{}, problem{};
        device.healthy = device.present && CM_Get_DevNode_Status(&state, &problem, node, 0) == CR_SUCCESS && !(state & DN_HAS_PROBLEM);
        device.installed = has_filter(filters(read_value(path, L"UpperFilters"))); result.push_back(std::move(device));
    }
    std::stable_sort(result.begin(), result.end(), [](const Device& a, const Device& b) { return a.present > b.present; });
    return result;
}
inline Device selected_device(const std::wstring& id) {
    require(allowed_id(id), "The target is not an allowed iPhone/iPad USB parent.");
    const auto all = devices(); const auto found = std::find_if(all.begin(), all.end(), [&](const auto& item) { return equal(item.id, id); });
    require(found != all.end() && found->present && found->healthy, "Reconnect the selected iPhone and wait for Windows USB discovery.");
    require(equal(found->service, L"usbccgp"), "The phone's parent USB driver is not usbccgp. No changes were made.");
    return *found;
}
inline void no_reparse(const fs::path& path) {
    for (auto part = fs::absolute(path); !part.empty();) {
        const auto attributes = GetFileAttributesW(part.c_str());
        require(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "A driver operation path contains a reparse point.");
        const auto parent = part.parent_path(); if (parent == part) break; part = parent;
    }
}
inline std::wstring hash_bytes(const BYTE* bytes, std::size_t size) {
    HCRYPTPROV provider{}; HCRYPTHASH hash{};
    require(CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE, "Cannot initialize SHA-256.");
    struct Cleanup { HCRYPTPROV p; HCRYPTHASH& h; ~Cleanup() { if (h) CryptDestroyHash(h); CryptReleaseContext(p, 0); } } cleanup{provider, hash};
    require(CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE, "Cannot create SHA-256.");
    require(size <= MAXDWORD && CryptHashData(hash, bytes, static_cast<DWORD>(size), 0), "Cannot hash driver payload.");
    BYTE digest[32]{}; DWORD length = sizeof(digest); require(CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == sizeof(digest), "Cannot finish SHA-256.");
    std::wostringstream value; value << std::hex << std::uppercase << std::setfill(L'0');
    for (auto byte : digest) value << std::setw(2) << unsigned(byte); return value.str();
}
struct Payload { unsigned id; const wchar_t* name; const wchar_t* hash; };
inline const Payload payloads[]{
    {IDR_FILTER_TOOL,L"install-filter.exe",L"DF2ABF387893332F28C4DF68B10A6B176DC9706142055DCCCCF447F5A9CEDE2D"},
    {IDR_FILTER_DRIVER,L"libusb0.sys",L"8058F2AFE6EF96A7D2DED432997FD8655970C9EA75A938EE4557D6A2CB4CC989"},
    {IDR_FILTER_DLL64,L"libusb0.dll",L"4F18B5D2C28AA66B648C8683C6D09B52B92CBBEE85984BBEFAD5F38A64BC2A14"},
    {IDR_FILTER_DLL32,L"libusb0_x86.dll",L"00CACA07869B19D10B370552AC7CC2F6F2EE246FC15DB11650F6CD3F4EF9B666"}
};
inline std::pair<const BYTE*, DWORD> resource(unsigned id) {
    const auto module = GetModuleHandleW(nullptr); const auto entry = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    require(entry != nullptr, "A bundled driver component is missing.");
    const auto size = SizeofResource(module, entry); const auto data = static_cast<const BYTE*>(LockResource(LoadResource(module, entry)));
    require(data && size && size <= 16 * 1024 * 1024, "Invalid embedded driver component."); return {data, size};
}
inline void verify_resources() { for (const auto& item : payloads) { const auto data = resource(item.id); require(hash_bytes(data.first, data.second) == item.hash, "Bundled driver payload hash mismatch."); } }
inline Handle locked_file(const fs::path& path) {
    no_reparse(path); Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    require(file.value != INVALID_HANDLE_VALUE, "Cannot lock a driver component for verification."); return file;
}
inline void verify_file(HANDLE file, const wchar_t* expected) {
    LARGE_INTEGER size{}; require(GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 16 * 1024 * 1024, "Invalid driver component size.");
    std::vector<BYTE> bytes(static_cast<std::size_t>(size.QuadPart)); DWORD read{}; LARGE_INTEGER begin{};
    require(SetFilePointerEx(file, begin, nullptr, FILE_BEGIN) && ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size(), "Cannot read a driver component.");
    require(hash_bytes(bytes.data(), bytes.size()) == expected, "A different libusb0 version is installed. It was left unchanged.");
}
inline void write_new(const fs::path& path, const BYTE* bytes, DWORD size) {
    no_reparse(path); Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(file.value != INVALID_HANDLE_VALUE, "Cannot create an operation file."); DWORD written{};
    require(WriteFile(file.value, bytes, size, &written, nullptr) && written == size && FlushFileBuffers(file.value), "Cannot save an operation file.");
}
inline void write_text(const fs::path& path, const std::wstring& text) {
    const auto value = std::wstring(1, L'\xFEFF') + text;
    write_new(path, reinterpret_cast<const BYTE*>(value.data()), static_cast<DWORD>(value.size() * sizeof(wchar_t)));
}
inline void protected_directory(const fs::path& path) {
    no_reparse(path); PSECURITY_DESCRIPTOR descriptor{};
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr), "Cannot create driver-operation permissions.");
    struct Cleanup { PSECURITY_DESCRIPTOR d; ~Cleanup() { LocalFree(d); } } cleanup{descriptor};
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    if (!CreateDirectoryW(path.c_str(), &attributes)) {
        require(GetLastError() == ERROR_ALREADY_EXISTS, "Cannot create the driver-operation folder."); no_reparse(path);
        PSID owner{}; PSECURITY_DESCRIPTOR existing{};
        require(GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &existing) == ERROR_SUCCESS, "Cannot verify the operation-folder owner.");
        BYTE admin[SECURITY_MAX_SID_SIZE]{}, system[SECURITY_MAX_SID_SIZE]{}; DWORD a = sizeof(admin), s = sizeof(system);
        const bool allowed = CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin, &a) && CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &s) && (EqualSid(owner, admin) || EqualSid(owner, system));
        LocalFree(existing); require(allowed, "The operation folder has an unexpected owner.");
        BOOL present{}, defaulted{}; PACL dacl{}; GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
        require(SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS, "Cannot protect the operation folder.");
    }
    no_reparse(path);
}
inline bool administrator() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY; PSID group{}; BOOL member{};
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &member); FreeSid(group);
    } return member != FALSE;
}
inline fs::path windows_directory() { wchar_t path[MAX_PATH]{}; require(GetWindowsDirectoryW(path, MAX_PATH) != 0, "Cannot locate Windows."); return path; }
inline bool service_valid() {
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); require(manager != nullptr, "Cannot query Windows driver services.");
    const auto service = OpenServiceW(manager, L"libusb0", SERVICE_QUERY_CONFIG);
    if (!service) { const auto missing = GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST; CloseServiceHandle(manager); require(missing, "Cannot inspect the capture-driver service."); return false; }
    DWORD bytes{}; QueryServiceConfigW(service, nullptr, 0, &bytes); std::vector<BYTE> buffer(bytes);
    const bool queried = bytes && QueryServiceConfigW(service, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data()), bytes, &bytes);
    CloseServiceHandle(service); CloseServiceHandle(manager); require(queried, "Cannot read capture-driver service configuration.");
    const auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    require(config->dwServiceType == SERVICE_KERNEL_DRIVER, "The existing libusb0 service is not a kernel driver.");
    std::wstring value = config->lpBinaryPathName; if (value.size() >= 2 && value.front() == L'\"' && value.back() == L'\"') value = value.substr(1, value.size() - 2);
    wchar_t expanded[32768]{}; require(ExpandEnvironmentStringsW(value.c_str(), expanded, 32768) > 0, "Cannot resolve the driver service path."); value = expanded;
    if (value.rfind(L"\\??\\", 0) == 0) value.erase(0, 4);
    if (uppercase(value).rfind(L"\\SYSTEMROOT\\", 0) == 0) value = (windows_directory() / value.substr(12)).wstring();
    if (uppercase(value).rfind(L"SYSTEM32\\", 0) == 0) value = (windows_directory() / value).wstring();
    require(equal(fs::absolute(value).lexically_normal().make_preferred().wstring(), (windows_directory() / L"System32/drivers/libusb0.sys").make_preferred().wstring()), "The libusb0 service points to an unexpected driver. It was left unchanged.");
    return true;
}
struct Snapshot { std::wstring path, name; Value before; };
inline std::vector<Snapshot> snapshot(const Device& device) {
    std::vector<Snapshot> values{{device_path(device.id), L"UpperFilters", read_value(device_path(device.id), L"UpperFilters")}};
    DEVINST parent{}; auto id = device.id; require(CM_Locate_DevNodeW(&parent, id.data(), 0) == CR_SUCCESS, "The selected phone disconnected.");
    DEVINST child{}; auto status = CM_Get_Child(&child, parent, 0);
    for (unsigned count = 0; status == CR_SUCCESS; ++count) {
        require(count < 128, "Too many USB child interfaces."); wchar_t child_id[512]{};
        require(CM_Get_Device_IDW(child, child_id, 512, 0) == CR_SUCCESS, "Cannot snapshot a USB child interface.");
        const auto path = device_path(child_id); values.push_back({path, L"UpperFilters", read_value(path, L"UpperFilters")});
        status = CM_Get_Sibling(&child, child, 0);
    }
    const std::wstring usb_class = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{36fc9e60-c465-11cf-8056-444553540000}";
    for (const auto name : {L"UpperFilters", L"LowerFilters"}) values.push_back({usb_class, name, read_value(usb_class, name)});
    return values;
}
inline std::wstring registry_backup(const std::vector<Snapshot>& values) {
    std::wostringstream data; data << L"Windows Registry Editor Version 5.00\r\n";
    for (const auto& value : values) {
        data << L"\r\n[HKEY_LOCAL_MACHINE\\" << value.path << L"]\r\n\"" << value.name << L"\"=";
        if (!value.before.exists) data << L"-\r\n";
        else {
            data << L"hex(" << std::hex << value.before.type << L"):";
            for (std::size_t i = 0; i < value.before.bytes.size(); ++i) { if (i) data << L","; if (i && i % 24 == 0) data << L"\\\r\n  "; data << std::setfill(L'0') << std::setw(2) << unsigned(value.before.bytes[i]); }
            data << L"\r\n";
        }
    } return data.str();
}
inline bool restore(const std::vector<Snapshot>& values) {
    bool complete = true;
    for (const auto& item : values) try {
        if (read_value(item.path, item.name.c_str()) == item.before) continue;
        Key key(item.path, KEY_QUERY_VALUE | KEY_SET_VALUE); LSTATUS result{};
        if (!item.before.exists) { result = RegDeleteValueW(key.value, item.name.c_str()); if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS; }
        else result = RegSetValueExW(key.value, item.name.c_str(), 0, item.before.type, item.before.bytes.data(), static_cast<DWORD>(item.before.bytes.size()));
        if (result != ERROR_SUCCESS) complete = false;
    } catch (...) { complete = false; }
    return complete;
}
inline bool filter_change_allowed(const std::vector<std::wstring>& before, const std::vector<std::wstring>& after, bool install) {
    const auto count = std::count_if(after.begin(), after.end(), [](const auto& v) { return equal(v, L"libusb0"); });
    if (count != (install ? 1 : 0)) return false;
    const auto without_capture = [](std::vector<std::wstring> items) { items.erase(std::remove_if(items.begin(), items.end(), [](const auto& v) { return equal(v, L"libusb0"); }), items.end()); for (auto& v : items) v = uppercase(v); return items; };
    return without_capture(before) == without_capture(after);
}
inline void verify_filter_change(const std::vector<Snapshot>& values, bool install) {
    const auto after = filters(read_value(values.front().path, L"UpperFilters"));
    require(filter_change_allowed(filters(values.front().before), after, install), "The filter state or an unrelated filter changed unexpectedly. Restoring the snapshot.");
    for (std::size_t i = 1; i < values.size(); ++i) require(read_value(values[i].path, values[i].name.c_str()) == values[i].before, "A child or class filter changed. Restoring the snapshot.");
}
inline void trusted_driver(const fs::path& path) {
    WINTRUST_FILE_INFO file{sizeof(file)}; file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{sizeof(trust)}; trust.dwUIChoice = WTD_UI_NONE; trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE; trust.pFile = &file; trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2; const auto status = WinVerifyTrust(nullptr, &action, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &action, &trust);
    require(status == ERROR_SUCCESS, "Windows does not trust the bundled kernel-driver signature. No driver changes were made.");
}
inline void run_filter(const fs::path& root, const std::wstring& id, bool install) {
    const auto tool = root / L"install-filter.exe";
    auto command = L"\"" + tool.wstring() + L"\" " + (install ? L"i" : L"u") + L" \"-di=" + id + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    require(CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, root.c_str(), &startup, &process), "The bundled exact-device filter tool could not start.");
    Handle child(process.hProcess), thread(process.hThread);
    // A driver operation must finish before rollback or another operation can start.
    require(WaitForSingleObject(child.value, INFINITE) == WAIT_OBJECT_0, "Cannot wait for the driver operation.");
    DWORD result{}; require(GetExitCodeProcess(child.value, &result) && result == 0, "The exact-device filter command failed. Review the operation backup.");
}
inline int apply(const std::wstring& id, bool install) {
    fs::path operation; std::vector<Snapshot> before; std::vector<fs::path> created;
    bool had_service = true, filter_started = false;
    Handle mutex; bool lock_taken{};
    struct Unlock { Handle& h; bool& taken; ~Unlock() { if (taken) ReleaseMutex(h.value); } } unlock{mutex, lock_taken};
    try {
        require(administrator(), "Driver changes require Windows administrator approval.");
        mutex = Handle(CreateMutexW(nullptr, FALSE, L"Global\\iPhoneMirror.Driver.Operation"));
        require(mutex.value && mutex.value != INVALID_HANDLE_VALUE, "Cannot open the driver-operation lock.");
        const auto acquired = WaitForSingleObject(mutex.value, 0);
        require(acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED, "Another iPhone driver operation is running.");
        lock_taken = true;
        const auto device = selected_device(id); verify_resources(); had_service = service_valid();
        PWSTR program_data{}; require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &program_data)), "Cannot locate ProgramData.");
        const auto root = fs::path(program_data) / L"USBMirror.Driver"; CoTaskMemFree(program_data); protected_directory(root);
        GUID guid{}; require(SUCCEEDED(CoCreateGuid(&guid)), "Cannot create an operation identifier."); wchar_t guid_text[40]{}; StringFromGUID2(guid, guid_text, 40);
        operation = root / guid_text; protected_directory(operation); const auto payload_root = operation / L"payload"; protected_directory(payload_root);
        std::vector<Handle> payload_locks;
        for (const auto& item : payloads) { const auto data = resource(item.id); const auto file = payload_root / item.name; write_new(file, data.first, data.second); auto locked = locked_file(file); verify_file(locked.value, item.hash); payload_locks.push_back(std::move(locked)); }
        trusted_driver(payload_root / L"libusb0.sys");
        before = snapshot(device); write_text(operation / L"UpperFilters-before.reg", registry_backup(before));
        std::vector<Handle> existing_locks;
        const auto windows = windows_directory();
        const fs::path destinations[]{windows / L"System32/drivers/libusb0.sys", windows / L"System32/libusb0.dll", windows / L"SysWOW64/libusb0.dll"};
        // Check every existing shared component before deploying any missing file.
        for (unsigned i = 0; i < 3; ++i) if (fs::exists(destinations[i])) {
            auto file = locked_file(destinations[i]); verify_file(file.value, payloads[i + 1].hash); existing_locks.push_back(std::move(file));
        }
        if (install) for (unsigned i = 0; i < 3; ++i) if (!fs::exists(destinations[i])) {
            no_reparse(destinations[i]); require(CopyFileW((payload_root / payloads[i + 1].name).c_str(), destinations[i].c_str(), TRUE), "Cannot deploy a missing driver component."); created.push_back(destinations[i]);
            auto file = locked_file(destinations[i]); verify_file(file.value, payloads[i + 1].hash); existing_locks.push_back(std::move(file));
        }
        selected_device(id); filter_started = true; run_filter(payload_root, id, install);
        verify_filter_change(before, install);
        if (install) require(service_valid(), "The capture-driver service was not installed.");
        write_text(operation / L"result.txt", L"SUCCESS\r\nReconnect the selected iPhone.\r\n");
        MessageBoxW(nullptr, L"The selected iPhone's capture filter was updated. Unplug and reconnect the phone.", L"USB Mirror driver setup", MB_OK | MB_ICONINFORMATION);
        return 0;
    } catch (const std::exception& error) {
        bool complete = restore(before);
        if (!had_service && filter_started) {
            const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (manager) {
                const auto service = OpenServiceW(manager, L"libusb0", SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
                if (service) { SERVICE_STATUS status{}; ControlService(service, SERVICE_CONTROL_STOP, &status); if (!DeleteService(service) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) complete = false; CloseServiceHandle(service); }
                else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) complete = false;
                CloseServiceHandle(manager);
            } else complete = false;
        }
        for (const auto& file : created) if (!DeleteFileW(file.c_str())) complete = false;
        const std::string reason = error.what(); std::wstring message(reason.begin(), reason.end());
        if (filter_started || !created.empty()) message += complete ? L"\n\nCaptured state was restored. Reconnect the phone." : L"\n\nRollback was incomplete. Reconnect the phone and review the saved registry backup.";
        if (!operation.empty()) {
            message += L"\n\nOperation folder: " + operation.wstring();
            try { write_text(operation / L"result.txt", L"FAILED\r\n" + message); } catch (...) {}
        }
        MessageBoxW(nullptr, message.c_str(), L"USB Mirror driver setup", MB_OK | MB_ICONERROR); return 1;
    }
}
}
