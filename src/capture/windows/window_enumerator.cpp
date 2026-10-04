#include "capture/windows/window_enumerator.h"

#include <dwmapi.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <string>

namespace cloud_stream::capture::windows {
namespace {

[[nodiscard]] std::string to_utf8(const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), result.data(), size, nullptr,
                              nullptr) != size) {
        return {};
    }
    return result;
}

[[nodiscard]] std::string process_name(const HWND window) {
    DWORD process_id = 0;
    ::GetWindowThreadProcessId(window, &process_id);
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return {};
    }
    std::wstring path(32768, L'\0');
    DWORD path_size = static_cast<DWORD>(path.size());
    const bool found = ::QueryFullProcessImageNameW(process, 0, path.data(), &path_size) != FALSE;
    ::CloseHandle(process);
    if (!found) {
        return {};
    }
    path.resize(path_size);
    return to_utf8(std::filesystem::path(path).filename().wstring());
}

BOOL CALLBACK collect_window(const HWND window, const LPARAM parameter) {
    auto& result = *reinterpret_cast<std::vector<WindowInfo>*>(parameter);
    if (!::IsWindowVisible(window) || ::GetWindowTextLengthW(window) == 0) {
        return TRUE;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return TRUE;
    }

    std::wstring title(static_cast<std::size_t>(::GetWindowTextLengthW(window)) + 1U, L'\0');
    const int copied = ::GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (copied <= 0) {
        return TRUE;
    }
    title.resize(static_cast<std::size_t>(copied));
    auto utf8_title = to_utf8(title);
    if (utf8_title.empty()) {
        return TRUE;
    }
    result.push_back({
        .id = reinterpret_cast<std::uintptr_t>(window),
        .title = std::move(utf8_title),
        .process_name = process_name(window),
    });
    return TRUE;
}

[[nodiscard]] std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char symbol) {
        return static_cast<char>(std::tolower(symbol));
    });
    return result;
}

} // namespace

std::vector<WindowInfo> enumerate_capturable_windows() {
    std::vector<WindowInfo> result;
    ::EnumWindows(collect_window, reinterpret_cast<LPARAM>(&result));
    std::sort(result.begin(), result.end(), [](const WindowInfo& left, const WindowInfo& right) {
        return left.title < right.title;
    });
    return result;
}

std::optional<WindowInfo> find_window_by_id(const std::uintptr_t id,
                                            const std::vector<WindowInfo>& windows) {
    const auto found = std::find_if(windows.begin(), windows.end(),
                                    [id](const WindowInfo& value) { return value.id == id; });
    return found == windows.end() ? std::nullopt : std::optional{*found};
}

std::vector<WindowInfo> find_windows_by_title(const std::string_view title,
                                              const std::vector<WindowInfo>& windows) {
    const auto needle = lower_ascii(title);
    std::vector<WindowInfo> result;
    std::copy_if(windows.begin(), windows.end(), std::back_inserter(result),
                 [&needle](const WindowInfo& value) {
                     return lower_ascii(value.title).find(needle) != std::string::npos;
                 });
    return result;
}

} // namespace cloud_stream::capture::windows
