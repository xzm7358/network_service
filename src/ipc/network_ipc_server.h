#ifndef NETWORK_SERVICE_IPC_SERVER_H
#define NETWORK_SERVICE_IPC_SERVER_H

#include <cstdint>
#include <string>
#include <vector>

#include "ipc/network_ipc_v1_event.h"

namespace network_service {

class NetworkDaemon;

class NetworkIpcServer {
public:
    explicit NetworkIpcServer(NetworkDaemon &daemon);
    ~NetworkIpcServer();

    bool listen(const std::string &socket_path);
    void run();
    void stop();

    int wake_write_fd() const;
    static int wake_fd();
    static void set_wake_fd(int fd);

private:
    std::string handle_request(const std::string &request);
    void handle_client(int client_fd);
    void handle_v0_client(int client_fd, const std::vector<std::uint8_t> &initial);
    void handle_v1_client(int client_fd, const std::vector<std::uint8_t> &initial);

    NetworkDaemon &daemon_;
    int listen_fd_ = -1;
    int wake_read_fd_ = -1;
    int wake_write_fd_ = -1;
    std::string socket_path_;
    bool running_ = false;
    std::uint64_t generation_ = 1;
    std::uint64_t next_session_sequence_ = 1;
    ipc_v1::EventSequencer event_sequencer_;
};

} // namespace network_service

#endif // NETWORK_SERVICE_IPC_SERVER_H
