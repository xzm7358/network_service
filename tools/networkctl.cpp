#include "network_service/v1_client.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <poll.h>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr const char *kDefaultSocket = "/tmp/smart_hmi_network.sock";
constexpr int kDefaultTimeoutMs = 5000;
constexpr int kDefaultPollMs = 100;
constexpr int kDefaultScanTimeoutMs = 10000;
constexpr int kMaxJsonDepth = 16;

using Clock = std::chrono::steady_clock;

struct Options {
    std::string socket_path = kDefaultSocket;
    std::string command;
    std::string ssid;
    std::string psk;
    bool ssid_provided = false;
    bool psk_provided = false;
    int timeout_ms = kDefaultTimeoutMs;
    int poll_ms = kDefaultPollMs;
    int scan_timeout_ms = kDefaultScanTimeoutMs;
};

class JsonReader {
public:
    explicit JsonReader(std::string_view input) : input_(input) {}

    bool findMember(std::string_view wanted, std::string_view *raw) {
        if (raw == nullptr) return false;
        skipWhitespace();
        if (!consume('{')) return false;
        skipWhitespace();
        if (consume('}')) return false;

        bool found = false;
        for (;;) {
            std::string key;
            if (!parseString(&key)) return false;
            skipWhitespace();
            if (!consume(':')) return false;
            skipWhitespace();
            const std::size_t begin = position_;
            if (!skipValue(0)) return false;
            const std::size_t end = position_;
            if (key == wanted) {
                if (found) return false;
                *raw = input_.substr(begin, end - begin);
                found = true;
            }
            skipWhitespace();
            if (consume('}')) break;
            if (!consume(',')) return false;
            skipWhitespace();
        }
        skipWhitespace();
        return found && position_ == input_.size();
    }

    bool parseStringValue(std::string *out) {
        if (out == nullptr) return false;
        skipWhitespace();
        if (!parseString(out)) return false;
        skipWhitespace();
        return position_ == input_.size();
    }

    bool parseInteger(int *out) {
        if (out == nullptr) return false;
        skipWhitespace();
        bool negative = consume('-');
        if (position_ >= input_.size() || input_[position_] < '0' ||
            input_[position_] > '9') {
            return false;
        }

        std::int64_t value = 0;
        while (position_ < input_.size() && input_[position_] >= '0' &&
               input_[position_] <= '9') {
            const int digit = input_[position_] - '0';
            if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
            ++position_;
        }
        if (negative) value = -value;
        skipWhitespace();
        if (position_ != input_.size() || value < INT_MIN || value > INT_MAX) {
            return false;
        }
        *out = static_cast<int>(value);
        return true;
    }

private:
    static bool appendUtf8(std::uint32_t code_point, std::string *out) {
        if (out == nullptr || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        if (code_point <= 0x7fU) {
            out->push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7ffU) {
            out->push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
            out->push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        } else if (code_point <= 0xffffU) {
            out->push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
            out->push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
            out->push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        } else {
            out->push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
            out->push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
            out->push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
            out->push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        }
        return true;
    }

    static int hexValue(char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    bool parseHex4(std::uint32_t *out) {
        if (out == nullptr || position_ + 4 > input_.size()) return false;
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = hexValue(input_[position_++]);
            if (digit < 0) return false;
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        *out = value;
        return true;
    }

    bool parseString(std::string *out) {
        if (out == nullptr || !consume('"')) return false;
        out->clear();
        while (position_ < input_.size()) {
            const unsigned char value = static_cast<unsigned char>(input_[position_++]);
            if (value == '"') return true;
            if (value < 0x20U) return false;
            if (value != '\\') {
                out->push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
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
                if (!parseHex4(&first)) return false;
                if (first >= 0xd800U && first <= 0xdbffU) {
                    if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u') {
                        return false;
                    }
                    position_ += 2;
                    std::uint32_t second = 0;
                    if (!parseHex4(&second) || second < 0xdc00U || second > 0xdfffU) {
                        return false;
                    }
                    const std::uint32_t code_point =
                        0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U);
                    if (!appendUtf8(code_point, out)) return false;
                } else if (!appendUtf8(first, out)) {
                    return false;
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool skipString() {
        std::string ignored;
        return parseString(&ignored);
    }

    bool skipValue(int depth) {
        if (depth > kMaxJsonDepth || position_ >= input_.size()) return false;
        switch (input_[position_]) {
        case '{': return skipObject(depth + 1);
        case '[': return skipArray(depth + 1);
        case '"': return skipString();
        case 't': return consumeLiteral("true");
        case 'f': return consumeLiteral("false");
        case 'n': return consumeLiteral("null");
        default: return skipNumber();
        }
    }

    bool skipObject(int depth) {
        if (!consume('{')) return false;
        skipWhitespace();
        if (consume('}')) return true;
        for (;;) {
            if (!skipString()) return false;
            skipWhitespace();
            if (!consume(':')) return false;
            skipWhitespace();
            if (!skipValue(depth)) return false;
            skipWhitespace();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skipWhitespace();
        }
    }

    bool skipArray(int depth) {
        if (!consume('[')) return false;
        skipWhitespace();
        if (consume(']')) return true;
        for (;;) {
            if (!skipValue(depth)) return false;
            skipWhitespace();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skipWhitespace();
        }
    }

    bool skipNumber() {
        const std::size_t start = position_;
        (void)consume('-');
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9') {
                return false;
            }
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') return false;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            if (position_ >= input_.size() || input_[position_] < '0' ||
                input_[position_] > '9') {
                return false;
            }
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= input_.size() || input_[position_] < '0' ||
                input_[position_] > '9') {
                return false;
            }
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        return position_ > start;
    }

    bool consumeLiteral(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }

    void skipWhitespace() {
        while (position_ < input_.size()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' && value != '\n' && value != '\r') break;
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

bool jsonMember(std::string_view object, std::string_view key, std::string_view *raw) {
    JsonReader reader(object);
    return reader.findMember(key, raw);
}

bool jsonString(std::string_view object, std::string_view key, std::string *value) {
    std::string_view raw;
    if (!jsonMember(object, key, &raw)) return false;
    JsonReader reader(raw);
    return reader.parseStringValue(value);
}

bool jsonInteger(std::string_view object, std::string_view key, int *value) {
    std::string_view raw;
    if (!jsonMember(object, key, &raw)) return false;
    JsonReader reader(raw);
    return reader.parseInteger(value);
}

bool parseResponse(const std::string &response,
                   int *status,
                   std::string_view *result,
                   std::string *error_message) {
    if (status == nullptr || result == nullptr || error_message == nullptr) return false;
    if (!jsonInteger(response, "status", status)) return false;
    error_message->clear();
    *result = {};
    if (*status >= 200 && *status < 300) {
        return jsonMember(response, "result", result);
    }

    std::string_view error_object;
    if (jsonMember(response, "error", &error_object)) {
        if (!jsonString(error_object, "message", error_message)) {
            *error_message = std::string(error_object);
        }
    }
    if (error_message->empty()) *error_message = response;
    return true;
}

std::string scanError(std::string_view result) {
    std::string error;
    if (jsonString(result, "error", &error)) return error;
    return {};
}

bool positiveInteger(const std::string &text, int *value) {
    if (value == nullptr || text.empty()) return false;
    std::int64_t parsed = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        if (parsed > (std::numeric_limits<std::int64_t>::max() - (ch - '0')) / 10) {
            return false;
        }
        parsed = parsed * 10 + static_cast<std::int64_t>(ch - '0');
        if (parsed > INT_MAX) return false;
    }
    if (parsed <= 0) return false;
    *value = static_cast<int>(parsed);
    return true;
}

void printUsage(const char *argv0, std::ostream &out) {
    out << "Usage: " << argv0
        << " [--socket PATH] [--timeout-ms N] [--poll-ms N] [--scan-timeout-ms N]"
           " COMMAND [ARGS...]\n\n"
           "Commands:\n"
           "  status                  Print the full network snapshot as JSON\n"
           "  scan                    Start a Wi-Fi scan and wait for its result\n"
           "  wifi-on                 Enable Wi-Fi\n"
           "  wifi-off                Disable Wi-Fi\n"
           "  connect SSID PSK        Connect to a Wi-Fi network\n"
           "  subscribe               Print network events until interrupted\n";
}

bool parseArgs(int argc, char **argv, Options *options, std::string *error) {
    if (options == nullptr || error == nullptr) return false;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto requireValue = [&](std::string *value) -> bool {
            if (value == nullptr || index + 1 >= argc) return false;
            *value = argv[++index];
            return !value->empty();
        };
        auto requireNumber = [&](int *value) -> bool {
            std::string text;
            return requireValue(&text) && positiveInteger(text, value);
        };

        if (arg == "--socket") {
            if (!requireValue(&options->socket_path)) {
                *error = "--socket requires a non-empty path";
                return false;
            }
        } else if (arg == "--timeout-ms") {
            if (!requireNumber(&options->timeout_ms)) {
                *error = "--timeout-ms requires a positive integer";
                return false;
            }
        } else if (arg == "--poll-ms") {
            if (!requireNumber(&options->poll_ms)) {
                *error = "--poll-ms requires a positive integer";
                return false;
            }
        } else if (arg == "--scan-timeout-ms") {
            if (!requireNumber(&options->scan_timeout_ms)) {
                *error = "--scan-timeout-ms requires a positive integer";
                return false;
            }
        } else if (arg == "--help" || arg == "-h") {
            *error = "help";
            return false;
        } else if (!arg.empty() && arg.front() == '-') {
            *error = "unknown option: " + arg;
            return false;
        } else if (options->command.empty()) {
            options->command = arg;
        } else if (options->command == "connect" && !options->ssid_provided) {
            options->ssid = arg;
            options->ssid_provided = true;
        } else if (options->command == "connect" && !options->psk_provided) {
            options->psk = arg;
            options->psk_provided = true;
        } else {
            *error = "unexpected argument: " + arg;
            return false;
        }
    }

    if (options->command.empty()) {
        *error = "missing command";
        return false;
    }
    if (options->command == "connect" && !options->ssid_provided) {
        *error = "connect requires SSID and PSK";
        return false;
    }
    if (options->command == "connect" && !options->psk_provided) {
        *error = "connect requires SSID and PSK";
        return false;
    }
    if (options->command != "status" && options->command != "scan" &&
        options->command != "wifi-on" && options->command != "wifi-off" &&
        options->command != "connect" && options->command != "subscribe") {
        *error = "unknown command: " + options->command;
        return false;
    }
    return true;
}

int fail(std::ostream &err, const std::string &message) {
    err << message << '\n';
    return 1;
}

bool request(network_service::NetworkServiceClient &client,
             const std::string &method,
             const std::string &params,
             int timeout_ms,
             std::string *response,
             std::ostream &err) {
    std::string client_error;
    if (client.request(method, params, *response, client_error, timeout_ms)) return true;
    (void)fail(err, client_error.empty() ? "network service request failed" : client_error);
    return false;
}

bool successfulResponse(const std::string &response,
                        std::string_view *result,
                        std::ostream &err) {
    int status = 0;
    std::string message;
    if (!parseResponse(response, &status, result, &message)) {
        (void)fail(err, "networkctl: malformed network service response");
        return false;
    }
    if (status < 200 || status >= 300) {
        (void)fail(err, message);
        return false;
    }
    return true;
}

int runStatus(network_service::NetworkServiceClient &client,
              const Options &options,
              std::ostream &out,
              std::ostream &err) {
    std::string response;
    if (!request(client, "network.snapshot", "{}", options.timeout_ms, &response, err)) {
        return 1;
    }
    std::string_view result;
    if (!successfulResponse(response, &result, err)) return 1;
    std::string_view snapshot;
    if (!jsonMember(result, "snapshot", &snapshot)) {
        return fail(err, "networkctl: snapshot missing from network service response");
    }
    out << snapshot << '\n';
    return 0;
}

int runScan(network_service::NetworkServiceClient &client,
            const Options &options,
            std::ostream &out,
            std::ostream &err) {
    std::string response;
    if (!request(client, "wifi.scan.start", "{}", options.timeout_ms, &response, err)) {
        return 1;
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(options.scan_timeout_ms);
    for (;;) {
        std::string_view result;
        if (!successfulResponse(response, &result, err)) return 1;

        std::string state;
        if (!jsonString(result, "state", &state)) {
            return fail(err, "networkctl: scan response has no state");
        }
        if (state == "ready") {
            std::string_view results;
            if (!jsonMember(result, "results", &results)) {
                return fail(err, "networkctl: completed scan has no results");
            }
            out << results << '\n';
            return 0;
        }
        if (state == "failed") {
            const std::string message = scanError(result);
            return fail(err, message.empty() ? "wifi scan failed" : message);
        }
        if (state != "scanning" && state != "idle") {
            return fail(err, "networkctl: unknown scan state: " + state);
        }
        if (Clock::now() >= deadline) {
            return fail(err, "wifi scan timed out");
        }

        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      deadline - Clock::now())
                                      .count();
        if (remaining_ms <= 0) return fail(err, "wifi scan timed out");
        const auto delay_ms = std::min<std::int64_t>(options.poll_ms, remaining_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        const auto request_remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                               deadline - Clock::now())
                                               .count();
        if (request_remaining_ms <= 0) return fail(err, "wifi scan timed out");
        const int status_timeout_ms = static_cast<int>(std::min<std::int64_t>(
            options.timeout_ms, request_remaining_ms));
        if (!request(client, "wifi.scan.status", "{}", status_timeout_ms, &response, err)) {
            return 1;
        }
    }
}

int runCommand(network_service::NetworkServiceClient &client,
               const Options &options,
               std::ostream &out,
               std::ostream &err) {
    if (options.command == "status") return runStatus(client, options, out, err);
    if (options.command == "scan") return runScan(client, options, out, err);

    if (options.command == "subscribe") {
        std::string response;
        if (!request(client, "network.events.subscribe", "{}", options.timeout_ms,
                     &response, err)) {
            return 1;
        }
        std::string_view ignored_result;
        if (!successfulResponse(response, &ignored_result, err)) return 1;

        for (;;) {
            std::string event;
            std::string read_error;
            if (client.readNext(event, read_error)) {
                out << event << '\n';
                out.flush();
                continue;
            }
            if (!read_error.empty()) return fail(err, read_error);

            pollfd descriptor{};
            descriptor.fd = client.fd();
            descriptor.events = POLLIN;
            for (;;) {
                const int poll_result = ::poll(&descriptor, 1, -1);
                if (poll_result > 0) break;
                if (poll_result < 0 && errno == EINTR) continue;
                return fail(err, std::string("networkctl: event wait failed: ") +
                                     std::strerror(errno));
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
                (descriptor.revents & POLLIN) == 0) {
                return fail(err, "network service closed the event session");
            }
        }
    }

    std::string method;
    std::string params;
    if (options.command == "wifi-on" || options.command == "wifi-off") {
        method = "wifi.set_enabled";
        params = std::string("{\"enabled\":") +
                 (options.command == "wifi-on" ? "true}" : "false}");
    } else if (options.command == "connect") {
        method = "wifi.connect";
        params = "{\"ssid\":" + network_service::jsonQuote(options.ssid) +
                 ",\"password\":" + network_service::jsonQuote(options.psk) + "}";
    } else {
        return fail(err, "networkctl: command dispatch failed");
    }

    std::string response;
    if (!request(client, method, params, options.timeout_ms, &response, err)) return 1;
    std::string_view result;
    if (!successfulResponse(response, &result, err)) return 1;
    out << result << '\n';
    return 0;
}

int networkctlMain(int argc, char **argv) {
    Options options;
    std::string parse_error;
    if (!parseArgs(argc, argv, &options, &parse_error)) {
        if (parse_error == "help") {
            printUsage(argv[0], std::cout);
            return 0;
        }
        if (!parse_error.empty()) std::cerr << "networkctl: " << parse_error << '\n';
        printUsage(argv[0], std::cerr);
        return 2;
    }

    network_service::NetworkServiceClient client;
    std::string error;
    if (!client.connectTo(options.socket_path, error, options.timeout_ms)) {
        return fail(std::cerr, error.empty() ? "network service connection failed" : error);
    }
    return runCommand(client, options, std::cout, std::cerr);
}

} // namespace

int main(int argc, char **argv) { return networkctlMain(argc, argv); }
