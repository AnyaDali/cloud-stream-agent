#include "protocol/stream_payloads.h"

#include "protocol/message_header.h"

#include <bit>
#include <limits>

namespace cloud_stream::protocol {
namespace {

void append_u16(std::vector<std::uint8_t>& destination, const std::uint16_t value) {
    destination.push_back(static_cast<std::uint8_t>(value >> 8U));
    destination.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& destination, const std::uint32_t value) {
    destination.push_back(static_cast<std::uint8_t>(value >> 24U));
    destination.push_back(static_cast<std::uint8_t>(value >> 16U));
    destination.push_back(static_cast<std::uint8_t>(value >> 8U));
    destination.push_back(static_cast<std::uint8_t>(value));
}

void append_u64(std::vector<std::uint8_t>& destination, const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        destination.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
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

[[nodiscard]] bool is_known_role(const PeerRole role) {
    return role == PeerRole::server || role == PeerRole::client;
}

[[nodiscard]] bool is_known_codec(const VideoCodec codec) {
    return codec == VideoCodec::raw || codec == VideoCodec::h264;
}

[[nodiscard]] bool is_known_pixel_format(const PixelFormat format) {
    return format == PixelFormat::unspecified || format == PixelFormat::rgb24 ||
           format == PixelFormat::nv12 || format == PixelFormat::yuv420p;
}

[[nodiscard]] bool is_valid_codec_format(const VideoCodec codec, const PixelFormat format) {
    if (codec == VideoCodec::raw) {
        return format == PixelFormat::rgb24;
    }
    return format == PixelFormat::unspecified;
}

[[nodiscard]] bool is_valid_utf8(const std::string& value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[index]);
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto continuation = static_cast<std::uint8_t>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80U) ||
                              (continuation_count == 2 && code_point < 0x800U) ||
                              (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

} // namespace

std::optional<std::vector<std::uint8_t>> encode_hello(const HelloPayload& payload) {
    constexpr std::uint16_t kKnownCapabilities = capability_raw_rgb24 | capability_h264_annex_b;
    if (!is_known_role(payload.role) || (payload.capabilities & ~kKnownCapabilities) != 0 ||
        payload.max_payload_size < kVideoPacketBaseSize + 1 ||
        payload.max_payload_size > kMaxPayloadSize) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> result;
    result.reserve(kHelloPayloadSize);
    result.push_back(static_cast<std::uint8_t>(payload.role));
    result.push_back(0);
    append_u16(result, payload.capabilities);
    append_u32(result, payload.max_payload_size);
    return result;
}

std::optional<HelloPayload> decode_hello(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kHelloPayloadSize || bytes[1] != 0) {
        return std::nullopt;
    }

    const HelloPayload result{
        .role = static_cast<PeerRole>(bytes[0]),
        .capabilities = read_u16(bytes.subspan<2, 2>()),
        .max_payload_size = read_u32(bytes.subspan<4, 4>()),
    };
    return encode_hello(result).has_value() ? std::optional{result} : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> encode_stream_config(const StreamConfigPayload& payload) {
    constexpr std::size_t kRgbChannels = 3;
    const auto pixel_count = static_cast<std::size_t>(payload.width) * payload.height;
    const bool raw_frame_fits =
        payload.codec != VideoCodec::raw ||
        (pixel_count <= (kMaxPayloadSize - kVideoPacketBaseSize) / kRgbChannels);
    if (!is_known_codec(payload.codec) || !is_known_pixel_format(payload.pixel_format) ||
        !is_valid_codec_format(payload.codec, payload.pixel_format) || payload.flags != 0 ||
        payload.width == 0 || payload.width > kMaxVideoDimension || payload.height == 0 ||
        payload.height > kMaxVideoDimension || payload.time_base_numerator == 0 ||
        payload.time_base_numerator > kMaxTimeBaseComponent || payload.time_base_denominator == 0 ||
        payload.time_base_denominator > kMaxTimeBaseComponent ||
        (payload.codec == VideoCodec::raw && !payload.extradata.empty()) || !raw_frame_fits ||
        payload.extradata.size() > kMaxPayloadSize - kStreamConfigBaseSize ||
        payload.extradata.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> result;
    result.reserve(kStreamConfigBaseSize + payload.extradata.size());
    result.push_back(static_cast<std::uint8_t>(payload.codec));
    result.push_back(static_cast<std::uint8_t>(payload.pixel_format));
    append_u16(result, payload.flags);
    append_u16(result, payload.width);
    append_u16(result, payload.height);
    append_u32(result, payload.time_base_numerator);
    append_u32(result, payload.time_base_denominator);
    append_u32(result, static_cast<std::uint32_t>(payload.extradata.size()));
    result.insert(result.end(), payload.extradata.begin(), payload.extradata.end());
    return result;
}

std::optional<StreamConfigPayload> decode_stream_config(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kStreamConfigBaseSize) {
        return std::nullopt;
    }

    const auto extradata_size = read_u32(bytes.subspan<16, 4>());
    if (extradata_size != bytes.size() - kStreamConfigBaseSize) {
        return std::nullopt;
    }

    StreamConfigPayload result{
        .codec = static_cast<VideoCodec>(bytes[0]),
        .pixel_format = static_cast<PixelFormat>(bytes[1]),
        .flags = read_u16(bytes.subspan<2, 2>()),
        .width = read_u16(bytes.subspan<4, 2>()),
        .height = read_u16(bytes.subspan<6, 2>()),
        .time_base_numerator = read_u32(bytes.subspan<8, 4>()),
        .time_base_denominator = read_u32(bytes.subspan<12, 4>()),
        .extradata = std::vector<std::uint8_t>(bytes.begin() + kStreamConfigBaseSize, bytes.end()),
    };
    return encode_stream_config(result).has_value() ? std::optional{std::move(result)}
                                                    : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> encode_video_packet(const VideoPacketPayload& payload) {
    if (payload.duration < 0 || (payload.flags & ~video_packet_keyframe) != 0 ||
        payload.data.empty() || payload.data.size() > kMaxPayloadSize - kVideoPacketBaseSize) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> result;
    result.reserve(kVideoPacketBaseSize + payload.data.size());
    append_u64(result, std::bit_cast<std::uint64_t>(payload.pts));
    append_u64(result, std::bit_cast<std::uint64_t>(payload.dts));
    append_u64(result, std::bit_cast<std::uint64_t>(payload.duration));
    append_u16(result, payload.flags);
    append_u16(result, 0);
    result.insert(result.end(), payload.data.begin(), payload.data.end());
    return result;
}

std::optional<VideoPacketPayload> decode_video_packet(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() <= kVideoPacketBaseSize || bytes.size() > kMaxPayloadSize || bytes[26] != 0 ||
        bytes[27] != 0) {
        return std::nullopt;
    }

    const auto duration = std::bit_cast<std::int64_t>(read_u64(bytes.subspan<16, 8>()));
    const auto flags = read_u16(bytes.subspan<24, 2>());
    if (duration < 0 || (flags & ~video_packet_keyframe) != 0) {
        return std::nullopt;
    }

    return VideoPacketPayload{
        .pts = std::bit_cast<std::int64_t>(read_u64(bytes.subspan<0, 8>())),
        .dts = std::bit_cast<std::int64_t>(read_u64(bytes.subspan<8, 8>())),
        .duration = duration,
        .flags = flags,
        .data = std::vector<std::uint8_t>(bytes.begin() + kVideoPacketBaseSize, bytes.end()),
    };
}

std::optional<std::vector<std::uint8_t>> encode_error(const ErrorPayload& payload) {
    if (payload.message.size() > kMaxErrorMessageSize || !is_valid_utf8(payload.message)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> result;
    result.reserve(sizeof(std::uint32_t) + payload.message.size());
    append_u32(result, payload.code);
    result.insert(result.end(), payload.message.begin(), payload.message.end());
    return result;
}

std::optional<ErrorPayload> decode_error(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < sizeof(std::uint32_t) ||
        bytes.size() > sizeof(std::uint32_t) + kMaxErrorMessageSize) {
        return std::nullopt;
    }
    ErrorPayload result{
        .code = read_u32(bytes.subspan<0, 4>()),
        .message = std::string(bytes.begin() + sizeof(std::uint32_t), bytes.end()),
    };
    return is_valid_utf8(result.message) ? std::optional{std::move(result)} : std::nullopt;
}

} // namespace cloud_stream::protocol
