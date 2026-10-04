#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace cloud_stream::media {

struct EncodedFrame {
    std::uint64_t frame_id{0};
    std::int64_t pts{0};
    bool keyframe{false};
    std::uint32_t config_revision{0};
    std::shared_ptr<const std::vector<std::uint8_t>> payload;
};

} // namespace cloud_stream::media
