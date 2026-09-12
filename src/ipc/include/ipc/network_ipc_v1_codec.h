#ifndef NETWORK_IPC_V1_CODEC_H
#define NETWORK_IPC_V1_CODEC_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace network_service {
namespace ipc_v1 {

constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kMaxPayloadBytes = 64 * 1024;
constexpr std::uint8_t kProtocolVersion = 1;

enum class MessageType : std::uint8_t {
    Hello = 1,
    Ready = 2,
    Request = 3,
    Response = 4,
    Event = 5,
    Error = 6,
};

enum class CodecError {
    None = 0,
    InvalidMagic,
    UnsupportedVersion,
    InvalidMessageType,
    NonZeroFlags,
    PayloadTooLarge,
};

enum class DecodeStatus {
    NeedMore = 0,
    FrameReady,
    Error,
};

struct FrameHeader {
    std::uint8_t version = kProtocolVersion;
    MessageType type = MessageType::Error;
    std::uint16_t flags = 0;
    std::uint32_t payload_length = 0;
};

struct Frame {
    FrameHeader header;
    std::string payload;
};

const char *codec_error_string(CodecError error);
bool is_valid_message_type(std::uint8_t value);

bool decode_header(const std::uint8_t *data,
                   std::size_t size,
                   FrameHeader &out,
                   CodecError &error);

std::vector<std::uint8_t> encode_frame(MessageType type,
                                       const std::string &payload,
                                       CodecError &error);

class FrameDecoder {
public:
    DecodeStatus feed(const std::uint8_t *data, std::size_t size);
    DecodeStatus feed(const std::vector<std::uint8_t> &data);

    bool has_frame() const;
    Frame take_frame();
    CodecError error() const;
    void reset();

private:
    DecodeStatus parse_available();

    std::vector<std::uint8_t> buffer_;
    std::deque<Frame> frames_;
    CodecError error_ = CodecError::None;
};

} // namespace ipc_v1
} // namespace network_service

#endif // NETWORK_IPC_V1_CODEC_H
