#include "pipeline/frame_reassembler.h"

#include "protocol/udp_datagram.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace cloud_stream::pipeline {

FrameReassembler::FrameReassembler(const std::uint64_t session_id,
                                   const std::uint16_t maximum_datagram_size,
                                   const std::size_t maximum_incomplete_frames)
    : session_id_(session_id), maximum_datagram_size_(maximum_datagram_size),
      payload_capacity_(maximum_datagram_size - protocol::kVideoDatagramHeaderSize),
      maximum_incomplete_frames_(maximum_incomplete_frames) {
    if (session_id_ == 0 || maximum_datagram_size_ <= protocol::kVideoDatagramHeaderSize ||
        maximum_incomplete_frames_ == 0) {
        throw std::invalid_argument("invalid frame reassembler configuration");
    }
}

std::optional<media::EncodedFrame>
FrameReassembler::consume(const std::span<const std::uint8_t> datagram) {
    discard_expired();
    const auto decoded = protocol::decode_video_datagram(datagram, maximum_datagram_size_);
    if (!decoded || decoded->header.session_id != session_id_) {
        ++invalid_datagrams_;
        return std::nullopt;
    }

    const auto& header = decoded->header;
    const auto expected_fragments =
        (static_cast<std::size_t>(header.frame_size) + payload_capacity_ - 1U) /
        payload_capacity_;
    const auto offset = static_cast<std::size_t>(header.fragment_index) * payload_capacity_;
    if (expected_fragments != header.fragment_count || offset >= header.frame_size ||
        decoded->payload.size() > header.frame_size - offset ||
        (header.fragment_index + 1U < header.fragment_count &&
         decoded->payload.size() != payload_capacity_) ||
        (header.fragment_index + 1U == header.fragment_count &&
         offset + decoded->payload.size() != header.frame_size)) {
        ++invalid_datagrams_;
        return std::nullopt;
    }

    auto iterator = frames_.find(header.frame_id);
    if (iterator == frames_.end()) {
        make_room();
        PartialFrame partial{
            .pts = header.pts,
            .keyframe = (header.flags & protocol::video_datagram_keyframe) != 0,
            .fragment_count = header.fragment_count,
            .received_count = 0,
            .received = std::vector<bool>(header.fragment_count, false),
            .data = std::vector<std::uint8_t>(header.frame_size),
            .updated_at = std::chrono::steady_clock::now(),
        };
        iterator = frames_.emplace(header.frame_id, std::move(partial)).first;
    }

    auto& partial = iterator->second;
    if (partial.fragment_count != header.fragment_count || partial.data.size() != header.frame_size ||
        partial.pts != header.pts ||
        partial.keyframe != ((header.flags & protocol::video_datagram_keyframe) != 0)) {
        frames_.erase(iterator);
        ++invalid_datagrams_;
        ++dropped_frames_;
        return std::nullopt;
    }

    partial.updated_at = std::chrono::steady_clock::now();
    if (!partial.received[header.fragment_index]) {
        std::memcpy(partial.data.data() + offset, decoded->payload.data(), decoded->payload.size());
        partial.received[header.fragment_index] = true;
        ++partial.received_count;
    }
    if (partial.received_count != partial.fragment_count) {
        return std::nullopt;
    }

    media::EncodedFrame frame{
        .frame_id = header.frame_id,
        .pts = header.pts,
        .keyframe = partial.keyframe,
        .config_revision = 0,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(partial.data)),
    };
    frames_.erase(iterator);
    return frame;
}

std::uint64_t FrameReassembler::invalid_datagrams() const noexcept {
    return invalid_datagrams_;
}

std::uint64_t FrameReassembler::dropped_frames() const noexcept {
    return dropped_frames_;
}

void FrameReassembler::discard_expired() {
    constexpr auto kTimeout = std::chrono::milliseconds(250);
    const auto now = std::chrono::steady_clock::now();
    for (auto iterator = frames_.begin(); iterator != frames_.end();) {
        if (now - iterator->second.updated_at > kTimeout) {
            iterator = frames_.erase(iterator);
            ++dropped_frames_;
        } else {
            ++iterator;
        }
    }
}

void FrameReassembler::make_room() {
    if (frames_.size() < maximum_incomplete_frames_) {
        return;
    }
    const auto oldest = std::min_element(
        frames_.begin(), frames_.end(), [](const auto& left, const auto& right) {
            return left.second.updated_at < right.second.updated_at;
        });
    frames_.erase(oldest);
    ++dropped_frames_;
}

} // namespace cloud_stream::pipeline
