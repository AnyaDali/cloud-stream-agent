#include "pipeline/broadcast_buffer.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace cloud_stream::pipeline {

BroadcastBuffer::BroadcastBuffer(const std::size_t maximum_frames,
                                 const std::size_t maximum_bytes)
    : maximum_frames_(maximum_frames), maximum_bytes_(maximum_bytes) {
    if (maximum_frames_ == 0 || maximum_bytes_ == 0) {
        throw std::invalid_argument("broadcast buffer limits must be positive");
    }
}

StreamSequence BroadcastBuffer::publish(std::shared_ptr<const media::EncodedFrame> frame) {
    if (finished_) {
        throw std::logic_error("cannot publish after broadcast buffer is finished");
    }
    if (!frame || !frame->payload) {
        throw std::invalid_argument("broadcast frame and payload must be present");
    }
    if (next_sequence_ == std::numeric_limits<StreamSequence>::max()) {
        throw std::overflow_error("broadcast sequence exhausted");
    }

    const auto sequence = next_sequence_++;
    current_bytes_ += frame->payload->size();
    entries_.push_back(std::make_shared<const BroadcastEntry>(
        BroadcastEntry{.sequence = sequence, .frame = std::move(frame)}));
    trim();
    return sequence;
}

BroadcastReadResult BroadcastBuffer::read(const StreamSequence sequence) const {
    if (entries_.empty() || sequence >= next_sequence_) {
        return {
            .status = BroadcastReadResult::Status::empty,
            .first_available_sequence = first_sequence_,
            .entry = nullptr,
        };
    }
    if (sequence < first_sequence_) {
        return {
            .status = BroadcastReadResult::Status::lagged,
            .first_available_sequence = first_sequence_,
            .entry = nullptr,
        };
    }

    const auto index = static_cast<std::size_t>(sequence - first_sequence_);
    if (index >= entries_.size()) {
        return {
            .status = BroadcastReadResult::Status::empty,
            .first_available_sequence = first_sequence_,
            .entry = nullptr,
        };
    }
    return {
        .status = BroadcastReadResult::Status::available,
        .first_available_sequence = first_sequence_,
        .entry = entries_[index],
    };
}

StreamSequence BroadcastBuffer::start_sequence_for_new_reader() const {
    for (auto iterator = entries_.rbegin(); iterator != entries_.rend(); ++iterator) {
        if ((*iterator)->frame->keyframe) {
            return (*iterator)->sequence;
        }
    }
    return next_sequence_;
}

StreamSequence BroadcastBuffer::first_sequence() const noexcept {
    return first_sequence_;
}

StreamSequence BroadcastBuffer::next_sequence() const noexcept {
    return next_sequence_;
}

bool BroadcastBuffer::finished() const noexcept {
    return finished_;
}

std::size_t BroadcastBuffer::size() const noexcept {
    return entries_.size();
}

std::size_t BroadcastBuffer::byte_size() const noexcept {
    return current_bytes_;
}

void BroadcastBuffer::finish() noexcept {
    finished_ = true;
}

void BroadcastBuffer::trim() {
    while (entries_.size() > 1 &&
           (entries_.size() > maximum_frames_ || current_bytes_ > maximum_bytes_)) {
        current_bytes_ -= entries_.front()->frame->payload->size();
        entries_.pop_front();
    }
    first_sequence_ = entries_.empty() ? next_sequence_ : entries_.front()->sequence;
}

} // namespace cloud_stream::pipeline
