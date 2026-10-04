#pragma once

#include "media/encoded_frame.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

namespace cloud_stream::pipeline {

using StreamSequence = std::uint64_t;

struct BroadcastEntry {
    StreamSequence sequence{0};
    std::shared_ptr<const media::EncodedFrame> frame;
};

struct BroadcastReadResult {
    enum class Status : std::uint8_t {
        empty,
        available,
        lagged,
    };

    Status status{Status::empty};
    StreamSequence first_available_sequence{0};
    std::shared_ptr<const BroadcastEntry> entry;
};

class BroadcastBuffer {
public:
    BroadcastBuffer(std::size_t maximum_frames, std::size_t maximum_bytes);

    [[nodiscard]] StreamSequence publish(std::shared_ptr<const media::EncodedFrame> frame);
    [[nodiscard]] BroadcastReadResult read(StreamSequence sequence) const;
    [[nodiscard]] StreamSequence start_sequence_for_new_reader() const;
    [[nodiscard]] StreamSequence first_sequence() const noexcept;
    [[nodiscard]] StreamSequence next_sequence() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t byte_size() const noexcept;

    void finish() noexcept;

private:
    void trim();

    std::size_t maximum_frames_{0};
    std::size_t maximum_bytes_{0};
    std::size_t current_bytes_{0};
    StreamSequence first_sequence_{0};
    StreamSequence next_sequence_{0};
    std::deque<std::shared_ptr<const BroadcastEntry>> entries_;
    bool finished_{false};
};

} // namespace cloud_stream::pipeline
