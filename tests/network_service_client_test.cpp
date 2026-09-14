#include "network_service/v1_client.h"
#include "ipc/network_ipc_v1_codec.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace network_service::ipc_v1;

void read_all(int fd, void *data, std::size_t size) {
    auto *bytes = static_cast<std::uint8_t *>(data);
    while (size > 0) {
        const ssize_t count = ::recv(fd, bytes, size, 0);
        assert(count > 0);
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
}

Frame receive(int fd) {
    std::uint8_t bytes[kHeaderSize]{};
    read_all(fd, bytes, sizeof(bytes));
    Frame frame;
    CodecError error = CodecError::None;
    assert(decode_header(bytes, sizeof(bytes), frame.header, error));
    frame.payload.assign(frame.header.payload_length, '\0');
    if (!frame.payload.empty()) read_all(fd, frame.payload.data(), frame.payload.size());
    return frame;
}

void send(int fd, MessageType type, const std::string &payload) {
    CodecError error = CodecError::None;
    const auto frame = encode_frame(type, payload, error);
    assert(error == CodecError::None);
    std::size_t offset = 0;
    while (offset < frame.size()) {
        const ssize_t count = ::send(fd, frame.data() + offset, frame.size() - offset, 0);
        assert(count > 0);
        offset += static_cast<std::size_t>(count);
    }
}
} // namespace

int main() {
    char directory[] = "/tmp/ns-client-test-XXXXXX";
    assert(::mkdtemp(directory));
    const std::string folder = directory;
    const std::string path = folder + "/network.sock";
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    assert(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    assert(::listen(listener, 1) == 0);

    std::mutex mutex;
    std::condition_variable condition;
    bool send_gap = false;
    bool finish = false;
    std::thread server([&] {
        const int fd = ::accept(listener, nullptr, nullptr);
        assert(fd >= 0);
        const auto hello = receive(fd);
        assert(hello.header.type == MessageType::Hello);
        send(fd, MessageType::Ready,
             R"({"version":1,"service":"network_service","sessionId":"test","generation":9,"capabilities":["events","snapshot-rebase"]})");
        const auto request = receive(fd);
        assert(request.header.type == MessageType::Request);
        assert(request.payload.find("\"method\":\"network.snapshot\"") != std::string::npos);
        send(fd, MessageType::Event,
             R"({"event":"network.state.changed","generation":9,"seq":1,"payload":{"changed":["wifi"]}})");
        send(fd, MessageType::Response,
             R"({"requestId":1,"status":200,"result":{"generation":9,"snapshotSeq":1,"snapshot":{}}})");
        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] { return send_gap; });
        }
        send(fd, MessageType::Event,
             R"({"event":"network.state.changed","generation":9,"seq":3,"payload":{"changed":["route"]}})");
        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] { return finish; });
        }
        ::close(fd);
    });

    network_service::NetworkServiceClient client;
    std::string error;
    assert(client.connectTo(path, error));
    std::string response;
    assert(client.request("network.snapshot", "{}", response, error));
    assert(client.generation() == 9 && client.snapshotSequence() == 1);
    std::string event;
    assert(!client.readNext(event, error) && error.empty());
    {
        std::lock_guard<std::mutex> lock(mutex);
        send_gap = true;
        condition.notify_all();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(client.readNext(event, error));
    assert(client.requiresSnapshotRebase());
    client.close();
    {
        std::lock_guard<std::mutex> lock(mutex);
        finish = true;
        condition.notify_all();
    }
    server.join();
    ::close(listener);
    ::unlink(path.c_str());
    ::rmdir(folder.c_str());
}
