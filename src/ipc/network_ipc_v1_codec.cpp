#include "ipc/network_ipc_v1_codec.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace network_service {
namespace ipc_v1 {

namespace {

constexpr std::uint8_t kMagic[4] = {'N', 'S', 'P', '1'};

std::uint16_t read_be16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

std::uint32_t read_be32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

void write_be16(std::uint8_t *p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    p[1] = static_cast<std::uint8_t>(value & 0xffu);
}

void write_be32(std::uint8_t *p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
    p[1] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    p[2] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    p[3] = static_cast<std::uint8_t>(value & 0xffu);
}

} // namespace

const char *codec_error_string(CodecError error) {
    switch (error) {
    case CodecError::None: return "none";
    case CodecError::InvalidMagic: return "invalid magic";
    case CodecError::UnsupportedVersion: return "unsupported version";
    case CodecError::InvalidMessageType: return "invalid message type";
    case CodecError::NonZeroFlags: return "non-zero reserved flags";
    case CodecError::PayloadTooLarge: return "payload too large";
    }
    return "unknown codec error";
}

bool is_valid_message_type(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(MessageType::Hello) &&
           value <= static_cast<std::uint8_t>(MessageType::Error);
}

bool decode_header(const std::uint8_t *data,
                   std::size_t size,
                   FrameHeader &out,
                   CodecError &error) {
    error = CodecError::None;
    if (data == nullptr || size < kHeaderSize) {
        return false;
    }
    if (!std::equal(kMagic, kMagic + 4, data)) {
        error = CodecError::InvalidMagic;
        return false;
    }
    if (data[4] != kProtocolVersion) {
        error = CodecError::UnsupportedVersion;
        return false;
    }
    if (!is_valid_message_type(data[5])) {
        error = CodecError::InvalidMessageType;
        return false;
    }

    const std::uint16_t flags = read_be16(data + 6);
    if (flags != 0) {
        error = CodecError::NonZeroFlags;
        return false;
    }

    const std::uint32_t payload_length = read_be32(data + 8);
    if (payload_length > kMaxPayloadBytes) {
        error = CodecError::PayloadTooLarge;
        return false;
    }

    out.version = data[4];
    out.type = static_cast<MessageType>(data[5]);
    out.flags = flags;
    out.payload_length = payload_length;
    return true;
}

std::vector<std::uint8_t> encode_frame(MessageType type,
                                       const std::string &payload,
                                       CodecError &error) {
    error = CodecError::None;
    if (!is_valid_message_type(static_cast<std::uint8_t>(type))) {
        error = CodecError::InvalidMessageType;
        return {};
    }
    if (payload.size() > kMaxPayloadBytes) {
        error = CodecError::PayloadTooLarge;
        return {};
    }

    std::vector<std::uint8_t> out(kHeaderSize + payload.size());
    std::copy(kMagic, kMagic + 4, out.begin());
    out[4] = kProtocolVersion;
    out[5] = static_cast<std::uint8_t>(type);
    write_be16(out.data() + 6, 0);
    write_be32(out.data() + 8, static_cast<std::uint32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(),
              out.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));
    return out;
}

DecodeStatus FrameDecoder::feed(const std::uint8_t *data, std::size_t size) {
    if (error_ != CodecError::None) {
        return DecodeStatus::Error;
    }
    if (size == 0) {
        return parse_available();
    }
    if (data == nullptr) {
        error_ = CodecError::InvalidMagic;
        return DecodeStatus::Error;
    }
    buffer_.insert(buffer_.end(), data, data + size);
    return parse_available();
}

DecodeStatus FrameDecoder::feed(const std::vector<std::uint8_t> &data) {
    return feed(data.data(), data.size());
}

bool FrameDecoder::has_frame() const {
    return !frames_.empty();
}

Frame FrameDecoder::take_frame() {
    if (frames_.empty()) {
        throw std::logic_error("no decoded frame available");
    }
    Frame frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}

CodecError FrameDecoder::error() const {
    return error_;
}

void FrameDecoder::reset() {
    buffer_.clear();
    frames_.clear();
    error_ = CodecError::None;
}

DecodeStatus FrameDecoder::parse_available() {
    while (buffer_.size() >= kHeaderSize) {
        FrameHeader header;
        CodecError parse_error = CodecError::None;
        if (!decode_header(buffer_.data(), buffer_.size(), header, parse_error)) {
            if (parse_error == CodecError::None) {
                return frames_.empty() ? DecodeStatus::NeedMore : DecodeStatus::FrameReady;
            }
            error_ = parse_error;
            return DecodeStatus::Error;
        }

        const std::size_t frame_size =
            kHeaderSize + static_cast<std::size_t>(header.payload_length);
        if (buffer_.size() < frame_size) {
            return frames_.empty() ? DecodeStatus::NeedMore : DecodeStatus::FrameReady;
        }

        Frame frame;
        frame.header = header;
        frame.payload.assign(
            reinterpret_cast<const char *>(buffer_.data() + kHeaderSize),
            static_cast<std::size_t>(header.payload_length));
        frames_.push_back(std::move(frame));
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
    }

    return frames_.empty() ? DecodeStatus::NeedMore : DecodeStatus::FrameReady;
}

} // namespace ipc_v1
} // namespace network_service
