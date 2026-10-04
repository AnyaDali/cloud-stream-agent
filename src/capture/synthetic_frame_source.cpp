#include "capture/synthetic_frame_source.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace cloud_stream::capture {

SyntheticFrameSource::SyntheticFrameSource(asio::any_io_executor executor,
                                           const SyntheticFrameSourceConfig config)
    : executor_(std::move(executor)), timer_(executor_), config_(config) {
    if (config_.width == 0 || config_.height == 0 || config_.frame_count == 0) {
        throw std::invalid_argument("synthetic frame source dimensions and frame count must be positive");
    }
}

void SyntheticFrameSource::start(FrameHandler on_frame, CompletionHandler on_complete) {
    if (started_) {
        throw std::logic_error("synthetic frame source has already been started");
    }
    started_ = true;

    asio::co_spawn(
        executor_,
        [this, on_frame = std::move(on_frame), on_complete = std::move(on_complete)]()
            -> asio::awaitable<void> {
            for (std::uint64_t frame_id = 0; frame_id < config_.frame_count && !stopping_;
                 ++frame_id) {
                on_frame(make_frame(frame_id));
                if (config_.interval_ms == 0 || frame_id + 1 == config_.frame_count) {
                    continue;
                }

                timer_.expires_after(std::chrono::milliseconds(config_.interval_ms));
                std::error_code error;
                co_await timer_.async_wait(asio::redirect_error(asio::use_awaitable, error));
                if (error && error != asio::error::operation_aborted) {
                    throw std::system_error(error);
                }
            }
            on_complete();
        },
        asio::detached);
}

void SyntheticFrameSource::stop() {
    stopping_ = true;
    static_cast<void>(timer_.cancel());
}

RawFrame SyntheticFrameSource::make_frame(const std::uint64_t frame_id) const {
    constexpr std::size_t kChannels = 3;
    RawFrame frame{
        .frame_id = frame_id,
        .capture_timestamp_ms = static_cast<std::int64_t>(frame_id) * config_.interval_ms,
        .width = config_.width,
        .height = config_.height,
        .stride = static_cast<std::uint32_t>(config_.width * kChannels),
        .pixel_format = PixelFormat::rgb24,
        .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(config_.width) *
                                           config_.height * kChannels),
    };

    for (std::uint32_t y = 0; y < config_.height; ++y) {
        for (std::uint32_t x = 0; x < config_.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * config_.width + x) * kChannels;
            frame.pixels[offset] = static_cast<std::uint8_t>((x + frame_id * 4U) % 256U);
            frame.pixels[offset + 1] = static_cast<std::uint8_t>((y + frame_id * 2U) % 256U);
            frame.pixels[offset + 2] =
                static_cast<std::uint8_t>((x + y + frame_id * 7U) % 256U);
        }
    }
    return frame;
}

} // namespace cloud_stream::capture
