// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace usbmirror {
class HardwareLease {
    HANDLE mutex_{}; bool owned_{};
    explicit HardwareLease(HANDLE mutex, bool owned) : mutex_(mutex), owned_(owned) {}
public:
    HardwareLease(const HardwareLease&) = delete;
    static std::unique_ptr<HardwareLease> acquire(const wchar_t* name = L"Global\\USBMirror.UsbExclusive") {
        auto mutex = CreateMutexW(nullptr, FALSE, name);
        if (!mutex) return {};
        const auto wait = WaitForSingleObject(mutex, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(mutex); return {}; }
        return std::unique_ptr<HardwareLease>(new HardwareLease(mutex, true));
    }
    ~HardwareLease() { if (owned_) ReleaseMutex(mutex_); if (mutex_) CloseHandle(mutex_); }
};
}
