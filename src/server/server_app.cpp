#include "server/server_app.h"

#include <asio/ip/address.hpp>
#include <asio/post.hpp>

#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <utility>

namespace cloud_stream::server {

ServerApp::ServerApp(ServerConfig config)
    : config_(std::move(config)), signals_(context_, SIGINT, SIGTERM),
      broadcast_buffer_(config_.broadcast_maximum_frames, config_.broadcast_maximum_bytes),
      raw_frame_queue_(config_.raw_frame_queue_capacity),
      encoder_({
          .width = config_.video_width,
          .height = config_.video_height,
          .frames_per_second = config_.frames_per_second,
          .bitrate_kbps = config_.bitrate_kbps,
          .keyframe_interval = config_.frames_per_second,
      }),
      source_({
          .window_id = config_.window_id,
          .maximum_frames_per_second = config_.frames_per_second,
      }),
      client_registry_(context_.get_executor(), broadcast_buffer_, encoder_.stream_config(),
                       config_.maximum_clients, [this] { encoder_.request_keyframe(); },
                       [this] { encoder_.request_keyframe(); }),
      client_acceptor_(
          context_.get_executor(),
          {asio::ip::make_address(config_.bind_address), config_.port},
          [this](asio::ip::tcp::socket socket) {
              client_registry_.start_session(std::move(socket));
          }) {}

ServerApp::~ServerApp() {
    raw_frame_queue_.close();
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
}

int ServerApp::run() {
    const auto endpoint = client_acceptor_.local_endpoint();
    std::cout << "listening_on=" << endpoint.address().to_string() << ':' << endpoint.port()
              << '\n'
              << "max_clients=" << config_.maximum_clients << '\n'
              << "capture_window_id=0x" << std::hex << config_.window_id << std::dec << '\n'
              << "video=" << config_.video_width << 'x' << config_.video_height << '@'
              << config_.frames_per_second << " codec=h264 transport=udp\n"
              << std::flush;

    encoder_thread_ = std::thread([this] { encoder_loop(); });
    client_registry_.start();
    client_acceptor_.start();
    start_source();
    signals_.async_wait([this](const std::error_code& error, const int signal) {
        if (!error) {
            std::cout << "shutdown_signal=" << signal << '\n';
            stop();
        }
    });

    context_.run();
    raw_frame_queue_.close();
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
    return 0;
}

void ServerApp::start_source() {
    if (source_started_ || stopping_) {
        return;
    }
    source_started_ = true;
    source_.start(
        [this](capture::RawFrame frame) {
            static_cast<void>(raw_frame_queue_.push(std::move(frame)));
        },
        [this] { raw_frame_queue_.close(); });
}

void ServerApp::encoder_loop() {
    try {
        capture::RawFrame frame;
        while (raw_frame_queue_.wait_pop(frame)) {
            auto encoded_frames = encoder_.encode(std::move(frame));
            for (auto& encoded : encoded_frames) {
                asio::post(context_,
                           [this, encoded = std::move(encoded)] { publish_frame(encoded); });
            }
        }
        for (auto& encoded : encoder_.flush()) {
            asio::post(context_,
                       [this, encoded = std::move(encoded)] { publish_frame(encoded); });
        }
    } catch (const std::exception& error) {
        const std::string message = error.what();
        asio::post(context_, [this, message] {
            std::cerr << "encoder_error=" << message << '\n';
            finish_stream();
        });
        return;
    }
    asio::post(context_, [this] { finish_stream(); });
}

void ServerApp::publish_frame(std::shared_ptr<const media::EncodedFrame> frame) {
    if (stream_finished_ || stopping_) {
        return;
    }
    static_cast<void>(broadcast_buffer_.publish(std::move(frame)));
    client_registry_.notify_data_available();
}

void ServerApp::finish_stream() {
    if (stream_finished_) {
        return;
    }
    stream_finished_ = true;
    broadcast_buffer_.finish();
    client_acceptor_.stop();
    client_registry_.finish_stream();
    std::error_code ignored;
    signals_.cancel(ignored);
    std::cout << "stream_finished frames_published=" << broadcast_buffer_.next_sequence()
              << " raw_frames_dropped=" << raw_frame_queue_.dropped_count() << '\n';
}

void ServerApp::stop() {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    source_.stop();
    raw_frame_queue_.close();
    client_acceptor_.stop();
    client_registry_.stop();
    if (!stream_finished_) {
        stream_finished_ = true;
        broadcast_buffer_.finish();
    }
    std::error_code ignored;
    signals_.cancel(ignored);
}

} // namespace cloud_stream::server
