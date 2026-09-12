#include "ipc/network_ipc_v1_business_dispatch.h"

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include "ipc/network_ipc_representation.h"
#include "ipc/network_ipc_v1_codec.h"
#include "service/network_daemon.h"

namespace network_service {
namespace ipc_v1 {
namespace {

constexpr int kMaxJsonDepth = 16;

class JsonReader {
public:
    explicit JsonReader(std::string_view input) : input_(input) {}

    bool find_member(const char *wanted, std::string_view *raw) {
        if (wanted == nullptr || raw == nullptr) return false;
        skip_ws();
        if (!consume('{')) return false;
        skip_ws();
        bool found = false;
        if (consume('}')) {
            skip_ws();
            return false;
        }
        for (;;) {
            std::string key;
            if (!parse_string(&key)) return false;
            skip_ws();
            if (!consume(':')) return false;
            skip_ws();
            const std::size_t begin = pos_;
            if (!skip_value(0)) return false;
            const std::size_t end = pos_;
            if (key == wanted) {
                if (found) return false;
                *raw = input_.substr(begin, end - begin);
                found = true;
            }
            skip_ws();
            if (consume('}')) break;
            if (!consume(',')) return false;
            skip_ws();
        }
        skip_ws();
        return found && pos_ == input_.size();
    }

    bool parse_full_string(std::string *out) {
        if (out == nullptr) return false;
        skip_ws();
        if (!parse_string(out)) return false;
        skip_ws();
        return pos_ == input_.size();
    }

private:
    static bool append_utf8(std::uint32_t cp, std::string *out) {
        if (out == nullptr || cp > 0x10ffffU ||
            (cp >= 0xd800U && cp <= 0xdfffU)) {
            return false;
        }
        if (cp <= 0x7fU) {
            out->push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ffU) {
            out->push_back(static_cast<char>(0xc0U | (cp >> 6U)));
            out->push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
        } else if (cp <= 0xffffU) {
            out->push_back(static_cast<char>(0xe0U | (cp >> 12U)));
            out->push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
            out->push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
        } else {
            out->push_back(static_cast<char>(0xf0U | (cp >> 18U)));
            out->push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3fU)));
            out->push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
            out->push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
        }
        return true;
    }

    static int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool parse_hex4(std::uint32_t *out) {
        if (out == nullptr || pos_ + 4 > input_.size()) return false;
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hex_value(input_[pos_++]);
            if (h < 0) return false;
            value = (value << 4U) | static_cast<std::uint32_t>(h);
        }
        *out = value;
        return true;
    }

    bool parse_string(std::string *out) {
        if (out == nullptr || !consume('"')) return false;
        out->clear();
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20U) return false;
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= input_.size()) return false;
            const char escaped = input_[pos_++];
            switch (escaped) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                std::uint32_t first = 0;
                if (!parse_hex4(&first)) return false;
                if (first >= 0xd800U && first <= 0xdbffU) {
                    if (pos_ + 2 > input_.size() || input_[pos_] != '\\' ||
                        input_[pos_ + 1] != 'u') {
                        return false;
                    }
                    pos_ += 2;
                    std::uint32_t second = 0;
                    if (!parse_hex4(&second) || second < 0xdc00U || second > 0xdfffU) {
                        return false;
                    }
                    const std::uint32_t cp =
                        0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U);
                    if (!append_utf8(cp, out)) return false;
                } else {
                    if (!append_utf8(first, out)) return false;
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool skip_string() {
        std::string ignored;
        return parse_string(&ignored);
    }

    bool skip_value(int depth) {
        if (depth > kMaxJsonDepth || pos_ >= input_.size()) return false;
        switch (input_[pos_]) {
        case '{': return skip_object(depth + 1);
        case '[': return skip_array(depth + 1);
        case '"': return skip_string();
        case 't': return literal("true");
        case 'f': return literal("false");
        case 'n': return literal("null");
        default: return skip_number();
        }
    }

    bool skip_object(int depth) {
        if (!consume('{')) return false;
        skip_ws();
        if (consume('}')) return true;
        for (;;) {
            if (!skip_string()) return false;
            skip_ws();
            if (!consume(':')) return false;
            skip_ws();
            if (!skip_value(depth)) return false;
            skip_ws();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skip_ws();
        }
    }

    bool skip_array(int depth) {
        if (!consume('[')) return false;
        skip_ws();
        if (consume(']')) return true;
        for (;;) {
            if (!skip_value(depth)) return false;
            skip_ws();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skip_ws();
        }
    }

    bool skip_number() {
        const std::size_t start = pos_;
        if (consume('-') && pos_ >= input_.size()) return false;
        if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '0') {
            ++pos_;
            if (pos_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                return false;
            }
        } else {
            if (!std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
            while (pos_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                ++pos_;
            }
        }
        if (consume('.')) {
            if (pos_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
            while (pos_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (pos_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
            while (pos_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        return pos_ > start;
    }

    bool literal(std::string_view text) {
        if (input_.substr(pos_, text.size()) != text) return false;
        pos_ += text.size();
        return true;
    }

    void skip_ws() {
        while (pos_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char expected) {
        if (pos_ < input_.size() && input_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::string_view input_;
    std::size_t pos_ = 0;
};

bool member(std::string_view object, const char *key, std::string_view *raw) {
    JsonReader reader(object);
    return reader.find_member(key, raw);
}

bool read_string(std::string_view object, const char *key, std::string *out) {
    std::string_view raw;
    if (!member(object, key, &raw)) return false;
    JsonReader reader(raw);
    return reader.parse_full_string(out);
}

bool read_bool(std::string_view object, const char *key, bool *out) {
    if (out == nullptr) return false;
    std::string_view raw;
    if (!member(object, key, &raw)) return false;
    if (raw == "true") {
        *out = true;
        return true;
    }
    if (raw == "false") {
        *out = false;
        return true;
    }
    return false;
}

std::vector<std::uint8_t> encode_response_payload(const std::string &payload) {
    CodecError error = CodecError::None;
    auto frame = encode_frame(MessageType::Response, payload, error);
    if (error != CodecError::None) return {};
    return frame;
}

std::vector<std::uint8_t> response_error(std::uint64_t request_id,
                                         int status,
                                         const char *code,
                                         const std::string &message) {
    std::ostringstream os;
    os << "{\"requestId\":" << request_id
       << ",\"status\":" << status
       << ",\"error\":{\"code\":\"" << code
       << "\",\"message\":\"" << ipc_representation::json_escape(message)
       << "\"}}";
    return encode_response_payload(os.str());
}

std::vector<std::uint8_t> response_success(std::uint64_t request_id,
                                           int status,
                                           const std::string &payload_json) {
    std::ostringstream os;
    os << "{\"requestId\":" << request_id
       << ",\"status\":" << status
       << ",\"result\":" << payload_json << "}";
    return encode_response_payload(os.str());
}

template <typename T>
std::vector<std::uint8_t> operation_response(
    std::uint64_t request_id,
    const NetworkOperationResult<T> &result,
    std::string (*payload_encoder)(const T &)) {
    if (!result.ok()) {
        return response_error(request_id, result.status, "OPERATION_FAILED", result.error);
    }
    return response_success(request_id, result.status, payload_encoder(result.value));
}

std::vector<std::uint8_t> invalid_params(std::uint64_t request_id,
                                         const std::string &message) {
    return response_error(request_id, 400, "INVALID_PARAMS", message);
}

} // namespace

std::vector<std::uint8_t> dispatch_business_request(
    NetworkDaemon &daemon,
    std::uint64_t request_id,
    const std::string &method,
    const std::string &params_json) {
    if (method == "eth.get_config") {
        return operation_response(request_id,
                                  daemon.eth_get_config(),
                                  ipc_representation::ethernet_config_payload);
    }
    if (method == "eth.set_dhcp") {
        return operation_response(request_id,
                                  daemon.eth_set_dhcp(),
                                  ipc_representation::ethernet_config_payload);
    }
    if (method == "eth.set_static") {
        std::string ip;
        std::string mask;
        std::string gateway;
        std::string dns;
        if (!read_string(params_json, "ip", &ip) ||
            !read_string(params_json, "mask", &mask) ||
            !read_string(params_json, "gateway", &gateway) ||
            !read_string(params_json, "dns", &dns)) {
            return invalid_params(request_id,
                                  "eth.set_static requires string ip/mask/gateway/dns");
        }
        return operation_response(request_id,
                                  daemon.eth_set_static(ip, mask, gateway, dns),
                                  ipc_representation::ethernet_config_payload);
    }
    if (method == "wifi.scan.start") {
        return operation_response(request_id,
                                  daemon.wifi_scan_start(),
                                  ipc_representation::wifi_scan_status_payload);
    }
    if (method == "wifi.scan.status") {
        return operation_response(request_id,
                                  daemon.wifi_scan_status(),
                                  ipc_representation::wifi_scan_status_payload);
    }
    if (method == "wifi.set_enabled") {
        bool enabled = false;
        if (!read_bool(params_json, "enabled", &enabled)) {
            return invalid_params(request_id,
                                  "wifi.set_enabled requires boolean enabled");
        }
        return operation_response(request_id,
                                  daemon.wifi_set_enabled(enabled),
                                  ipc_representation::wifi_enabled_payload);
    }
    if (method == "wifi.connect") {
        std::string ssid;
        std::string password;
        if (!read_string(params_json, "ssid", &ssid) || ssid.empty() ||
            !read_string(params_json, "password", &password)) {
            return invalid_params(request_id,
                                  "wifi.connect requires non-empty string ssid and string password");
        }
        return operation_response(request_id,
                                  daemon.wifi_connect(ssid, password),
                                  ipc_representation::wifi_command_payload);
    }
    if (method == "wifi.connect_saved") {
        std::string ssid;
        if (!read_string(params_json, "ssid", &ssid) || ssid.empty()) {
            return invalid_params(request_id,
                                  "wifi.connect_saved requires non-empty string ssid");
        }
        return operation_response(request_id,
                                  daemon.wifi_connect_saved(ssid),
                                  ipc_representation::wifi_command_payload);
    }
    if (method == "wifi.saved_list") {
        return operation_response(request_id,
                                  daemon.wifi_list_saved(),
                                  ipc_representation::wifi_saved_payload);
    }
    if (method == "wifi.forget") {
        std::string ssid;
        if (!read_string(params_json, "ssid", &ssid) || ssid.empty()) {
            return invalid_params(request_id,
                                  "wifi.forget requires non-empty string ssid");
        }
        return operation_response(request_id,
                                  daemon.wifi_forget(ssid),
                                  ipc_representation::wifi_command_payload);
    }
    if (method == "wifi.autoconnect") {
        std::string ssid;
        bool enabled = false;
        if (!read_string(params_json, "ssid", &ssid) || ssid.empty() ||
            !read_bool(params_json, "enabled", &enabled)) {
            return invalid_params(request_id,
                                  "wifi.autoconnect requires non-empty string ssid and boolean enabled");
        }
        return operation_response(request_id,
                                  daemon.wifi_set_autoconnect(ssid, enabled),
                                  ipc_representation::wifi_command_payload);
    }
    if (method == "wifi.disconnect") {
        return operation_response(request_id,
                                  daemon.wifi_disconnect(),
                                  ipc_representation::wifi_command_payload);
    }
    return response_error(request_id, 404, "METHOD_NOT_FOUND", "unknown method");
}

} // namespace ipc_v1
} // namespace network_service
