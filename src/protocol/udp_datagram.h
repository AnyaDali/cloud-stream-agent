#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cloud_stream::protocol {

inline constexpr std::size_t kUdpProbeSize = 32;
inline constexpr std::size_t kVideoDatagramHeaderSize = 56;
inline constexpr std::uint16_t kDefaultMaximumDatagramSize = 1200;
inline constexpr std::uint32_t kInitialKeyEpoch = 0;

enum VideoDatagramFlags : std::uint16_t {
    video_datagram_keyframe = 1U << 0U,
};

struct UdpProbe {
    std::uint64_t session_id{0};
    std::array<std::uint8_t, 16> token{};
};

struct VideoDatagramHeader {
    std::uint16_t flags{0};
    std::uint64_t session_id{0};
    std::uint32_t key_epoch{kInitialKeyEpoch};
    std::uint16_t fragment_index{0};
    std::uint16_t fragment_count{0};
    std::uint64_t packet_number{0};
    std::uint64_t frame_id{0};
    std::int64_t pts{0};
    std::uint32_t frame_size{0};
    std::uint16_t payload_size{0};
};

struct DecodedVideoDatagram {
    VideoDatagramHeader header;
    std::span<const std::uint8_t> payload;
};

[[nodiscard]] std::array<std::uint8_t, kUdpProbeSize> encode_udp_probe(const UdpProbe& probe);
[[nodiscard]] std::optional<UdpProbe> decode_udp_probe(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
encode_video_datagram(const VideoDatagramHeader& header, std::span<const std::uint8_t> payload,
                      std::uint16_t maximum_datagram_size);
[[nodiscard]] std::optional<DecodedVideoDatagram>
decode_video_datagram(std::span<const std::uint8_t> bytes,
                      std::uint16_t maximum_datagram_size);

} // namespace cloud_stream::protocol
