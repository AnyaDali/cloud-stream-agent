#pragma once

#include "media/encoded_frame.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace cloud_stream::pipeline {

class FrameReassembler {
public:
    FrameReassembler(std::uint64_t session_id, std::uint16_t maximum_datagram_size,
                     std::size_t maximum_incomplete_frames = 3);

    [[nodiscard]] std::optional<media::EncodedFrame>
    consume(std::span<const std::uint8_t> datagram);
    [[nodiscard]] std::uint64_t invalid_datagrams() const noexcept;
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept;

private:
    struct PartialFrame {
        std::int64_t pts{0};
        bool keyframe{false};
        std::uint16_t fragment_count{0};
        std::uint16_t received_count{0};
        std::vector<bool> received;
        std::vector<std::uint8_t> data;
        std::chrono::steady_clock::time_point updated_at;
    };

    void discard_expired();
    void make_room();

    std::uint64_t session_id_{0};
    std::uint16_t maximum_datagram_size_{0};
    std::size_t payload_capacity_{0};
    std::size_t maximum_incomplete_frames_{0};
    std::unordered_map<std::uint64_t, PartialFrame> frames_;
    std::uint64_t invalid_datagrams_{0};
    std::uint64_t dropped_frames_{0};
};

} // namespace cloud_stream::pipeline
