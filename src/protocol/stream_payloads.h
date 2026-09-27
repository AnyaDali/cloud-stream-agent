#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cloud_stream::protocol {

inline constexpr std::size_t kHelloPayloadSize = 8;
inline constexpr std::size_t kStreamConfigBaseSize = 20;
inline constexpr std::size_t kVideoPacketBaseSize = 28;
inline constexpr std::uint16_t kMaxVideoDimension = 8192;
inline constexpr std::uint32_t kMaxTimeBaseComponent = 1'000'000'000;
inline constexpr std::size_t kMaxErrorMessageSize = 4096;

enum class PeerRole : std::uint8_t {
    server = 1,
    client = 2,
};

enum class VideoCodec : std::uint8_t {
    raw = 0,
    h264 = 1,
};

enum class PixelFormat : std::uint8_t {
    unspecified = 0,
    rgb24 = 1,
    nv12 = 2,
    yuv420p = 3,
};

enum CapabilityFlags : std::uint16_t {
    capability_raw_rgb24 = 1U << 0U,
    capability_h264_annex_b = 1U << 1U,
};

enum VideoPacketFlags : std::uint16_t {
    video_packet_keyframe = 1U << 0U,
};

struct HelloPayload {
    PeerRole role{PeerRole::client};
    std::uint16_t capabilities{0};
    std::uint32_t max_payload_size{0};

    friend bool operator==(const HelloPayload&, const HelloPayload&) = default;
};

struct StreamConfigPayload {
    VideoCodec codec{VideoCodec::raw};
    PixelFormat pixel_format{PixelFormat::rgb24};
    std::uint16_t flags{0};
    std::uint16_t width{0};
    std::uint16_t height{0};
    std::uint32_t time_base_numerator{0};
    std::uint32_t time_base_denominator{0};
    std::vector<std::uint8_t> extradata;

    friend bool operator==(const StreamConfigPayload&, const StreamConfigPayload&) = default;
};

struct VideoPacketPayload {
    std::int64_t pts{0};
    std::int64_t dts{0};
    std::int64_t duration{0};
    std::uint16_t flags{0};
    std::vector<std::uint8_t> data;

    friend bool operator==(const VideoPacketPayload&, const VideoPacketPayload&) = default;
};

struct ErrorPayload {
    std::uint32_t code{0};
    std::string message;

    friend bool operator==(const ErrorPayload&, const ErrorPayload&) = default;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_hello(const HelloPayload& payload);
[[nodiscard]] std::optional<HelloPayload> decode_hello(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
encode_stream_config(const StreamConfigPayload& payload);
[[nodiscard]] std::optional<StreamConfigPayload>
decode_stream_config(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
encode_video_packet(const VideoPacketPayload& payload);
[[nodiscard]] std::optional<VideoPacketPayload>
decode_video_packet(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_error(const ErrorPayload& payload);
[[nodiscard]] std::optional<ErrorPayload> decode_error(std::span<const std::uint8_t> bytes);

} // namespace cloud_stream::protocol
