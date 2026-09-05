#include "ipc/network_ipc_v1_codec.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using network_service::ipc_v1::CodecError;
using network_service::ipc_v1::DecodeStatus;
using network_service::ipc_v1::Frame;
using network_service::ipc_v1::FrameDecoder;
using network_service::ipc_v1::MessageType;
using network_service::ipc_v1::encode_frame;
using network_service::ipc_v1::kHeaderSize;
using network_service::ipc_v1::kMaxPayloadBytes;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_encode_decode_round_trip() {
    CodecError error = CodecError::None;
    const std::string payload = R"({"requestId":7})";
    std::vector<std::uint8_t> bytes = encode_frame(MessageType::Request, payload, error);

    require(error == CodecError::None, "round-trip encode returned error");
    require(bytes.size() == kHeaderSize + payload.size(), "encoded frame size mismatch");
    require(bytes[0] == 'N' && bytes[1] == 'S' && bytes[2] == 'P' && bytes[3] == '1',
            "encoded magic mismatch");
    require(bytes[4] == 1, "encoded version mismatch");
    require(bytes[5] == 3, "encoded message type mismatch");
    require(bytes[6] == 0 && bytes[7] == 0, "encoded flags must be zero");

    FrameDecoder decoder;
    require(decoder.feed(bytes) == DecodeStatus::FrameReady, "complete frame not decoded");
    require(decoder.has_frame(), "decoder did not queue decoded frame");
    Frame frame = decoder.take_frame();
    require(frame.header.version == 1, "decoded version mismatch");
    require(frame.header.type == MessageType::Request, "decoded type mismatch");
    require(frame.payload == payload, "decoded payload mismatch");
}

void test_partial_delivery() {
    CodecError error = CodecError::None;
    std::vector<std::uint8_t> bytes =
        encode_frame(MessageType::Hello, R"({"minVersion":1,"maxVersion":1})", error);
    require(error == CodecError::None, "partial-delivery encode returned error");

    FrameDecoder decoder;
    require(decoder.feed(bytes.data(), 3) == DecodeStatus::NeedMore,
            "partial magic should require more bytes");
    require(decoder.feed(bytes.data() + 3, kHeaderSize - 3) == DecodeStatus::NeedMore,
            "header-only delivery should require payload");
    require(decoder.feed(bytes.data() + kHeaderSize, bytes.size() - kHeaderSize) ==
                DecodeStatus::FrameReady,
            "completed partial delivery did not produce frame");
    require(decoder.take_frame().header.type == MessageType::Hello,
            "partial delivery decoded wrong type");
}

void expect_header_error(std::vector<std::uint8_t> bytes,
                         CodecError expected,
                         const std::string &context) {
    FrameDecoder decoder;
    require(decoder.feed(bytes) == DecodeStatus::Error, context + ": expected decode error");
    require(decoder.error() == expected, context + ": wrong decode error");
}

void test_invalid_headers() {
    CodecError error = CodecError::None;
    const std::vector<std::uint8_t> base = encode_frame(MessageType::Hello, "{}", error);
    require(error == CodecError::None, "invalid-header baseline encode failed");

    std::vector<std::uint8_t> bytes = base;
    bytes[0] = 'B';
    expect_header_error(bytes, CodecError::InvalidMagic, "invalid magic");

    bytes = base;
    bytes[4] = 2;
    expect_header_error(bytes, CodecError::UnsupportedVersion, "unsupported version");

    bytes = base;
    bytes[5] = 99;
    expect_header_error(bytes, CodecError::InvalidMessageType, "invalid type");

    bytes = base;
    bytes[7] = 1;
    expect_header_error(bytes, CodecError::NonZeroFlags, "non-zero flags");

    bytes = base;
    bytes[8] = 0x00;
    bytes[9] = 0x01;
    bytes[10] = 0x00;
    bytes[11] = 0x01;
    expect_header_error(bytes, CodecError::PayloadTooLarge, "oversized declared payload");
}

void test_oversized_encode_rejected() {
    CodecError error = CodecError::None;
    const std::string payload(kMaxPayloadBytes + 1, 'x');
    const std::vector<std::uint8_t> bytes = encode_frame(MessageType::Request, payload, error);
    require(bytes.empty(), "oversized payload unexpectedly encoded");
    require(error == CodecError::PayloadTooLarge, "oversized payload returned wrong error");
}

void test_multiple_frames_in_one_read() {
    CodecError error = CodecError::None;
    std::vector<std::uint8_t> combined = encode_frame(MessageType::Hello, "{}", error);
    require(error == CodecError::None, "first multi-frame encode failed");
    std::vector<std::uint8_t> second =
        encode_frame(MessageType::Request, R"({"requestId":9})", error);
    require(error == CodecError::None, "second multi-frame encode failed");
    combined.insert(combined.end(), second.begin(), second.end());

    FrameDecoder decoder;
    require(decoder.feed(combined) == DecodeStatus::FrameReady,
            "multiple frames were not decoded");
    require(decoder.take_frame().header.type == MessageType::Hello,
            "first queued frame has wrong type");
    require(decoder.take_frame().header.type == MessageType::Request,
            "second queued frame has wrong type");
}

void test_decoder_error_is_sticky_until_reset() {
    CodecError error = CodecError::None;
    std::vector<std::uint8_t> invalid = encode_frame(MessageType::Hello, "{}", error);
    invalid[0] = 'X';

    FrameDecoder decoder;
    require(decoder.feed(invalid) == DecodeStatus::Error, "invalid frame did not fail");

    std::vector<std::uint8_t> valid = encode_frame(MessageType::Hello, "{}", error);
    require(decoder.feed(valid) == DecodeStatus::Error,
            "decoder accepted data after terminal codec error without reset");

    decoder.reset();
    require(decoder.feed(valid) == DecodeStatus::FrameReady,
            "decoder did not recover after explicit reset");
}

} // namespace

int main() {
    try {
        test_encode_decode_round_trip();
        test_partial_delivery();
        test_invalid_headers();
        test_oversized_encode_rejected();
        test_multiple_frames_in_one_read();
        test_decoder_error_is_sticky_until_reset();
    } catch (const std::exception &ex) {
        std::cerr << "network_ipc_v1_codec_test: FAIL: " << ex.what() << '\n';
        return 1;
    }

    std::cout << "network_ipc_v1_codec_test: PASS\n";
    return 0;
}
