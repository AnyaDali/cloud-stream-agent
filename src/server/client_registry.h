#pragma once

#include "pipeline/broadcast_buffer.h"
#include "protocol/stream_payloads.h"
#include "server/client_session.h"

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/ip/tcp.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <variant>

namespace cloud_stream::server {

class ClientRegistry {
public:
    using FirstReadyHandler = std::function<void()>;
    using KeyframeRequestHandler = std::function<void()>;

    ClientRegistry(asio::any_io_executor executor, pipeline::BroadcastBuffer& broadcast_buffer,
                   protocol::StreamConfigPayload stream_config, std::size_t maximum_clients,
                   FirstReadyHandler on_first_ready,
                   KeyframeRequestHandler on_keyframe_request);

    void start();
    void start_session(asio::ip::tcp::socket socket);
    void notify_data_available();
    void finish_stream();
    void stop();

private:
    struct ClientConnected {
        asio::ip::tcp::socket socket;
    };

    struct ClientReady {
        ClientId id{0};
    };

    struct ClientDisconnected {
        ClientId id{0};
    };

    struct DataAvailable {};
    struct StreamFinished {};
    struct StopRequested {};

    using ClientEvent = std::variant<DataAvailable, ClientConnected, ClientReady,
                                     ClientDisconnected, StreamFinished, StopRequested>;
    using EventChannel =
        asio::experimental::channel<asio::any_io_executor, void(std::error_code, ClientEvent)>;

    enum class State : std::uint8_t {
        created,
        running,
        finishing,
        stopping,
        stopped,
    };

    void post_event(ClientEvent event);
    asio::awaitable<void> event_loop();
    void handle_event(ClientConnected event);
    void handle_event(ClientReady event);
    void handle_event(ClientDisconnected event);
    void handle_event(DataAvailable event);
    void handle_event(StreamFinished event);
    void handle_event(StopRequested event);
    void wake_sessions();
    [[nodiscard]] bool should_exit() const noexcept;

    asio::any_io_executor executor_;
    EventChannel events_;
    pipeline::BroadcastBuffer& broadcast_buffer_;
    protocol::StreamConfigPayload stream_config_;
    std::size_t maximum_clients_{0};
    FirstReadyHandler on_first_ready_;
    KeyframeRequestHandler on_keyframe_request_;
    std::unordered_map<ClientId, std::shared_ptr<ClientSession>> sessions_;
    ClientId next_client_id_{1};
    bool first_client_ready_{false};
    bool started_{false};
    State state_{State::created};
};

} // namespace cloud_stream::server
