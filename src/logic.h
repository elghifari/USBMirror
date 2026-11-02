// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace usbmirror {
inline float pcm16_peak(const std::uint8_t* bytes, std::size_t count) noexcept {
    unsigned peak{};
    for (std::size_t index = 0; index + 1 < count; index += 2) {
        const unsigned word = unsigned(bytes[index]) | (unsigned(bytes[index + 1]) << 8U);
        const int sample = word & 0x8000U ? int(word) - 65536 : int(word);
        peak = std::max(peak, sample < 0 ? unsigned(-sample) : unsigned(sample));
    }
    return peak / 32768.0F;
}
inline unsigned retry_seconds(unsigned failures) noexcept {
    return 2U << std::min(failures, 4U);
}
inline bool valid_fps(unsigned fps) noexcept {
    return fps == 0 || fps == 30 || fps == 60 || fps == 120;
}
struct Size { unsigned width; unsigned height; };
inline unsigned rotate_quarters(unsigned rotation, int delta) noexcept {
    return static_cast<unsigned>((static_cast<int>(rotation % 4) + delta % 4 + 4) % 4);
}
inline Size rotated_size(unsigned width, unsigned height, unsigned rotation) noexcept {
    return rotation % 2 ? Size{height, width} : Size{width, height};
}
inline Size fit(unsigned width, unsigned height, unsigned max_width, unsigned max_height) noexcept {
    if (!width || !height || !max_width || !max_height) return {0, 0};
    const double scale = std::min(1.0, std::min(double(max_width) / width, double(max_height) / height));
    return {std::max(1U, unsigned(width * scale)), std::max(1U, unsigned(height * scale))};
}
inline bool should_auto_connect(bool enabled, bool desired, bool have_session,
    bool competing_app, bool remembered_device_present, bool backoff_elapsed) noexcept {
    return enabled && desired && !have_session && !competing_app &&
        remembered_device_present && backoff_elapsed;
}
}
