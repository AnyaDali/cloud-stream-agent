#include "protocol/message_header.h"

#include <algorithm>
#include <stdexcept>

namespace cloud_stream::protocol {
namespace {

void write_u16(std::span<std::uint8_t> destination, const std::uint16_t value) {
    destination[0] = static_cast<std::uint8_t>(value >> 8U);
    destination[1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::span<std::uint8_t> destination, const std::uint32_t value) {
    destination[0] = static_cast<std::uint8_t>(value >> 24U);
    destination[1] = static_cast<std::uint8_t>(value >> 16U);
    destination[2] = static_cast<std::uint8_t>(value >> 8U);
    destination[3] = static_cast<std::uint8_t>(value);
}

[[nodiscard]] std::uint16_t read_u16(const std::span<const std::uint8_t> source) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(source[0]) << 8U) |
                                      static_cast<std::uint16_t>(source[1]));
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::uint8_t> source) {
    return (static_cast<std::uint32_t>(source[0]) << 24U) |
           (static_cast<std::uint32_t>(source[1]) << 16U) |
           (static_cast<std::uint32_t>(source[2]) << 8U) | static_cast<std::uint32_t>(source[3]);
}

[[nodiscard]] bool is_known_message_type(const std::uint8_t raw_type) {
    return raw_type >= static_cast<std::uint8_t>(MessageType::hello) &&
           raw_type <= static_cast<std::uint8_t>(MessageType::end);
}

} // namespace

std::array<std::uint8_t, kHeaderSize> encode_header(const MessageHeader& header) {
    if (!is_valid_header(header)) {
        throw std::invalid_argument("invalid protocol message header");
    }

    std::array<std::uint8_t, kHeaderSize> result{};
    std::copy(kMagic.begin(), kMagic.end(), result.begin());
    result[4] = kProtocolVersion;
    result[5] = static_cast<std::uint8_t>(header.type);
    write_u16(std::span(result).subspan<6, 2>(), header.flags);
    write_u32(std::span(result).subspan<8, 4>(), header.payload_size);
    write_u32(std::span(result).subspan<12, 4>(), header.sequence);
    return result;
}

bool is_valid_header(const MessageHeader& header) {
    return is_known_message_type(static_cast<std::uint8_t>(header.type)) && header.flags == 0 &&
           header.payload_size <= kMaxPayloadSize;
}

std::optional<MessageHeader> decode_header(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kHeaderSize || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
        bytes[4] != kProtocolVersion || !is_known_message_type(bytes[5])) {
        return std::nullopt;
    }

    const auto payload_size = read_u32(bytes.subspan<8, 4>());
    if (payload_size > kMaxPayloadSize) {
        return std::nullopt;
    }

    const MessageHeader header{
        .type = static_cast<MessageType>(bytes[5]),
        .flags = read_u16(bytes.subspan<6, 2>()),
        .payload_size = payload_size,
        .sequence = read_u32(bytes.subspan<12, 4>()),
    };
    return is_valid_header(header) ? std::optional{header} : std::nullopt;
}

} // namespace cloud_stream::protocol
