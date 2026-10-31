// SPDX-License-Identifier: GPL-3.0-only
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include "bundle_format.h"
#include "resource.h"

namespace bundle=usbmirror::bundle;
namespace fs=std::filesystem;
using bundle::require;
struct LocalMemory { HLOCAL value{}; ~LocalMemory(){if(value)LocalFree(value);} };
struct TokenIdentity {
    std::vector<BYTE> bytes; std::wstring text;
    PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(bytes.data())->User.Sid; }
    TokenIdentity() {
        HANDLE token{};require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token),"Windows could not identify the current user.");bundle::Handle handle(token);
        DWORD size{};GetTokenInformation(token,TokenUser,nullptr,0,&size);require(size>0,"Windows could not read the current user.");bytes.resize(size);
        require(GetTokenInformation(token,TokenUser,bytes.data(),size,&size),"Windows could not read the current user.");
        PWSTR value{};require(ConvertSidToStringSidW(sid(),&value),"Windows could not resolve the current user.");LocalMemory memory{value};text=value;
    }
};
void no_reparse(const fs::path& path) {
    for(auto item=fs::absolute(path);!item.empty();) {
        const auto attributes=GetFileAttributesW(item.c_str());
        require(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"The application cache contains a linked directory or file.");
        auto parent=item.parent_path();if(parent==item)break;item=parent;
    }
}
bundle::Handle directory(const fs::path& path,const TokenIdentity& identity,bool protect) {
    no_reparse(path);const auto sddl=L"O:"+identity.text+L"D:P(A;OICI;FA;;;"+identity.text+L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    PSECURITY_DESCRIPTOR descriptor{};require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr),"Windows could not prepare the cache permissions.");LocalMemory memory{descriptor};
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    if(!CreateDirectoryW(path.c_str(),&security))require(GetLastError()==ERROR_ALREADY_EXISTS,"Windows could not create the application cache.");
    bundle::Handle handle(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|READ_CONTROL|(protect?WRITE_DAC:0),FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    require(handle.value!=INVALID_HANDLE_VALUE,"Windows could not open the application cache.");BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(handle.value,&info) && (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"The application cache is not a regular directory.");
    PSID owner{};PSECURITY_DESCRIPTOR previous{};
    require(GetSecurityInfo(handle.value,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&previous)==ERROR_SUCCESS,"Windows could not check the cache owner.");LocalMemory old{previous};
    BYTE system[SECURITY_MAX_SID_SIZE]{},admin[SECURITY_MAX_SID_SIZE]{};DWORD system_size=sizeof(system),admin_size=sizeof(admin);
    require(CreateWellKnownSid(WinLocalSystemSid,nullptr,system,&system_size) && CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admin,&admin_size),"Windows could not check the cache permissions.");
    require(EqualSid(owner,identity.sid()) || EqualSid(owner,system) || EqualSid(owner,admin),"The application cache belongs to another user.");
    if(protect) {
        BOOL present{},defaulted{};PACL dacl{};require(GetSecurityDescriptorDacl(descriptor,&present,&dacl,&defaulted),"Windows could not read the cache permissions.");
        require(SetSecurityInfo(handle.value,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,dacl,nullptr)==ERROR_SUCCESS,"Windows could not protect the application cache.");
    }
    return handle;
}
bundle::Handle checked_file(const fs::path& path,const bundle::File& expected) {
    no_reparse(path);bundle::Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(file.value==INVALID_HANDLE_VALUE) {
        const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)return bundle::Handle{};
        throw std::runtime_error("Windows could not open a cached application file. Close USB Mirror and try again.");
    }
    BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(file.value,&info) && !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),"A cached application file is linked or invalid.");
    LARGE_INTEGER length{};require(GetFileSizeEx(file.value,&length),"Windows could not check a cached file.");
    if(length.QuadPart!=expected.size)return bundle::Handle{};
    const auto bytes=bundle::read(file.value);if(bundle::hash(bytes.data(),bytes.size())!=expected.digest)return bundle::Handle{};
    return file;
}
void install_file(const fs::path& target,const bundle::File& file) {
    GUID guid{};require(SUCCEEDED(CoCreateGuid(&guid)),"Windows could not create a temporary filename.");wchar_t text[40]{};StringFromGUID2(guid,text,40);
    const auto temporary=target.parent_path()/(L"."+target.filename().wstring()+L"."+text+L".tmp");
    struct Cleanup { fs::path path;bool committed{};~Cleanup(){if(!committed)DeleteFileW(path.c_str());} } cleanup{temporary};
    {
        bundle::Handle output(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));
        require(output.value!=INVALID_HANDLE_VALUE,"Windows could not create a cached application file.");DWORD written{};
        require(WriteFile(output.value,file.data,file.size,&written,nullptr) && written==file.size && FlushFileBuffers(output.value),"Windows could not save a cached application file.");
    }
    if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());throw std::runtime_error("Windows could not update an application file. Close USB Mirror and try again.");
    }
    cleanup.committed=true;
}
struct Runtime { fs::path path;std::vector<bundle::Handle> locks;unsigned written{}; };
Runtime prepare(const bundle::Archive& archive) {
    for(const auto* name:{L"USBMirror.exe",L"USBMirror.Driver.exe",L"iPhoneMirror.Core.dll",L"libusb-1.0.dll",L"libusb0.dll",L"iPhoneMirror.UsbConfigurationSwitch.exe",L"iPhoneMirror.VirtualCamera.dll",L"USBMirror.CameraSetup.exe"})
        require(std::any_of(archive.files.begin(),archive.files.end(),[&](const auto& file){return file.name==name;}),"The embedded package is missing an application component.");
    TokenIdentity identity;const auto digest=bundle::hex(archive.digest);const auto name=L"Global\\USBMirror.Bundle."+identity.text+L"."+digest;
    bundle::Handle mutex(CreateMutexW(nullptr,FALSE,name.c_str()));require(mutex.value && mutex.value!=INVALID_HANDLE_VALUE,"Windows could not open the application cache lock.");
    const auto acquired=WaitForSingleObject(mutex.value,60000);require(acquired==WAIT_OBJECT_0 || acquired==WAIT_ABANDONED,"Another launch is still preparing the application files. Try again shortly.");
    struct Unlock { HANDLE h;~Unlock(){ReleaseMutex(h);} } unlock{mutex.value};
    PWSTR local{};require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)),"Windows could not locate your application-data folder.");
    const auto root=fs::path(local)/L"USBMirror";CoTaskMemFree(local);Runtime runtime;
    runtime.locks.push_back(directory(root,identity,false));runtime.locks.push_back(directory(root/L"Runtime",identity,true));
    runtime.path=root/L"Runtime"/digest;runtime.locks.push_back(directory(runtime.path,identity,true));std::set<fs::path> folders;
    for(const auto& file:archive.files) {
        auto relative=fs::path(file.name).parent_path();std::vector<fs::path> parents;
        while(!relative.empty()){parents.push_back(relative);relative=relative.parent_path();}
        for(auto i=parents.rbegin();i!=parents.rend();++i)if(folders.insert(*i).second)runtime.locks.push_back(directory(runtime.path/ *i,identity,true));
        auto cached=checked_file(runtime.path/file.name,file);
        if(cached.value==INVALID_HANDLE_VALUE){install_file(runtime.path/file.name,file);++runtime.written;cached=checked_file(runtime.path/file.name,file);require(cached.value!=INVALID_HANDLE_VALUE,"A cached file failed its integrity check.");}
        runtime.locks.push_back(std::move(cached));
    }
    return runtime;
}
std::string json_string(const std::string& value) {
    std::string result="\"";for(auto c:value){if(c=='\\' || c=='\"')result+='\\';require(static_cast<unsigned char>(c)>=32,"A report path contains an invalid character.");result+=c;}return result+'\"';
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int count{};auto** arguments=CommandLineToArgvW(GetCommandLineW(),&count);int result=1;
    try {
        require(arguments!=nullptr,"Windows could not read the launch arguments.");
        const auto resource=FindResourceW(instance,MAKEINTRESOURCEW(IDR_APP_BUNDLE),RT_RCDATA);require(resource!=nullptr,"The embedded application package is missing.");
        const auto size=SizeofResource(instance,resource);const auto* data=static_cast<const BYTE*>(LockResource(LoadResource(instance,resource)));
        require(data && size,"The embedded application package is empty.");const auto archive=bundle::unpack(data,size);auto runtime=prepare(archive);
        if(count==3 && wcscmp(arguments[1],L"--bundle-check")==0) {
            std::ofstream report(fs::path(arguments[2]),std::ios::binary|std::ios::trunc);
            report<<"{\"passed\":true,\"files\":"<<archive.files.size()<<",\"filesWritten\":"<<runtime.written<<",\"cacheDirectory\":"<<json_string(runtime.path.u8string())<<",\"applicationStarted\":false,\"driverChangesPerformed\":false}";
            require(report.good(),"Windows could not save the packaging report.");result=0;
        } else {
            const auto executable=runtime.path/L"USBMirror.exe";auto command=bundle::quote(executable.wstring());
            for(int i=1;i<count;++i){command+=L' ';command+=bundle::quote(arguments[i]);}
            STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=static_cast<WORD>(show);PROCESS_INFORMATION child{};
            require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child),"Windows could not start USB Mirror from its application cache.");
            bundle::Handle process(child.hProcess),thread(child.hThread);
            require(WaitForSingleObject(process.value,INFINITE)==WAIT_OBJECT_0,"Windows could not wait for USB Mirror to finish.");
            DWORD code{};require(GetExitCodeProcess(process.value,&code),"Windows could not read the application result.");result=static_cast<int>(code);
        }
    }catch(const std::exception& error){const std::string text=error.what();const std::wstring message(text.begin(),text.end());MessageBoxW(nullptr,message.c_str(),L"USB Mirror",MB_OK|MB_ICONERROR);}
    if(arguments)LocalFree(arguments);CoUninitialize();return result;
}
