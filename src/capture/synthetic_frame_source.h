#pragma once

#include "capture/raw_frame.h"

#include <asio/any_io_executor.hpp>
#include <asio/steady_timer.hpp>

#include <cstdint>
#include <functional>

namespace cloud_stream::capture {

struct SyntheticFrameSourceConfig {
    std::uint32_t width{320};
    std::uint32_t height{180};
    std::uint32_t frame_count{120};
    std::uint32_t interval_ms{33};
};

class SyntheticFrameSource {
public:
    using FrameHandler = std::function<void(RawFrame)>;
    using CompletionHandler = std::function<void()>;

    SyntheticFrameSource(asio::any_io_executor executor, SyntheticFrameSourceConfig config);

    void start(FrameHandler on_frame, CompletionHandler on_complete);
    void stop();

private:
    [[nodiscard]] RawFrame make_frame(std::uint64_t frame_id) const;

    asio::any_io_executor executor_;
    asio::steady_timer timer_;
    SyntheticFrameSourceConfig config_;
    bool started_{false};
    bool stopping_{false};
};

} // namespace cloud_stream::capture
