#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace cloud_stream::protocol {

inline constexpr std::array<std::uint8_t, 4> kMagic{'C', 'S', 'T', 'R'};
inline constexpr std::uint8_t kProtocolVersion = 2;
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::uint32_t kMaxPayloadSize = 8U * 1024U * 1024U;

enum class MessageType : std::uint8_t {
    hello = 1,
    udp_config = 2,
    udp_ready = 3,
    stream_config = 4,
    request_keyframe = 5,
    ping = 6,
    pong = 7,
    error = 8,
    end = 9,
};

struct MessageHeader {
    MessageType type{MessageType::hello};
    std::uint16_t flags{0};
    std::uint32_t payload_size{0};
    std::uint32_t sequence{0};

    friend bool operator==(const MessageHeader&, const MessageHeader&) = default;
};

[[nodiscard]] std::array<std::uint8_t, kHeaderSize> encode_header(const MessageHeader& header);
[[nodiscard]] std::optional<MessageHeader> decode_header(std::span<const std::uint8_t> bytes);
[[nodiscard]] bool is_valid_header(const MessageHeader& header);

} // namespace cloud_stream::protocol
