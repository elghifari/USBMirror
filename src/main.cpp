// SPDX-License-Identifier: GPL-3.0-only
// USB Mirror: a native Windows USB mirroring frontend. See SOURCE.md for component sources.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "CoreApi.h"
#include "VirtualCameraApi.h"
#include "logic.h"
#include "ui_theme.h"
#include "resource.h"
#include "hardware_lease.h"

namespace fs = std::filesystem;
using namespace iPhoneMirror;
using Clock = std::chrono::steady_clock;
constexpr UINT UpdateMessage = WM_APP + 1;
constexpr UINT ExitMessage = WM_APP + 2;
constexpr UINT SavedDeviceMessage = WM_APP + 3;
constexpr wchar_t MainClass[] = L"USBMirror.Control.v1";
constexpr wchar_t PreviewClass[] = L"USBMirror.Preview.v1";
constexpr wchar_t PreviewTitle[] = L"USB Mirror Preview";

fs::path executable_directory() {
    std::vector<wchar_t> value(32768);
    const auto length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size()) throw std::runtime_error("Cannot locate the application directory.");
    return fs::path(std::wstring(value.data(), length)).parent_path();
}
std::wstring system_error(DWORD code) {
    wchar_t* value{};
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&value), 0, nullptr);
    std::wstring result = length ? std::wstring(value, length) : L"Windows error " + std::to_wstring(code);
    if (value) LocalFree(value);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}
bool original_app_running() {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W item{sizeof(item)};
    bool found{};
    if (Process32FirstW(snapshot, &item)) do {
        if (_wcsicmp(item.szExeFile, L"iPhoneMirror.exe") == 0) { found = true; break; }
    } while (Process32NextW(snapshot, &item));
    CloseHandle(snapshot);
    return found;
}
bool write_window_bitmap(HWND window, const fs::path& file) {
    RECT rectangle{}; GetWindowRect(window, &rectangle);
    const auto width = rectangle.right - rectangle.left;
    const auto height = rectangle.bottom - rectangle.top;
    auto screen = GetDC(window); auto memory = CreateCompatibleDC(screen);
    auto bitmap = CreateCompatibleBitmap(screen, width, height);
    auto previous = SelectObject(memory, bitmap);
    const auto printed = PrintWindow(window, memory, 2);
    SelectObject(memory, previous);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    const auto copied = GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    BITMAPFILEHEADER header{}; header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    const bool ok = printed && copied == height && output.good();
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, screen);
    return ok;
}
template<class Function> Function load_function(HMODULE module, const char* name) {
    auto function = GetProcAddress(module, name);
    if (!function) throw std::runtime_error(std::string("Missing capture API: ") + name);
    return reinterpret_cast<Function>(function);
}
struct Core {
    HMODULE module{};
    bool initialized{};
#define CORE_API(name) decltype(&im_##name) name{}
    CORE_API(initialize); CORE_API(shutdown); CORE_API(api_version); CORE_API(last_error);
    CORE_API(refresh_devices_ex); CORE_API(get_environment); CORE_API(is_libusb0_device_available);
    CORE_API(session_create); CORE_API(session_stop); CORE_API(session_destroy);
    CORE_API(session_get_status); CORE_API(session_get_video_output_status);
    CORE_API(session_attach_preview); CORE_API(session_set_corner_profile);
    CORE_API(session_set_window_rotation);
    CORE_API(session_set_video_preferences); CORE_API(session_set_pipeline_preferences);
    CORE_API(session_set_audio_enabled); CORE_API(session_set_audio_volume);
    CORE_API(session_copy_latest_video_frame); CORE_API(session_get_latest_video_timestamp);
    CORE_API(session_copy_next_audio_packet);
#undef CORE_API
    void load(const fs::path& directory) {
        module = LoadLibraryExW((directory / L"iPhoneMirror.Core.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) throw std::runtime_error("Cannot load capture files. Extract the complete USB Mirror folder before launching it.");
#define LOAD_CORE(name) name = load_function<decltype(name)>(module, "im_" #name)
        LOAD_CORE(initialize); LOAD_CORE(shutdown); LOAD_CORE(api_version); LOAD_CORE(last_error);
        LOAD_CORE(refresh_devices_ex); LOAD_CORE(get_environment); LOAD_CORE(is_libusb0_device_available);
        LOAD_CORE(session_create); LOAD_CORE(session_stop); LOAD_CORE(session_destroy);
        LOAD_CORE(session_get_status); LOAD_CORE(session_get_video_output_status);
        LOAD_CORE(session_attach_preview); LOAD_CORE(session_set_corner_profile);
        LOAD_CORE(session_set_window_rotation);
        LOAD_CORE(session_set_video_preferences); LOAD_CORE(session_set_pipeline_preferences);
        LOAD_CORE(session_set_audio_enabled); LOAD_CORE(session_set_audio_volume);
        LOAD_CORE(session_copy_latest_video_frame); LOAD_CORE(session_get_latest_video_timestamp);
        LOAD_CORE(session_copy_next_audio_packet);
#undef LOAD_CORE
        if (api_version() != ApiVersion) throw std::runtime_error("Capture API version mismatch. Rebuild using the matching v1.8.3 runtime.");
        if (initialize() != 0) throw std::runtime_error("The capture runtime could not initialize.");
        initialized = true;
    }
    ~Core() {
        if (initialized) shutdown();
        if (module) FreeLibrary(module);
    }
};
struct Camera {
    HMODULE module{};
    bool running{};
    decltype(&im_vcam_get_status) get_status{};
    decltype(&im_vcam_start_ex) start{};
    decltype(&im_vcam_publish_bgra) publish{};
    decltype(&im_vcam_stop) stop{};
    bool load(const fs::path& directory) {
        if (module) return true;
        module = LoadLibraryExW((directory / L"iPhoneMirror.VirtualCamera.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) return false;
        try {
            get_status = load_function<decltype(get_status)>(module, "im_vcam_get_status");
            start = load_function<decltype(start)>(module, "im_vcam_start_ex");
            publish = load_function<decltype(publish)>(module, "im_vcam_publish_bgra");
            stop = load_function<decltype(stop)>(module, "im_vcam_stop");
            return true;
        } catch (...) { FreeLibrary(module); module = nullptr; return false; }
    }
    void close() { if (running && stop) stop(); running = false; }
    ~Camera() { close(); if (module) FreeLibrary(module); }
};
struct Preferences {
    unsigned fps{60}, resolution{}, decoder{}, color{};
    unsigned usb_mode{}, preview_rotation{};
    bool audio{true}, reconnect{true}, portrait_camera{};
    unsigned volume{100};
    std::wstring device;
};
fs::path settings_path() {
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        throw std::runtime_error("Cannot locate local application settings.");
    fs::path directory = fs::path(local) / L"USBMirror";
    CoTaskMemFree(local);
    return directory / L"settings.ini";
}
void configure_capture_log() {
    fs::path path;
    try {
        const auto directory = settings_path().parent_path() / L"Logs";
        std::error_code error; fs::create_directories(directory, error);
        if (!error) path = directory / L"capture.log";
    } catch (...) {}
    if (path.empty()) {
        std::error_code error; const auto temporary = fs::temp_directory_path(error);
        if (!error) path = temporary / L"USBMirror-capture.log";
    }
    if (!path.empty()) SetEnvironmentVariableW(L"IPHONE_MIRROR_LOG_FILE", path.c_str());
}
Preferences read_preferences(const fs::path& file) {
    Preferences value;
    auto number = [&](const wchar_t* key, unsigned fallback) { return GetPrivateProfileIntW(L"USBMirror", key, fallback, file.c_str()); };
    value.fps = number(L"FPS", 60); if (!usbmirror::valid_fps(value.fps)) value.fps = 60;
    value.resolution = std::min(number(L"Resolution", 0), 2U);
    value.decoder = std::min(number(L"Decoder", 0), 2U);
    value.color = std::min(number(L"Color", 0), 1U);
    value.usb_mode = std::min(number(L"USBMode", 0), 1U);
    value.preview_rotation = number(L"PreviewRotation", 0) % 4;
    value.volume = std::min(number(L"Volume", 100), 100U);
    value.audio = number(L"Audio", 1) != 0;
    value.reconnect = number(L"Reconnect", 1) != 0;
    value.portrait_camera = number(L"PortraitCamera", 0) != 0;
    wchar_t device[MaxUdid]{};
    GetPrivateProfileStringW(L"USBMirror", L"Device", L"", device, MaxUdid, file.c_str());
    value.device = device;
    return value;
}
bool save_preferences(const fs::path& file, const Preferences& value) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    if (error) return false;
    bool ok = true;
    auto number = [&](const wchar_t* key, unsigned data) {
        if (!WritePrivateProfileStringW(L"USBMirror", key, std::to_wstring(data).c_str(), file.c_str())) ok = false;
    };
    number(L"FPS", value.fps); number(L"Resolution", value.resolution);
    number(L"Decoder", value.decoder); number(L"Color", value.color);
    number(L"USBMode", value.usb_mode);
    number(L"PreviewRotation", value.preview_rotation);
    number(L"Volume", value.volume); number(L"Audio", value.audio);
    number(L"Reconnect", value.reconnect); number(L"PortraitCamera", value.portrait_camera);
    if (!WritePrivateProfileStringW(L"USBMirror", L"Device", value.device.c_str(), file.c_str())) ok = false;
    return ok;
}
std::wstring readable_failure(CaptureFailureKind kind, int code) {
    std::wstring result;
    switch (kind) {
    case CaptureFailureKind::Driver: result = L"USB capture driver is not ready. Open Driver setup, then reconnect the phone."; break;
    case CaptureFailureKind::DeviceDisconnected: result = L"Phone disconnected. Reconnect the USB cable and unlock the phone."; break;
    case CaptureFailureKind::NoVideoFrames: result = L"Connected, but no video arrived. Unlock the phone and reconnect."; break;
    case CaptureFailureKind::Timeout: result = L"The USB connection timed out. Unlock the phone and reconnect."; break;
    case CaptureFailureKind::ExistingSession: result = L"Another capture session is using the phone. Close it before connecting."; break;
    default: result = L"USB capture stopped. Reconnect the phone or open Driver setup."; break;
    }
    if (code) result += L" (Code " + std::to_wstring(code) + L")";
    return result;
}
struct Snapshot {
    std::vector<DeviceInfo> devices;
    std::wstring message{L"Starting capture runtime..."};
    CaptureStatus capture{};
    VideoOutputStatus video{};
    bool active{}, busy{}, competing{}, fatal{}, camera_available{}, camera_supported{}, camera_registered{}, camera_running{};
    bool desired{}, closing{}, usb_missing{};
    bool audio_packets_received{}, audio_meter_available{};
    float audio_peak{};
    unsigned preview_rotation{};
};
enum class Action { Connect, Disconnect, Refresh, Preferences, CameraToggle, CameraRefresh, UsbPresence, Exit };
struct Command { Action action; ::Preferences preferences; };
class Worker {
    HWND owner_, preview_;
    fs::path runtime_;
    Core core_;
    Camera camera_;
    Preferences preferences_;
    std::thread thread_;
    std::mutex queue_mutex_, snapshot_mutex_;
    std::condition_variable wake_;
    std::deque<Command> commands_;
    Snapshot snapshot_;
    SessionHandle session_{};
    std::unique_ptr<usbmirror::HardwareLease> capture_lease_;
    bool quit_{}, desired_{};
    unsigned failures_{};
    Clock::time_point next_retry_{}, next_refresh_{}, next_status_{}, next_camera_{};
    std::int64_t camera_timestamp_{};
    std::vector<std::uint8_t> camera_pixels_;
    std::uint64_t audio_sequence_{};
    std::vector<std::uint8_t> audio_probe_buffer_;
    Clock::time_point audio_measurement_at_{};
    void notify() {
        snapshot_.active = session_ != 0;
        snapshot_.desired = desired_;
        snapshot_.camera_running = camera_.running;
        { std::scoped_lock lock(snapshot_mutex_); published_ = snapshot_; }
        PostMessageW(owner_, UpdateMessage, 0, 0);
    }
    Snapshot published_;
    void refresh_usb_presence() {
        EnvironmentInfo environment{}; environment.struct_size = sizeof(environment);
        if (core_.get_environment(&environment) == 0)
            snapshot_.usb_missing = environment.physical_apple_usb_devices == 0;
    }
    void refresh_devices(bool metadata) {
        std::vector<DeviceInfo> devices(32);
        std::uint32_t count = static_cast<std::uint32_t>(devices.size());
        const auto result = core_.refresh_devices_ex(devices.data(), &count, metadata ? 1 : 0);
        if (result == static_cast<int>(Result::BufferTooSmall) && count <= 128) {
            devices.resize(count);
            count = static_cast<std::uint32_t>(devices.size());
            if (core_.refresh_devices_ex(devices.data(), &count, metadata ? 1 : 0) != 0) return;
        } else if (result != 0) return;
        if (count > devices.size()) return;
        devices.resize(count);
        devices.erase(std::remove_if(devices.begin(), devices.end(), [](const DeviceInfo& item) {
            return !item.usb_connected || !item.udid[0] || wcsncmp(item.udid, L"airplay://", 10) == 0;
        }), devices.end());
        snapshot_.devices = std::move(devices);
    }
    void refresh_camera() {
        snapshot_.camera_available = camera_.load(runtime_);
        if (!snapshot_.camera_available) return;
        virtual_camera::Status status{};
        status.struct_size = sizeof(status);
        if (camera_.get_status(&status) >= 0) {
            snapshot_.camera_supported = status.supported != 0;
            snapshot_.camera_registered = status.registered != 0;
        }
    }
    void stop_session(bool shutdown = false) {
        if (!session_) { capture_lease_.reset(); return; }
        snapshot_.busy = true;
        snapshot_.message = shutdown ? L"Closing the USB connection safely..." : L"Disconnecting...";
        notify();
        camera_.close();
        const auto result = core_.session_stop(session_);
        core_.session_destroy(session_);
        session_ = 0;
        capture_lease_.reset();
        snapshot_.capture = {};
        snapshot_.video = {};
        snapshot_.preview_rotation = 0;
        snapshot_.audio_packets_received = false;
        snapshot_.audio_meter_available = false;
        snapshot_.audio_peak = 0;
        audio_sequence_ = 0;
        audio_measurement_at_ = {};
        snapshot_.busy = false;
        snapshot_.message = result == 0 ? L"Disconnected. Your phone is ready for normal USB use." :
            L"Capture stopped. Unplug and reconnect the phone before using USB again.";
    }
    void update_preferences() {
        if (!session_) return;
        const auto width = preferences_.resolution == 1 ? 1920U : preferences_.resolution == 2 ? 1280U : 0U;
        const auto height = preferences_.resolution == 1 ? 1080U : preferences_.resolution == 2 ? 720U : 0U;
        core_.session_set_video_preferences(session_, width, height, preferences_.fps);
        core_.session_set_pipeline_preferences(session_, preferences_.decoder, preferences_.color);
        core_.session_set_audio_enabled(session_, preferences_.audio ? 1 : 0);
        core_.session_set_audio_volume(session_, preferences_.volume / 100.0F);
        apply_preview_rotation();
    }
    void apply_preview_rotation(bool force = false) {
        if (!session_ || (!force && snapshot_.preview_rotation == preferences_.preview_rotation)) return;
        const auto result = core_.session_set_window_rotation(session_, preview_, static_cast<std::int32_t>(preferences_.preview_rotation));
        if (result == 0) snapshot_.preview_rotation = preferences_.preview_rotation;
        else snapshot_.message = L"Could not rotate the preview. (Code " + std::to_wstring(result) + L")";
    }
    bool device_present() const {
        return std::any_of(snapshot_.devices.begin(), snapshot_.devices.end(), [&](const DeviceInfo& item) {
            return preferences_.device == item.udid && item.lockdown_accessible != 0;
        });
    }
    void start_session() {
        if (session_) return;
        if (original_app_running()) {
            snapshot_.competing = true;
            snapshot_.message = L"Another mirroring app is open. Close it before connecting with USB Mirror.";
            next_retry_ = Clock::now() + std::chrono::seconds(3);
            return;
        }
        if (preferences_.device.empty()) { desired_ = false; snapshot_.message = L"Select your USB-connected iPhone first."; return; }
        snapshot_.busy = true;
        snapshot_.message = L"Checking your phone and starting USB capture...";
        notify();
        capture_lease_ = usbmirror::HardwareLease::acquire();
        if (!capture_lease_) {
            snapshot_.busy = false;
            snapshot_.message = L"Driver setup is using USB. Waiting for it to finish...";
            next_retry_ = Clock::now() + std::chrono::seconds(3); return;
        }
        struct FailedStart { SessionHandle& session; std::unique_ptr<usbmirror::HardwareLease>& lease;
            ~FailedStart() { if (!session) lease.reset(); } } failed_start{session_, capture_lease_};
        std::int32_t driver_ready{};
        const auto probe = core_.is_libusb0_device_available(preferences_.device.c_str(), &driver_ready);
        if (probe != 0 || !driver_ready) {
            snapshot_.busy = false; desired_ = false;
            snapshot_.message = L"USB capture driver is not ready for this phone. Open Driver setup, then reconnect it.";
            return;
        }
        CaptureOptions options{};
        options.struct_size = sizeof(options); options.api_version = ApiVersion;
        options.requested_width = preferences_.resolution == 1 ? 1920U : preferences_.resolution == 2 ? 1280U : 0U;
        options.requested_height = preferences_.resolution == 1 ? 1080U : preferences_.resolution == 2 ? 720U : 0U;
        options.target_fps = preferences_.fps; options.play_audio = preferences_.audio ? 1 : 0;
        options.audio_volume = preferences_.volume / 100.0F;
        options.reserved[2] = preferences_.usb_mode; // 0=A/Demo; 1=existing B/compatibility mode.
        options.reserved[3] = preferences_.decoder; options.reserved[4] = preferences_.color;
        const auto result = core_.session_create(preferences_.device.c_str(), &options, &session_);
        snapshot_.busy = false;
        if (result != 0 || !session_) {
            session_ = 0;
            next_retry_ = Clock::now() + std::chrono::seconds(usbmirror::retry_seconds(failures_++));
            snapshot_.message = L"Could not start USB capture. Reconnect the phone or open Driver setup. (Code " + std::to_wstring(result) + L")";
            if (result == static_cast<int>(Result::DriverSafetyBlocked) || result == static_cast<int>(Result::InvalidArgument)) desired_ = false;
            return;
        }
        core_.session_set_corner_profile(session_, 0.0F, 2.36F);
        const auto attach = core_.session_attach_preview(session_, preview_);
        if (attach != 0) {
            stop_session(); desired_ = false;
            snapshot_.message = L"The video preview could not start. (Code " + std::to_wstring(attach) + L")";
            return;
        }
        snapshot_.message = L"USB connected. Waiting for video...";
        apply_preview_rotation(true);
        camera_timestamp_ = 0;
        audio_sequence_ = 0;
        audio_measurement_at_ = {};
    }
    void meter_audio() {
        if (!session_) return;
        if (audio_probe_buffer_.empty()) audio_probe_buffer_.resize(16384);
        float peak{};
        bool received{}, measured{};
        for (unsigned index = 0; index < 256; ++index) {
            AudioPacketInfo info{}; info.struct_size = sizeof(info);
            auto capacity = static_cast<std::uint32_t>(audio_probe_buffer_.size());
            auto result = core_.session_copy_next_audio_packet(session_, audio_sequence_, &info,
                audio_probe_buffer_.data(), &capacity);
            if (result == static_cast<int>(Result::BufferTooSmall) && capacity <= 1024U * 1024U) {
                audio_probe_buffer_.resize(capacity);
                result = core_.session_copy_next_audio_packet(session_, audio_sequence_, &info,
                    audio_probe_buffer_.data(), &capacity);
            }
            if (result != 0 || info.sequence <= audio_sequence_ || capacity > audio_probe_buffer_.size()) break;
            audio_sequence_ = info.sequence;
            received = true;
            if (info.bits_per_sample == 16) {
                measured = true;
                peak = std::max(peak, usbmirror::pcm16_peak(audio_probe_buffer_.data(), capacity));
            }
        }
        snapshot_.audio_packets_received = snapshot_.audio_packets_received || received;
        if (received) { snapshot_.audio_meter_available = measured; snapshot_.audio_peak = peak; audio_measurement_at_ = Clock::now(); }
        else if (Clock::now() - audio_measurement_at_ > std::chrono::seconds(2)) {
            snapshot_.audio_packets_received = false; snapshot_.audio_meter_available = false; snapshot_.audio_peak = 0;
        }
    }
    void poll_status() {
        if (!session_) return;
        CaptureStatus status{}; status.struct_size = sizeof(status);
        if (core_.session_get_status(session_, &status) != 0) return;
        snapshot_.capture = status;
        meter_audio();
        VideoOutputStatus video{}; video.struct_size = sizeof(video);
        if (core_.session_get_video_output_status(session_, preview_, &video) == 0) snapshot_.video = video;
        switch (status.state) {
        case CaptureState::Streaming:
            failures_ = 0;
            snapshot_.message = L"Mirroring over USB. Capture the USB Mirror Preview window in your streaming app.";
            break;
        case CaptureState::ActivatingUsb: snapshot_.message = L"Activating USB screen capture..."; break;
        case CaptureState::WaitingForDevice: snapshot_.message = L"Waiting for the phone to reconnect to USB..."; break;
        case CaptureState::Handshaking: snapshot_.message = L"Phone connected. Waiting for video..."; break;
        case CaptureState::Error:
        case CaptureState::Stopped: {
            const auto failure = readable_failure(status.failure_kind, status.error_code);
            bool recoverable = status.failure_kind == CaptureFailureKind::DeviceDisconnected ||
                status.failure_kind == CaptureFailureKind::UsbConnection || status.failure_kind == CaptureFailureKind::Timeout ||
                status.failure_kind == CaptureFailureKind::SystemClosed;
            stop_session();
            snapshot_.message = failure;
            // The baseline can report an unplug as NoVideoFrames after its
            // handshake watchdog expires. Retry only when the phone is absent,
            // then wait for normal device discovery; do not reset a connected
            // phone indefinitely when an app simply stops supplying video.
            if (status.failure_kind == CaptureFailureKind::NoVideoFrames) {
                EnvironmentInfo environment{}; environment.struct_size = sizeof(environment);
                if (core_.get_environment(&environment) == 0 && environment.physical_apple_usb_devices == 0) {
                    recoverable = true;
                    snapshot_.message = preferences_.reconnect ?
                        L"Windows cannot see your phone. Reconnect and unlock it; mirroring will resume automatically." :
                        L"Windows cannot see your phone. Reconnect and unlock it, then click Connect.";
                    refresh_devices(false);
                }
            }
            if (!preferences_.reconnect || !recoverable) desired_ = false;
            next_retry_ = Clock::now() + std::chrono::seconds(usbmirror::retry_seconds(failures_++));
            break;
        }
        default: break;
        }
    }
    void toggle_camera() {
        refresh_camera();
        if (camera_.running) { camera_.close(); snapshot_.message = L"Virtual camera stopped. USB mirroring continues."; return; }
        if (!session_ || snapshot_.capture.state != CaptureState::Streaming) { snapshot_.message = L"Start USB mirroring before starting the virtual camera."; return; }
        if (!snapshot_.camera_supported || !snapshot_.camera_registered) { snapshot_.message = L"Install the optional camera first, then start it here."; return; }
        const unsigned width = preferences_.portrait_camera ? 1080U : 1920U;
        const unsigned height = preferences_.portrait_camera ? 1920U : 1080U;
        const auto result = camera_.start(L"USB Mirror Camera", width, height, 60);
        if (result < 0) {
            std::wostringstream text; text << L"Windows could not start the camera. (0x" << std::hex << unsigned(result) << L")";
            snapshot_.message = text.str(); return;
        }
        camera_.running = true; camera_timestamp_ = 0; next_camera_ = Clock::now();
        snapshot_.message = L"USB Mirror Camera is available. Capture game audio separately through Windows.";
    }
    void pump_camera() {
        if (!camera_.running || !session_ || Clock::now() < next_camera_) return;
        next_camera_ = Clock::now() + std::chrono::microseconds(16667);
        std::int64_t timestamp{};
        if (core_.session_get_latest_video_timestamp(session_, &timestamp) != 0 || !timestamp || timestamp == camera_timestamp_) return;
        VideoFrameInfo info{}; info.struct_size = sizeof(info);
        if (camera_pixels_.empty()) camera_pixels_.resize(1920U * 1080U * 4U);
        std::uint32_t size = static_cast<std::uint32_t>(camera_pixels_.size());
        const auto max_width = preferences_.portrait_camera ? 1080U : 1920U;
        const auto max_height = preferences_.portrait_camera ? 1920U : 1080U;
        const auto result = core_.session_copy_latest_video_frame(session_, &info, camera_pixels_.data(), &size, max_width, max_height);
        if (result != 0 || !info.width || !info.height || size > camera_pixels_.size()) return;
        if (camera_.publish(camera_pixels_.data(), info.width, info.height, info.stride, info.timestamp_100ns) < 0) {
            camera_.close(); snapshot_.message = L"Camera output stopped. USB window mirroring is still running.";
            notify(); return;
        }
        camera_timestamp_ = timestamp;
    }
    void run() {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            core_.load(runtime_);
            refresh_usb_presence();
            refresh_camera();
            refresh_devices(true);
            desired_ = preferences_.reconnect && !preferences_.device.empty();
            snapshot_.message = desired_ ? L"Waiting for your remembered iPhone. Connect it by USB and unlock it." :
                L"Connect your iPhone by USB, unlock it, and accept Trust This Computer.";
            notify();
            while (!quit_) {
                std::deque<Command> commands;
                {
                    std::unique_lock lock(queue_mutex_);
                    wake_.wait_for(lock, camera_.running ? std::chrono::milliseconds(8) : std::chrono::milliseconds(200), [&] { return !commands_.empty(); });
                    commands.swap(commands_);
                }
                for (const auto& command : commands) {
                    if (command.action == Action::Exit) { desired_ = false; quit_ = true; break; }
                    const auto old_camera_orientation = preferences_.portrait_camera;
                    preferences_ = command.preferences;
                    if (camera_.running && old_camera_orientation != preferences_.portrait_camera) camera_.close();
                    switch (command.action) {
                    case Action::Connect: desired_ = true; failures_ = 0; next_retry_ = {}; start_session(); break;
                    case Action::Disconnect: desired_ = false; stop_session(); break;
                    case Action::Refresh: refresh_usb_presence(); refresh_devices(true); refresh_camera(); break;
                    case Action::UsbPresence: refresh_usb_presence(); break;
                    case Action::Preferences: update_preferences(); break;
                    case Action::CameraToggle: toggle_camera(); break;
                    case Action::CameraRefresh: refresh_camera(); break;
                    default: break;
                    }
                    notify();
                }
                if (quit_) break;
                const auto now = Clock::now();
                if (now >= next_refresh_) {
                    const bool was_competing = snapshot_.competing;
                    snapshot_.competing = original_app_running();
                    if (!snapshot_.busy) refresh_devices(false);
                    if (!session_ && snapshot_.competing) snapshot_.message = L"Another mirroring app is open. Close it before connecting with USB Mirror.";
                    else if (!session_ && was_competing) snapshot_.message = L"Ready. Select your USB-connected iPhone and click Connect.";
                    next_refresh_ = now + std::chrono::seconds(3);
                }
                if (usbmirror::should_auto_connect(preferences_.reconnect, desired_, session_ != 0,
                    snapshot_.competing, device_present(), now >= next_retry_)) start_session();
                if (now >= next_status_) { poll_status(); notify(); next_status_ = now + std::chrono::milliseconds(500); }
                pump_camera();
            }
            snapshot_.closing = true;
            stop_session(true);
            camera_.close();
            // Destroy native resources while this worker's COM apartment is alive.
            if (core_.initialized) { core_.shutdown(); core_.initialized = false; }
            capture_lease_.reset();
        } catch (const std::exception& error) {
            const std::string message = error.what();
            snapshot_.message.assign(message.begin(), message.end());
            snapshot_.fatal = true; notify();
            camera_.close();
            if (core_.initialized) { core_.shutdown(); core_.initialized = false; }
            capture_lease_.reset();
        }
        if (SUCCEEDED(com)) CoUninitialize();
        if (quit_) PostMessageW(owner_, ExitMessage, 0, 0);
    }
public:
    Worker(HWND owner, HWND preview, fs::path runtime, Preferences preferences) :
        owner_(owner), preview_(preview), runtime_(std::move(runtime)), preferences_(std::move(preferences)) {}
    void start() { thread_ = std::thread([this] { run(); }); }
    void send(Action action, const Preferences& preferences) {
        { std::scoped_lock lock(queue_mutex_); commands_.push_back({action, preferences}); }
        wake_.notify_one();
    }
    Snapshot snapshot() { std::scoped_lock lock(snapshot_mutex_); return published_; }
    ~Worker() { if (thread_.joinable()) { send(Action::Exit, ::Preferences{}); thread_.join(); } }
};

enum ControlId { Devices = 100, Refresh, Connect, ShowPreview, Fps, Resolution, Decoder,
    Color, Audio, Volume, Reconnect, AudioRouting, Driver, CameraButton, CameraSetup,
    CameraPortrait, StatusText, MetricsText, CameraText, Help, AudioLevelText, UsbStandard, UsbCompatibility,
    ToolsPopup, VolumeText, RotateLeft, RotateRight, RotateReset };
class Application;
Application* application{};
LRESULT CALLBACK main_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK preview_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
class Application {
    HINSTANCE instance_;
    HFONT font_{}, heading_font_{}, small_font_{}, section_font_{};
    HBRUSH background_{}, preview_background_{}, surface_brush_{}, field_brush_{};
    HMENU tools_menu_{}, usb_mode_menu_{}, rotation_menu_{};
    HWND heading_{};
    HICON app_icon_{};
    Preferences preferences_;
    fs::path directory_, settings_;
    std::unique_ptr<Worker> worker_;
    Snapshot snapshot_;
    std::vector<DeviceInfo> devices_;
    bool demo_{}, closing_{}, preview_visible_{}, device_list_initialized_{};
    unsigned last_width_{}, last_height_{};
    UINT dpi_{96};
    std::wstring ui_test_state_;
    bool live_input_{};
    int scaled(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    HWND control(int id) const { return GetDlgItem(window, id); }
    HWND add(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id = 0) {
        const auto handle = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            scaled(x), scaled(y), scaled(width), scaled(height), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (_wcsicmp(type, WC_BUTTONW) == 0) usbmirror::ui::style(handle, (style & BS_TYPEMASK) == BS_AUTOCHECKBOX ? 1 : 0);
        else if (_wcsicmp(type, TRACKBAR_CLASSW) == 0) usbmirror::ui::style(handle, 3);
        return handle;
    }
    HWND label(const wchar_t* text, int x, int y, int width, int height, int id = 0, unsigned role = 0) {
        const auto handle = add(WC_STATICW, text, SS_NOPREFIX | SS_ENDELLIPSIS, x, y, width, height, id);
        SetPropW(handle, L"USBMirror.TextRole", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(role)));
        if (role) SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(role == 2 ? section_font_ : small_font_), TRUE);
        return handle;
    }
    void combo(int id, int x, int y, int width, const std::vector<const wchar_t*>& values, unsigned selection) {
        const auto handle = add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, x, y, width, 220, id);
        SendMessageW(handle, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scaled(30));
        SendMessageW(handle, CB_SETITEMHEIGHT, 0, scaled(30));
        usbmirror::ui::style(handle, 2);
        for (const auto* value : values) SendMessageW(handle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
        SendMessageW(handle, CB_SETCURSEL, selection, 0);
    }
    unsigned selected(int id) const { const auto value = SendMessageW(control(id), CB_GETCURSEL, 0, 0); return value < 0 ? 0U : static_cast<unsigned>(value); }
    void collect() {
        preferences_.fps = std::vector<unsigned>{30, 60, 120, 0}[std::min(selected(Fps), 3U)];
        preferences_.resolution = std::min(selected(Resolution), 2U);
        preferences_.decoder = std::min(selected(Decoder), 2U);
        preferences_.color = std::min(selected(Color), 1U);
        preferences_.audio = Button_GetCheck(control(Audio)) == BST_CHECKED;
        preferences_.reconnect = Button_GetCheck(control(Reconnect)) == BST_CHECKED;
        preferences_.portrait_camera = Button_GetCheck(control(CameraPortrait)) == BST_CHECKED;
        preferences_.volume = static_cast<unsigned>(SendMessageW(control(Volume), TBM_GETPOS, 0, 0));
        const auto volume = std::to_wstring(preferences_.volume) + L"%";
        SetWindowTextW(control(VolumeText), volume.c_str());
        const auto index = selected(Devices);
        if (index < devices_.size()) preferences_.device = devices_[index].udid;
    }
    void persist() { if (!demo_) save_preferences(settings_, preferences_); }
    void dispatch(Action action) { collect(); persist(); if (worker_) worker_->send(action, preferences_); }
    void fit_preview(unsigned width, unsigned height, unsigned rotation) {
        const auto oriented = usbmirror::rotated_size(width, height, rotation);
        width = oriented.width; height = oriented.height;
        if (!width || !height) return;
        const bool orientation_changed = last_width_ && ((last_width_ > last_height_) != (width > height));
        const bool first_frame = !last_width_;
        last_width_ = width; last_height_ = height;
        if (!first_frame && !orientation_changed) return;
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(preview, MONITOR_DEFAULTTONEAREST), &monitor);
        const auto fit = usbmirror::fit(width, height,
            static_cast<unsigned>(std::max(240L, monitor.rcWork.right - monitor.rcWork.left - 120)),
            static_cast<unsigned>(std::max(200L, monitor.rcWork.bottom - monitor.rcWork.top - 140)));
        RECT rectangle{0, 0, static_cast<LONG>(fit.width), static_cast<LONG>(fit.height)};
        AdjustWindowRectExForDpi(&rectangle, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(preview));
        RECT current{}; GetWindowRect(preview, &current);
        const auto window_width = rectangle.right - rectangle.left;
        const auto window_height = rectangle.bottom - rectangle.top;
        const auto x = std::clamp(current.left, monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - window_width));
        const auto y = std::clamp(current.top, monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - window_height));
        SetWindowPos(preview, nullptr, x, y, window_width, window_height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void install_camera() {
        const auto runtime = directory_;
        const auto helper = runtime / L"USBMirror.CameraSetup.exe";
        const auto component = runtime / L"iPhoneMirror.VirtualCamera.dll";
        if (!fs::exists(helper) || !fs::exists(component)) { MessageBoxW(window, L"The optional camera files are missing. Extract the complete USB Mirror folder.", L"USB Mirror", MB_OK | MB_ICONERROR); return; }
        const auto parameters = L"install \"" + component.wstring() + L"\"";
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.fMask = SEE_MASK_NOCLOSEPROCESS;
        execute.hwnd = window; execute.lpVerb = L"runas"; execute.lpFile = helper.c_str();
        execute.lpParameters = parameters.c_str(); execute.lpDirectory = runtime.c_str(); execute.nShow = SW_HIDE;
        if (!ShellExecuteExW(&execute)) {
            if (GetLastError() != ERROR_CANCELLED) MessageBoxW(window, system_error(GetLastError()).c_str(), L"Camera installation", MB_OK | MB_ICONERROR);
            return;
        }
        if (execute.hProcess) { camera_install_ = execute.hProcess; SetTimer(window, 2, 500, nullptr); EnableWindow(control(CameraSetup), FALSE); }
    }
    HANDLE camera_install_{};
    void driver_setup() {
        const auto driver = directory_ / L"USBMirror.Driver.exe";
        if (!fs::exists(driver)) {
            MessageBoxW(window, L"USBMirror.Driver.exe is missing. Extract the complete USB Mirror folder.", L"Driver setup", MB_OK | MB_ICONINFORMATION);
            return;
        }
        ShellExecuteW(window, L"open", driver.c_str(), nullptr, driver.parent_path().c_str(), SW_SHOWNORMAL);
    }
    void make_fonts() {
        const auto make = [&](int size, int weight) {
            return CreateFontW(-scaled(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        font_ = make(14, FW_NORMAL); heading_font_ = make(26, FW_SEMIBOLD);
        small_font_ = make(12, FW_NORMAL); section_font_ = make(16, FW_SEMIBOLD);
    }
    RECT bounds(int x, int y, int width, int height) const { return {scaled(x), scaled(y), scaled(x + width), scaled(y + height)}; }
    void paint(HDC dc) {
        namespace ui = usbmirror::ui;
        RECT client{}; GetClientRect(window, &client); FillRect(dc, &client, background_);
        ui::line(dc, scaled(44), scaled(277), scaled(716), scaled(277), ui::border);
        ui::line(dc, scaled(468), scaled(300), scaled(468), scaled(512), ui::border);
        ui::line(dc, scaled(44), scaled(539), scaled(716), scaled(539), ui::border);
        if (app_icon_) DrawIconEx(dc, scaled(24), scaled(27), app_icon_, scaled(44), scaled(44), 0, nullptr, DI_NORMAL);
        auto badge = bounds(566, 114, 150, 27);
        const auto badge_text = closing_ ? L"Closing" : snapshot_.fatal ? L"Unavailable" : live_input_ ? L"Connected" :
            snapshot_.busy && !snapshot_.usb_missing ? L"Connecting" : snapshot_.desired ? L"Waiting for iPhone" :
            !devices_.empty() ? L"Ready to connect" : L"No iPhone";
        const auto badge_color = live_input_ ? ui::accent : snapshot_.fatal ? RGB(247, 153, 153) :
            snapshot_.desired ? RGB(235, 196, 127) : ui::muted;
        const auto dot = bounds(579, 125, 5, 5);
        const auto brush = CreateSolidBrush(badge_color); const auto old_brush = SelectObject(dc, brush);
        const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
        SelectObject(dc, old_pen); SelectObject(dc, old_brush); DeleteObject(brush);
        badge.left += scaled(25); badge.right -= scaled(8);
        ui::label(dc, badge_text, badge, small_font_, badge_color);
        auto meter = bounds(496, 447, 220, 5);
        ui::box(dc, meter, ui::border, ui::border);
        if (live_input_ && snapshot_.audio_meter_available && snapshot_.audio_peak > 0) {
            meter.right = meter.left + static_cast<LONG>((meter.right - meter.left) * std::min(1.0F, snapshot_.audio_peak));
            if (meter.right > meter.left) ui::box(dc, meter, ui::accent, ui::accent);
        }
    }
public:
    HWND window{}, preview{};
    fs::path ui_test_output;
    explicit Application(HINSTANCE instance, bool demo, fs::path ui_output = {}, std::wstring ui_state = {}) : instance_(instance),
        directory_(executable_directory()), demo_(demo), ui_test_state_(std::move(ui_state)), ui_test_output(std::move(ui_output)) {
        settings_ = settings_path();
        if (!demo) preferences_ = read_preferences(settings_);
    }
    bool create() {
        background_ = CreateSolidBrush(usbmirror::ui::background); preview_background_ = CreateSolidBrush(RGB(10, 12, 16));
        surface_brush_ = CreateSolidBrush(usbmirror::ui::surface); field_brush_ = CreateSolidBrush(usbmirror::ui::field);
        WNDCLASSEXW main_class{sizeof(main_class)};
        main_class.style = CS_HREDRAW | CS_VREDRAW; main_class.lpfnWndProc = main_proc;
        main_class.hInstance = instance_; main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        main_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_USB_MIRROR));
        main_class.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_USB_MIRROR), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        app_icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_USB_MIRROR), IMAGE_ICON, 64, 64, LR_SHARED));
        main_class.hbrBackground = background_; main_class.lpszClassName = MainClass;
        RegisterClassExW(&main_class);
        WNDCLASSEXW preview_class = main_class;
        preview_class.lpfnWndProc = preview_proc; preview_class.hbrBackground = preview_background_;
        preview_class.lpszClassName = PreviewClass;
        RegisterClassExW(&preview_class);
        window = CreateWindowExW(0, MainClass, demo_ ? L"USB Mirror — UI preview" : L"USB Mirror",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 780, 760, nullptr, nullptr, instance_, nullptr);
        if (!window) return false;
        dpi_ = GetDpiForWindow(window);
        RECT desired{0, 0, scaled(760), scaled(704)};
        AdjustWindowRectExForDpi(&desired, GetWindowLongW(window, GWL_STYLE), FALSE, 0, dpi_);
        SetWindowPos(window, nullptr, 0, 0, desired.right - desired.left, desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);
        make_fonts();
        BOOL dark = TRUE; DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        const COLORREF caption = usbmirror::ui::background, caption_text = usbmirror::ui::text;
        DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
        DwmSetWindowAttribute(window, 36, &caption_text, sizeof(caption_text));
        heading_ = label(L"USB Mirror", 84, 30, 400, 36);
        SendMessageW(heading_, WM_SETFONT, reinterpret_cast<WPARAM>(heading_font_), TRUE);
        add(WC_BUTTONW, L"&Tools  \u00b7\u00b7\u00b7", BS_OWNERDRAW | WS_TABSTOP, 644, 34, 92, 34, ToolsPopup);
        label(L"Connection", 44, 115, 380, 24, 0, 2);
        combo(Devices, 44, 152, 410, {}, 0);
        add(WC_BUTTONW, L"Refresh", BS_OWNERDRAW | WS_TABSTOP, 466, 152, 94, 36, Refresh);
        add(WC_BUTTONW, L"Connect", BS_OWNERDRAW | WS_TABSTOP, 572, 152, 144, 36, Connect);
        add(WC_BUTTONW, L"Reconnect automatically", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 199, 430, 26, Reconnect);
        Button_SetCheck(control(Reconnect), preferences_.reconnect ? BST_CHECKED : BST_UNCHECKED);
        label(L"Starting USB Mirror...", 44, 235, 672, 20, StatusText, 1);
        label(L"Capture window", 44, 300, 396, 24, 0, 2);
        label(L"Size", 44, 338, 188, 18, 0, 1);
        label(L"Preview FPS cap", 252, 338, 188, 18, 0, 1);
        combo(Resolution, 44, 360, 188, {L"Native", L"1080p limit", L"720p limit"}, preferences_.resolution);
        const auto fps_index = preferences_.fps == 30 ? 0U : preferences_.fps == 60 ? 1U : preferences_.fps == 120 ? 2U : 3U;
        combo(Fps, 252, 360, 188, {L"30 fps", L"60 fps", L"120 fps", L"Uncapped"}, fps_index);
        label(L"Decoder", 44, 408, 188, 18, 0, 1);
        label(L"Color", 252, 408, 188, 18, 0, 1);
        combo(Decoder, 44, 430, 188, {L"Automatic", L"Hardware preferred", L"Software"}, preferences_.decoder);
        combo(Color, 252, 430, 188, {L"Automatic", L"SDR"}, preferences_.color);
        add(WC_BUTTONW, L"Open capture window", BS_OWNERDRAW | WS_TABSTOP, 44, 478, 396, 34, ShowPreview);
        label(L"Audio", 496, 300, 220, 24, 0, 2);
        add(WC_BUTTONW, L"Play iPhone audio", BS_AUTOCHECKBOX | WS_TABSTOP, 496, 337, 220, 26, Audio);
        Button_SetCheck(control(Audio), preferences_.audio ? BST_CHECKED : BST_UNCHECKED);
        label(L"Volume", 496, 375, 140, 18, 0, 1);
        label(L"100%", 652, 375, 64, 18, VolumeText, 1);
        add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 492, 395, 228, 30, Volume);
        SendMessageW(control(Volume), TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(control(Volume), TBM_SETPOS, TRUE, preferences_.volume);
        add(WC_BUTTONW, L"Choose audio output", BS_OWNERDRAW | WS_TABSTOP, 496, 478, 220, 34, AudioRouting);
        label(L"Input: waiting for iPhone", 496, 426, 220, 18, AudioLevelText, 1);
        label(L"Camera output", 44, 560, 172, 24, 0, 2);
        label(L"Optional · video only", 228, 564, 488, 18, CameraText, 1);
        add(WC_BUTTONW, L"Start camera", BS_OWNERDRAW | WS_TABSTOP, 44, 598, 144, 34, CameraButton);
        add(WC_BUTTONW, L"Install camera...", BS_OWNERDRAW | WS_TABSTOP, 200, 598, 144, 34, CameraSetup);
        add(WC_BUTTONW, L"Portrait 1080 × 1920", BS_AUTOCHECKBOX | WS_TABSTOP, 368, 602, 348, 26, CameraPortrait);
        Button_SetCheck(control(CameraPortrait), preferences_.portrait_camera ? BST_CHECKED : BST_UNCHECKED);
        label(L"", 24, 668, 712, 20, MetricsText, 1);
        tools_menu_ = CreatePopupMenu();
        usb_mode_menu_ = CreatePopupMenu();
        AppendMenuW(usb_mode_menu_, MF_STRING, UsbStandard, L"Standard");
        AppendMenuW(usb_mode_menu_, MF_STRING, UsbCompatibility, L"Compatibility");
        AppendMenuW(tools_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(usb_mode_menu_), L"USB mode");
        CheckMenuRadioItem(usb_mode_menu_, UsbStandard, UsbCompatibility,
            preferences_.usb_mode ? UsbCompatibility : UsbStandard, MF_BYCOMMAND);
        rotation_menu_ = CreatePopupMenu();
        AppendMenuW(rotation_menu_, MF_STRING, RotateLeft, L"Rotate &left\tCtrl+Left");
        AppendMenuW(rotation_menu_, MF_STRING, RotateRight, L"Rotate &right\tCtrl+Right");
        AppendMenuW(rotation_menu_, MF_STRING, RotateReset, L"&Reset rotation\tCtrl+0");
        AppendMenuW(tools_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(rotation_menu_), L"Preview rotation");
        AppendMenuW(tools_menu_, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(tools_menu_, MF_STRING, Driver, L"Driver setup...");
        AppendMenuW(tools_menu_, MF_STRING, Help, L"Quick guide");
        preview = CreateWindowExW(0, PreviewClass, PreviewTitle, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, scaled(580), scaled(800), nullptr, nullptr, instance_, nullptr);
        if (!preview) return false;
        DwmSetWindowAttribute(preview, 20, &dark, sizeof(dark));
        ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window);
        if (demo_) {
            snapshot_.message = L"Connect your iPhone with a USB cable and unlock it.";
            snapshot_.camera_available = true; snapshot_.camera_supported = true;
            snapshot_.usb_missing = true;
            if (ui_test_state_ == L"streaming" || ui_test_state_ == L"rotation") {
                DeviceInfo device{}; wcscpy_s(device.name, L"iPhone 18 Pro Max"); wcscpy_s(device.udid, L"demo");
                wcscpy_s(device.os_version, L"27.0.1"); snapshot_.devices.push_back(device);
                snapshot_.usb_missing = false; snapshot_.active = true; snapshot_.desired = true;
                snapshot_.capture.state = CaptureState::Streaming; snapshot_.capture.width = 1206;
                snapshot_.capture.height = 2620; snapshot_.capture.fps = 60; snapshot_.capture.audio_sample_rate = 48000;
                snapshot_.video.decoder_runtime_mode = DecoderRuntimeMode::Hardware;
                snapshot_.audio_packets_received = snapshot_.audio_meter_available = true; snapshot_.audio_peak = 0.72F;
                snapshot_.message = L"Mirroring over USB. Your capture window is ready.";
            } else if (ui_test_state_ == L"waiting") {
                // A native session may take time to shut down after unplugging.
                // Exercise that case with deliberately stale stream/audio data.
                snapshot_.desired = snapshot_.active = true; snapshot_.capture.state = CaptureState::Streaming;
                snapshot_.capture.width = 1206; snapshot_.capture.height = 2620; snapshot_.capture.fps = 60;
                snapshot_.audio_packets_received = snapshot_.audio_meter_available = true; snapshot_.audio_peak = 0.85F;
                snapshot_.message = L"Waiting for your iPhone. Reconnect the USB cable and unlock it.";
            }
            apply_snapshot(snapshot_);
            if (!ui_test_output.empty()) {
                // Override a hidden test-process startup hint for PrintWindow.
                ShowWindow(window, SW_SHOW); UpdateWindow(window);
                SetTimer(window, 3, 500, nullptr);
            }
        } else {
            worker_ = std::make_unique<Worker>(window, preview, directory_, preferences_);
            worker_->start();
        }
        return true;
    }
    void show_preview() { preview_visible_ = true; ShowWindow(preview, SW_SHOWNORMAL); SetForegroundWindow(preview); }
    void hide_preview() { preview_visible_ = false; ShowWindow(preview, SW_HIDE); }
    bool demo() const { return demo_; }
    bool active() const { return live_input_; }
    void rotate_preview(int id) {
        if (closing_ || snapshot_.busy) return;
        preferences_.preview_rotation = id == RotateReset ? 0U :
            usbmirror::rotate_quarters(preferences_.preview_rotation, id == RotateLeft ? -1 : 1);
        dispatch(Action::Preferences);
        if (demo_) {
            snapshot_.preview_rotation = preferences_.preview_rotation;
            apply_snapshot(snapshot_);
        }
        if (!preview_visible_) show_preview();
    }
    void preview_menu(int x, int y) {
        if (x == -1 && y == -1) {
            RECT rectangle{}; GetWindowRect(preview, &rectangle);
            x = rectangle.left + 24; y = rectangle.top + 48;
        }
        TrackPopupMenuEx(rotation_menu_, TPM_LEFTALIGN | TPM_TOPALIGN, x, y, preview, nullptr);
    }
    void apply_snapshot(const Snapshot& value) {
        const bool was_active = snapshot_.active;
        const bool was_streaming = live_input_;
        snapshot_ = value;
        live_input_ = value.active && !value.usb_missing && value.capture.state == CaptureState::Streaming;
        std::wstring selected_device = preferences_.device;
        const auto selected_index = selected(Devices);
        if (selected_index < devices_.size()) selected_device = devices_[selected_index].udid;
        auto visible_devices = value.devices;
        if (value.usb_missing) visible_devices.clear();
        else if (value.active && visible_devices.empty()) {
            // QuickTime capture can hide the phone from normal USB discovery.
            // Retain its identity while the physical USB device is still present.
            const auto remembered = std::find_if(devices_.begin(), devices_.end(), [&](const DeviceInfo& item) {
                return selected_device == item.udid;
            });
            if (remembered != devices_.end()) visible_devices.push_back(*remembered);
            else if (!selected_device.empty()) {
                DeviceInfo device{}; wcscpy_s(device.name, L"iPhone · USB capture");
                wcsncpy_s(device.udid, selected_device.c_str(), _TRUNCATE); visible_devices.push_back(device);
            }
        }
        bool changed = !device_list_initialized_ || devices_.size() != visible_devices.size();
        if (!changed) for (std::size_t i = 0; i < devices_.size(); ++i) {
            if (wcscmp(devices_[i].udid, visible_devices[i].udid) || wcscmp(devices_[i].name, visible_devices[i].name)) { changed = true; break; }
        }
        if (changed) {
            device_list_initialized_ = true;
            devices_ = std::move(visible_devices);
            SendMessageW(control(Devices), CB_RESETCONTENT, 0, 0);
            unsigned selection{};
            for (unsigned i = 0; i < devices_.size(); ++i) {
                const auto text = std::wstring(devices_[i].name[0] ? devices_[i].name : L"iPhone") +
                    (devices_[i].os_version[0] ? L"  ·  iOS " + std::wstring(devices_[i].os_version) : L"");
                SendMessageW(control(Devices), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
                if (selected_device == devices_[i].udid) selection = i;
            }
            if (devices_.empty()) SendMessageW(control(Devices), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No USB phone detected"));
            SendMessageW(control(Devices), CB_SETCURSEL, selection, 0);
        }
        const auto status_text = value.usb_missing && !value.fatal && !closing_ ?
            value.active ? L"iPhone unplugged. Waiting for the USB session to close safely..." :
            value.desired ? L"Waiting for your iPhone. Reconnect the USB cable and unlock it." :
            L"Connect your iPhone with a USB cable and unlock it." : value.message.c_str();
        SetWindowTextW(control(StatusText), status_text);
        EnableMenuItem(usb_mode_menu_, UsbStandard, MF_BYCOMMAND | (value.active || value.busy ? MF_GRAYED : MF_ENABLED));
        EnableMenuItem(usb_mode_menu_, UsbCompatibility, MF_BYCOMMAND | (value.active || value.busy ? MF_GRAYED : MF_ENABLED));
        for (const auto id : {RotateLeft, RotateRight, RotateReset})
            EnableMenuItem(rotation_menu_, id, MF_BYCOMMAND | (closing_ || value.busy ? MF_GRAYED : MF_ENABLED));
        const auto audio_level = !live_input_ ? L"Input: waiting for iPhone" :
            !value.audio_packets_received ? L"Input: no recent audio" :
            !value.audio_meter_available ? L"Input: audio received" :
            value.audio_peak == 0.0F ? L"Input: silence · 0%" : nullptr;
        const auto measured_audio = L"Input level · " + std::to_wstring(std::max(1U, unsigned(value.audio_peak * 100))) + L"%";
        SetWindowTextW(control(AudioLevelText), audio_level ? audio_level : measured_audio.c_str());
        const auto connect_text = closing_ ? L"Closing..." : value.busy ? L"Please wait..." :
            value.active ? L"Disconnect" : value.desired ? L"Cancel reconnect" : L"Connect";
        SetWindowTextW(control(Connect), connect_text);
        const auto volume = std::to_wstring(preferences_.volume) + L"%";
        SetWindowTextW(control(VolumeText), volume.c_str());
        EnableWindow(control(Connect), !closing_ && !value.fatal && !value.busy && (value.active || value.desired || (!devices_.empty() && !value.competing)));
        EnableWindow(control(Devices), !value.active && !value.busy && !closing_);
        EnableWindow(control(Refresh), !value.busy && !closing_);
        EnableWindow(control(CameraButton), !closing_ && value.camera_supported && value.camera_registered && live_input_);
        EnableWindow(control(CameraSetup), !closing_ && value.camera_available && value.camera_supported && !value.camera_running && !camera_install_);
        EnableWindow(control(CameraPortrait), !value.camera_running && !closing_);
        SetWindowTextW(control(CameraButton), value.camera_running ? L"Stop camera" : L"Start camera");
        SetWindowTextW(control(CameraSetup), value.camera_registered ? L"Repair camera..." : L"Install camera...");
        if (!live_input_) {
            SetWindowTextW(control(MetricsText), L"");
            last_width_ = last_height_ = 0;
        }
        else {
            const auto& status = value.capture;
            std::wostringstream text;
            text << status.width << L" × " << status.height << L"  ·  Incoming " << std::fixed << std::setprecision(1) << status.fps << L" fps";
            const auto mode = value.video.decoder_runtime_mode;
            text << L"  ·  " << (mode == DecoderRuntimeMode::Hardware ? L"Hardware decode" : mode == DecoderRuntimeMode::Software ? L"Software decode" : L"Decoder starting");
            if (status.audio_sample_rate) text << L"  ·  " << status.audio_sample_rate / 1000 << L" kHz audio";
            SetWindowTextW(control(MetricsText), text.str().c_str());
            fit_preview(status.width, status.height, value.preview_rotation);
            if (!was_streaming && ui_test_output.empty()) show_preview();
        }
        if (value.active && !was_active) persist();
        auto badge = bounds(566, 114, 150, 27); InvalidateRect(window, &badge, FALSE);
        auto meter = bounds(496, 447, 220, 5); InvalidateRect(window, &meter, FALSE);
        InvalidateRect(preview, nullptr, FALSE);
    }
    LRESULT handle(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_PRINTCLIENT: paint(reinterpret_cast<HDC>(wparam)); return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint_info{}; const auto dc = BeginPaint(handle, &paint_info);
            RECT client{}; GetClientRect(handle, &client);
            const auto memory = CreateCompatibleDC(dc); const auto bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            const auto previous = SelectObject(memory, bitmap); paint(memory);
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); EndPaint(handle, &paint_info); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MEASUREITEM: {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (item->CtlType == ODT_COMBOBOX) { item->itemHeight = scaled(30); return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (item->CtlType == ODT_BUTTON) { usbmirror::ui::button(*item); return TRUE; }
            if (item->CtlType == ODT_COMBOBOX) { usbmirror::ui::combo_item(*item); return TRUE; }
            break;
        }
        case WM_DEVICECHANGE:
            if (wparam == 7 && worker_ && !closing_) worker_->send(Action::UsbPresence, preferences_);
            break;
        case UpdateMessage: if (worker_) apply_snapshot(worker_->snapshot()); return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wparam); const int event = HIWORD(wparam);
            if (id == ToolsPopup && event == BN_CLICKED) {
                RECT rectangle{}; GetWindowRect(control(ToolsPopup), &rectangle);
                TrackPopupMenuEx(tools_menu_, TPM_RIGHTALIGN | TPM_TOPALIGN, rectangle.right, rectangle.bottom + scaled(6), window, nullptr);
                return 0;
            }
            if (id == Connect && event == BN_CLICKED) {
                if (demo_) { show_preview(); return 0; }
                const bool stop = snapshot_.active || snapshot_.desired;
                dispatch(stop ? Action::Disconnect : Action::Connect);
                return 0;
            }
            if (id == Refresh && event == BN_CLICKED) { dispatch(Action::Refresh); return 0; }
            if (id == ShowPreview && event == BN_CLICKED) { show_preview(); return 0; }
            if (id == AudioRouting && event == BN_CLICKED) { ShellExecuteW(window, L"open", L"ms-settings:apps-volume", nullptr, nullptr, SW_SHOWNORMAL); return 0; }
            if (id == Driver) { driver_setup(); return 0; }
            if (id == RotateLeft || id == RotateRight || id == RotateReset) { rotate_preview(id); return 0; }
            if (id == UsbStandard || id == UsbCompatibility) {
                if (snapshot_.active || snapshot_.busy) return 0;
                preferences_.usb_mode = id == UsbCompatibility ? 1U : 0U;
                CheckMenuRadioItem(usb_mode_menu_, UsbStandard, UsbCompatibility, id, MF_BYCOMMAND);
                dispatch(Action::Preferences);
                return 0;
            }
            if (id == Help) { ShellExecuteW(window, L"open", (directory_ / L"README.txt").c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0; }
            if (id == CameraButton && event == BN_CLICKED) { dispatch(Action::CameraToggle); return 0; }
            if (id == CameraSetup && event == BN_CLICKED && !demo_) { install_camera(); return 0; }
            if (((id == Fps || id == Resolution || id == Decoder || id == Color || id == Devices) && event == CBN_SELCHANGE) ||
                ((id == Audio || id == Reconnect || id == CameraPortrait) && event == BN_CLICKED)) {
                dispatch(Action::Preferences); return 0;
            }
            break;
        }
        case WM_HSCROLL: if (reinterpret_cast<HWND>(lparam) == control(Volume)) dispatch(Action::Preferences); return 0;
        case WM_CTLCOLORSTATIC: {
            const auto child = reinterpret_cast<HWND>(lparam); auto dc = reinterpret_cast<HDC>(wparam);
            RECT rectangle{}; GetWindowRect(child, &rectangle); MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&rectangle), 2);
            const bool card = rectangle.top >= scaled(100) && rectangle.top < scaled(648);
            const auto role = reinterpret_cast<UINT_PTR>(GetPropW(child, L"USBMirror.TextRole"));
            SetTextColor(dc, role == 1 ? usbmirror::ui::muted : usbmirror::ui::text);
            SetBkColor(dc, card ? usbmirror::ui::surface : usbmirror::ui::background);
            return reinterpret_cast<LRESULT>(card ? surface_brush_ : background_);
        }
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wparam); SetTextColor(dc, usbmirror::ui::text); SetBkColor(dc, usbmirror::ui::field);
            return reinterpret_cast<LRESULT>(field_brush_);
        }
        case WM_TIMER:
            if (wparam == 3 && !ui_test_output.empty()) {
                KillTimer(window, 3);
                RECT client{}; GetClientRect(window, &client);
                unsigned checked{}, outside{};
                unsigned rotation_checks{}, rotation_failures{};
                if (ui_test_state_ == L"rotation") {
                    const auto check_rotation = [&](unsigned expected) {
                        RECT content{}; GetClientRect(preview, &content);
                        ++rotation_checks;
                        if (preferences_.preview_rotation != expected || snapshot_.preview_rotation != expected ||
                            ((content.right > content.bottom) != (expected % 2 != 0))) ++rotation_failures;
                    };
                    SendMessageW(preview, WM_COMMAND, RotateLeft, 0); check_rotation(3);
                    SendMessageW(preview, WM_COMMAND, RotateReset, 0); check_rotation(0);
                    for (unsigned expected = 1; expected <= 4; ++expected) {
                        SendMessageW(preview, WM_COMMAND, RotateRight, 0); check_rotation(expected % 4);
                    }
                    BYTE keyboard[256]{};
                    if (GetKeyboardState(keyboard)) {
                        BYTE control_keyboard[256]; CopyMemory(control_keyboard, keyboard, sizeof(keyboard));
                        control_keyboard[VK_CONTROL] = 0x80;
                        SetKeyboardState(control_keyboard);
                        SendMessageW(preview, WM_KEYDOWN, VK_LEFT, 0); check_rotation(3);
                        SendMessageW(preview, WM_KEYDOWN, VK_RIGHT, 0); check_rotation(0);
                        SendMessageW(preview, WM_KEYDOWN, VK_RIGHT, 0); check_rotation(1);
                        SendMessageW(preview, WM_KEYDOWN, '0', 0); check_rotation(0);
                        SetKeyboardState(keyboard);
                    } else ++rotation_failures;
                    show_preview(); UpdateWindow(preview);
                    auto preview_image = ui_test_output; preview_image.replace_filename(L"rotation-preview.bmp");
                    if (!write_window_bitmap(preview, preview_image)) ++rotation_failures;
                }
                for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                    RECT rectangle{}; GetWindowRect(child, &rectangle);
                    MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&rectangle), 2);
                    ++checked;
                    if (rectangle.left < 0 || rectangle.top < 0 || rectangle.right > client.right || rectangle.bottom > client.bottom) ++outside;
                }
                const bool image_ok = write_window_bitmap(window, ui_test_output);
                auto report_path = ui_test_output; report_path.replace_extension(L"json");
                std::ofstream report(report_path);
                report << "{\"controlsChecked\":" << checked << ",\"outsideClient\":" << outside
                    << ",\"screenshotSaved\":" << (image_ok ? "true" : "false")
                    << ",\"liveInputVisible\":" << (live_input_ ? "true" : "false")
                    << ",\"physicalUsbMissing\":" << (snapshot_.usb_missing ? "true" : "false")
                    << ",\"rotationChecks\":" << rotation_checks << ",\"rotationFailures\":" << rotation_failures
                    << ",\"captureStarted\":false}";
                report.close(); PostMessageW(window, WM_CLOSE, 0, 0); return 0;
            }
            if (wparam == 2 && camera_install_ && WaitForSingleObject(camera_install_, 0) == WAIT_OBJECT_0) {
                DWORD exit_code{}; GetExitCodeProcess(camera_install_, &exit_code);
                CloseHandle(camera_install_); camera_install_ = nullptr; KillTimer(window, 2);
                if (exit_code) MessageBoxW(window, L"Camera setup did not complete. You can continue using window capture.", L"Camera setup", MB_OK | MB_ICONINFORMATION);
                dispatch(Action::CameraRefresh);
            }
            return 0;
        case WM_CLOSE:
            if (closing_) return 0;
            closing_ = true; collect(); persist();
            if (worker_ && !snapshot_.fatal) {
                EnableWindow(control(Connect), FALSE);
                SetWindowTextW(control(StatusText), L"Closing the USB connection safely...");
                worker_->send(Action::Exit, preferences_);
            } else DestroyWindow(window);
            return 0;
        case ExitMessage: DestroyWindow(window); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_DPICHANGED: {
            const auto old_dpi = dpi_;
            const auto new_dpi = static_cast<UINT>(HIWORD(wparam));
            struct Layout { HWND handle; RECT rectangle; bool combo; };
            std::vector<Layout> layout;
            for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                RECT rectangle{}; GetWindowRect(child, &rectangle);
                MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&rectangle), 2);
                wchar_t type[64]{}; GetClassNameW(child, type, 64);
                layout.push_back({child, rectangle, _wcsicmp(type, WC_COMBOBOXW) == 0});
            }
            if (new_dpi && old_dpi) dpi_ = new_dpi;
            const auto* rectangle = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(handle, nullptr, rectangle->left, rectangle->top, rectangle->right - rectangle->left,
                rectangle->bottom - rectangle->top, SWP_NOZORDER | SWP_NOACTIVATE);
            const auto old_font = font_, old_heading = heading_font_, old_small = small_font_, old_section = section_font_;
            make_fonts();
            for (const auto& item : layout) {
                const auto convert = [&](LONG value) { return MulDiv(value, static_cast<int>(dpi_), static_cast<int>(old_dpi)); };
                SetWindowPos(item.handle, nullptr, convert(item.rectangle.left), convert(item.rectangle.top),
                    convert(item.rectangle.right - item.rectangle.left),
                    item.combo ? scaled(220) : convert(item.rectangle.bottom - item.rectangle.top), SWP_NOZORDER | SWP_NOACTIVATE);
                const auto role = reinterpret_cast<UINT_PTR>(GetPropW(item.handle, L"USBMirror.TextRole"));
                const auto font = item.handle == heading_ ? heading_font_ : role == 1 ? small_font_ : role == 2 ? section_font_ : font_;
                SendMessageW(item.handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                if (item.combo) {
                    SendMessageW(item.handle, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scaled(30));
                    SendMessageW(item.handle, CB_SETITEMHEIGHT, 0, scaled(30));
                }
            }
            if (old_font) DeleteObject(old_font); if (old_heading) DeleteObject(old_heading);
            if (old_small) DeleteObject(old_small); if (old_section) DeleteObject(old_section);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        }
        return DefWindowProcW(handle, message, wparam, lparam);
    }
    ~Application() {
        worker_.reset();
        if (preview) DestroyWindow(preview);
        if (camera_install_) CloseHandle(camera_install_);
        if (font_) DeleteObject(font_); if (heading_font_) DeleteObject(heading_font_);
        if (small_font_) DeleteObject(small_font_); if (section_font_) DeleteObject(section_font_);
        if (tools_menu_) DestroyMenu(tools_menu_);
        if (background_) DeleteObject(background_); if (preview_background_) DeleteObject(preview_background_);
        if (surface_brush_) DeleteObject(surface_brush_); if (field_brush_) DeleteObject(field_brush_);
    }
};
LRESULT CALLBACK main_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return application ? application->handle(window, message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK preview_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE && application) { application->hide_preview(); return 0; }
    if (message == WM_CONTEXTMENU && application) {
        application->preview_menu(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)); return 0;
    }
    if (message == WM_COMMAND && application &&
        (LOWORD(wparam) == RotateLeft || LOWORD(wparam) == RotateRight || LOWORD(wparam) == RotateReset)) {
        application->rotate_preview(LOWORD(wparam)); return 0;
    }
    if (message == WM_KEYDOWN && application && GetKeyState(VK_CONTROL) < 0) {
        if (wparam == VK_LEFT) { application->rotate_preview(RotateLeft); return 0; }
        if (wparam == VK_RIGHT) { application->rotate_preview(RotateRight); return 0; }
        if (wparam == '0' || wparam == VK_NUMPAD0) { application->rotate_preview(RotateReset); return 0; }
    }
    if (message == WM_GETMINMAXINFO) {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam); info->ptMinTrackSize = {240, 180}; return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; auto dc = BeginPaint(window, &paint);
        RECT rect{}; GetClientRect(window, &rect);
        if (application && (application->demo() || !application->active())) {
            FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetTextColor(dc, RGB(210, 221, 238)); SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, L"USB Mirror Preview\n\nYour phone image appears here.\nCapture this window in your streaming app.", -1, &rect,
                DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        }
        EndPaint(window, &paint); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
int self_test(const fs::path& output, bool probe) {
    unsigned assertions{}, failures{};
    auto test = [&](bool condition) { ++assertions; if (!condition) ++failures; };
    test(usbmirror::fit(1920, 1080, 1280, 720).width == 1280);
    test(usbmirror::fit(1080, 1920, 1280, 720).height == 720);
    test(usbmirror::fit(0, 1920, 1280, 720).width == 0);
    test(usbmirror::fit(480, 640, 1280, 720).width == 480);
    test(usbmirror::rotate_quarters(0, -1) == 3 && usbmirror::rotate_quarters(3, 1) == 0);
    test(usbmirror::rotate_quarters(1, -1) == 0 && usbmirror::rotate_quarters(0, 4) == 0);
    test(usbmirror::rotated_size(1206, 2620, 1).width == 2620);
    test(usbmirror::rotated_size(1206, 2620, 3).height == 1206);
    test(usbmirror::rotated_size(1206, 2620, 2).height == 2620);
    {
        auto file = output; file.replace_extension(L"rotation-settings.ini");
        ::Preferences original; original.preview_rotation = 3; original.volume = 42;
        const auto saved = save_preferences(file, original);
        const auto restored = read_preferences(file);
        test(saved && restored.preview_rotation == 3 && restored.volume == 42);
        WritePrivateProfileStringW(L"USBMirror", L"PreviewRotation", nullptr, file.c_str());
        test(read_preferences(file).preview_rotation == 0);
        std::error_code error; fs::remove(file, error);
    }
    test(usbmirror::retry_seconds(0) == 2 && usbmirror::retry_seconds(100) == 32);
    test(usbmirror::valid_fps(120) && usbmirror::valid_fps(0) && !usbmirror::valid_fps(100000));
    test(!usbmirror::should_auto_connect(true, true, false, true, true, true));
    test(!usbmirror::should_auto_connect(true, false, false, false, true, true));
    test(!usbmirror::should_auto_connect(true, true, true, false, true, true));
    test(!usbmirror::should_auto_connect(true, true, false, false, false, true));
    test(!usbmirror::should_auto_connect(true, true, false, false, true, false));
    test(usbmirror::should_auto_connect(true, true, false, false, true, true));
    const std::uint8_t silence[]{0, 0, 0, 0};
    const std::uint8_t negative_full_scale[]{0, 128};
    const std::uint8_t positive_full_scale[]{255, 127};
    test(usbmirror::pcm16_peak(silence, sizeof(silence)) == 0.0F);
    test(usbmirror::pcm16_peak(negative_full_scale, sizeof(negative_full_scale)) == 1.0F);
    test(usbmirror::pcm16_peak(positive_full_scale, sizeof(positive_full_scale)) > 0.999F);
    test(usbmirror::pcm16_peak(positive_full_scale, 1) == 0.0F);
    test(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_USB_MIRROR), IMAGE_ICON, 16, 16, LR_SHARED) != nullptr);
    test(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_USB_MIRROR), IMAGE_ICON, 256, 256, LR_SHARED) != nullptr);
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    unsigned api{}, device_count{}; int environment_result{-999}, wireless_running{}, camera_supported{}, camera_registered{};
    std::string error;
    try {
        Core core; core.load(executable_directory()); api = core.api_version();
        test(api == ApiVersion);
        // Only read environment and usbmux metadata; never open capture or change USB configuration.
        EnvironmentInfo environment{}; environment.struct_size = sizeof(environment);
        environment_result = core.get_environment(&environment); test(environment_result == 0);
        using WirelessStatus = decltype(&im_wireless_receiver_get_status);
        const auto wireless_status = load_function<WirelessStatus>(core.module, "im_wireless_receiver_get_status");
        std::int32_t ready{}; test(wireless_status(&wireless_running, &ready) == 0 && !wireless_running);
        // Exercise the real D3D11 preview creation/cleanup without a capture session.
        const auto preview = CreateWindowExW(0, L"STATIC", L"USB Mirror renderer test", WS_POPUP,
            0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        test(preview != nullptr);
        if (preview) {
            const auto attach = load_function<decltype(&im_attach_preview_window)>(core.module, "im_attach_preview_window");
            const auto detach = load_function<decltype(&im_detach_preview_window)>(core.module, "im_detach_preview_window");
            test(attach(preview) == 0);
            detach(); DestroyWindow(preview);
        }
        if (probe) {
            std::vector<DeviceInfo> devices(32); device_count = static_cast<unsigned>(devices.size());
            const auto result = core.refresh_devices_ex(devices.data(), &device_count, 0);
            test(result == 0);
        }
        Camera camera;
        test(camera.load(executable_directory()));
        if (camera.module) {
            virtual_camera::Status status{}; status.struct_size = sizeof(status);
            test(camera.get_status(&status) >= 0);
            camera_supported = status.supported; camera_registered = status.registered;
        }
    } catch (const std::exception& exception) { ++failures; error = exception.what(); }
    if (SUCCEEDED(com)) CoUninitialize();
    std::ofstream report(output);
    report << "{\n  \"passed\": " << (failures == 0 ? "true" : "false") << ",\n  \"assertions\": " << assertions
        << ",\n  \"failures\": " << failures << ",\n  \"coreApi\": " << api << ",\n  \"environmentResult\": " << environment_result
        << ",\n  \"wirelessReceiverRunning\": " << wireless_running << ",\n  \"deviceCount\": " << device_count
        << ",\n  \"cameraSupported\": " << camera_supported << ",\n  \"cameraRegistered\": " << camera_registered
        << ",\n  \"captureStarted\": false,\n  \"error\": \"" << error << "\"\n}\n";
    return failures ? 1 : 0;
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    configure_capture_log();
    int count{}; auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool demo{};
    try {
        if (count > 1 && (_wcsicmp(arguments[1], L"--self-test") == 0 || _wcsicmp(arguments[1], L"--probe") == 0)) {
            const auto output = count > 2 ? fs::path(arguments[2]) : executable_directory() / L"self-test.json";
            const auto result = self_test(output, _wcsicmp(arguments[1], L"--probe") == 0);
            LocalFree(arguments); return result;
        }
        const bool ui_test = count > 2 && _wcsicmp(arguments[1], L"--ui-test") == 0;
        const auto ui_output = ui_test ? fs::path(arguments[2]) : fs::path{};
        const auto ui_state = ui_test && count > 3 ? std::wstring(arguments[3]) : std::wstring{};
        demo = ui_test || (count > 1 && _wcsicmp(arguments[1], L"--demo") == 0);
        LocalFree(arguments); arguments = nullptr;
        const auto mutex = CreateMutexW(nullptr, FALSE, demo ? L"Local\\USBMirror.Demo.v1" : L"Local\\USBMirror.Native.v1");
        if (!mutex) throw std::runtime_error("Could not create the application lock.");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (auto existing = FindWindowW(MainClass, nullptr)) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
            CloseHandle(mutex); return 0;
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        {
            Application app(instance, demo, ui_output, ui_state); application = &app;
            if (!app.create()) throw std::runtime_error("Could not create the application window.");
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (!IsDialogMessageW(app.window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            application = nullptr;
        }
        CoUninitialize(); CloseHandle(mutex); return 0;
    } catch (const std::exception& error) {
        if (arguments) LocalFree(arguments);
        const std::string text = error.what(); const std::wstring message(text.begin(), text.end());
        MessageBoxW(nullptr, message.c_str(), L"USB Mirror", MB_OK | MB_ICONERROR); return 1;
    }
}
