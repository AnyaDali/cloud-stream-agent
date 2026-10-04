#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>

#include <functional>

namespace cloud_stream::server {

class ClientAcceptor {
public:
    using AcceptHandler = std::function<void(asio::ip::tcp::socket)>;

    ClientAcceptor(asio::any_io_executor executor, const asio::ip::tcp::endpoint& endpoint,
                   AcceptHandler on_accept);

    void start();
    void stop();
    [[nodiscard]] asio::ip::tcp::endpoint local_endpoint() const;

private:
    asio::awaitable<void> accept_loop();

    asio::ip::tcp::acceptor acceptor_;
    AcceptHandler on_accept_;
    bool started_{false};
    bool stopping_{false};
};

} // namespace cloud_stream::server
