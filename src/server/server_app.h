#pragma once

#include "capture/raw_frame.h"
#include "capture/windows/windows_graphics_capture.h"
#include "media/ffmpeg/h264_encoder.h"
#include "pipeline/bounded_latest_queue.h"
#include "pipeline/broadcast_buffer.h"
#include "server/client_acceptor.h"
#include "server/client_registry.h"

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace cloud_stream::server {

struct ServerConfig {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{9010};
    std::uintptr_t window_id{0};
    std::uint16_t video_width{1280};
    std::uint16_t video_height{720};
    std::uint32_t frames_per_second{30};
    std::uint32_t bitrate_kbps{4000};
    std::size_t maximum_clients{8};
    std::size_t raw_frame_queue_capacity{2};
    std::size_t broadcast_maximum_frames{120};
    std::size_t broadcast_maximum_bytes{16U * 1024U * 1024U};
};

class ServerApp {
public:
    explicit ServerApp(ServerConfig config);
    ~ServerApp();

    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;

    int run();

private:
    void start_source();
    void encoder_loop();
    void publish_frame(std::shared_ptr<const media::EncodedFrame> frame);
    void finish_stream();
    void stop();

    ServerConfig config_;
    asio::io_context context_;
    asio::signal_set signals_;
    pipeline::BroadcastBuffer broadcast_buffer_;
    pipeline::BoundedLatestQueue<capture::RawFrame> raw_frame_queue_;
    media::H264Encoder encoder_;
    capture::windows::WindowsGraphicsCaptureSource source_;
    ClientRegistry client_registry_;
    ClientAcceptor client_acceptor_;
    std::thread encoder_thread_;
    bool source_started_{false};
    bool stopping_{false};
    bool stream_finished_{false};
};

} // namespace cloud_stream::server
