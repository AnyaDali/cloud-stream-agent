#pragma once

#include "media/encoded_frame.h"

#include <cstdint>
#include <vector>

namespace cloud_stream::pipeline {

class UdpPacketizer {
public:
    UdpPacketizer(std::uint64_t session_id, std::uint16_t maximum_datagram_size);

    [[nodiscard]] std::vector<std::vector<std::uint8_t>>
    packetize(const media::EncodedFrame& frame);

private:
    std::uint64_t session_id_{0};
    std::uint64_t next_packet_number_{0};
    std::uint16_t maximum_datagram_size_{0};
};

} // namespace cloud_stream::pipeline
