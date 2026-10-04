#pragma once

#include "media/frame_encoder.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace cloud_stream::media {

struct H264EncoderConfig {
    std::uint16_t width{1280};
    std::uint16_t height{720};
    std::uint32_t frames_per_second{30};
    std::uint32_t bitrate_kbps{4000};
    std::uint32_t keyframe_interval{30};
};

class H264Encoder final : public FrameEncoder {
public:
    explicit H264Encoder(H264EncoderConfig config);
    ~H264Encoder() override;

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    [[nodiscard]] const protocol::StreamConfigPayload& stream_config() const noexcept override;
    [[nodiscard]] std::vector<std::shared_ptr<const EncodedFrame>>
    encode(capture::RawFrame frame) override;
    [[nodiscard]] std::vector<std::shared_ptr<const EncodedFrame>> flush() override;
    void request_keyframe() override;

private:
    [[nodiscard]] std::vector<std::shared_ptr<const EncodedFrame>> drain_packets();

    H264EncoderConfig config_;
    protocol::StreamConfigPayload stream_config_;
    AVCodecContext* codec_context_{nullptr};
    AVFrame* frame_{nullptr};
    AVPacket* packet_{nullptr};
    SwsContext* scale_context_{nullptr};
    std::atomic_bool keyframe_requested_{true};
};

} // namespace cloud_stream::media
