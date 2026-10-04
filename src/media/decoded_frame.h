#pragma once

#include <cstdint>
#include <vector>

namespace cloud_stream::media {

struct DecodedFrame {
    std::uint64_t frame_id{0};
    std::int64_t pts{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0};
    std::vector<std::uint8_t> rgb24;
};

} // namespace cloud_stream::media
