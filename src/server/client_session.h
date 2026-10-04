#pragma once

#include "pipeline/broadcast_buffer.h"
#include "protocol/stream_payloads.h"

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include <cstdint>
#include <functional>
#include <memory>

namespace cloud_stream::server {

using ClientId = std::uint64_t;

class ClientSession final : public std::enable_shared_from_this<ClientSession> {
public:
    using ReadyHandler = std::function<void(ClientId)>;
    using ClosedHandler = std::function<void(ClientId)>;
    using KeyframeRequestHandler = std::function<void()>;

    ClientSession(ClientId id, asio::ip::tcp::socket socket,
                  pipeline::BroadcastBuffer& broadcast_buffer,
                  protocol::StreamConfigPayload stream_config, ReadyHandler on_ready,
                  ClosedHandler on_closed, KeyframeRequestHandler on_keyframe_request);

    void start();
    void notify_data_available();
    void stop();

private:
    asio::awaitable<void> run();
    asio::awaitable<void> perform_handshake();
    asio::awaitable<void> send_stream();
    void arm_handshake_deadline();
    void complete();

    ClientId id_{0};
    asio::ip::tcp::socket socket_;
    asio::steady_timer data_available_;
    asio::steady_timer handshake_deadline_;
    pipeline::BroadcastBuffer& broadcast_buffer_;
    protocol::StreamConfigPayload stream_config_;
    ReadyHandler on_ready_;
    ClosedHandler on_closed_;
    KeyframeRequestHandler on_keyframe_request_;
    pipeline::StreamSequence next_stream_sequence_{0};
    std::uint32_t next_message_sequence_{0};
    std::uint32_t peer_max_payload_size_{0};
    bool waiting_for_keyframe_{true};
    bool ready_{false};
    bool stopping_{false};
    bool completed_{false};
};

} // namespace cloud_stream::server
