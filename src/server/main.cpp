#include "net/tcp_message_stream.h"
#include "protocol/stream_payloads.h"

#include <asio.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t kFrameWidth = 320;
constexpr std::uint16_t kFrameHeight = 180;

template <typename Value> Value parse_number(const char* text, const char* name) {
    Value result{};
    const std::string_view input{text};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), result);
    if (error != std::errc{} || end != input.data() + input.size()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return result;
}

std::vector<std::uint8_t> make_frame(const std::uint32_t frame_index) {
    constexpr std::size_t kChannels = 3;
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(kFrameWidth) * kFrameHeight *
                                    kChannels);
    for (std::uint16_t y = 0; y < kFrameHeight; ++y) {
        for (std::uint16_t x = 0; x < kFrameWidth; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * kFrameWidth + x) * kChannels;
            frame[offset] = static_cast<std::uint8_t>((x + frame_index * 4U) % 256U);
            frame[offset + 1] = static_cast<std::uint8_t>((y + frame_index * 2U) % 256U);
            frame[offset + 2] = static_cast<std::uint8_t>((x + y + frame_index * 7U) % 256U);
        }
    }
    return frame;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::uint16_t port =
            argc > 1 ? parse_number<std::uint16_t>(argv[1], "port") : std::uint16_t{9010};
        const std::uint32_t frame_count =
            argc > 2 ? parse_number<std::uint32_t>(argv[2], "frame count") : std::uint32_t{120};
        const std::uint32_t interval_ms =
            argc > 3 ? parse_number<std::uint32_t>(argv[3], "frame interval") : std::uint32_t{33};
        const std::string_view bind_address = argc > 4 ? argv[4] : "127.0.0.1";
        if (frame_count == 0 || frame_count > std::numeric_limits<std::uint32_t>::max() - 2U) {
            throw std::invalid_argument("frame count must be in range [1, UINT32_MAX - 2]");
        }

        asio::io_context context;
        const auto address = asio::ip::make_address(bind_address);
        asio::ip::tcp::acceptor acceptor(context, {address, port});
        const auto bound_port = acceptor.local_endpoint().port();
        std::cout << "listening_on=" << address.to_string() << ':' << bound_port << '\n'
                  << std::flush;

        asio::ip::tcp::socket socket(context);
        acceptor.accept(socket);
        cloud_stream::net::configure_socket(socket);

        const auto client_hello_message = cloud_stream::net::receive_message(socket);
        if (client_hello_message.header.sequence != 0 ||
            client_hello_message.header.type != cloud_stream::protocol::MessageType::hello) {
            throw std::runtime_error("expected client HELLO with sequence 0");
        }
        const auto client_hello =
            cloud_stream::protocol::decode_hello(client_hello_message.payload);
        if (!client_hello || client_hello->role != cloud_stream::protocol::PeerRole::client ||
            (client_hello->capabilities & cloud_stream::protocol::capability_raw_rgb24) == 0) {
            throw std::runtime_error("client does not support RAW_RGB24");
        }
        constexpr std::size_t kRawFramePayloadSize =
            cloud_stream::protocol::kVideoPacketBaseSize +
            static_cast<std::size_t>(kFrameWidth) * kFrameHeight * 3U;
        if (client_hello->max_payload_size < kRawFramePayloadSize) {
            const auto error_payload = cloud_stream::protocol::encode_error({
                .code = 1001,
                .message = "limit too small",
            });
            if (error_payload && error_payload->size() <= client_hello->max_payload_size) {
                cloud_stream::net::send_message(socket, cloud_stream::protocol::MessageType::error,
                                                0, 0, *error_payload);
            }
            throw std::runtime_error(
                "client payload limit is too small for the selected RAW stream");
        }
        if (interval_ms != 0 &&
            static_cast<std::uint64_t>(frame_count - 1U) >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                    interval_ms) {
            throw std::invalid_argument("frame timestamps exceed int64 range");
        }

        std::uint32_t sequence = 0;
        const auto server_hello = cloud_stream::protocol::encode_hello({
            .role = cloud_stream::protocol::PeerRole::server,
            .capabilities = cloud_stream::protocol::capability_raw_rgb24,
            .max_payload_size = cloud_stream::protocol::kMaxPayloadSize,
        });
        if (!server_hello) {
            throw std::logic_error("failed to encode server HELLO");
        }
        cloud_stream::net::send_message(socket, cloud_stream::protocol::MessageType::hello, 0,
                                        sequence++, *server_hello);

        const auto stream_config = cloud_stream::protocol::encode_stream_config({
            .codec = cloud_stream::protocol::VideoCodec::raw,
            .pixel_format = cloud_stream::protocol::PixelFormat::rgb24,
            .flags = 0,
            .width = kFrameWidth,
            .height = kFrameHeight,
            .time_base_numerator = 1,
            .time_base_denominator = 1000,
            .extradata = {},
        });
        if (!stream_config || stream_config->size() > client_hello->max_payload_size) {
            throw std::runtime_error("STREAM_CONFIG exceeds negotiated payload limit");
        }
        cloud_stream::net::send_message(socket, cloud_stream::protocol::MessageType::stream_config,
                                        0, sequence++, *stream_config);

        for (std::uint32_t frame_index = 0; frame_index < frame_count; ++frame_index) {
            const auto packet = cloud_stream::protocol::encode_video_packet({
                .pts = static_cast<std::int64_t>(frame_index) * interval_ms,
                .dts = static_cast<std::int64_t>(frame_index) * interval_ms,
                .duration = interval_ms,
                .flags = cloud_stream::protocol::video_packet_keyframe,
                .data = make_frame(frame_index),
            });
            if (!packet || packet->size() > client_hello->max_payload_size) {
                throw std::runtime_error("VIDEO_PACKET exceeds negotiated payload limit");
            }
            cloud_stream::net::send_message(
                socket, cloud_stream::protocol::MessageType::video_packet, 0, sequence++, *packet);
            if (interval_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            }
        }

        cloud_stream::net::send_message(socket, cloud_stream::protocol::MessageType::end, 0,
                                        sequence, {});
        std::cout << "frames_sent=" << frame_count << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "stream-server error: " << error.what() << '\n';
        return 1;
    }
}
