#pragma once

#include "media/frame_encoder.h"

#include <cstdint>

namespace cloud_stream::media {

class RawFrameEncoder final : public FrameEncoder {
public:
    RawFrameEncoder(std::uint16_t width, std::uint16_t height, std::uint32_t interval_ms);

    [[nodiscard]] const protocol::StreamConfigPayload& stream_config() const noexcept override;
    [[nodiscard]] std::shared_ptr<const EncodedFrame>
    encode(capture::RawFrame frame) override;
    void request_keyframe() override;

private:
    protocol::StreamConfigPayload stream_config_;
    std::uint32_t interval_ms_{0};
};

} // namespace cloud_stream::media
