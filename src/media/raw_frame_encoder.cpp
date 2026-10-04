#include "media/raw_frame_encoder.h"

#include <stdexcept>
#include <utility>

namespace cloud_stream::media {

RawFrameEncoder::RawFrameEncoder(const std::uint16_t width, const std::uint16_t height,
                                 const std::uint32_t interval_ms)
    : stream_config_{
          .codec = protocol::VideoCodec::raw,
          .pixel_format = protocol::PixelFormat::rgb24,
          .flags = 0,
          .width = width,
          .height = height,
          .time_base_numerator = 1,
          .time_base_denominator = 1000,
          .extradata = {},
      },
      interval_ms_(interval_ms) {
    if (!protocol::encode_stream_config(stream_config_)) {
        throw std::invalid_argument("invalid RAW encoder configuration");
    }
}

const protocol::StreamConfigPayload& RawFrameEncoder::stream_config() const noexcept {
    return stream_config_;
}

std::vector<std::shared_ptr<const EncodedFrame>> RawFrameEncoder::encode(capture::RawFrame frame) {
    if (frame.pixel_format != capture::PixelFormat::rgb24 ||
        frame.width != stream_config_.width || frame.height != stream_config_.height) {
        throw std::runtime_error("captured frame does not match RAW encoder configuration");
    }
    const auto pts = frame.capture_timestamp_ms;
    auto encoded = std::make_shared<const EncodedFrame>(EncodedFrame{
        .frame_id = frame.frame_id,
        .pts = pts,
        .keyframe = true,
        .config_revision = 0,
        .payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(frame.pixels)),
    });
    return {std::move(encoded)};
}

std::vector<std::shared_ptr<const EncodedFrame>> RawFrameEncoder::flush() {
    return {};
}

void RawFrameEncoder::request_keyframe() {
    // Every RAW frame is independently decodable.
}

} // namespace cloud_stream::media
