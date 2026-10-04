#pragma once

#include "media/decoded_frame.h"
#include "media/encoded_frame.h"

#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace cloud_stream::media {

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    [[nodiscard]] std::vector<DecodedFrame> decode(const EncodedFrame& encoded);
    [[nodiscard]] std::vector<DecodedFrame> flush();
    void reset() noexcept;

private:
    [[nodiscard]] std::vector<DecodedFrame> drain_frames();

    AVCodecContext* codec_context_{nullptr};
    AVFrame* frame_{nullptr};
    AVPacket* packet_{nullptr};
    SwsContext* scale_context_{nullptr};
    std::uint64_t last_frame_id_{0};
};

} // namespace cloud_stream::media
