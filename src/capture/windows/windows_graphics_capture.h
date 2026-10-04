#pragma once

#include "capture/raw_frame.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace cloud_stream::capture::windows {

struct WindowsGraphicsCaptureConfig {
    std::uintptr_t window_id{0};
    std::uint32_t maximum_frames_per_second{30};
};

class WindowsGraphicsCaptureSource {
public:
    using FrameHandler = std::function<void(RawFrame)>;
    using CompletionHandler = std::function<void()>;

    explicit WindowsGraphicsCaptureSource(WindowsGraphicsCaptureConfig config);
    ~WindowsGraphicsCaptureSource();

    WindowsGraphicsCaptureSource(const WindowsGraphicsCaptureSource&) = delete;
    WindowsGraphicsCaptureSource& operator=(const WindowsGraphicsCaptureSource&) = delete;

    void start(FrameHandler on_frame, CompletionHandler on_complete);
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cloud_stream::capture::windows
