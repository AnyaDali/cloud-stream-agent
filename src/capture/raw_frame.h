#pragma once

#include <cstdint>
#include <vector>

namespace cloud_stream::capture {

enum class PixelFormat : std::uint8_t {
    rgb24 = 1,
};

struct RawFrame {
    std::uint64_t frame_id{0};
    std::int64_t capture_timestamp_ms{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0};
    PixelFormat pixel_format{PixelFormat::rgb24};
    std::vector<std::uint8_t> pixels;
};

} // namespace cloud_stream::capture
