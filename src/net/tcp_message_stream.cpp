#include "net/tcp_message_stream.h"

#include <asio/async_result.hpp>
#include <asio/read.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <array>
#include <limits>
#include <stdexcept>

namespace cloud_stream::net {

void configure_socket(asio::ip::tcp::socket& socket) {
#if defined(_WIN32)
    constexpr DWORD kTimeoutMs = 10'000;
    const auto* timeout = reinterpret_cast<const char*>(&kTimeoutMs);
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, timeout,
                     sizeof(kTimeoutMs)) != 0 ||
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, timeout,
                     sizeof(kTimeoutMs)) != 0) {
        throw std::runtime_error("failed to configure socket timeout");
    }
#else
    static_cast<void>(socket);
#endif
}

void send_message(asio::ip::tcp::socket& socket, const protocol::MessageType type,
                  const std::uint16_t flags, const std::uint32_t sequence,
                  const std::span<const std::uint8_t> payload) {
    if (payload.size() > protocol::kMaxPayloadSize ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("message payload exceeds protocol limit");
    }

    const protocol::MessageHeader header{
        .type = type,
        .flags = flags,
        .payload_size = static_cast<std::uint32_t>(payload.size()),
        .sequence = sequence,
    };
    const auto encoded_header = protocol::encode_header(header);
    const std::array buffers{
        asio::buffer(encoded_header),
        asio::buffer(payload.data(), payload.size()),
    };
    asio::write(socket, buffers);
}

ReceivedMessage receive_message(asio::ip::tcp::socket& socket) {
    std::array<std::uint8_t, protocol::kHeaderSize> encoded_header{};
    asio::read(socket, asio::buffer(encoded_header));

    const auto header = protocol::decode_header(encoded_header);
    if (!header) {
        throw std::runtime_error("invalid protocol message header");
    }

    std::vector<std::uint8_t> payload(header->payload_size);
    if (!payload.empty()) {
        asio::read(socket, asio::buffer(payload));
    }
    return ReceivedMessage{.header = *header, .payload = std::move(payload)};
}

asio::awaitable<void> async_send_message(asio::ip::tcp::socket& socket,
                                         const protocol::MessageType type,
                                         const std::uint16_t flags,
                                         const std::uint32_t sequence,
                                         const std::span<const std::uint8_t> payload) {
    if (payload.size() > protocol::kMaxPayloadSize ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("message payload exceeds protocol limit");
    }

    const protocol::MessageHeader header{
        .type = type,
        .flags = flags,
        .payload_size = static_cast<std::uint32_t>(payload.size()),
        .sequence = sequence,
    };
    const auto encoded_header = protocol::encode_header(header);
    const std::array buffers{
        asio::buffer(encoded_header),
        asio::buffer(payload.data(), payload.size()),
    };
    co_await asio::async_write(socket, buffers, asio::use_awaitable);
}

asio::awaitable<ReceivedMessage> async_receive_message(asio::ip::tcp::socket& socket) {
    std::array<std::uint8_t, protocol::kHeaderSize> encoded_header{};
    co_await asio::async_read(socket, asio::buffer(encoded_header), asio::use_awaitable);

    const auto header = protocol::decode_header(encoded_header);
    if (!header) {
        throw std::runtime_error("invalid protocol message header");
    }

    std::vector<std::uint8_t> payload(header->payload_size);
    if (!payload.empty()) {
        co_await asio::async_read(socket, asio::buffer(payload), asio::use_awaitable);
    }
    co_return ReceivedMessage{.header = *header, .payload = std::move(payload)};
}

} // namespace cloud_stream::net
