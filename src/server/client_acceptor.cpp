#include "server/client_acceptor.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>

#include <iostream>
#include <stdexcept>
#include <utility>

namespace cloud_stream::server {

ClientAcceptor::ClientAcceptor(asio::any_io_executor executor,
                               const asio::ip::tcp::endpoint& endpoint,
                               AcceptHandler on_accept)
    : acceptor_(std::move(executor)), on_accept_(std::move(on_accept)) {
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(asio::socket_base::max_listen_connections);
}

void ClientAcceptor::start() {
    if (started_) {
        throw std::logic_error("client acceptor has already been started");
    }
    started_ = true;
    asio::co_spawn(acceptor_.get_executor(), accept_loop(), asio::detached);
}

void ClientAcceptor::stop() {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    std::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
}

asio::ip::tcp::endpoint ClientAcceptor::local_endpoint() const {
    return acceptor_.local_endpoint();
}

asio::awaitable<void> ClientAcceptor::accept_loop() {
    while (!stopping_) {
        std::error_code error;
        auto socket = co_await acceptor_.async_accept(
            asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            if (error != asio::error::operation_aborted && !stopping_) {
                std::cerr << "accept_error=" << error.message() << '\n';
            }
            co_return;
        }
        on_accept_(std::move(socket));
    }
}

} // namespace cloud_stream::server
