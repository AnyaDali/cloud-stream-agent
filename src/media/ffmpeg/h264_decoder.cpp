#include "media/ffmpeg/h264_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <cstring>
#include <stdexcept>
#include <string>

namespace cloud_stream::media {
namespace {

[[nodiscard]] std::runtime_error ffmpeg_error(const char* operation, const int code) {
    char description[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, description, sizeof(description));
    return std::runtime_error(std::string(operation) + ": " + description);
}

} // namespace

H264Decoder::H264Decoder() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        throw std::runtime_error("FFmpeg H.264 decoder is not available");
    }
    codec_context_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (codec_context_ == nullptr || frame_ == nullptr || packet_ == nullptr) {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&codec_context_);
        throw std::bad_alloc();
    }
    const int open_result = avcodec_open2(codec_context_, codec, nullptr);
    if (open_result < 0) {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&codec_context_);
        throw ffmpeg_error("failed to open H.264 decoder", open_result);
    }
}

H264Decoder::~H264Decoder() {
    sws_freeContext(scale_context_);
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&codec_context_);
}

std::vector<DecodedFrame> H264Decoder::decode(const EncodedFrame& encoded) {
    if (!encoded.payload || encoded.payload->empty()) {
        throw std::invalid_argument("cannot decode an empty H.264 packet");
    }
    const int packet_result = av_new_packet(packet_, static_cast<int>(encoded.payload->size()));
    if (packet_result < 0) {
        throw ffmpeg_error("failed to allocate decoder packet", packet_result);
    }
    std::memcpy(packet_->data, encoded.payload->data(), encoded.payload->size());
    packet_->pts = encoded.pts;
    packet_->dts = encoded.pts;
    last_frame_id_ = encoded.frame_id;
    const int send_result = avcodec_send_packet(codec_context_, packet_);
    av_packet_unref(packet_);
    if (send_result < 0) {
        throw ffmpeg_error("failed to submit H.264 packet", send_result);
    }
    return drain_frames();
}

std::vector<DecodedFrame> H264Decoder::flush() {
    const int send_result = avcodec_send_packet(codec_context_, nullptr);
    if (send_result < 0 && send_result != AVERROR_EOF) {
        throw ffmpeg_error("failed to flush H.264 decoder", send_result);
    }
    return drain_frames();
}

void H264Decoder::reset() noexcept {
    avcodec_flush_buffers(codec_context_);
    av_packet_unref(packet_);
    av_frame_unref(frame_);
}

std::vector<DecodedFrame> H264Decoder::drain_frames() {
    std::vector<DecodedFrame> result;
    while (true) {
        const int receive_result = avcodec_receive_frame(codec_context_, frame_);
        if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
            break;
        }
        if (receive_result < 0) {
            throw ffmpeg_error("failed to receive decoded frame", receive_result);
        }

        const auto width = static_cast<std::uint32_t>(frame_->width);
        const auto height = static_cast<std::uint32_t>(frame_->height);
        const auto stride = width * 3U;
        DecodedFrame decoded{
            .frame_id = last_frame_id_,
            .pts = frame_->pts == AV_NOPTS_VALUE ? 0 : frame_->pts,
            .width = width,
            .height = height,
            .stride = stride,
            .rgb24 = std::vector<std::uint8_t>(static_cast<std::size_t>(stride) * height),
        };
        scale_context_ = sws_getCachedContext(
            scale_context_, frame_->width, frame_->height,
            static_cast<AVPixelFormat>(frame_->format), frame_->width, frame_->height,
            AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (scale_context_ == nullptr) {
            throw std::runtime_error("failed to create decoder color conversion context");
        }
        std::uint8_t* destination[] = {decoded.rgb24.data(), nullptr, nullptr, nullptr};
        const int destination_stride[] = {static_cast<int>(stride), 0, 0, 0};
        const int scaled = sws_scale(scale_context_, frame_->data, frame_->linesize, 0,
                                     frame_->height, destination, destination_stride);
        if (scaled != frame_->height) {
            throw std::runtime_error("FFmpeg did not convert the complete decoded frame");
        }
        result.push_back(std::move(decoded));
        av_frame_unref(frame_);
    }
    return result;
}

} // namespace cloud_stream::media
