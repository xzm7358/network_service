#include "ipc/network_ipc_v1_codec.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using network_service::ipc_v1::CodecError;
using network_service::ipc_v1::Frame;
using network_service::ipc_v1::MessageType;

enum class Scenario {
    Ping,
    Version,
    Status,
    ScanSuccess,
    ScanFailure,
    ScanResults,
    SavedList,
    WifiOn,
    WifiOff,
    Connect,
    ConnectSaved,
    Disconnect,
    EthStatus,
    PolicyState,
    Route,
    Dns,
    Forget,
    Autoconnect,
    Policy,
    ApplyRoute,
    Subscribe,
};

void readAll(int fd, void *data, std::size_t size) {
    auto *bytes = static_cast<unsigned char *>(data);
    while (size > 0) {
        const ssize_t count = ::recv(fd, bytes, size, 0);
        assert(count > 0);
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
}

Frame receiveFrame(int fd) {
    unsigned char header_bytes[network_service::ipc_v1::kHeaderSize]{};
    readAll(fd, header_bytes, sizeof(header_bytes));

    Frame frame;
    CodecError error = CodecError::None;
    assert(network_service::ipc_v1::decode_header(
        header_bytes, sizeof(header_bytes), frame.header, error));
    frame.payload.assign(frame.header.payload_length, '\0');
    if (!frame.payload.empty()) {
        readAll(fd, frame.payload.data(), frame.payload.size());
    }
    return frame;
}

void sendFrame(int fd, MessageType type, const std::string &payload) {
    CodecError error = CodecError::None;
    const auto bytes = network_service::ipc_v1::encode_frame(type, payload, error);
    assert(error == CodecError::None);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::send(fd, bytes.data() + offset, bytes.size() - offset, 0);
        assert(count > 0);
        offset += static_cast<std::size_t>(count);
    }
}

void sendReady(int fd) {
    sendFrame(fd, MessageType::Ready,
              R"({"version":1,"service":"network_service","sessionId":"test","generation":7,"capabilities":["events","snapshot-rebase"]})");
}

void sendResponse(int fd, const std::string &result, int request_id = 1) {
    sendFrame(fd, MessageType::Response,
              "{\"requestId\":" + std::to_string(request_id) +
                  ",\"status\":200,\"result\":" + result + "}");
}

void serveScenario(int listener, Scenario scenario) {
    const int fd = ::accept(listener, nullptr, nullptr);
    assert(fd >= 0);
    const Frame hello = receiveFrame(fd);
    assert(hello.header.type == MessageType::Hello);
    assert(hello.payload.find("\"minVersion\":1") != std::string::npos);
    sendReady(fd);

    const Frame request = receiveFrame(fd);
    assert(request.header.type == MessageType::Request);
    if (scenario == Scenario::Ping || scenario == Scenario::Version) {
        assert(request.payload.find("\"method\":\"network.ping\"") !=
               std::string::npos);
        sendResponse(fd,
                     R"({"service":"network_service","protocolVersion":1})");
    } else if (scenario == Scenario::Status) {
        assert(request.payload.find("\"method\":\"network.snapshot\"") !=
               std::string::npos);
        sendResponse(fd,
                     R"({"generation":7,"snapshotSeq":0,"snapshot":{"online":true,"wifi":{"ssid":"Lab"}}})");
    } else if (scenario == Scenario::ScanSuccess) {
        assert(request.payload.find("\"method\":\"wifi.scan.start\"") !=
               std::string::npos);
        sendFrame(fd, MessageType::Response,
                  R"({"requestId":1,"status":202,"result":{"scanId":1,"state":"scanning","error":"","results":{"count":0,"aps":[]}}})");
        const Frame status_request = receiveFrame(fd);
        assert(status_request.payload.find("\"method\":\"wifi.scan.status\"") !=
               std::string::npos);
        sendResponse(fd,
                     R"({"scanId":1,"state":"ready","error":"","results":{"count":1,"aps":[{"ssid":"Lab"}]}})",
                     2);
    } else if (scenario == Scenario::ScanFailure) {
        assert(request.payload.find("\"method\":\"wifi.scan.start\"") !=
               std::string::npos);
        sendFrame(fd, MessageType::Response,
                  R"({"requestId":1,"status":202,"result":{"scanId":2,"state":"scanning","error":"","results":{"count":0,"aps":[]}}})");
        const Frame status_request = receiveFrame(fd);
        assert(status_request.payload.find("\"method\":\"wifi.scan.status\"") !=
               std::string::npos);
        sendResponse(fd,
                     R"({"scanId":2,"state":"failed","error":"wpa_ctrl connect failed","results":{"count":0,"aps":[]}})",
                     2);
    } else if (scenario == Scenario::ScanResults) {
        assert(request.payload.find("\"method\":\"wifi.scan.status\"") !=
               std::string::npos);
        sendResponse(fd,
                     R"({"scanId":3,"state":"ready","error":"","results":{"count":1,"aps":[{"ssid":"Lab"}]}})");
    } else if (scenario == Scenario::SavedList) {
        assert(request.payload.find("\"method\":\"wifi.saved_list\"") !=
               std::string::npos);
        sendResponse(fd,
                     R"({"count":1,"saved":[{"ssid":"Lab","network_id":3}]})");
    } else if (scenario == Scenario::WifiOn || scenario == Scenario::WifiOff) {
        assert(request.payload.find("\"method\":\"wifi.set_enabled\"") !=
               std::string::npos);
        const bool enabled = scenario == Scenario::WifiOn;
        const std::string expected = std::string("\"enabled\":") +
                                     (enabled ? "true" : "false");
        assert(request.payload.find(expected) != std::string::npos);
        sendResponse(fd, std::string("{\"enabled\":") +
                             (enabled ? "true}" : "false}"));
    } else if (scenario == Scenario::Connect) {
        assert(request.payload.find("\"method\":\"wifi.connect\"") !=
               std::string::npos);
        assert(request.payload.find("\"ssid\":\"Lab \\\"WiFi\\\"\"") !=
               std::string::npos);
        assert(request.payload.find("\"password\":\"p\\\\ss\"") !=
               std::string::npos);
        sendResponse(fd, R"({"requested":"connect"})");
    } else if (scenario == Scenario::ConnectSaved) {
        assert(request.payload.find("\"method\":\"wifi.connect_saved\"") !=
               std::string::npos);
        assert(request.payload.find("\"ssid\":\"Lab\"") != std::string::npos);
        sendResponse(fd, R"({"requested":"connect_saved"})");
    } else if (scenario == Scenario::Disconnect) {
        assert(request.payload.find("\"method\":\"wifi.disconnect\"") !=
               std::string::npos);
        sendResponse(fd, R"({"requested":"disconnect"})");
    } else if (scenario == Scenario::EthStatus) {
        assert(request.payload.find("\"method\":\"network.snapshot\"") !=
               std::string::npos);
        sendResponse(fd,
                     R"({"generation":7,"snapshotSeq":0,"snapshot":{"online":false,"eth":{"iface":"eth0","has_ip":true,"ip4":"192.0.2.10"}}})");
    } else if (scenario == Scenario::PolicyState) {
        assert(request.payload.find("\"method\":\"network.route_policy.get\"") !=
               std::string::npos);
        sendResponse(fd, R"({"policy":"ethernet_preferred","persistent":false})");
    } else if (scenario == Scenario::Route) {
        assert(request.payload.find("\"method\":\"network.snapshot\"") !=
               std::string::npos);
        sendResponse(fd,
                     R"({"generation":7,"snapshotSeq":0,"snapshot":{"online":true,"primary_iface":"wlan0","route_policy":"wifi_preferred","eth":{"iface":"eth0"},"wifi":{"iface":"wlan0"}}})");
    } else if (scenario == Scenario::Dns) {
        assert(request.payload.find("\"method\":\"network.snapshot\"") !=
               std::string::npos);
        sendResponse(fd,
                     R"({"generation":7,"snapshotSeq":0,"snapshot":{"online":true,"dns_available":true,"dns4":"1.1.1.1","dns_policy":"overwrite"}})");
    } else if (scenario == Scenario::Forget) {
        assert(request.payload.find("\"method\":\"wifi.forget\"") !=
               std::string::npos);
        assert(request.payload.find("\"ssid\":\"Lab\"") != std::string::npos);
        sendResponse(fd, R"({"requested":"forget"})");
    } else if (scenario == Scenario::Autoconnect) {
        assert(request.payload.find("\"method\":\"wifi.autoconnect\"") !=
               std::string::npos);
        assert(request.payload.find("\"ssid\":\"Lab\"") != std::string::npos);
        assert(request.payload.find("\"enabled\":true") != std::string::npos);
        sendResponse(fd, R"({"requested":"autoconnect","enabled":true})");
    } else if (scenario == Scenario::Policy) {
        assert(request.payload.find("\"method\":\"network.route_policy.apply\"") !=
               std::string::npos);
        assert(request.payload.find("\"policy\":\"wifi_only\"") != std::string::npos);
        sendResponse(fd, R"({"policy":"wifi_only","persistent":false})");
    } else if (scenario == Scenario::ApplyRoute) {
        assert(request.payload.find("\"method\":\"network.route_policy.get\"") !=
               std::string::npos);
        sendResponse(fd, R"({"policy":"wifi_preferred","persistent":false})");
        const Frame apply_request = receiveFrame(fd);
        assert(apply_request.payload.find(
                   "\"method\":\"network.route_policy.apply\"") != std::string::npos);
        assert(apply_request.payload.find("\"policy\":\"wifi_preferred\"") !=
               std::string::npos);
        sendResponse(fd, R"({"policy":"wifi_preferred","persistent":false})", 2);
    } else {
        assert(scenario == Scenario::Subscribe);
        assert(request.payload.find("\"method\":\"network.events.subscribe\"") !=
               std::string::npos);
        sendResponse(fd, R"({"subscribed":true})");
        sendFrame(fd, MessageType::Event,
                  R"({"event":"network.state.changed","generation":7,"seq":1,"payload":{"changed":["wifi"]}})");
    }

    // Network IPC v1 sessions are persistent. Keep the peer open long enough
    // for the client to drain the response and any queued event before the
    // test closes the session.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
}

std::string readPipe(int fd) {
    std::string output;
    char buffer[256];
    for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count <= 0) break;
        output.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    return output;
}

struct ProcessResult {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

ProcessResult runCli(const std::string &binary,
                     const std::string &socket_path,
                     const std::vector<std::string> &arguments) {
    int stdout_pipe[2]{};
    int stderr_pipe[2]{};
    assert(::pipe(stdout_pipe) == 0);
    assert(::pipe(stderr_pipe) == 0);

    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        assert(::dup2(stdout_pipe[1], STDOUT_FILENO) >= 0);
        assert(::dup2(stderr_pipe[1], STDERR_FILENO) >= 0);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);

        std::vector<std::string> command;
        command.push_back(binary);
        command.push_back("--socket");
        command.push_back(socket_path);
        command.insert(command.end(), arguments.begin(), arguments.end());
        std::vector<char *> argv;
        argv.reserve(command.size() + 1);
        for (std::string &argument : command) {
            argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(binary.c_str(), argv.data());
        _exit(127);
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    ProcessResult result;
    assert(WIFEXITED(status));
    result.exit_code = WEXITSTATUS(status);
    result.stdout_text = readPipe(stdout_pipe[0]);
    result.stderr_text = readPipe(stderr_pipe[0]);
    return result;
}

void runScenario(int listener,
                 Scenario scenario,
                 const std::string &binary,
                 const std::string &socket_path,
                 const std::vector<std::string> &arguments,
                 int expected_exit_code,
                 const std::string &expected_stdout,
                 const std::string &expected_stderr) {
    std::thread server([&] { serveScenario(listener, scenario); });
    const ProcessResult result = runCli(binary, socket_path, arguments);
    server.join();
    if (result.exit_code != expected_exit_code) {
        std::cerr << "networkctl scenario failed: expected " << expected_exit_code
                  << ", got " << result.exit_code << " stdout=" << result.stdout_text
                  << " stderr=" << result.stderr_text << '\n';
    }
    if (result.stdout_text.find(expected_stdout) == std::string::npos ||
        result.stderr_text.find(expected_stderr) == std::string::npos) {
        std::cerr << "networkctl output mismatch: stdout=" << result.stdout_text
                  << " stderr=" << result.stderr_text << '\n';
    }
    assert(result.exit_code == expected_exit_code);
    assert(result.stdout_text.find(expected_stdout) != std::string::npos);
    assert(result.stderr_text.find(expected_stderr) != std::string::npos);
}

} // namespace

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string binary = argv[1];
    if (::access(binary.c_str(), X_OK) != 0) {
        std::cerr << "networkctl test binary is not executable: " << binary << '\n';
        return 1;
    }

    char directory[] = "/tmp/networkctl-test-XXXXXX";
    assert(::mkdtemp(directory) != nullptr);
    const std::string folder = directory;
    const std::string socket_path = folder + "/network.sock";

    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
    assert(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    assert(::listen(listener, 8) == 0);

    runScenario(listener, Scenario::Ping, binary, socket_path, {"ping"}, 0,
                R"({"service":"network_service","protocolVersion":1})", "");
    runScenario(listener, Scenario::Version, binary, socket_path, {"version"}, 0,
                "1", "");
    runScenario(listener, Scenario::Status, binary, socket_path, {"status"}, 0,
                R"({"online":true,"wifi":{"ssid":"Lab"}})", "");
    runScenario(listener, Scenario::ScanSuccess, binary, socket_path, {"scan"}, 0,
                R"({"count":1,"aps":[{"ssid":"Lab"}]})", "");
    runScenario(listener, Scenario::ScanFailure, binary, socket_path, {"scan"}, 1, "",
                "wpa_ctrl connect failed");
    runScenario(listener, Scenario::ScanResults, binary, socket_path, {"scan-results"}, 0,
                R"({"count":1,"aps":[{"ssid":"Lab"}]})", "");
    runScenario(listener, Scenario::SavedList, binary, socket_path, {"saved-list"}, 0,
                R"({"count":1,"saved":[{"ssid":"Lab","network_id":3}]})", "");
    runScenario(listener, Scenario::WifiOn, binary, socket_path, {"wifi-on"}, 0,
                R"({"enabled":true})", "");
    runScenario(listener, Scenario::WifiOff, binary, socket_path, {"wifi-off"}, 0,
                R"({"enabled":false})", "");
    runScenario(listener, Scenario::Connect, binary, socket_path,
                {"connect", "Lab \"WiFi\"", "p\\ss"}, 0,
                R"({"requested":"connect"})", "");
    runScenario(listener, Scenario::ConnectSaved, binary, socket_path,
                {"connect-saved", "Lab"}, 0,
                R"({"requested":"connect_saved"})", "");
    runScenario(listener, Scenario::Disconnect, binary, socket_path, {"disconnect"}, 0,
                R"({"requested":"disconnect"})", "");
    runScenario(listener, Scenario::EthStatus, binary, socket_path, {"eth-status"}, 0,
                R"({"iface":"eth0","has_ip":true,"ip4":"192.0.2.10"})", "");
    runScenario(listener, Scenario::PolicyState, binary, socket_path, {"policy-state"}, 0,
                R"({"policy":"ethernet_preferred","persistent":false})", "");
    runScenario(listener, Scenario::Route, binary, socket_path, {"route"}, 0,
                R"({"route_policy":"wifi_preferred","primary_iface":"wlan0","online":true,"eth":{"iface":"eth0"},"wifi":{"iface":"wlan0"}})", "");
    runScenario(listener, Scenario::Dns, binary, socket_path, {"dns"}, 0,
                R"({"dns_available":true,"dns4":"1.1.1.1","dns_policy":"overwrite"})", "");
    runScenario(listener, Scenario::Forget, binary, socket_path, {"forget", "Lab"}, 0,
                R"({"requested":"forget"})", "");
    runScenario(listener, Scenario::Autoconnect, binary, socket_path,
                {"autoconnect", "Lab", "on"}, 0,
                R"({"requested":"autoconnect","enabled":true})", "");
    runScenario(listener, Scenario::Policy, binary, socket_path,
                {"policy", "wifi-only"}, 0,
                R"({"policy":"wifi_only","persistent":false})", "");
    runScenario(listener, Scenario::ApplyRoute, binary, socket_path, {"apply-route"}, 0,
                R"({"policy":"wifi_preferred","persistent":false})", "");
    runScenario(listener, Scenario::Subscribe, binary, socket_path, {"subscribe"}, 1,
                R"({"event":"network.state.changed","generation":7,"seq":1,"payload":{"changed":["wifi"]}})",
                "network service closed the IPC session");

    ::close(listener);
    ::unlink(socket_path.c_str());
    ::rmdir(folder.c_str());
    std::cout << "networkctl CLI tests passed\n";
    return 0;
}
