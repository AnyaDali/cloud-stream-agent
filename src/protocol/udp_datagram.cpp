#include "protocol/udp_datagram.h"

#include "protocol/message_header.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace cloud_stream::protocol {
namespace {

constexpr std::array<std::uint8_t, 4> kProbeMagic{'C', 'S', 'P', 'B'};
constexpr std::array<std::uint8_t, 4> kVideoMagic{'C', 'S', 'V', 'D'};
constexpr std::uint8_t kProbeType = 1;
constexpr std::uint8_t kVideoType = 1;

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

void write_u64(std::span<std::uint8_t> destination, const std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        destination[index] = static_cast<std::uint8_t>(value >> (56U - index * 8U));
    }
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

[[nodiscard]] std::uint64_t read_u64(const std::span<const std::uint8_t> source) {
    std::uint64_t result = 0;
    for (const auto byte : source) {
        result = (result << 8U) | byte;
    }
    return result;
}

} // namespace

std::array<std::uint8_t, kUdpProbeSize> encode_udp_probe(const UdpProbe& probe) {
    std::array<std::uint8_t, kUdpProbeSize> result{};
    std::copy(kProbeMagic.begin(), kProbeMagic.end(), result.begin());
    result[4] = kProtocolVersion;
    result[5] = kProbeType;
    write_u64(std::span(result).subspan<8, 8>(), probe.session_id);
    std::copy(probe.token.begin(), probe.token.end(), result.begin() + 16);
    return result;
}

std::optional<UdpProbe> decode_udp_probe(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kUdpProbeSize ||
        !std::equal(kProbeMagic.begin(), kProbeMagic.end(), bytes.begin()) ||
        bytes[4] != kProtocolVersion || bytes[5] != kProbeType || bytes[6] != 0 ||
        bytes[7] != 0) {
        return std::nullopt;
    }
    UdpProbe result{.session_id = read_u64(bytes.subspan<8, 8>()), .token = {}};
    std::copy_n(bytes.begin() + 16, result.token.size(), result.token.begin());
    return result.session_id == 0 ? std::nullopt : std::optional{result};
}

std::optional<std::vector<std::uint8_t>>
encode_video_datagram(const VideoDatagramHeader& header,
                      const std::span<const std::uint8_t> payload,
                      const std::uint16_t maximum_datagram_size) {
    if (maximum_datagram_size < kVideoDatagramHeaderSize || header.session_id == 0 ||
        header.key_epoch != kInitialKeyEpoch ||
        header.fragment_count == 0 || header.fragment_index >= header.fragment_count ||
        header.frame_size == 0 || header.frame_size > kMaxPayloadSize || payload.empty() ||
        payload.size() > maximum_datagram_size - kVideoDatagramHeaderSize ||
        payload.size() > std::numeric_limits<std::uint16_t>::max() ||
        header.payload_size != payload.size() ||
        (header.flags & ~video_datagram_keyframe) != 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> result(kVideoDatagramHeaderSize + payload.size());
    std::copy(kVideoMagic.begin(), kVideoMagic.end(), result.begin());
    result[4] = kProtocolVersion;
    result[5] = kVideoType;
    write_u16(std::span(result).subspan<6, 2>(), header.flags);
    write_u64(std::span(result).subspan<8, 8>(), header.session_id);
    write_u32(std::span(result).subspan<16, 4>(), header.key_epoch);
    write_u16(std::span(result).subspan<20, 2>(), header.fragment_index);
    write_u16(std::span(result).subspan<22, 2>(), header.fragment_count);
    write_u64(std::span(result).subspan<24, 8>(), header.packet_number);
    write_u64(std::span(result).subspan<32, 8>(), header.frame_id);
    write_u64(std::span(result).subspan<40, 8>(), std::bit_cast<std::uint64_t>(header.pts));
    write_u32(std::span(result).subspan<48, 4>(), header.frame_size);
    write_u16(std::span(result).subspan<52, 2>(), header.payload_size);
    std::copy(payload.begin(), payload.end(), result.begin() + kVideoDatagramHeaderSize);
    return result;
}

std::optional<DecodedVideoDatagram>
decode_video_datagram(const std::span<const std::uint8_t> bytes,
                      const std::uint16_t maximum_datagram_size) {
    if (bytes.size() <= kVideoDatagramHeaderSize || bytes.size() > maximum_datagram_size ||
        !std::equal(kVideoMagic.begin(), kVideoMagic.end(), bytes.begin()) ||
        bytes[4] != kProtocolVersion || bytes[5] != kVideoType || bytes[54] != 0 ||
        bytes[55] != 0) {
        return std::nullopt;
    }

    const VideoDatagramHeader header{
        .flags = read_u16(bytes.subspan<6, 2>()),
        .session_id = read_u64(bytes.subspan<8, 8>()),
        .key_epoch = read_u32(bytes.subspan<16, 4>()),
        .fragment_index = read_u16(bytes.subspan<20, 2>()),
        .fragment_count = read_u16(bytes.subspan<22, 2>()),
        .packet_number = read_u64(bytes.subspan<24, 8>()),
        .frame_id = read_u64(bytes.subspan<32, 8>()),
        .pts = std::bit_cast<std::int64_t>(read_u64(bytes.subspan<40, 8>())),
        .frame_size = read_u32(bytes.subspan<48, 4>()),
        .payload_size = read_u16(bytes.subspan<52, 2>()),
    };
    const auto payload = bytes.subspan(kVideoDatagramHeaderSize);
    if (header.session_id == 0 || header.key_epoch != kInitialKeyEpoch ||
        header.fragment_count == 0 || header.fragment_index >= header.fragment_count ||
        header.frame_size == 0 || header.frame_size > kMaxPayloadSize ||
        header.payload_size != payload.size() ||
        (header.flags & ~video_datagram_keyframe) != 0) {
        return std::nullopt;
    }
    return DecodedVideoDatagram{.header = header, .payload = payload};
}

} // namespace cloud_stream::protocol
