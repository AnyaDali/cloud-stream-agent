#pragma once

#include "capture/raw_frame.h"
#include "media/encoded_frame.h"
#include "protocol/stream_payloads.h"

#include <memory>
#include <vector>

namespace cloud_stream::media {

class FrameEncoder {
public:
    virtual ~FrameEncoder() = default;

    [[nodiscard]] virtual const protocol::StreamConfigPayload& stream_config() const noexcept = 0;
    [[nodiscard]] virtual std::vector<std::shared_ptr<const EncodedFrame>>
    encode(capture::RawFrame frame) = 0;
    [[nodiscard]] virtual std::vector<std::shared_ptr<const EncodedFrame>> flush() = 0;
    virtual void request_keyframe() = 0;
};

} // namespace cloud_stream::media
