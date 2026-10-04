#include "protocol/message_header.h"
#include "protocol/stream_payloads.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

bool expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

template <typename Callable>
bool expect_invalid_argument(Callable&& callable, const char* message) {
    try {
        callable();
    } catch (const std::invalid_argument&) {
        return true;
    }
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

bool test_header_golden_bytes() {
    using namespace cloud_stream::protocol;
    const MessageHeader header{
        .type = MessageType::video_packet,
        .flags = 0,
        .payload_size = 65'535,
        .sequence = 0x01020304,
    };
    const std::array<std::uint8_t, kHeaderSize> golden{
        'C', 'S', 'T', 'R', 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01, 0x02, 0x03, 0x04,
    };
    return expect(encode_header(header) == golden,
                  "header encoding must match golden wire bytes") &&
           expect(decode_header(golden) == header, "golden wire bytes must decode independently");
}

bool test_header_rejections() {
    using namespace cloud_stream::protocol;
    const auto valid = encode_header({
        .type = MessageType::hello,
        .flags = 0,
        .payload_size = kMaxPayloadSize,
        .sequence = 0,
    });

    auto wrong_magic = valid;
    wrong_magic[0] = 'X';
    auto wrong_version = valid;
    wrong_version[4] = 2;
    auto unknown_type = valid;
    unknown_type[5] = 0xff;
    auto nonzero_flags = valid;
    nonzero_flags[7] = 1;
    auto oversized = valid;
    oversized[8] = 0x00;
    oversized[9] = 0x80;
    oversized[10] = 0x00;
    oversized[11] = 0x01;
    const std::array<std::uint8_t, 3> truncated{'C', 'S', 'T'};

    return expect(!decode_header(wrong_magic), "wrong magic must be rejected") &&
           expect(!decode_header(wrong_version), "wrong version must be rejected") &&
           expect(!decode_header(unknown_type), "unknown message type must be rejected") &&
           expect(!decode_header(nonzero_flags), "unknown header flags must be rejected") &&
           expect(!decode_header(oversized), "payload above 8 MiB must be rejected") &&
           expect(!decode_header(truncated), "truncated header must be rejected") &&
           expect_invalid_argument(
               [] {
                   static_cast<void>(encode_header({
                       .type = MessageType::hello,
                       .flags = 0,
                       .payload_size = kMaxPayloadSize + 1,
                       .sequence = 0,
                   }));
               },
               "encoder must reject payload above 8 MiB");
}

bool test_payload_golden_bytes() {
    using namespace cloud_stream::protocol;

    const HelloPayload hello{
        .role = PeerRole::client,
        .capabilities = capability_raw_rgb24,
        .max_payload_size = kMaxPayloadSize,
    };
    const std::vector<std::uint8_t> hello_golden{0x02, 0x00, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00};
    const auto encoded_hello = encode_hello(hello);
    if (!expect(encoded_hello == hello_golden, "HELLO must match golden bytes") ||
        !expect(decode_hello(hello_golden) == hello, "golden HELLO must decode")) {
        return false;
    }

    const StreamConfigPayload config{
        .codec = VideoCodec::raw,
        .pixel_format = PixelFormat::rgb24,
        .flags = 0,
        .width = 320,
        .height = 180,
        .time_base_numerator = 1,
        .time_base_denominator = 1000,
        .extradata = {},
    };
    const std::vector<std::uint8_t> config_golden{
        0x00, 0x01, 0x00, 0x00, 0x01, 0x40, 0x00, 0xb4, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00, 0x00, 0x00,
    };
    const auto encoded_config = encode_stream_config(config);
    if (!expect(encoded_config == config_golden, "STREAM_CONFIG must match golden bytes") ||
        !expect(decode_stream_config(config_golden) == config,
                "golden STREAM_CONFIG must decode")) {
        return false;
    }

    const VideoPacketPayload packet{
        .pts = 1,
        .dts = -1,
        .duration = 33,
        .flags = video_packet_keyframe,
        .data = {0xde, 0xad},
    };
    const std::vector<std::uint8_t> packet_golden{
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x01, 0x00, 0x00, 0xde, 0xad,
    };
    const auto encoded_packet = encode_video_packet(packet);
    return expect(encoded_packet == packet_golden, "VIDEO_PACKET must match golden bytes") &&
           expect(decode_video_packet(packet_golden) == packet, "golden VIDEO_PACKET must decode");
}

bool test_payload_rejections() {
    using namespace cloud_stream::protocol;
    auto bad_hello = *encode_hello({
        .role = PeerRole::client,
        .capabilities = capability_raw_rgb24,
        .max_payload_size = kMaxPayloadSize,
    });
    bad_hello[1] = 1;
    const std::vector<std::uint8_t> too_small_limit_hello{
        0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1c,
    };

    auto bad_config = *encode_stream_config({
        .codec = VideoCodec::raw,
        .pixel_format = PixelFormat::rgb24,
        .flags = 0,
        .width = 320,
        .height = 180,
        .time_base_numerator = 1,
        .time_base_denominator = 30,
        .extradata = {},
    });
    bad_config[19] = 1;

    auto bad_packet = *encode_video_packet({
        .pts = 0,
        .dts = 0,
        .duration = 1,
        .flags = 0,
        .data = {1},
    });
    bad_packet[27] = 1;
    std::vector<std::uint8_t> oversized_error(sizeof(std::uint32_t) + kMaxErrorMessageSize + 1,
                                              'a');

    return expect(!decode_hello(bad_hello), "HELLO reserved byte must be zero") &&
           expect(!decode_hello(too_small_limit_hello),
                  "HELLO limit must fit non-empty VIDEO_PACKET") &&
           expect(!decode_stream_config(bad_config), "STREAM_CONFIG extradata length must match") &&
           expect(!decode_video_packet(bad_packet), "VIDEO_PACKET reserved bytes must be zero") &&
           expect(!encode_stream_config({
                      .codec = VideoCodec::raw,
                      .pixel_format = PixelFormat::unspecified,
                      .flags = 0,
                      .width = 320,
                      .height = 180,
                      .time_base_numerator = 1,
                      .time_base_denominator = 30,
                      .extradata = {},
                  }),
                  "RAW video must use RGB24 in protocol v1") &&
           expect(!encode_stream_config({
                      .codec = VideoCodec::raw,
                      .pixel_format = PixelFormat::rgb24,
                      .flags = 0,
                      .width = kMaxVideoDimension,
                      .height = kMaxVideoDimension,
                      .time_base_numerator = 1,
                      .time_base_denominator = 30,
                      .extradata = {},
                  }),
                  "RAW frame must fit the absolute message limit") &&
           expect(!encode_video_packet({
                      .pts = 0,
                      .dts = 0,
                      .duration = -1,
                      .flags = 0,
                      .data = {1},
                  }),
                  "negative packet duration must be rejected") &&
           expect(!encode_error({.code = 1, .message = std::string("\xc0\xaf", 2)}),
                  "invalid UTF-8 error text must be rejected") &&
           expect(!decode_error(oversized_error),
                  "oversized ERROR text must be rejected before copy");
}

} // namespace

int main() {
    if (!test_header_golden_bytes() || !test_header_rejections() || !test_payload_golden_bytes() ||
        !test_payload_rejections()) {
        return 1;
    }
    std::cout << "protocol tests passed\n";
    return 0;
}
