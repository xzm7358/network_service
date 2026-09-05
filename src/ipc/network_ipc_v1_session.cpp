#include "ipc/network_ipc_v1_session.h"

#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace network_service {
namespace ipc_v1 {

namespace {

constexpr int kMaxJsonDepth = 16;

void skip_ws(const std::string &text, std::size_t &pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
}

bool parse_string(const std::string &text, std::size_t &pos, std::string *out) {
    if (pos >= text.size() || text[pos] != '"') return false;
    ++pos;
    std::string value;
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == '"') {
            if (out != nullptr) *out = std::move(value);
            return true;
        }
        if (static_cast<unsigned char>(ch) < 0x20u) return false;
        if (ch != '\\') {
            if (out != nullptr) value += ch;
            continue;
        }
        if (pos >= text.size()) return false;
        const char escaped = text[pos++];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            if (out != nullptr) value += escaped;
            break;
        case 'u':
            if (pos + 4 > text.size()) return false;
            for (int i = 0; i < 4; ++i) {
                if (std::isxdigit(static_cast<unsigned char>(text[pos + static_cast<std::size_t>(i)])) == 0) {
                    return false;
                }
            }
            pos += 4;
            if (out != nullptr) value += '?';
            break;
        default:
            return false;
        }
    }
    return false;
}

bool skip_value(const std::string &text, std::size_t &pos, int depth);

bool skip_array(const std::string &text, std::size_t &pos, int depth) {
    if (depth > kMaxJsonDepth || pos >= text.size() || text[pos] != '[') return false;
    ++pos;
    skip_ws(text, pos);
    if (pos < text.size() && text[pos] == ']') {
        ++pos;
        return true;
    }
    while (true) {
        if (!skip_value(text, pos, depth + 1)) return false;
        skip_ws(text, pos);
        if (pos >= text.size()) return false;
        if (text[pos] == ']') {
            ++pos;
            return true;
        }
        if (text[pos] != ',') return false;
        ++pos;
        skip_ws(text, pos);
    }
}

bool skip_object(const std::string &text, std::size_t &pos, int depth) {
    if (depth > kMaxJsonDepth || pos >= text.size() || text[pos] != '{') return false;
    ++pos;
    skip_ws(text, pos);
    if (pos < text.size() && text[pos] == '}') {
        ++pos;
        return true;
    }
    while (true) {
        if (!parse_string(text, pos, nullptr)) return false;
        skip_ws(text, pos);
        if (pos >= text.size() || text[pos] != ':') return false;
        ++pos;
        skip_ws(text, pos);
        if (!skip_value(text, pos, depth + 1)) return false;
        skip_ws(text, pos);
        if (pos >= text.size()) return false;
        if (text[pos] == '}') {
            ++pos;
            return true;
        }
        if (text[pos] != ',') return false;
        ++pos;
        skip_ws(text, pos);
    }
}

bool skip_number(const std::string &text, std::size_t &pos) {
    const std::size_t start = pos;
    if (pos < text.size() && text[pos] == '-') ++pos;
    if (pos >= text.size()) return false;
    if (text[pos] == '0') {
        ++pos;
    } else {
        if (text[pos] < '1' || text[pos] > '9') return false;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) ++pos;
    }
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        const std::size_t fraction_start = pos;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) ++pos;
        if (pos == fraction_start) return false;
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
        const std::size_t exponent_start = pos;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) ++pos;
        if (pos == exponent_start) return false;
    }
    return pos > start;
}

bool skip_literal(const std::string &text, std::size_t &pos, const char *literal) {
    std::size_t i = 0;
    while (literal[i] != '\0') {
        if (pos + i >= text.size() || text[pos + i] != literal[i]) return false;
        ++i;
    }
    pos += i;
    return true;
}

bool skip_value(const std::string &text, std::size_t &pos, int depth) {
    if (depth > kMaxJsonDepth) return false;
    skip_ws(text, pos);
    if (pos >= text.size()) return false;
    switch (text[pos]) {
    case '"': return parse_string(text, pos, nullptr);
    case '{': return skip_object(text, pos, depth);
    case '[': return skip_array(text, pos, depth);
    case 't': return skip_literal(text, pos, "true");
    case 'f': return skip_literal(text, pos, "false");
    case 'n': return skip_literal(text, pos, "null");
    default: return skip_number(text, pos);
    }
}

bool parse_uint64(const std::string &text, std::size_t &pos, std::uint64_t &value) {
    if (pos >= text.size() || std::isdigit(static_cast<unsigned char>(text[pos])) == 0) return false;
    std::uint64_t result = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
        const unsigned digit = static_cast<unsigned>(text[pos] - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) return false;
        result = result * 10u + digit;
        ++pos;
    }
    value = result;
    return true;
}

bool parse_hello_versions(const std::string &text,
                          std::uint64_t &min_version,
                          std::uint64_t &max_version) {
    std::size_t pos = 0;
    bool have_min = false;
    bool have_max = false;
    skip_ws(text, pos);
    if (pos >= text.size() || text[pos] != '{') return false;
    ++pos;
    skip_ws(text, pos);
    if (pos < text.size() && text[pos] == '}') return false;

    while (true) {
        std::string key;
        if (!parse_string(text, pos, &key)) return false;
        skip_ws(text, pos);
        if (pos >= text.size() || text[pos] != ':') return false;
        ++pos;
        skip_ws(text, pos);

        if (key == "minVersion" || key == "maxVersion") {
            std::uint64_t parsed = 0;
            if (!parse_uint64(text, pos, parsed)) return false;
            if (key == "minVersion") {
                if (have_min) return false;
                min_version = parsed;
                have_min = true;
            } else {
                if (have_max) return false;
                max_version = parsed;
                have_max = true;
            }
        } else if (!skip_value(text, pos, 1)) {
            return false;
        }

        skip_ws(text, pos);
        if (pos >= text.size()) return false;
        if (text[pos] == '}') {
            ++pos;
            break;
        }
        if (text[pos] != ',') return false;
        ++pos;
        skip_ws(text, pos);
    }

    skip_ws(text, pos);
    return pos == text.size() && have_min && have_max && min_version <= max_version;
}

std::vector<std::uint8_t> encode_json(MessageType type, const std::string &payload) {
    CodecError error = CodecError::None;
    std::vector<std::uint8_t> encoded = encode_frame(type, payload, error);
    if (error != CodecError::None) return {};
    return encoded;
}

} // namespace

Session::Session(std::uint64_t generation, std::string session_id)
    : generation_(generation == 0 ? 1 : generation),
      session_id_(std::move(session_id)) {}

Session::HandleResult Session::handle_frame(const Frame &frame) {
    if (state_ == State::Closed) return {{}, true};

    if (state_ == State::AwaitHello) {
        if (frame.header.type != MessageType::Hello) {
            return error_result("SESSION_NOT_READY", "HELLO required before REQUEST", true);
        }

        std::uint64_t min_version = 0;
        std::uint64_t max_version = 0;
        if (!parse_hello_versions(frame.payload, min_version, max_version)) {
            return error_result("INVALID_HELLO", "HELLO payload is invalid", true);
        }
        if (min_version > kProtocolVersion || max_version < kProtocolVersion) {
            return error_result("UNSUPPORTED_VERSION", "protocol version 1 is not supported by client range", true);
        }

        state_ = State::Ready;
        return ready_result();
    }

    if (frame.header.type == MessageType::Request) {
        return error_result("REQUEST_NOT_IMPLEMENTED", "v1 request dispatch is not implemented yet", false);
    }

    return error_result("PROTOCOL_VIOLATION", "unexpected client frame for READY session", true);
}

Session::State Session::state() const {
    return state_;
}

bool Session::ready() const {
    return state_ == State::Ready;
}

Session::HandleResult Session::error_result(const char *code,
                                            const char *message,
                                            bool close_after_send) {
    std::ostringstream os;
    os << "{\"code\":\"" << code << "\",\"message\":\"" << message << "\"}";
    if (close_after_send) state_ = State::Closed;
    return {encode_json(MessageType::Error, os.str()), close_after_send};
}

Session::HandleResult Session::ready_result() {
    std::ostringstream os;
    os << "{\"version\":1,\"service\":\"network_service\",\"sessionId\":\""
       << session_id_ << "\",\"generation\":" << generation_
       << ",\"capabilities\":[]}";
    return {encode_json(MessageType::Ready, os.str()), false};
}

} // namespace ipc_v1
} // namespace network_service
