#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace cloud_stream::protocol {

inline constexpr std::array<std::uint8_t, 4> kMagic{'C', 'S', 'T', 'R'};
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::uint32_t kMaxPayloadSize = 8U * 1024U * 1024U;

enum class MessageType : std::uint8_t {
    hello = 1,
    stream_config = 2,
    video_packet = 3,
    error = 4,
    end = 5,
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
