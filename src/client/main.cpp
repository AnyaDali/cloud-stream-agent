#include "media/decoded_frame.h"
#include "media/ffmpeg/h264_decoder.h"
#include "net/tcp_message_stream.h"
#include "pipeline/bounded_latest_queue.h"
#include "pipeline/frame_reassembler.h"
#include "protocol/stream_payloads.h"
#include "protocol/udp_datagram.h"

#include <asio.hpp>
#include <SDL3/SDL.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct ClientConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t tcp_port{9010};
    std::filesystem::path output;
};

struct Statistics {
    std::atomic<std::uint64_t> udp_datagrams{0};
    std::atomic<std::uint64_t> udp_bytes{0};
    std::atomic<std::uint64_t> encoded_frames{0};
    std::atomic<std::uint64_t> decoded_frames{0};
    std::atomic<std::uint64_t> decode_errors{0};
    std::atomic<std::int64_t> last_pts{0};
    std::atomic<std::int64_t> last_frame_unix_ms{0};
};

[[nodiscard]] std::uint16_t parse_port(const char* text) {
    std::uint16_t result{};
    const std::string_view input{text};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), result);
    if (error != std::errc{} || end != input.data() + input.size() || result == 0) {
        throw std::invalid_argument("invalid TCP port");
    }
    return result;
}

void atomic_replace(const std::filesystem::path& temporary,
                    const std::filesystem::path& target) {
    if (!::MoveFileExW(temporary.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                "failed to publish artifact");
    }
}

void write_frame(const std::filesystem::path& output,
                 const cloud_stream::media::DecodedFrame& frame) {
    if (!output.parent_path().empty()) {
        std::filesystem::create_directories(output.parent_path());
    }
    const auto temporary = std::filesystem::path(output.string() + ".tmp");
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("failed to open latest-frame artifact");
    }
    stream << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
    stream.write(reinterpret_cast<const char*>(frame.rgb24.data()),
                 static_cast<std::streamsize>(frame.rgb24.size()));
    stream.close();
    if (!stream) {
        throw std::runtime_error("failed to write latest-frame artifact");
    }
    atomic_replace(temporary, output);
}

void write_metrics(const std::filesystem::path& output, const Statistics& statistics,
                   const cloud_stream::media::DecodedFrame& frame,
                   const bool stream_ended) {
    const auto metrics_path = output.parent_path() / "stream-metrics.json";
    const auto temporary = std::filesystem::path(metrics_path.string() + ".tmp");
    std::ofstream stream(temporary, std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("failed to open metrics artifact");
    }
    stream << "{\n"
           << "  \"schema_version\": 2,\n"
           << "  \"connection_state\": \"" << (stream_ended ? "ended" : "streaming")
           << "\",\n"
           << "  \"codec\": \"h264\",\n"
           << "  \"transport\": \"udp\",\n"
           << "  \"width\": " << frame.width << ",\n"
           << "  \"height\": " << frame.height << ",\n"
           << "  \"udp_datagrams_received_total\": " << statistics.udp_datagrams.load()
           << ",\n"
           << "  \"encoded_frames_received_total\": " << statistics.encoded_frames.load()
           << ",\n"
           << "  \"frames_decoded_total\": " << statistics.decoded_frames.load() << ",\n"
           << "  \"decode_errors_total\": " << statistics.decode_errors.load() << ",\n"
           << "  \"udp_bytes_received_total\": " << statistics.udp_bytes.load()
           << ",\n"
           << "  \"last_pts\": " << statistics.last_pts.load() << ",\n"
           << "  \"last_frame_received_unix_ms\": "
           << statistics.last_frame_unix_ms.load() << ",\n"
           << "  \"latest_frame_file\": \"" << output.filename().string() << "\"\n"
           << "}\n";
    stream.close();
    if (!stream) {
        throw std::runtime_error("failed to write metrics artifact");
    }
    atomic_replace(temporary, metrics_path);
}

class ClientApp {
public:
    explicit ClientApp(ClientConfig config)
        : config_(std::move(config)), encoded_frames_(8) {}

    ~ClientApp() {
        stop_.store(true);
        encoded_frames_.close();
        join_workers();
    }

    int run() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }
        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        SDL_Texture* texture = nullptr;
        if (!SDL_CreateWindowAndRenderer("Cloud Stream Agent", 960, 540,
                                         SDL_WINDOW_RESIZABLE, &window, &renderer)) {
            const std::string error = SDL_GetError();
            SDL_Quit();
            throw std::runtime_error("SDL window creation failed: " + error);
        }

        network_thread_ = std::thread([this] { network_loop(); });
        decoder_thread_ = std::thread([this] { decoder_loop(); });

        std::uint64_t displayed_frame_id = std::numeric_limits<std::uint64_t>::max();
        std::uint32_t texture_width = 0;
        std::uint32_t texture_height = 0;
        while (!stop_.load()) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    stop_.store(true);
                }
            }

            std::shared_ptr<const cloud_stream::media::DecodedFrame> latest;
            {
                std::lock_guard lock(latest_mutex_);
                latest = latest_frame_;
            }
            if (latest && latest->frame_id != displayed_frame_id) {
                if (texture == nullptr || texture_width != latest->width ||
                    texture_height != latest->height) {
                    SDL_DestroyTexture(texture);
                    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                                SDL_TEXTUREACCESS_STREAMING,
                                                static_cast<int>(latest->width),
                                                static_cast<int>(latest->height));
                    if (texture == nullptr) {
                        set_error(std::string("SDL texture creation failed: ") + SDL_GetError());
                        stop_.store(true);
                        break;
                    }
                    texture_width = latest->width;
                    texture_height = latest->height;
                }
                if (!SDL_UpdateTexture(texture, nullptr, latest->rgb24.data(),
                                       static_cast<int>(latest->stride))) {
                    set_error(std::string("SDL texture update failed: ") + SDL_GetError());
                    stop_.store(true);
                    break;
                }
                displayed_frame_id = latest->frame_id;
            }

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            if (texture != nullptr) {
                SDL_RenderTexture(renderer, texture, nullptr, nullptr);
            }
            SDL_RenderPresent(renderer);

            if (network_finished_.load() && decoder_finished_.load()) {
                stop_.store(true);
            }
            SDL_Delay(10);
        }

        stop_.store(true);
        join_workers();
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();

        const auto error = current_error();
        if (error) {
            std::cerr << "stream-client error: " << *error << '\n';
            return 1;
        }
        std::cout << "frames_received=" << statistics_.encoded_frames.load()
                  << " frames_decoded=" << statistics_.decoded_frames.load()
                  << " bytes_received=" << statistics_.udp_bytes.load()
                  << " latest_frame=" << config_.output.string() << '\n';
        return 0;
    }

private:
    void network_loop() {
        try {
            asio::io_context context;
            asio::ip::tcp::resolver resolver(context);
            asio::ip::tcp::socket tcp_socket(context);
            asio::connect(tcp_socket,
                          resolver.resolve(config_.host, std::to_string(config_.tcp_port)));
            cloud_stream::net::configure_socket(tcp_socket);

            const auto client_hello = cloud_stream::protocol::encode_hello({
                .role = cloud_stream::protocol::PeerRole::client,
                .capabilities = cloud_stream::protocol::capability_h264_annex_b,
                .max_payload_size = cloud_stream::protocol::kMaxPayloadSize,
            });
            if (!client_hello) {
                throw std::logic_error("failed to encode client HELLO");
            }
            cloud_stream::net::send_message(tcp_socket,
                                             cloud_stream::protocol::MessageType::hello, 0, 0,
                                             *client_hello);

            auto message = receive_control(tcp_socket, 0,
                                           cloud_stream::protocol::MessageType::hello);
            const auto server_hello = cloud_stream::protocol::decode_hello(message.payload);
            if (!server_hello || server_hello->role != cloud_stream::protocol::PeerRole::server ||
                (server_hello->capabilities &
                 cloud_stream::protocol::capability_h264_annex_b) == 0) {
                throw std::runtime_error("server HELLO does not offer H.264 Annex B");
            }

            message = receive_control(tcp_socket, 1,
                                      cloud_stream::protocol::MessageType::udp_config);
            const auto udp_config = cloud_stream::protocol::decode_udp_config(message.payload);
            if (!udp_config) {
                throw std::runtime_error("invalid UDP_CONFIG");
            }

            const auto server_address = tcp_socket.remote_endpoint().address();
            const asio::ip::udp::endpoint server_udp_endpoint(server_address,
                                                               udp_config->server_port);
            asio::ip::udp::socket udp_socket(
                context, server_address.is_v6()
                             ? asio::ip::udp::endpoint(asio::ip::udp::v6(), 0)
                             : asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
            udp_socket.set_option(asio::socket_base::receive_buffer_size(4 * 1024 * 1024));
            const auto probe = cloud_stream::protocol::encode_udp_probe({
                .session_id = udp_config->session_id,
                .token = udp_config->probe_token,
            });
            udp_socket.send_to(asio::buffer(probe), server_udp_endpoint);

            message = receive_control(tcp_socket, 2,
                                      cloud_stream::protocol::MessageType::udp_ready);
            if (!message.payload.empty()) {
                throw std::runtime_error("UDP_READY payload must be empty");
            }
            message = receive_control(tcp_socket, 3,
                                      cloud_stream::protocol::MessageType::stream_config);
            const auto stream_config =
                cloud_stream::protocol::decode_stream_config(message.payload);
            if (!stream_config ||
                stream_config->codec != cloud_stream::protocol::VideoCodec::h264 ||
                stream_config->pixel_format !=
                    cloud_stream::protocol::PixelFormat::unspecified) {
                throw std::runtime_error("unsupported STREAM_CONFIG");
            }
            cloud_stream::net::send_message(
                tcp_socket, cloud_stream::protocol::MessageType::request_keyframe, 0, 1, {});

            cloud_stream::pipeline::FrameReassembler reassembler(
                udp_config->session_id, udp_config->maximum_datagram_size);
            udp_socket.non_blocking(true);
            tcp_socket.non_blocking(true);
            std::vector<std::uint8_t> control_bytes;
            std::uint32_t expected_server_sequence = 4;
            std::vector<std::uint8_t> datagram(udp_config->maximum_datagram_size);
            bool stream_ended = false;
            bool waiting_for_keyframe = true;

            while (!stop_.load() && !stream_ended) {
                bool progressed = false;
                for (;;) {
                    asio::ip::udp::endpoint sender;
                    std::error_code error;
                    const auto size = udp_socket.receive_from(asio::buffer(datagram), sender, 0,
                                                              error);
                    if (error == asio::error::would_block || error == asio::error::try_again) {
                        break;
                    }
                    if (error) {
                        throw std::system_error(error);
                    }
                    progressed = true;
                    if (sender != server_udp_endpoint) {
                        continue;
                    }
                    ++statistics_.udp_datagrams;
                    statistics_.udp_bytes += size;
                    auto frame = reassembler.consume(
                        std::span<const std::uint8_t>(datagram.data(), size));
                    if (frame) {
                        if (waiting_for_keyframe && !frame->keyframe) {
                            continue;
                        }
                        waiting_for_keyframe = false;
                        ++statistics_.encoded_frames;
                        statistics_.last_pts.store(frame->pts);
                        static_cast<void>(encoded_frames_.push(std::move(*frame)));
                    }
                }

                std::array<std::uint8_t, 4096> chunk{};
                for (;;) {
                    std::error_code error;
                    const auto size = tcp_socket.read_some(asio::buffer(chunk), error);
                    if (error == asio::error::would_block || error == asio::error::try_again) {
                        break;
                    }
                    if (error == asio::error::eof) {
                        throw std::runtime_error("TCP control channel closed before END");
                    }
                    if (error) {
                        throw std::system_error(error);
                    }
                    progressed = true;
                    control_bytes.insert(control_bytes.end(), chunk.begin(), chunk.begin() + size);
                }
                while (control_bytes.size() >= cloud_stream::protocol::kHeaderSize) {
                    const auto header = cloud_stream::protocol::decode_header(std::span(
                        control_bytes.data(), cloud_stream::protocol::kHeaderSize));
                    if (!header) {
                        throw std::runtime_error("invalid TCP control header");
                    }
                    const auto message_size = cloud_stream::protocol::kHeaderSize +
                                              static_cast<std::size_t>(header->payload_size);
                    if (control_bytes.size() < message_size) {
                        break;
                    }
                    if (header->sequence != expected_server_sequence++) {
                        throw std::runtime_error("unexpected server control sequence");
                    }
                    const std::span<const std::uint8_t> payload(
                        control_bytes.data() + cloud_stream::protocol::kHeaderSize,
                        header->payload_size);
                    if (header->type == cloud_stream::protocol::MessageType::end &&
                        payload.empty()) {
                        stream_ended = true;
                    } else if (header->type == cloud_stream::protocol::MessageType::error) {
                        const auto server_error = cloud_stream::protocol::decode_error(payload);
                        throw std::runtime_error(server_error
                                                     ? "server error: " + server_error->message
                                                     : "invalid server ERROR payload");
                    } else {
                        throw std::runtime_error("unexpected TCP control message");
                    }
                    control_bytes.erase(control_bytes.begin(),
                                        control_bytes.begin() + message_size);
                }
                if (!progressed) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        } catch (const std::exception& error) {
            if (!stop_.load()) {
                set_error(error.what());
                stop_.store(true);
            }
        }
        encoded_frames_.close();
        network_finished_.store(true);
    }

    void decoder_loop() {
        try {
            cloud_stream::media::H264Decoder decoder;
            cloud_stream::media::EncodedFrame encoded;
            bool decoder_synchronized = false;
            while (encoded_frames_.wait_pop(encoded)) {
                if (!decoder_synchronized && !encoded.keyframe) {
                    continue;
                }
                std::vector<cloud_stream::media::DecodedFrame> decoded;
                try {
                    decoded = decoder.decode(encoded);
                    if (encoded.keyframe) {
                        decoder_synchronized = true;
                    }
                } catch (const std::exception& error) {
                    ++statistics_.decode_errors;
                    std::cerr << "decoder_packet_error=" << error.what()
                              << " waiting_for_keyframe=true\n";
                    decoder.reset();
                    decoder_synchronized = false;
                    continue;
                }
                for (auto& frame : decoded) {
                    publish_decoded_frame(std::move(frame), false);
                }
            }
            for (auto& frame : decoder.flush()) {
                publish_decoded_frame(std::move(frame), true);
            }
            std::shared_ptr<const cloud_stream::media::DecodedFrame> latest;
            {
                std::lock_guard lock(latest_mutex_);
                latest = latest_frame_;
            }
            if (latest) {
                write_frame(config_.output, *latest);
                write_metrics(config_.output, statistics_, *latest, true);
            }
        } catch (const std::exception& error) {
            if (!stop_.load()) {
                set_error(error.what());
                stop_.store(true);
            }
        }
        decoder_finished_.store(true);
    }

    void publish_decoded_frame(cloud_stream::media::DecodedFrame frame,
                               const bool stream_ended) {
        ++statistics_.decoded_frames;
        const auto received_at = std::chrono::system_clock::now().time_since_epoch();
        statistics_.last_frame_unix_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(received_at).count());
        const auto now = std::chrono::steady_clock::now();
        if (last_artifact_at_.time_since_epoch().count() == 0 ||
            now - last_artifact_at_ >= std::chrono::milliseconds(500) || stream_ended) {
            write_frame(config_.output, frame);
            write_metrics(config_.output, statistics_, frame, stream_ended);
            last_artifact_at_ = now;
        }
        auto shared = std::make_shared<const cloud_stream::media::DecodedFrame>(std::move(frame));
        std::lock_guard lock(latest_mutex_);
        latest_frame_ = std::move(shared);
    }

    [[nodiscard]] static cloud_stream::net::ReceivedMessage
    receive_control(asio::ip::tcp::socket& socket, const std::uint32_t sequence,
                    const cloud_stream::protocol::MessageType type) {
        auto message = cloud_stream::net::receive_message(socket);
        if (message.header.sequence != sequence || message.header.type != type) {
            throw std::runtime_error("unexpected TCP handshake message");
        }
        return message;
    }

    void set_error(std::string value) {
        std::lock_guard lock(error_mutex_);
        if (!error_) {
            error_ = std::move(value);
        }
    }

    [[nodiscard]] std::optional<std::string> current_error() const {
        std::lock_guard lock(error_mutex_);
        return error_;
    }

    void join_workers() {
        if (network_thread_.joinable()) {
            network_thread_.join();
        }
        if (decoder_thread_.joinable()) {
            decoder_thread_.join();
        }
    }

    ClientConfig config_;
    cloud_stream::pipeline::BoundedLatestQueue<cloud_stream::media::EncodedFrame>
        encoded_frames_;
    Statistics statistics_;
    std::thread network_thread_;
    std::thread decoder_thread_;
    std::atomic_bool stop_{false};
    std::atomic_bool network_finished_{false};
    std::atomic_bool decoder_finished_{false};
    mutable std::mutex error_mutex_;
    std::optional<std::string> error_;
    std::mutex latest_mutex_;
    std::shared_ptr<const cloud_stream::media::DecodedFrame> latest_frame_;
    std::chrono::steady_clock::time_point last_artifact_at_{};
};

[[nodiscard]] ClientConfig parse_config(const int argc, char** argv) {
    ClientConfig result;
    result.host = argc > 1 ? argv[1] : result.host;
    result.tcp_port = argc > 2 ? parse_port(argv[2]) : result.tcp_port;
    result.output = argc > 3
                        ? std::filesystem::path(argv[3])
                        : std::filesystem::temp_directory_path() / "cloud-stream-agent" /
                              ("client-" + std::to_string(::GetCurrentProcessId())) /
                              "latest-frame.ppm";
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        ClientApp app(parse_config(argc, argv));
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "stream-client error: " << error.what() << '\n';
        return 1;
    }
}
