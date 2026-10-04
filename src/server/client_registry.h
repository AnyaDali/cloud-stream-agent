#pragma once

#include "pipeline/broadcast_buffer.h"
#include "protocol/stream_payloads.h"
#include "server/client_session.h"

#include <asio/any_io_executor.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/strand.hpp>

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

    [[nodiscard]] asio::any_io_executor executor() const;
    void start_session(asio::ip::tcp::socket socket);
    void publish_frame(std::shared_ptr<const media::EncodedFrame> frame);
    void finish_stream();
    void stop();

private:
    void do_start_session(asio::ip::tcp::socket socket);
    void do_publish_frame(std::shared_ptr<const media::EncodedFrame> frame);
    void do_finish_stream();
    void do_stop();
    void notify_data_available();
    void handle_ready(ClientId id);
    void handle_closed(ClientId id);

    asio::strand<asio::any_io_executor> strand_;
    pipeline::BroadcastBuffer& broadcast_buffer_;
    protocol::StreamConfigPayload stream_config_;
    std::size_t maximum_clients_{0};
    FirstReadyHandler on_first_ready_;
    KeyframeRequestHandler on_keyframe_request_;
    std::unordered_map<ClientId, std::shared_ptr<ClientSession>> sessions_;
    ClientId next_client_id_{1};
    bool first_client_ready_{false};
    bool stream_finished_{false};
    bool stopping_{false};
};

} // namespace cloud_stream::server
