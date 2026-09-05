#include "ipc/network_ipc_v1_event.h"

#include <limits>
#include <sstream>

#include "ipc/network_ipc_v1_codec.h"

namespace network_service {
namespace ipc_v1 {

namespace {

std::string json_escape(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20u) {
                out += '?';
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

} // namespace

EventSequencer::EventSequencer(std::uint64_t generation)
    : generation_(generation == 0 ? 1 : generation) {}

std::uint64_t EventSequencer::generation() const {
    return generation_;
}

std::uint64_t EventSequencer::next_sequence() const {
    return next_sequence_;
}

std::uint64_t EventSequencer::last_sequence() const {
    if (next_sequence_ == 0) return std::numeric_limits<std::uint64_t>::max();
    return next_sequence_ - 1;
}

std::vector<std::uint8_t> EventSequencer::encode_event(const std::string &event_name,
                                                       const std::string &payload_json) {
    if (event_name.empty() || payload_json.empty() || next_sequence_ == 0) return {};

    const std::uint64_t sequence = next_sequence_;
    std::ostringstream os;
    os << "{\"event\":\"" << json_escape(event_name)
       << "\",\"generation\":" << generation_
       << ",\"seq\":" << sequence
       << ",\"payload\":" << payload_json << "}";

    CodecError error = CodecError::None;
    std::vector<std::uint8_t> encoded = encode_frame(MessageType::Event, os.str(), error);
    if (error != CodecError::None || encoded.empty()) return {};

    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        next_sequence_ = 0;
    } else {
        ++next_sequence_;
    }
    return encoded;
}

} // namespace ipc_v1
} // namespace network_service
