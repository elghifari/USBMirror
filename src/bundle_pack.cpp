// SPDX-License-Identifier: GPL-3.0-only
#include <iostream>
#include "bundle_format.h"
namespace bundle=usbmirror::bundle;
namespace fs=std::filesystem;
int tests() {
    unsigned assertions{},failures{}; const auto test=[&](bool condition) { ++assertions; if(!condition)++failures; };
    test(bundle::safe_name(L"USBMirror.exe")); test(bundle::safe_name(L"licenses\\driver-GPL.txt"));
    for(const auto* name:{L"..\\evil.dll",L"C:\\evil.dll",L"\\\\server\\file",L"licenses/../file",L"file.dll:stream",L"file.",L"CON.txt",L"a//file",L"./file",L"a\\LPT1.txt"})test(!bundle::safe_name(name));
    const bundle::Bytes contents{'h','e','l','l','o'}; bundle::Bytes archive;
    bundle::append_u32(archive,0x31424d55); bundle::append_u32(archive,1);
    const std::string name="README.txt"; bundle::append_u32(archive,static_cast<unsigned>(name.size()));archive.insert(archive.end(),name.begin(),name.end());
    bundle::append_u32(archive,static_cast<unsigned>(contents.size()));const auto digest=bundle::hash(contents.data(),contents.size());archive.insert(archive.end(),digest.begin(),digest.end());archive.insert(archive.end(),contents.begin(),contents.end());
    auto compressed=bundle::pack(archive); auto restored=bundle::unpack(compressed.data(),compressed.size());test(restored.bytes==archive && restored.files.size()==1);
    compressed[8]^=1;bool rejected{};try{bundle::unpack(compressed.data(),compressed.size());}catch(...){rejected=true;}test(rejected);
    auto damaged=archive;damaged.back()^=1;rejected=false;try{bundle::parse(damaged);}catch(...){rejected=true;}test(rejected);
    test(bundle::quote(L"C:\\folder with spaces\\")==L"\"C:\\folder with spaces\\\\\"");
    test(bundle::quote(L"a\"b")==L"\"a\\\"b\"");
    std::cout<<"Bundle format: "<<assertions<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
int wmain(int count,wchar_t** arguments) {
    try {
        if(count==2 && wcscmp(arguments[1],L"--self-test")==0)return tests();
        bundle::require(count==3,"Usage: BundlePack <runtime directory> <output bundle>");const fs::path root=fs::absolute(arguments[1]);
        std::vector<fs::path> files;for(const auto& item:fs::recursive_directory_iterator(root)) {
            bundle::require(!(GetFileAttributesW(item.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT),"Linked files cannot be bundled.");if(item.is_regular_file())files.push_back(item.path());
        }
        std::sort(files.begin(),files.end());bundle::require(!files.empty() && files.size()<=64,"Invalid runtime file count.");
        bundle::Bytes archive;bundle::append_u32(archive,0x31424d55);bundle::append_u32(archive,static_cast<unsigned>(files.size()));
        for(const auto& file:files) {
            const auto relative=fs::relative(file,root).generic_wstring();bundle::require(bundle::safe_name(relative),"A runtime filename is unsafe.");
            std::string name;for(auto c:relative)name+=static_cast<char>(c);const auto data=bundle::read(file);
            bundle::append_u32(archive,static_cast<unsigned>(name.size()));archive.insert(archive.end(),name.begin(),name.end());bundle::append_u32(archive,static_cast<unsigned>(data.size()));
            const auto digest=bundle::hash(data.data(),data.size());archive.insert(archive.end(),digest.begin(),digest.end());archive.insert(archive.end(),data.begin(),data.end());
            bundle::require(archive.size()<=bundle::MaximumBytes,"The runtime package is too large.");
        }
        const auto data=bundle::pack(archive);std::ofstream output(fs::path(arguments[2]),std::ios::binary|std::ios::trunc);
        output.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));bundle::require(output.good(),"The runtime bundle could not be saved.");
        std::cout<<"Bundled "<<files.size()<<" files: "<<archive.size()<<" -> "<<data.size()<<" bytes\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
