#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cloud_stream::capture::windows {

struct WindowInfo {
    std::uintptr_t id{0};
    std::string title;
    std::string process_name;
};

[[nodiscard]] std::vector<WindowInfo> enumerate_capturable_windows();
[[nodiscard]] std::optional<WindowInfo>
find_window_by_id(std::uintptr_t id, const std::vector<WindowInfo>& windows);
[[nodiscard]] std::vector<WindowInfo>
find_windows_by_title(std::string_view title, const std::vector<WindowInfo>& windows);

} // namespace cloud_stream::capture::windows
