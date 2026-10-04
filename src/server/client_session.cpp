#include "server/client_session.h"

#include "net/tcp_message_stream.h"
#include "protocol/udp_datagram.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace cloud_stream::server {
namespace {

[[nodiscard]] std::uint64_t make_session_id() {
    std::random_device random;
    const auto high = static_cast<std::uint64_t>(random()) << 32U;
    const auto low = static_cast<std::uint64_t>(random());
    const auto result = high | low;
    return result == 0 ? 1 : result;
}

} // namespace

ClientSession::ClientSession(ClientId id, asio::ip::tcp::socket socket,
                             pipeline::BroadcastBuffer& broadcast_buffer,
                             protocol::StreamConfigPayload stream_config, ReadyHandler on_ready,
                             ClosedHandler on_closed,
                             KeyframeRequestHandler on_keyframe_request)
    : id_(id), session_id_(make_session_id()), socket_(std::move(socket)),
      udp_socket_(socket_.get_executor()), data_available_(socket_.get_executor()),
      handshake_deadline_(socket_.get_executor()), udp_pacing_(socket_.get_executor()),
      broadcast_buffer_(broadcast_buffer),
      stream_config_(std::move(stream_config)), on_ready_(std::move(on_ready)),
      on_closed_(std::move(on_closed)),
      on_keyframe_request_(std::move(on_keyframe_request)),
      packetizer_(session_id_, protocol::kDefaultMaximumDatagramSize) {
    std::random_device random;
    for (auto& byte : probe_token_) {
        byte = static_cast<std::uint8_t>(random());
    }
}

void ClientSession::start() {
    auto self = shared_from_this();
    asio::co_spawn(
        socket_.get_executor(),
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
    static_cast<void>(udp_pacing_.cancel());
    udp_socket_.cancel(ignored);
    udp_socket_.close(ignored);
    socket_.cancel(ignored);
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
}

asio::awaitable<void> ClientSession::run() {
    try {
        net::configure_socket(socket_);
        co_await perform_handshake();
        using namespace asio::experimental::awaitable_operators;
        static_cast<void>(co_await (send_stream_guarded() || monitor_control_guarded()));
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
        (client_hello->capabilities & protocol::capability_h264_annex_b) == 0) {
        throw std::runtime_error("client does not support H264 Annex B");
    }
    peer_max_payload_size_ = client_hello->max_payload_size;

    const auto server_hello = protocol::encode_hello({
        .role = protocol::PeerRole::server,
        .capabilities = protocol::capability_h264_annex_b,
        .max_payload_size = protocol::kMaxPayloadSize,
    });
    const auto local_tcp_endpoint = socket_.local_endpoint();
    udp_socket_.open(local_tcp_endpoint.protocol() == asio::ip::tcp::v6()
                         ? asio::ip::udp::v6()
                         : asio::ip::udp::v4());
    udp_socket_.set_option(asio::socket_base::send_buffer_size(4 * 1024 * 1024));
    udp_socket_.bind({local_tcp_endpoint.address(), 0});
    const auto udp_config = protocol::encode_udp_config({
        .session_id = session_id_,
        .probe_token = probe_token_,
        .server_port = udp_socket_.local_endpoint().port(),
        .maximum_datagram_size = protocol::kDefaultMaximumDatagramSize,
        .key_epoch = protocol::kInitialKeyEpoch,
    });
    const auto stream_config = protocol::encode_stream_config(stream_config_);
    if (!server_hello || !udp_config || !stream_config) {
        throw std::logic_error("failed to encode server handshake");
    }
    if (udp_config->size() > peer_max_payload_size_ ||
        stream_config->size() > peer_max_payload_size_) {
        throw std::runtime_error("STREAM_CONFIG exceeds negotiated payload limit");
    }

    co_await net::async_send_message(socket_, protocol::MessageType::hello, 0,
                                     next_message_sequence_++, *server_hello);
    co_await net::async_send_message(socket_, protocol::MessageType::udp_config, 0,
                                     next_message_sequence_++, *udp_config);
    co_await wait_for_udp_probe();
    co_await net::async_send_message(socket_, protocol::MessageType::udp_ready, 0,
                                     next_message_sequence_++, {});
    co_await net::async_send_message(socket_, protocol::MessageType::stream_config, 0,
                                     next_message_sequence_++, *stream_config);

    static_cast<void>(handshake_deadline_.cancel());
    next_stream_sequence_ = broadcast_buffer_.start_sequence_for_new_reader();
    waiting_for_keyframe_ = true;
    ready_ = true;
    on_ready_(id_);
    std::cout << "client_id=" << id_ << " state=streaming transport=udp session_id="
              << session_id_ << '\n';
}

asio::awaitable<void> ClientSession::wait_for_udp_probe() {
    std::array<std::uint8_t, protocol::kUdpProbeSize> bytes{};
    const auto tcp_peer = socket_.remote_endpoint().address();
    while (!stopping_) {
        asio::ip::udp::endpoint sender;
        const auto received = co_await udp_socket_.async_receive_from(asio::buffer(bytes), sender,
                                                                      asio::use_awaitable);
        const auto probe = protocol::decode_udp_probe(
            std::span<const std::uint8_t>(bytes.data(), received));
        if (probe && probe->session_id == session_id_ && probe->token == probe_token_ &&
            sender.address() == tcp_peer) {
            client_udp_endpoint_ = sender;
            co_return;
        }
    }
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
            const auto datagrams = packetizer_.packetize(*result.entry->frame);
            for (std::size_t index = 0; index < datagrams.size(); ++index) {
                const auto& datagram = datagrams[index];
                const auto sent = co_await udp_socket_.async_send_to(
                    asio::buffer(datagram), client_udp_endpoint_, asio::use_awaitable);
                if (sent != datagram.size()) {
                    throw std::runtime_error("partial UDP datagram send");
                }
                constexpr std::size_t kDatagramsPerBurst = 16;
                if ((index + 1U) % kDatagramsPerBurst == 0 && index + 1U < datagrams.size()) {
                    udp_pacing_.expires_after(std::chrono::milliseconds(1));
                    co_await udp_pacing_.async_wait(asio::use_awaitable);
                }
            }
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

asio::awaitable<void> ClientSession::monitor_control() {
    std::uint32_t expected_sequence = 1;
    while (!stopping_) {
        const auto message = co_await net::async_receive_message(socket_);
        if (message.header.sequence != expected_sequence) {
            throw std::runtime_error("unexpected client control sequence");
        }
        if (expected_sequence == std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("client control sequence would wrap");
        }
        ++expected_sequence;
        if (message.header.type == protocol::MessageType::request_keyframe &&
            message.payload.empty()) {
            waiting_for_keyframe_ = true;
            on_keyframe_request_();
            continue;
        }
        throw std::runtime_error("unsupported client control message");
    }
}

asio::awaitable<void> ClientSession::send_stream_guarded() {
    try {
        co_await send_stream();
    } catch (const std::system_error& error) {
        if (!stopping_ && error.code() != asio::error::operation_aborted) {
            std::cerr << "client_id=" << id_ << " video_error=" << error.what() << '\n';
        }
    } catch (const std::exception& error) {
        if (!stopping_) {
            std::cerr << "client_id=" << id_ << " video_error=" << error.what() << '\n';
        }
    }
}

asio::awaitable<void> ClientSession::monitor_control_guarded() {
    try {
        co_await monitor_control();
    } catch (const std::system_error& error) {
        if (!stopping_ && error.code() != asio::error::eof &&
            error.code() != asio::error::connection_reset &&
            error.code() != asio::error::operation_aborted) {
            std::cerr << "client_id=" << id_ << " control_error=" << error.what() << '\n';
        }
    } catch (const std::exception& error) {
        if (!stopping_) {
            std::cerr << "client_id=" << id_ << " control_error=" << error.what() << '\n';
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
