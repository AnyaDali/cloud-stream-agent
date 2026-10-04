#include "server/client_registry.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>

#include <iostream>
#include <stdexcept>
#include <utility>

namespace cloud_stream::server {

ClientRegistry::ClientRegistry(asio::any_io_executor executor,
                               pipeline::BroadcastBuffer& broadcast_buffer,
                               protocol::StreamConfigPayload stream_config,
                               const std::size_t maximum_clients,
                               FirstReadyHandler on_first_ready,
                               KeyframeRequestHandler on_keyframe_request)
    : executor_(std::move(executor)), events_(executor_, maximum_clients),
      broadcast_buffer_(broadcast_buffer),
      stream_config_(std::move(stream_config)), maximum_clients_(maximum_clients),
      on_first_ready_(std::move(on_first_ready)),
      on_keyframe_request_(std::move(on_keyframe_request)) {
    if (maximum_clients_ == 0) {
        throw std::invalid_argument("maximum client count must be positive");
    }
}

void ClientRegistry::start() {
    if (started_) {
        throw std::logic_error("client registry has already been started");
    }
    started_ = true;
    asio::co_spawn(executor_, event_loop(), asio::detached);
}

void ClientRegistry::start_session(asio::ip::tcp::socket socket) {
    post_event(ClientConnected{.socket = std::move(socket)});
}

void ClientRegistry::notify_data_available() {
    post_event(DataAvailable{});
}

void ClientRegistry::finish_stream() {
    post_event(StreamFinished{});
}

void ClientRegistry::stop() {
    post_event(StopRequested{});
}

void ClientRegistry::post_event(ClientEvent event) {
    events_.async_send(std::error_code{}, std::move(event), asio::detached);
}

asio::awaitable<void> ClientRegistry::event_loop() {
    state_ = State::running;
    while (state_ != State::stopped) {
        auto event = co_await events_.async_receive(asio::use_awaitable);
        std::visit(
            [this](auto&& value) {
                handle_event(std::forward<decltype(value)>(value));
            },
            std::move(event));

        if (should_exit()) {
            state_ = State::stopped;
            events_.close();
        }
    }
}

void ClientRegistry::handle_event(ClientConnected event) {
    if (state_ != State::running || sessions_.size() >= maximum_clients_) {
        std::error_code ignored;
        event.socket.close(ignored);
        const auto* reason = state_ == State::running ? "capacity" : "stopping";
        std::cerr << "client_rejected reason=" << reason
                  << " active_clients=" << sessions_.size() << '\n';
        return;
    }

    const auto client_id = next_client_id_++;
    auto session = std::make_shared<ClientSession>(
        client_id, std::move(event.socket), broadcast_buffer_, stream_config_,
        [this](const ClientId id) { post_event(ClientReady{.id = id}); },
        [this](const ClientId id) { post_event(ClientDisconnected{.id = id}); },
        on_keyframe_request_);
    sessions_.emplace(client_id, session);
    std::cout << "client_id=" << client_id << " state=accepted active_clients=" << sessions_.size()
              << '\n';
    session->start();
}

void ClientRegistry::handle_event(const ClientReady event) {
    if (!sessions_.contains(event.id) || state_ != State::running) {
        return;
    }
    if (!first_client_ready_) {
        first_client_ready_ = true;
        on_first_ready_();
    }
}

void ClientRegistry::handle_event(const ClientDisconnected event) {
    sessions_.erase(event.id);
    std::cout << "client_id=" << event.id
              << " state=closed active_clients=" << sessions_.size() << '\n';
}

void ClientRegistry::handle_event(DataAvailable) {
    if (state_ == State::running || state_ == State::finishing) {
        wake_sessions();
    }
}

void ClientRegistry::handle_event(StreamFinished) {
    if (state_ != State::running) {
        return;
    }
    state_ = State::finishing;
    wake_sessions();
}

void ClientRegistry::handle_event(StopRequested) {
    if (state_ == State::stopping || state_ == State::stopped) {
        return;
    }
    state_ = State::stopping;
    for (const auto& [client_id, session] : sessions_) {
        static_cast<void>(client_id);
        session->stop();
    }
}

void ClientRegistry::wake_sessions() {
    for (const auto& [client_id, session] : sessions_) {
        static_cast<void>(client_id);
        session->notify_data_available();
    }
}

bool ClientRegistry::should_exit() const noexcept {
    return sessions_.empty() &&
           (state_ == State::finishing || state_ == State::stopping);
}

} // namespace cloud_stream::server
