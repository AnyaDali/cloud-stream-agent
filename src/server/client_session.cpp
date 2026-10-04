#include "server/client_session.h"

#include "net/tcp_message_stream.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace cloud_stream::server {

ClientSession::ClientSession(ClientId id, asio::any_io_executor executor,
                             asio::ip::tcp::socket socket,
                             pipeline::BroadcastBuffer& broadcast_buffer,
                             protocol::StreamConfigPayload stream_config, ReadyHandler on_ready,
                             ClosedHandler on_closed,
                             KeyframeRequestHandler on_keyframe_request)
    : id_(id), executor_(std::move(executor)), socket_(std::move(socket)),
      data_available_(executor_), handshake_deadline_(executor_),
      broadcast_buffer_(broadcast_buffer),
      stream_config_(std::move(stream_config)), on_ready_(std::move(on_ready)),
      on_closed_(std::move(on_closed)),
      on_keyframe_request_(std::move(on_keyframe_request)) {}

void ClientSession::start() {
    auto self = shared_from_this();
    asio::co_spawn(
        executor_,
        [self]() -> asio::awaitable<void> {
            co_await self->run();
        },
        asio::detached);
}

void ClientSession::notify_data_available() {
    if (!ready_) {
        return;
    }
    static_cast<void>(data_available_.cancel_one());
}

void ClientSession::stop() {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    std::error_code ignored;
    static_cast<void>(data_available_.cancel());
    static_cast<void>(handshake_deadline_.cancel());
    socket_.cancel(ignored);
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
}

asio::awaitable<void> ClientSession::run() {
    try {
        net::configure_socket(socket_);
        co_await perform_handshake();
        co_await send_stream();
    } catch (const std::exception& error) {
        if (!stopping_) {
            std::cerr << "client_id=" << id_ << " error=" << error.what() << '\n';
        }
    }
    stop();
    complete();
}

asio::awaitable<void> ClientSession::perform_handshake() {
    arm_handshake_deadline();
    const auto client_hello_message = co_await net::async_receive_message(socket_);
    if (client_hello_message.header.sequence != 0 ||
        client_hello_message.header.type != protocol::MessageType::hello) {
        throw std::runtime_error("expected client HELLO with sequence 0");
    }

    const auto client_hello = protocol::decode_hello(client_hello_message.payload);
    if (!client_hello || client_hello->role != protocol::PeerRole::client ||
        (client_hello->capabilities & protocol::capability_raw_rgb24) == 0) {
        throw std::runtime_error("client does not support RAW_RGB24");
    }
    peer_max_payload_size_ = client_hello->max_payload_size;

    const auto server_hello = protocol::encode_hello({
        .role = protocol::PeerRole::server,
        .capabilities = protocol::capability_raw_rgb24,
        .max_payload_size = protocol::kMaxPayloadSize,
    });
    const auto stream_config = protocol::encode_stream_config(stream_config_);
    if (!server_hello || !stream_config) {
        throw std::logic_error("failed to encode server handshake");
    }
    if (stream_config->size() > peer_max_payload_size_) {
        throw std::runtime_error("STREAM_CONFIG exceeds negotiated payload limit");
    }

    co_await net::async_send_message(socket_, protocol::MessageType::hello, 0,
                                     next_message_sequence_++, *server_hello);
    co_await net::async_send_message(socket_, protocol::MessageType::stream_config, 0,
                                     next_message_sequence_++, *stream_config);

    static_cast<void>(handshake_deadline_.cancel());
    next_stream_sequence_ = broadcast_buffer_.start_sequence_for_new_reader();
    waiting_for_keyframe_ = true;
    ready_ = true;
    on_ready_(id_);
    std::cout << "client_id=" << id_ << " state=streaming\n";
}

asio::awaitable<void> ClientSession::send_stream() {
    while (!stopping_) {
        const auto result = broadcast_buffer_.read(next_stream_sequence_);
        if (result.status == pipeline::BroadcastReadResult::Status::lagged) {
            next_stream_sequence_ = result.first_available_sequence;
            waiting_for_keyframe_ = true;
            on_keyframe_request_();
            continue;
        }
        if (result.status == pipeline::BroadcastReadResult::Status::available) {
            if (!result.entry || !result.entry->frame || !result.entry->frame->payload) {
                throw std::logic_error("broadcast buffer returned an invalid entry");
            }
            if (waiting_for_keyframe_ && !result.entry->frame->keyframe) {
                ++next_stream_sequence_;
                continue;
            }
            waiting_for_keyframe_ = false;
            if (result.entry->frame->payload->size() > peer_max_payload_size_) {
                throw std::runtime_error("VIDEO_PACKET exceeds negotiated payload limit");
            }
            if (next_message_sequence_ == std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("server message sequence would wrap");
            }

            co_await net::async_send_message(socket_, protocol::MessageType::video_packet, 0,
                                             next_message_sequence_++,
                                             *result.entry->frame->payload);
            next_stream_sequence_ = result.entry->sequence + 1;
            continue;
        }

        if (broadcast_buffer_.finished()) {
            co_await net::async_send_message(socket_, protocol::MessageType::end, 0,
                                             next_message_sequence_, {});
            co_return;
        }

        data_available_.expires_at(std::chrono::steady_clock::time_point::max());
        std::error_code error;
        co_await data_available_.async_wait(asio::redirect_error(asio::use_awaitable, error));
        if (error && error != asio::error::operation_aborted) {
            throw std::system_error(error);
        }
    }
}

void ClientSession::arm_handshake_deadline() {
    handshake_deadline_.expires_after(std::chrono::seconds(10));
    const auto weak = weak_from_this();
    handshake_deadline_.async_wait([weak](const std::error_code& error) {
        if (error) {
            return;
        }
        if (const auto self = weak.lock()) {
            self->stop();
        }
    });
}

void ClientSession::complete() {
    if (completed_) {
        return;
    }
    completed_ = true;
    on_closed_(id_);
}

} // namespace cloud_stream::server
