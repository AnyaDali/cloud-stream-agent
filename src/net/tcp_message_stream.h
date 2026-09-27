#pragma once

#include "protocol/message_header.h"

#include <asio/ip/tcp.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace cloud_stream::net {

struct ReceivedMessage {
    protocol::MessageHeader header;
    std::vector<std::uint8_t> payload;
};

void configure_socket(asio::ip::tcp::socket& socket);

void send_message(asio::ip::tcp::socket& socket, protocol::MessageType type, std::uint16_t flags,
                  std::uint32_t sequence, std::span<const std::uint8_t> payload);

[[nodiscard]] ReceivedMessage receive_message(asio::ip::tcp::socket& socket);

} // namespace cloud_stream::net
