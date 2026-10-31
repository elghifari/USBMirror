// SPDX-License-Identifier: GPL-3.0-only
// Read-only diagnostics: report routing/mute/peak for one process. No recording.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
std::string utf8(const std::wstring& value) {
    const auto size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string output(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
    std::string escaped;
    for (auto item : output) { if (item == '\\' || item == '"') escaped.push_back('\\'); escaped.push_back(item); }
    return escaped;
}
std::wstring device_id(IMMDevice* device) {
    LPWSTR id{}; if (FAILED(device->GetId(&id))) return {};
    std::wstring result(id); CoTaskMemFree(id); return result;
}
int wmain(int count, wchar_t** arguments) {
    const DWORD target = count > 1 ? wcstoul(arguments[1], nullptr, 10) : 0;
    if (!target) return 2;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> enumerator;
    const auto created = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(created)) return 3;
    ComPtr<IMMDevice> multimedia, console;
    enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &multimedia);
    enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &console);
    const auto multimedia_id = multimedia ? device_id(multimedia.Get()) : L"";
    const auto console_id = console ? device_id(console.Get()) : L"";
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return 4;
    UINT device_count{}; devices->GetCount(&device_count);
    struct Session {
        std::wstring name;
        bool multimedia{}, console{}, endpoint_muted{}, session_muted{};
        float endpoint_volume{}, session_volume{}, peak{};
        bool has_peak{};
        int state{};
        ComPtr<IAudioMeterInformation> meter;
    };
    std::vector<Session> found;
    std::printf("{\"outputs\":[");
    bool first = true;
    for (UINT i = 0; i < device_count; ++i) {
        ComPtr<IMMDevice> device; if (FAILED(devices->Item(i, &device))) continue;
        ComPtr<IPropertyStore> properties; std::wstring name = L"Audio output";
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT value; PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) name = value.pwszVal;
            PropVariantClear(&value);
        }
        const auto id = device_id(device.Get());
        float endpoint_volume{}; BOOL endpoint_muted{};
        ComPtr<IAudioEndpointVolume> volume;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(volume.GetAddressOf())))) {
            volume->GetMasterVolumeLevelScalar(&endpoint_volume); volume->GetMute(&endpoint_muted);
        }
        if (!first) std::printf(","); first = false;
        std::printf("{\"name\":\"%s\",\"defaultMultimedia\":%s,\"defaultConsole\":%s,\"volume\":%.3f,\"muted\":%s}",
            utf8(name).c_str(), id == multimedia_id ? "true" : "false", id == console_id ? "true" : "false", endpoint_volume, endpoint_muted ? "true" : "false");
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.GetAddressOf())))) continue;
        ComPtr<IAudioSessionEnumerator> sessions; if (FAILED(manager->GetSessionEnumerator(&sessions))) continue;
        int session_count{}; sessions->GetCount(&session_count);
        for (int j = 0; j < session_count; ++j) {
            ComPtr<IAudioSessionControl> control; if (FAILED(sessions->GetSession(j, &control))) continue;
            ComPtr<IAudioSessionControl2> control2; if (FAILED(control.As(&control2))) continue;
            DWORD pid{}; if (FAILED(control2->GetProcessId(&pid)) || pid != target) continue;
            Session item; item.name = name; item.multimedia = id == multimedia_id; item.console = id == console_id;
            item.endpoint_volume = endpoint_volume; item.endpoint_muted = endpoint_muted != FALSE;
            ComPtr<ISimpleAudioVolume> session_volume;
            if (SUCCEEDED(control.As(&session_volume))) {
                BOOL muted{}; session_volume->GetMasterVolume(&item.session_volume); session_volume->GetMute(&muted); item.session_muted = muted != FALSE;
            }
            AudioSessionState state{}; control->GetState(&state); item.state = state;
            item.has_peak = SUCCEEDED(control.As(&item.meter)); found.push_back(std::move(item));
        }
    }
    for (unsigned sample = 0; sample < 100; ++sample) {
        for (auto& item : found) if (item.meter) { float peak{}; if (SUCCEEDED(item.meter->GetPeakValue(&peak))) item.peak = std::max(item.peak, peak); }
        Sleep(100);
    }
    std::printf("],\"targetProcess\":%lu,\"sessions\":[", target); first = true;
    for (const auto& item : found) {
        if (!first) std::printf(","); first = false;
        std::printf("{\"output\":\"%s\",\"state\":%d,\"sessionVolume\":%.3f,\"sessionMuted\":%s,\"endpointVolume\":%.3f,\"endpointMuted\":%s,\"meterAvailable\":%s,\"maxPeak\":%.6f}",
            utf8(item.name).c_str(), item.state, item.session_volume, item.session_muted ? "true" : "false",
            item.endpoint_volume, item.endpoint_muted ? "true" : "false", item.has_peak ? "true" : "false", item.peak);
    }
    std::printf("]}\n");
    return 0;
}
