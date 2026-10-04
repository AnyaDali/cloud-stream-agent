#pragma once

#include "pipeline/broadcast_buffer.h"
#include "protocol/stream_payloads.h"
#include "server/client_session.h"

#include <asio/any_io_executor.hpp>
#include <asio/ip/tcp.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>

namespace cloud_stream::server {

class ClientRegistry {
public:
    using FirstReadyHandler = std::function<void()>;
    using KeyframeRequestHandler = std::function<void()>;

    ClientRegistry(asio::any_io_executor executor, pipeline::BroadcastBuffer& broadcast_buffer,
                   protocol::StreamConfigPayload stream_config, std::size_t maximum_clients,
                   FirstReadyHandler on_first_ready,
                   KeyframeRequestHandler on_keyframe_request);

    void start_session(asio::ip::tcp::socket socket);
    void notify_data_available();
    void stop();

    [[nodiscard]] std::size_t size() const noexcept;

private:
    void handle_ready(ClientId id);
    void handle_closed(ClientId id);

    asio::any_io_executor executor_;
    pipeline::BroadcastBuffer& broadcast_buffer_;
    protocol::StreamConfigPayload stream_config_;
    std::size_t maximum_clients_{0};
    FirstReadyHandler on_first_ready_;
    KeyframeRequestHandler on_keyframe_request_;
    std::unordered_map<ClientId, std::shared_ptr<ClientSession>> sessions_;
    ClientId next_client_id_{1};
    bool first_client_ready_{false};
    bool stopping_{false};
};

} // namespace cloud_stream::server
