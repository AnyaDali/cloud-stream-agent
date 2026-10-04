#include "capture/windows/window_enumerator.h"
#include "server/server_app.h"

#include <winrt/base.h>

#include <charconv>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct CommandLine {
    cloud_stream::server::ServerConfig config;
    std::optional<std::uintptr_t> window_id;
    std::optional<std::string> window_title;
    bool list_windows{false};
    bool show_help{false};
};

template <typename Value> Value parse_number(const std::string_view input, const char* name) {
    Value result{};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), result);
    if (error != std::errc{} || end != input.data() + input.size()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return result;
}

[[nodiscard]] std::uintptr_t parse_window_id(const std::string& input) {
    std::size_t parsed = 0;
    const auto value = std::stoull(input, &parsed, 0);
    if (parsed != input.size()) {
        throw std::invalid_argument("invalid window id");
    }
    return static_cast<std::uintptr_t>(value);
}

[[nodiscard]] CommandLine parse_command_line(const int argc, char** argv) {
    CommandLine result;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--list-windows") {
            result.list_windows = true;
            continue;
        }
        if (option == "--help" || option == "-h") {
            result.show_help = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + option);
        }
        const std::string value = argv[++index];
        if (option == "--window-id") {
            result.window_id = parse_window_id(value);
        } else if (option == "--window-title") {
            result.window_title = value;
        } else if (option == "--tcp-port") {
            result.config.port = parse_number<std::uint16_t>(value, "TCP port");
        } else if (option == "--bind") {
            result.config.bind_address = value;
        } else if (option == "--max-clients") {
            result.config.maximum_clients = parse_number<std::size_t>(value, "maximum clients");
        } else if (option == "--width") {
            result.config.video_width = parse_number<std::uint16_t>(value, "video width");
        } else if (option == "--height") {
            result.config.video_height = parse_number<std::uint16_t>(value, "video height");
        } else if (option == "--fps") {
            result.config.frames_per_second = parse_number<std::uint32_t>(value, "FPS");
        } else if (option == "--bitrate-kbps") {
            result.config.bitrate_kbps = parse_number<std::uint32_t>(value, "bitrate");
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (result.window_id && result.window_title) {
        throw std::invalid_argument("use either --window-id or --window-title");
    }
    if (result.config.port == 0 || result.config.maximum_clients == 0 ||
        result.config.maximum_clients > 1024 || result.config.video_width == 0 ||
        result.config.video_height == 0 || result.config.video_width % 2U != 0 ||
        result.config.video_height % 2U != 0 || result.config.frames_per_second == 0 ||
        result.config.frames_per_second > 240 || result.config.bitrate_kbps == 0) {
        throw std::invalid_argument("server configuration contains an out-of-range value");
    }
    return result;
}

void print_usage() {
    std::cout
        << "Usage:\n"
        << "  stream-server --list-windows\n"
        << "  stream-server (--window-id ID | --window-title TEXT) [options]\n\n"
        << "Options: --tcp-port 9010 --bind 127.0.0.1 --max-clients 8\n"
        << "         --width 1280 --height 720 --fps 30 --bitrate-kbps 4000\n";
}

void print_windows(const std::vector<cloud_stream::capture::windows::WindowInfo>& windows) {
    for (const auto& window : windows) {
        std::cout << "id=0x" << std::hex << window.id << std::dec << " process="
                  << window.process_name << " title=" << window.title << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        auto command_line = parse_command_line(argc, argv);
        if (command_line.show_help) {
            print_usage();
            return 0;
        }
        const auto windows = cloud_stream::capture::windows::enumerate_capturable_windows();
        if (command_line.list_windows) {
            print_windows(windows);
            return 0;
        }

        if (command_line.window_id) {
            const auto selected = cloud_stream::capture::windows::find_window_by_id(
                *command_line.window_id, windows);
            if (!selected) {
                throw std::runtime_error("window id was not found; run --list-windows again");
            }
            command_line.config.window_id = selected->id;
            std::cout << "selected_window=" << selected->title << '\n';
        } else if (command_line.window_title) {
            const auto matches = cloud_stream::capture::windows::find_windows_by_title(
                *command_line.window_title, windows);
            if (matches.empty()) {
                throw std::runtime_error("window title was not found; run --list-windows");
            }
            if (matches.size() != 1) {
                std::cerr << "window title is ambiguous:\n";
                print_windows(matches);
                throw std::runtime_error("select one window with --window-id");
            }
            command_line.config.window_id = matches.front().id;
            std::cout << "selected_window=" << matches.front().title << '\n';
        } else {
            print_usage();
            throw std::invalid_argument("explicit window selection is required");
        }

        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        cloud_stream::server::ServerApp app(std::move(command_line.config));
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "stream-server error: " << error.what() << '\n';
        return 1;
    }
}
