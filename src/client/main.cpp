#include "net/tcp_message_stream.h"
#include "protocol/stream_payloads.h"

#include <asio.hpp>

#include <windows.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

std::uint16_t parse_port(const char* text) {
    std::uint16_t result{};
    const std::string_view input{text};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), result);
    if (error != std::errc{} || end != input.data() + input.size()) {
        throw std::invalid_argument("invalid port");
    }
    return result;
}

void atomic_replace(const std::filesystem::path& temporary, const std::filesystem::path& target) {
    if (!::MoveFileExW(temporary.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                "failed to publish artifact");
    }
}

void write_frame(const std::filesystem::path& output,
                 const cloud_stream::protocol::StreamConfigPayload& config,
                 const cloud_stream::protocol::VideoPacketPayload& packet) {
    constexpr std::size_t kRgbChannels = 3;
    const auto pixels = static_cast<std::size_t>(config.width) * config.height;
    if (pixels > cloud_stream::protocol::kMaxPayloadSize / kRgbChannels ||
        packet.data.size() != pixels * kRgbChannels) {
        throw std::runtime_error("RAW_RGB24 frame size does not match STREAM_CONFIG");
    }

    if (!output.parent_path().empty()) {
        std::filesystem::create_directories(output.parent_path());
    }
    const auto temporary = output.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("failed to open latest-frame artifact");
    }
    stream << "P6\n" << config.width << ' ' << config.height << "\n255\n";
    stream.write(reinterpret_cast<const char*>(packet.data.data()),
                 static_cast<std::streamsize>(packet.data.size()));
    stream.close();
    if (!stream) {
        throw std::runtime_error("failed to write latest-frame artifact");
    }
    atomic_replace(temporary, output);
}

void write_metrics(const std::filesystem::path& output, const std::uint64_t frames_received,
                   const std::uint64_t bytes_received, const std::uint32_t last_sequence,
                   const std::int64_t last_pts,
                   const cloud_stream::protocol::StreamConfigPayload& config,
                   const bool stream_ended, const std::int64_t last_frame_received_unix_ms) {
    const auto metrics_path = output.parent_path() / "stream-metrics.json";
    const auto temporary = metrics_path.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("failed to open metrics artifact");
    }
    stream << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"connection_state\": \"" << (stream_ended ? "ended" : "connected") << "\",\n"
           << "  \"codec\": \"raw_rgb24\",\n"
           << "  \"width\": " << config.width << ",\n"
           << "  \"height\": " << config.height << ",\n"
           << "  \"video_messages_received_total\": " << frames_received << ",\n"
           << "  \"frames_decoded_total\": " << frames_received << ",\n"
           << "  \"payload_bytes_received_total\": " << bytes_received << ",\n"
           << "  \"last_sequence\": " << last_sequence << ",\n"
           << "  \"last_pts\": " << last_pts << ",\n"
           << "  \"last_frame_received_unix_ms\": " << last_frame_received_unix_ms << ",\n"
           << "  \"latest_frame_file\": \"latest-frame.ppm\"\n"
           << "}\n";
    stream.close();
    if (!stream) {
        throw std::runtime_error("failed to write metrics artifact");
    }
    atomic_replace(temporary, metrics_path);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
        const std::uint16_t port = argc > 2 ? parse_port(argv[2]) : std::uint16_t{9010};
        const auto output = argc > 3 ? std::filesystem::path(argv[3])
                                     : std::filesystem::temp_directory_path() /
                                           "cloud-stream-agent" /
                                           ("client-" + std::to_string(::GetCurrentProcessId())) /
                                           "latest-frame.ppm";

        asio::io_context context;
        asio::ip::tcp::resolver resolver(context);
        asio::ip::tcp::socket socket(context);
        asio::connect(socket, resolver.resolve(host, std::to_string(port)));
        cloud_stream::net::configure_socket(socket);

        const auto client_hello = cloud_stream::protocol::encode_hello({
            .role = cloud_stream::protocol::PeerRole::client,
            .capabilities = cloud_stream::protocol::capability_raw_rgb24,
            .max_payload_size = cloud_stream::protocol::kMaxPayloadSize,
        });
        if (!client_hello) {
            throw std::logic_error("failed to encode client HELLO");
        }
        cloud_stream::net::send_message(socket, cloud_stream::protocol::MessageType::hello, 0, 0,
                                        *client_hello);

        std::uint32_t expected_sequence = 0;
        bool hello_received = false;
        bool stream_ended = false;
        std::optional<cloud_stream::protocol::StreamConfigPayload> config;
        std::uint64_t frames_received = 0;
        std::uint64_t bytes_received = 0;
        std::uint32_t last_sequence = 0;
        std::int64_t last_pts = 0;
        std::int64_t last_frame_received_unix_ms = 0;

        while (!stream_ended) {
            const auto message = cloud_stream::net::receive_message(socket);
            if (message.header.sequence != expected_sequence) {
                throw std::runtime_error("unexpected server message sequence");
            }
            last_sequence = message.header.sequence;

            switch (message.header.type) {
            case cloud_stream::protocol::MessageType::hello: {
                if (hello_received || config) {
                    throw std::runtime_error("HELLO is not valid in the current state");
                }
                const auto hello = cloud_stream::protocol::decode_hello(message.payload);
                if (!hello || hello->role != cloud_stream::protocol::PeerRole::server ||
                    (hello->capabilities & cloud_stream::protocol::capability_raw_rgb24) == 0) {
                    throw std::runtime_error("invalid server HELLO");
                }
                hello_received = true;
                break;
            }
            case cloud_stream::protocol::MessageType::stream_config: {
                if (!hello_received || config) {
                    throw std::runtime_error("STREAM_CONFIG is not valid in the current state");
                }
                config = cloud_stream::protocol::decode_stream_config(message.payload);
                if (!config || config->codec != cloud_stream::protocol::VideoCodec::raw ||
                    config->pixel_format != cloud_stream::protocol::PixelFormat::rgb24) {
                    throw std::runtime_error("unsupported STREAM_CONFIG");
                }
                break;
            }
            case cloud_stream::protocol::MessageType::video_packet: {
                if (!config) {
                    throw std::runtime_error("VIDEO_PACKET received before STREAM_CONFIG");
                }
                const auto packet = cloud_stream::protocol::decode_video_packet(message.payload);
                if (!packet) {
                    throw std::runtime_error("invalid VIDEO_PACKET");
                }
                const auto received_at = std::chrono::system_clock::now().time_since_epoch();
                last_frame_received_unix_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(received_at).count();
                ++frames_received;
                bytes_received += packet->data.size();
                last_pts = packet->pts;
                write_frame(output, *config, *packet);
                write_metrics(output, frames_received, bytes_received, last_sequence, last_pts,
                              *config, false, last_frame_received_unix_ms);
                break;
            }
            case cloud_stream::protocol::MessageType::error: {
                const auto error = cloud_stream::protocol::decode_error(message.payload);
                throw std::runtime_error(error ? "server error " + std::to_string(error->code) +
                                                     ": " + error->message
                                               : "invalid ERROR payload");
            }
            case cloud_stream::protocol::MessageType::end:
                if (!config || !message.payload.empty()) {
                    throw std::runtime_error("invalid END message");
                }
                stream_ended = true;
                break;
            }
            if (!stream_ended) {
                if (expected_sequence == std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error("server message sequence would wrap");
                }
                ++expected_sequence;
            }
        }

        if (frames_received == 0 || !config) {
            throw std::runtime_error("stream ended without a video frame");
        }
        write_metrics(output, frames_received, bytes_received, last_sequence, last_pts, *config,
                      true, last_frame_received_unix_ms);
        std::cout << "frames_received=" << frames_received << " bytes_received=" << bytes_received
                  << " latest_frame=" << output.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "stream-client error: " << error.what() << '\n';
        return 1;
    }
}
