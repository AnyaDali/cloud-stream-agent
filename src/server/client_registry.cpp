#include "server/client_registry.h"

#include <asio/post.hpp>

#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cloud_stream::server {

ClientRegistry::ClientRegistry(asio::any_io_executor executor,
                               pipeline::BroadcastBuffer& broadcast_buffer,
                               protocol::StreamConfigPayload stream_config,
                               const std::size_t maximum_clients,
                               FirstReadyHandler on_first_ready,
                               KeyframeRequestHandler on_keyframe_request)
    : strand_(asio::make_strand(std::move(executor))), broadcast_buffer_(broadcast_buffer),
      stream_config_(std::move(stream_config)), maximum_clients_(maximum_clients),
      on_first_ready_(std::move(on_first_ready)),
      on_keyframe_request_(std::move(on_keyframe_request)) {
    if (maximum_clients_ == 0) {
        throw std::invalid_argument("maximum client count must be positive");
    }
}

asio::any_io_executor ClientRegistry::executor() const {
    return strand_;
}

void ClientRegistry::start_session(asio::ip::tcp::socket socket) {
    asio::post(strand_, [this, socket = std::move(socket)]() mutable {
        do_start_session(std::move(socket));
    });
}

void ClientRegistry::publish_frame(std::shared_ptr<const media::EncodedFrame> frame) {
    asio::post(strand_,
               [this, frame = std::move(frame)]() mutable { do_publish_frame(std::move(frame)); });
}

void ClientRegistry::finish_stream() {
    asio::post(strand_, [this] { do_finish_stream(); });
}

void ClientRegistry::stop() {
    asio::post(strand_, [this] { do_stop(); });
}

void ClientRegistry::do_start_session(asio::ip::tcp::socket socket) {
    if (stopping_ || stream_finished_ || sessions_.size() >= maximum_clients_) {
        std::error_code ignored;
        socket.close(ignored);
        const auto* reason = stopping_ || stream_finished_ ? "stopping" : "capacity";
        std::cerr << "client_rejected reason=" << reason
                  << " active_clients=" << sessions_.size() << '\n';
        return;
    }

    const auto client_id = next_client_id_++;
    auto session = std::make_shared<ClientSession>(
        client_id, strand_, std::move(socket), broadcast_buffer_, stream_config_,
        [this](const ClientId id) { handle_ready(id); },
        [this](const ClientId id) { handle_closed(id); }, on_keyframe_request_);
    sessions_.emplace(client_id, session);
    std::cout << "client_id=" << client_id << " state=accepted active_clients=" << sessions_.size()
              << '\n';
    session->start();
}

void ClientRegistry::do_publish_frame(std::shared_ptr<const media::EncodedFrame> frame) {
    if (stopping_ || stream_finished_) {
        return;
    }
    static_cast<void>(broadcast_buffer_.publish(std::move(frame)));
    notify_data_available();
}

void ClientRegistry::do_finish_stream() {
    if (stopping_ || stream_finished_) {
        return;
    }
    stream_finished_ = true;
    broadcast_buffer_.finish();
    notify_data_available();
}

void ClientRegistry::notify_data_available() {
    for (const auto& [client_id, session] : sessions_) {
        static_cast<void>(client_id);
        session->notify_data_available();
    }
}

void ClientRegistry::do_stop() {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    if (!stream_finished_) {
        stream_finished_ = true;
        broadcast_buffer_.finish();
    }
    std::vector<std::shared_ptr<ClientSession>> sessions;
    sessions.reserve(sessions_.size());
    for (const auto& [client_id, session] : sessions_) {
        static_cast<void>(client_id);
        sessions.push_back(session);
    }
    for (const auto& session : sessions) {
        session->stop();
    }
}

void ClientRegistry::handle_ready(const ClientId id) {
    if (!sessions_.contains(id)) {
        return;
    }
    if (!first_client_ready_) {
        first_client_ready_ = true;
        on_first_ready_();
    }
}

void ClientRegistry::handle_closed(const ClientId id) {
    sessions_.erase(id);
    std::cout << "client_id=" << id << " state=closed active_clients=" << sessions_.size() << '\n';
}

} // namespace cloud_stream::server
