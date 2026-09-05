#ifndef NETWORK_IPC_V1_SESSION_H
#define NETWORK_IPC_V1_SESSION_H

#include <cstdint>
#include <string>
#include <vector>

#include "ipc/network_ipc_v1_codec.h"

namespace network_service {
namespace ipc_v1 {

class Session {
public:
    enum class State {
        AwaitHello = 0,
        Ready,
        Closed,
    };

    struct HandleResult {
        std::vector<std::uint8_t> response;
        bool close_after_send = false;
    };

    Session(std::uint64_t generation, std::string session_id);

    HandleResult handle_frame(const Frame &frame);
    State state() const;
    bool ready() const;

private:
    HandleResult error_result(const char *code, const char *message, bool close_after_send);
    HandleResult ready_result();
    HandleResult response_success(std::uint64_t request_id, const std::string &result_json);
    HandleResult response_error(std::uint64_t request_id,
                                int status,
                                const char *code,
                                const char *message);
    HandleResult handle_request(const std::string &payload);

    std::uint64_t generation_;
    std::string session_id_;
    State state_ = State::AwaitHello;
};

} // namespace ipc_v1
} // namespace network_service

#endif // NETWORK_IPC_V1_SESSION_H
