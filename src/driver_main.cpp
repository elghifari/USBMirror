// SPDX-License-Identifier: GPL-3.0-only
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "ui_theme.h"
#include "hardware_lease.h"
#include "driver_backend.h"

namespace driver = usbmirror::driver;
namespace ui = usbmirror::ui;
namespace fs = std::filesystem;
constexpr wchar_t DriverClass[] = L"USBMirror.DriverSetup.v1";
bool original_mirroring_running() {
    driver::Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot.value, &entry)) do {
        if (_wcsicmp(entry.szExeFile, L"iPhoneMirror.exe") == 0) return true;
    } while (Process32NextW(snapshot.value, &entry));
    return false;
}
fs::path own_file() { wchar_t path[32768]{}; driver::require(GetModuleFileNameW(nullptr, path, 32768) != 0, "Cannot locate driver setup."); return path; }
int driver_test(const fs::path& path) {
    unsigned assertions{}, failures{}, count{}; std::string error;
    auto test = [&](bool condition) { ++assertions; if (!condition) ++failures; };
    test(driver::allowed_id(L"USB\\VID_05AC&PID_12A8\\000081500012345678901234"));
    test(driver::allowed_id(L"usb\\vid_05ac&pid_12ab\\00008150-0012345678901234"));
    for (const auto bad : {L"USB\\VID_1234&PID_12A8\\000081500012345678901234", L"USB\\VID_05AC&PID_12A8&MI_00\\000081500012345678901234",
        L"USB\\VID_05AC&PID_0001\\000081500012345678901234", L"USB\\VID_05AC&PID_12A8\\short", L"USB\\VID_05AC&PID_12A8\\ABC12345\" /all",
        L"USB\\VID_05AC&PID_12A8\\ABC12345\\extra", L"USB\\VID_05AC&PID_12A8\\-ABC12345", L"USB\\VID_05AC&PID_12A8\\.......ABC"}) test(!driver::allowed_id(bad));
    const BYTE abc[]{'a','b','c'};
    test(driver::hash_bytes(abc, sizeof(abc)) == L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    test(driver::filter_change_allowed({L"OtherA",L"OtherB"},{L"libusb0",L"OtherA",L"OtherB"},true));
    test(driver::filter_change_allowed({L"OtherA",L"libusb0",L"OtherB"},{L"OtherA",L"OtherB"},false));
    test(!driver::filter_change_allowed({L"OtherA",L"OtherB"},{L"libusb0",L"OtherB",L"OtherA"},true));
    test(!driver::filter_change_allowed({L"OtherA"},{L"libusb0"},true));
    test(!driver::filter_change_allowed({L"OtherA"},{L"libusb0",L"OtherA",L"Unexpected"},true));
    test(!driver::filter_change_allowed({},{L"libusb0",L"libusb0"},true));
    test(!driver::filter_change_allowed({L"libusb0"},{L"libusb0"},false));
    bool malformed_rejected{};
    try { driver::Value invalid{true,REG_DWORD,{1,2,3,4}}; driver::filters(invalid); } catch (...) { malformed_rejected = true; }
    test(malformed_rejected);
    try {
        driver::verify_resources(); test(true);
        const auto icon = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_USB_MIRROR), IMAGE_ICON, 32, 32, LR_SHARED); test(icon != nullptr);
        count = static_cast<unsigned>(driver::devices().size()); test(true);
        const auto name = L"Local\\USBMirror.Driver.Test." + std::to_wstring(GetCurrentProcessId());
        auto first = usbmirror::HardwareLease::acquire(name.c_str()); test(first != nullptr);
        bool second_acquired{}; std::thread other([&] { second_acquired = usbmirror::HardwareLease::acquire(name.c_str()) != nullptr; }); other.join(); test(!second_acquired);
        first.reset(); auto again = usbmirror::HardwareLease::acquire(name.c_str()); test(again != nullptr);
    } catch (const std::exception& e) { ++failures; error = e.what(); }
    std::ofstream output(path); output << "{\"assertions\":" << assertions << ",\"failures\":" << failures << ",\"passed\":" << (!failures ? "true" : "false")
        << ",\"knownDeviceCount\":" << count << ",\"driverChangesPerformed\":false,\"error\":\"" << error << "\"}";
    return failures ? 1 : 0;
}
class DriverWindow {
    HINSTANCE instance_; HWND window_{}, combo_{}, status_{}, details_{}, refresh_{}, install_{}, remove_{};
    HFONT font_{}, title_{}; HBRUSH background_{}, field_{}; HWND heading_{}; UINT dpi_{96};
    HANDLE operation_{}; bool payload_ok_{}; std::vector<driver::Device> devices_;
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    void make_fonts() {
        font_ = CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        title_ = CreateFontW(-px(22),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    }
    HWND add(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id = 0) {
        auto child = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(width), px(height), window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (equal_class(type, WC_BUTTONW)) ui::style(child);
        return child;
    }
    static bool equal_class(const wchar_t* a, const wchar_t* b) { return _wcsicmp(a, b) == 0; }
    const driver::Device* selected() const { const auto i = SendMessageW(combo_, CB_GETCURSEL, 0, 0); return i >= 0 && static_cast<std::size_t>(i) < devices_.size() ? &devices_[i] : nullptr; }
    void update() {
        const auto device = selected();
        if (device) {
            const auto detail = std::wstring(device->present ? L"Connected" : L"Not connected") + L"  |  " +
                (device->installed ? L"Capture filter installed" : L"Capture filter missing") + L"\r\nParent driver: " + device->service;
            SetWindowTextW(details_, detail.c_str());
        } else SetWindowTextW(details_, L"No iPhone or iPad found. Connect and unlock your phone, then refresh.");
        const bool ready = device && device->present && device->healthy && driver::equal(device->service, L"usbccgp") && payload_ok_ && !operation_;
        EnableWindow(install_, ready); EnableWindow(remove_, ready && device->installed);
        EnableWindow(combo_, !operation_ && !devices_.empty()); EnableWindow(refresh_, !operation_);
    }
    void refresh() {
        try {
            const auto previous = selected() ? selected()->id : std::wstring{};
            devices_ = driver::devices(); SendMessageW(combo_, CB_RESETCONTENT, 0, 0); unsigned selection{};
            for (unsigned i = 0; i < devices_.size(); ++i) {
                const auto& device = devices_[i]; const auto suffix = device.id.substr(device.id.size() - std::min<std::size_t>(4, device.id.size()));
                const auto label = device.name + L"  (...)" + suffix + (device.present ? L"" : L"  - disconnected");
                SendMessageW(combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())); if (driver::equal(previous, device.id)) selection = i;
            }
            if (devices_.empty()) SendMessageW(combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No phone detected"));
            SendMessageW(combo_, CB_SETCURSEL, selection, 0);
        } catch (const std::exception& e) { const std::string error = e.what(); const std::wstring message(error.begin(), error.end()); SetWindowTextW(status_, message.c_str()); }
        update();
    }
    void operate(bool install) {
        const auto device = selected(); if (!device || operation_ || !payload_ok_) return;
        const auto text = install ? L"Install or repair the capture filter for this iPhone?\n\nDisconnect mirroring first. Other devices using libusb0 may reconnect. Reconnect the iPhone's USB cable afterwards." :
            L"Remove the capture filter from this iPhone?\n\nDisconnect mirroring first. USB Mirror will need the filter installed again before mirroring.";
        if (MessageBoxW(window_, text, L"USB Mirror driver setup", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return;
        const auto arguments = std::wstring(L"--apply ") + (install ? L"install" : L"remove") + L" \"" + device->id + L"\"";
        const auto executable = own_file(); SHELLEXECUTEINFOW request{sizeof(request)};
        request.fMask = SEE_MASK_NOCLOSEPROCESS; request.hwnd = window_; request.lpVerb = L"runas";
        request.lpFile = executable.c_str(); request.lpParameters = arguments.c_str(); request.nShow = SW_HIDE;
        if (!ShellExecuteExW(&request)) { SetWindowTextW(status_, GetLastError() == ERROR_CANCELLED ? L"Administrator approval was cancelled." : L"Could not start the driver operation."); return; }
        operation_ = request.hProcess; SetTimer(window_, 1, 500, nullptr); SetWindowTextW(status_, L"Updating the selected iPhone's capture filter..."); update();
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<DriverWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<DriverWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        return self ? self->handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_DPICHANGED: {
            const auto previous_dpi = dpi_; dpi_ = HIWORD(wparam);
            const auto* rectangle = reinterpret_cast<RECT*>(lparam);
            const auto old_font = font_, old_title = title_; make_fonts();
            for (auto child = GetWindow(window_,GW_CHILD); child; child = GetWindow(child,GW_HWNDNEXT)) {
                RECT location{}; GetWindowRect(child,&location); MapWindowPoints(HWND_DESKTOP,window_,reinterpret_cast<POINT*>(&location),2);
                wchar_t type[64]{}; GetClassNameW(child,type,64); const bool is_combo = equal_class(type,WC_COMBOBOXW);
                const auto scale = [&](LONG value) { return MulDiv(value,static_cast<int>(dpi_),static_cast<int>(previous_dpi)); };
                SetWindowPos(child,nullptr,scale(location.left),scale(location.top),scale(location.right-location.left),is_combo ? px(220) : scale(location.bottom-location.top),SWP_NOZORDER | SWP_NOACTIVATE);
                SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(child == heading_ ? title_ : font_),TRUE);
                if (is_combo) { SendMessageW(child,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),px(30)); SendMessageW(child,CB_SETITEMHEIGHT,0,px(30)); }
            }
            SetWindowPos(window_,nullptr,rectangle->left,rectangle->top,rectangle->right-rectangle->left,rectangle->bottom-rectangle->top,SWP_NOZORDER | SWP_NOACTIVATE);
            if (old_font) DeleteObject(old_font); if (old_title) DeleteObject(old_title); InvalidateRect(window_,nullptr,TRUE); return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == 1 && HIWORD(wparam) == CBN_SELCHANGE) { update(); return 0; }
            if (HIWORD(wparam) == BN_CLICKED) switch (LOWORD(wparam)) {
            case 2: refresh(); return 0;
            case 3: operate(true); return 0;
            case 4: operate(false); return 0;
            case 5: ShellExecuteW(window_, L"open", L"ms-windows-store://pdp/?ProductId=9NP83LWLPZ9K", nullptr, nullptr, SW_SHOWNORMAL); return 0;
            }
            break;
        case WM_MEASUREITEM: { auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam); if (item->CtlType == ODT_COMBOBOX) { item->itemHeight = px(30); return TRUE; } break; }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (item->CtlType == ODT_BUTTON) { ui::button(*item); return TRUE; }
            if (item->CtlType == ODT_COMBOBOX) { ui::combo_item(*item); return TRUE; } break;
        }
        case WM_CTLCOLORSTATIC: { auto dc = reinterpret_cast<HDC>(wparam); SetTextColor(dc, ui::text); SetBkColor(dc, ui::background); return reinterpret_cast<LRESULT>(background_); }
        case WM_CTLCOLORLISTBOX: { auto dc = reinterpret_cast<HDC>(wparam); SetTextColor(dc, ui::text); SetBkColor(dc, ui::field); return reinterpret_cast<LRESULT>(field_); }
        case WM_TIMER:
            if (operation_ && WaitForSingleObject(operation_, 0) == WAIT_OBJECT_0) {
                DWORD result{}; GetExitCodeProcess(operation_, &result); CloseHandle(operation_); operation_ = nullptr; KillTimer(window_, 1);
                refresh(); SetWindowTextW(status_, result == 0 ? L"Filter updated. Reconnect the phone." : L"Operation did not complete. Review the reported error before retrying.");
            } return 0;
        case WM_CLOSE:
            if (operation_) { MessageBoxW(window_, L"Wait for the driver operation to finish.", L"USB Mirror driver setup", MB_OK | MB_ICONINFORMATION); return 0; }
            DestroyWindow(window_); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }
public:
    explicit DriverWindow(HINSTANCE instance) : instance_(instance) {}
    int run() {
        background_ = CreateSolidBrush(ui::background); field_ = CreateSolidBrush(ui::field);
        WNDCLASSEXW type{sizeof(type)}; type.hInstance = instance_; type.lpfnWndProc = procedure; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_USB_MIRROR));
        type.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_USB_MIRROR), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        type.hbrBackground = background_; type.lpszClassName = DriverClass; RegisterClassExW(&type);
        window_ = CreateWindowExW(0, DriverClass, L"USB Mirror driver setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, 640, 430, nullptr, nullptr, instance_, this);
        driver::require(window_ != nullptr, "Cannot create driver setup.");
        dpi_ = GetDpiForWindow(window_);
        RECT size{0,0,px(620),px(392)}; AdjustWindowRectExForDpi(&size, GetWindowLongW(window_, GWL_STYLE), FALSE, 0, GetDpiForWindow(window_));
        SetWindowPos(window_, nullptr, 0, 0, size.right-size.left, size.bottom-size.top, SWP_NOMOVE | SWP_NOZORDER);
        make_fonts();
        heading_ = add(WC_STATICW,L"Driver setup",0,24,20,572,34); SendMessageW(heading_,WM_SETFONT,reinterpret_cast<WPARAM>(title_),TRUE);
        add(WC_STATICW,L"iPhone / iPad",0,24,74,572,22);
        combo_ = add(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,24,100,466,220,1);
        SendMessageW(combo_,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),px(30)); SendMessageW(combo_,CB_SETITEMHEIGHT,0,px(30)); ui::style(combo_,2);
        refresh_ = add(WC_BUTTONW,L"Refresh",BS_OWNERDRAW | WS_TABSTOP,502,100,94,36,2);
        details_ = add(WC_STATICW,L"",0,24,152,572,50);
        install_ = add(WC_BUTTONW,L"Install / repair filter",BS_OWNERDRAW | WS_TABSTOP,24,218,272,36,3);
        remove_ = add(WC_BUTTONW,L"Remove filter",BS_OWNERDRAW | WS_TABSTOP,308,218,288,36,4);
        add(WC_BUTTONW,L"Get Apple Devices",BS_OWNERDRAW | WS_TABSTOP,24,274,200,34,5);
        status_ = add(WC_STATICW,L"",0,24,324,572,52);
        BOOL dark = TRUE; DwmSetWindowAttribute(window_,20,&dark,sizeof(dark));
        try { driver::verify_resources(); payload_ok_ = true; SetWindowTextW(status_,L"Driver files have been verified.\r\nWindows will ask for administrator approval when you install or remove the driver."); }
        catch (const std::exception& e) { const std::string error=e.what(); const std::wstring value(error.begin(),error.end()); SetWindowTextW(status_,value.c_str()); }
        refresh(); ShowWindow(window_,SW_SHOWNORMAL); UpdateWindow(window_);
        MSG message{}; while (GetMessageW(&message,nullptr,0,0)>0) if (!IsDialogMessageW(window_,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        return 0;
    }
    ~DriverWindow() { if (font_) DeleteObject(font_); if (title_) DeleteObject(title_); if (background_) DeleteObject(background_); if (field_) DeleteObject(field_); if (operation_) CloseHandle(operation_); }
};
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32); CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int count{}; auto** arguments = CommandLineToArgvW(GetCommandLineW(),&count); int result{};
    try {
        if (count == 3 && _wcsicmp(arguments[1],L"--self-test")==0) result = driver_test(arguments[2]);
        else if (count == 4 && _wcsicmp(arguments[1],L"--apply")==0 && (wcscmp(arguments[2],L"install")==0 || wcscmp(arguments[2],L"remove")==0)) {
            driver::require(driver::administrator(),"Driver changes require Windows administrator approval.");
            driver::require(!original_mirroring_running(),"Close the other mirroring app before changing drivers.");
            auto exclusive = usbmirror::HardwareLease::acquire(); driver::require(exclusive != nullptr,"USB mirroring is active. Disconnect it before changing drivers.");
            result = driver::apply(arguments[3],wcscmp(arguments[2],L"install")==0);
        } else if (count == 1) {
            INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls); DriverWindow window(instance); result=window.run();
        } else throw std::runtime_error("Invalid driver setup arguments.");
    } catch (const std::exception& e) { const std::string error=e.what(); const std::wstring message(error.begin(),error.end()); MessageBoxW(nullptr,message.c_str(),L"USB Mirror driver setup",MB_OK | MB_ICONERROR); result=1; }
    LocalFree(arguments); CoUninitialize(); return result;
}
