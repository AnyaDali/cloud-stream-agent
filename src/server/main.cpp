#include "server/server_app.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

template <typename Value> Value parse_number(const char* text, const char* name) {
    Value result{};
    const std::string_view input{text};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), result);
    if (error != std::errc{} || end != input.data() + input.size()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return result;
}

cloud_stream::server::ServerConfig parse_config(const int argc, char** argv) {
    cloud_stream::server::ServerConfig config;
    config.port = argc > 1 ? parse_number<std::uint16_t>(argv[1], "port") : config.port;
    config.frame_count =
        argc > 2 ? parse_number<std::uint32_t>(argv[2], "frame count") : config.frame_count;
    config.interval_ms =
        argc > 3 ? parse_number<std::uint32_t>(argv[3], "frame interval") : config.interval_ms;
    config.bind_address = argc > 4 ? argv[4] : config.bind_address;
    config.maximum_clients =
        argc > 5 ? parse_number<std::size_t>(argv[5], "maximum clients")
                 : config.maximum_clients;

    if (config.frame_count == 0 ||
        config.frame_count > std::numeric_limits<std::uint32_t>::max() - 2U) {
        throw std::invalid_argument("frame count must be in range [1, UINT32_MAX - 2]");
    }
    if (config.maximum_clients == 0 || config.maximum_clients > 1024) {
        throw std::invalid_argument("maximum clients must be in range [1, 1024]");
    }
    if (config.interval_ms != 0 &&
        static_cast<std::uint64_t>(config.frame_count - 1U) >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                config.interval_ms) {
        throw std::invalid_argument("frame timestamps exceed int64 range");
    }
    return config;
}

} // namespace

int main(int argc, char** argv) {
    try {
        cloud_stream::server::ServerApp app(parse_config(argc, argv));
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "stream-server error: " << error.what() << '\n';
        return 1;
    }
}
