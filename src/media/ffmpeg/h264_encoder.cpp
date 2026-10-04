#include "media/ffmpeg/h264_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace cloud_stream::media {
namespace {

[[nodiscard]] std::runtime_error ffmpeg_error(const char* operation, const int code) {
    char description[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, description, sizeof(description));
    return std::runtime_error(std::string(operation) + ": " + description);
}

[[nodiscard]] AVPixelFormat to_av_pixel_format(const capture::PixelFormat format) {
    switch (format) {
    case capture::PixelFormat::rgb24:
        return AV_PIX_FMT_RGB24;
    case capture::PixelFormat::bgra32:
        return AV_PIX_FMT_BGRA;
    }
    throw std::runtime_error("unsupported capture pixel format");
}

} // namespace

H264Encoder::H264Encoder(H264EncoderConfig config)
    : config_(config),
      stream_config_{
          .codec = protocol::VideoCodec::h264,
          .pixel_format = protocol::PixelFormat::unspecified,
          .flags = 0,
          .width = config.width,
          .height = config.height,
          .time_base_numerator = 1,
          .time_base_denominator = config.frames_per_second,
          .extradata = {},
      } {
    if (config_.width < 2 || config_.height < 2 || config_.width % 2U != 0 ||
        config_.height % 2U != 0 || config_.frames_per_second == 0 ||
        config_.bitrate_kbps == 0 || config_.keyframe_interval == 0 ||
        !protocol::encode_stream_config(stream_config_)) {
        throw std::invalid_argument("invalid H.264 encoder configuration");
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (codec == nullptr) {
        throw std::runtime_error("FFmpeg libx264 encoder is not available");
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

    codec_context_->width = config_.width;
    codec_context_->height = config_.height;
    codec_context_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_context_->time_base = AVRational{1, static_cast<int>(config_.frames_per_second)};
    codec_context_->framerate = AVRational{static_cast<int>(config_.frames_per_second), 1};
    codec_context_->bit_rate = static_cast<std::int64_t>(config_.bitrate_kbps) * 1000;
    codec_context_->gop_size = static_cast<int>(config_.keyframe_interval);
    codec_context_->max_b_frames = 0;
    const int preset_result = av_opt_set(codec_context_->priv_data, "preset", "veryfast", 0);
    const int tune_result = av_opt_set(codec_context_->priv_data, "tune", "zerolatency", 0);
    const int parameters_result = av_opt_set(codec_context_->priv_data, "x264-params",
                                             "repeat-headers=1:annexb=1", 0);
    if (preset_result < 0 || tune_result < 0 || parameters_result < 0) {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&codec_context_);
        throw std::runtime_error("failed to configure libx264 low-latency options");
    }

    const int open_result = avcodec_open2(codec_context_, codec, nullptr);
    if (open_result < 0) {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&codec_context_);
        throw ffmpeg_error("failed to open H.264 encoder", open_result);
    }

    frame_->format = codec_context_->pix_fmt;
    frame_->width = codec_context_->width;
    frame_->height = codec_context_->height;
    const int buffer_result = av_frame_get_buffer(frame_, 32);
    if (buffer_result < 0) {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&codec_context_);
        throw ffmpeg_error("failed to allocate encoder frame", buffer_result);
    }
}

H264Encoder::~H264Encoder() {
    sws_freeContext(scale_context_);
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&codec_context_);
}

const protocol::StreamConfigPayload& H264Encoder::stream_config() const noexcept {
    return stream_config_;
}

std::vector<std::shared_ptr<const EncodedFrame>>
H264Encoder::encode(capture::RawFrame input) {
    const auto bytes_per_pixel = input.pixel_format == capture::PixelFormat::rgb24 ? 3U : 4U;
    if (input.width == 0 || input.height == 0 ||
        input.stride < input.width * bytes_per_pixel ||
        input.pixels.size() < static_cast<std::size_t>(input.stride) * input.height) {
        throw std::runtime_error("invalid raw frame supplied to H.264 encoder");
    }
    const int writable_result = av_frame_make_writable(frame_);
    if (writable_result < 0) {
        throw ffmpeg_error("failed to make encoder frame writable", writable_result);
    }

    std::uint32_t scaled_width = config_.width;
    std::uint32_t scaled_height = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(input.height) * scaled_width / input.width);
    if (scaled_height > config_.height) {
        scaled_height = config_.height;
        scaled_width = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(input.width) * scaled_height / input.height);
    }
    scaled_width = std::max<std::uint32_t>(2, scaled_width & ~1U);
    scaled_height = std::max<std::uint32_t>(2, scaled_height & ~1U);
    const auto x_offset = ((config_.width - scaled_width) / 2U) & ~1U;
    const auto y_offset = ((config_.height - scaled_height) / 2U) & ~1U;

    std::array<std::ptrdiff_t, 4> frame_strides{};
    for (std::size_t index = 0; index < frame_strides.size(); ++index) {
        frame_strides[index] = static_cast<std::ptrdiff_t>(frame_->linesize[index]);
    }
    const int fill_result = av_image_fill_black(
        frame_->data, frame_strides.data(), codec_context_->pix_fmt, AVCOL_RANGE_MPEG,
        codec_context_->width, codec_context_->height);
    if (fill_result < 0) {
        throw ffmpeg_error("failed to clear encoder frame", fill_result);
    }

    scale_context_ = sws_getCachedContext(
        scale_context_, static_cast<int>(input.width), static_cast<int>(input.height),
        to_av_pixel_format(input.pixel_format), static_cast<int>(scaled_width),
        static_cast<int>(scaled_height), codec_context_->pix_fmt, SWS_BILINEAR, nullptr, nullptr,
        nullptr);
    if (scale_context_ == nullptr) {
        throw std::runtime_error("failed to create FFmpeg scaling context");
    }
    const std::uint8_t* source_data[] = {input.pixels.data(), nullptr, nullptr, nullptr};
    const int source_stride[] = {static_cast<int>(input.stride), 0, 0, 0};
    std::uint8_t* destination_data[] = {
        frame_->data[0] + static_cast<std::size_t>(y_offset) * frame_->linesize[0] + x_offset,
        frame_->data[1] + static_cast<std::size_t>(y_offset / 2U) * frame_->linesize[1] +
            x_offset / 2U,
        frame_->data[2] + static_cast<std::size_t>(y_offset / 2U) * frame_->linesize[2] +
            x_offset / 2U,
        nullptr,
    };
    const int scaled = sws_scale(scale_context_, source_data, source_stride, 0,
                                 static_cast<int>(input.height), destination_data,
                                 frame_->linesize);
    if (scaled != static_cast<int>(scaled_height)) {
        throw std::runtime_error("FFmpeg did not scale the complete input frame");
    }

    frame_->pts = static_cast<std::int64_t>(input.frame_id);
    frame_->pict_type = keyframe_requested_.exchange(false) ? AV_PICTURE_TYPE_I
                                                            : AV_PICTURE_TYPE_NONE;
    const int send_result = avcodec_send_frame(codec_context_, frame_);
    if (send_result < 0) {
        throw ffmpeg_error("failed to submit frame to H.264 encoder", send_result);
    }
    return drain_packets();
}

std::vector<std::shared_ptr<const EncodedFrame>> H264Encoder::flush() {
    const int send_result = avcodec_send_frame(codec_context_, nullptr);
    if (send_result < 0 && send_result != AVERROR_EOF) {
        throw ffmpeg_error("failed to flush H.264 encoder", send_result);
    }
    return drain_packets();
}

void H264Encoder::request_keyframe() {
    keyframe_requested_.store(true);
}

std::vector<std::shared_ptr<const EncodedFrame>> H264Encoder::drain_packets() {
    std::vector<std::shared_ptr<const EncodedFrame>> result;
    while (true) {
        const int receive_result = avcodec_receive_packet(codec_context_, packet_);
        if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
            break;
        }
        if (receive_result < 0) {
            throw ffmpeg_error("failed to receive H.264 packet", receive_result);
        }

        auto bytes = std::make_shared<std::vector<std::uint8_t>>(
            packet_->data, packet_->data + packet_->size);
        const auto pts = packet_->pts == AV_NOPTS_VALUE ? 0 : packet_->pts;
        result.push_back(std::make_shared<const EncodedFrame>(EncodedFrame{
            .frame_id = static_cast<std::uint64_t>(pts < 0 ? 0 : pts),
            .pts = pts,
            .keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0,
            .config_revision = 0,
            .payload = std::move(bytes),
        }));
        av_packet_unref(packet_);
    }
    return result;
}

} // namespace cloud_stream::media
