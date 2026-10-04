#include "pipeline/udp_packetizer.h"

#include "protocol/message_header.h"
#include "protocol/udp_datagram.h"

#include <limits>
#include <stdexcept>

namespace cloud_stream::pipeline {

UdpPacketizer::UdpPacketizer(const std::uint64_t session_id,
                             const std::uint16_t maximum_datagram_size)
    : session_id_(session_id), maximum_datagram_size_(maximum_datagram_size) {
    if (session_id_ == 0 || maximum_datagram_size_ <= protocol::kVideoDatagramHeaderSize) {
        throw std::invalid_argument("invalid UDP packetizer configuration");
    }
}

std::vector<std::vector<std::uint8_t>>
UdpPacketizer::packetize(const media::EncodedFrame& frame) {
    if (!frame.payload || frame.payload->empty() ||
        frame.payload->size() > protocol::kMaxPayloadSize ||
        frame.payload->size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("encoded frame is not valid for UDP packetization");
    }

    const auto payload_capacity = maximum_datagram_size_ - protocol::kVideoDatagramHeaderSize;
    const auto fragment_count_value =
        (frame.payload->size() + payload_capacity - 1U) / payload_capacity;
    if (fragment_count_value == 0 ||
        fragment_count_value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("encoded frame requires too many UDP fragments");
    }
    const auto fragment_count = static_cast<std::uint16_t>(fragment_count_value);

    std::vector<std::vector<std::uint8_t>> result;
    result.reserve(fragment_count);
    for (std::uint16_t fragment_index = 0; fragment_index < fragment_count; ++fragment_index) {
        if (next_packet_number_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("UDP packet number exhausted");
        }
        const auto offset = static_cast<std::size_t>(fragment_index) * payload_capacity;
        const auto size = std::min<std::size_t>(payload_capacity, frame.payload->size() - offset);
        const auto payload = std::span(*frame.payload).subspan(offset, size);
        const auto datagram = protocol::encode_video_datagram(
            {
                .flags = static_cast<std::uint16_t>(
                    frame.keyframe ? protocol::video_datagram_keyframe : 0),
                .session_id = session_id_,
                .key_epoch = protocol::kInitialKeyEpoch,
                .fragment_index = fragment_index,
                .fragment_count = fragment_count,
                .packet_number = next_packet_number_++,
                .frame_id = frame.frame_id,
                .pts = frame.pts,
                .frame_size = static_cast<std::uint32_t>(frame.payload->size()),
                .payload_size = static_cast<std::uint16_t>(size),
            },
            payload, maximum_datagram_size_);
        if (!datagram) {
            throw std::logic_error("failed to encode UDP video datagram");
        }
        result.push_back(std::move(*datagram));
    }
    return result;
}

} // namespace cloud_stream::pipeline
