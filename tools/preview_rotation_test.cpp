// SPDX-License-Identifier: GPL-3.0-only
#include <windows.h>
#include <objbase.h>
#include <dwmapi.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "Renderer/D3D11PreviewRenderer.h"
#include "Logging.h"

namespace fs = std::filesystem;
using namespace iPhoneMirror;

struct TestWindow {
    HWND handle{};
    ~TestWindow() { if (handle) DestroyWindow(handle); }
};

std::array<int, 4> sample_quadrants(HWND window) {
    RECT client{}; GetClientRect(window, &client);
    POINT origin{}; ClientToScreen(window, &origin);
    auto screen = GetDC(nullptr);
    std::array<int, 4> samples{};
    for (unsigned i = 0; i < samples.size(); ++i) {
        const int x = origin.x + client.right * ((i % 2) ? 3 : 1) / 4;
        const int y = origin.y + client.bottom * ((i / 2) ? 3 : 1) / 4;
        const auto pixel = GetPixel(screen, x, y);
        samples[i] = pixel == CLR_INVALID ? -1 :
            (GetRValue(pixel) + GetGValue(pixel) + GetBValue(pixel)) / 3;
    }
    ReleaseDC(nullptr, screen);
    return samples;
}

bool matches_rotation(const std::array<int, 4>& samples, unsigned turns) {
    constexpr unsigned expected[4][4]{{0, 1, 2, 3}, {2, 0, 3, 1}, {3, 2, 1, 0}, {1, 3, 0, 2}};
    for (unsigned i = 0; i < samples.size(); ++i) {
        if (samples[i] < 0) return false;
        for (unsigned j = 0; j < samples.size(); ++j)
            if (expected[turns][i] < expected[turns][j] && samples[i] + 12 >= samples[j]) return false;
    }
    return true;
}

bool save_client(HWND window, const fs::path& path) {
    RECT client{}; GetClientRect(window, &client);
    POINT origin{}; ClientToScreen(window, &origin);
    auto screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    auto bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    auto old = SelectObject(memory, bitmap);
    const auto copied = BitBlt(memory, 0, 0, client.right, client.bottom, screen,
        origin.x, origin.y, SRCCOPY | CAPTUREBLT);
    SelectObject(memory, old);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = client.right; info.bmiHeader.biHeight = -client.bottom;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(client.right) * client.bottom * 4);
    const auto rows = GetDIBits(memory, bitmap, 0, client.bottom, pixels.data(), &info, DIB_RGB_COLORS);
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    const bool ok = copied && rows == client.bottom && output.good();
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
    return ok;
}

int main(int argc, char** argv) {
    const fs::path directory = argc > 1 ? fs::path(argv[1]) : fs::path("rotation-fixtures");
    unsigned failures{};
    const auto previous_window = GetForegroundWindow();
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        fs::create_directories(directory);
        const auto log = directory / L"renderer.log";
        SetEnvironmentVariableW(L"IPHONE_MIRROR_LOG_FILE", log.c_str());
        logging::initialize();
        auto frame = std::make_shared<media::DecodedFrame>();
        frame->width = 160; frame->height = 240; frame->stride = 160;
        frame->timestamp_100ns = 1; frame->received_at = std::chrono::steady_clock::now();
        frame->color.primaries = coremedia::ColorPrimaries::Bt709;
        frame->color.transfer = coremedia::TransferFunction::Bt709;
        frame->color.matrix = coremedia::MatrixCoefficients::Bt709;
        frame->color.range = coremedia::ColorRange::Full;
        frame->nv12.assign(160 * 240 * 3 / 2, 128);
        for (unsigned y = 0; y < frame->height; ++y)
            for (unsigned x = 0; x < frame->width; ++x)
                frame->nv12[y * frame->stride + x] = static_cast<std::uint8_t>(
                    32 + 64 * ((y >= 120 ? 2 : 0) + (x >= 80 ? 1 : 0)));
        TestWindow window;
        window.handle = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"USB Mirror rotation test",
            WS_OVERLAPPEDWINDOW, 40, 40, 360, 540, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window.handle) throw std::runtime_error("Cannot create the renderer test window.");
        renderer::D3D11PreviewRenderer renderer(window.handle, [frame] { return frame; });
        renderer.set_corner_profile(0.0F, 2.36F);
        renderer.set_max_fps(60);
        for (unsigned turns = 0; turns < 4; ++turns) {
            RECT rectangle{0, 0, turns % 2 ? 480L : 320L, turns % 2 ? 320L : 480L};
            AdjustWindowRectEx(&rectangle, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOPMOST);
            SetWindowPos(window.handle, HWND_TOPMOST, 40, 40, rectangle.right - rectangle.left,
                rectangle.bottom - rectangle.top, SWP_SHOWWINDOW);
            renderer.set_rotation(static_cast<int>(turns));
            std::array<int, 4> samples{};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            do {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                DwmFlush();
                samples = sample_quadrants(window.handle);
                if (matches_rotation(samples, turns)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            } while (std::chrono::steady_clock::now() < deadline);
            const bool passed = matches_rotation(samples, turns) &&
                save_client(window.handle, directory / ("rotation-" + std::to_string(turns * 90) + ".bmp"));
            if (!passed) ++failures;
            std::cout << "rotation=" << turns * 90 << " passed=" << passed << " quadrant_levels=";
            for (const auto value : samples) std::cout << value << ' ';
            std::cout << '\n';
        }
    } catch (const std::exception& error) {
        ++failures; std::cerr << error.what() << '\n';
    }
    logging::shutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    if (previous_window && IsWindow(previous_window)) SetForegroundWindow(previous_window);
    std::cout << "Rendered rotation failures: " << failures << '\n';
    return failures ? 1 : 0;
}
