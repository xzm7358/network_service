#ifndef NETWORK_IPC_V1_EVENT_H
#define NETWORK_IPC_V1_EVENT_H

#include <cstdint>
#include <string>
#include <vector>

namespace network_service {
namespace ipc_v1 {

class EventSequencer {
public:
    explicit EventSequencer(std::uint64_t generation);

    std::uint64_t generation() const;
    std::uint64_t next_sequence() const;
    std::uint64_t last_sequence() const;

    std::vector<std::uint8_t> encode_event(const std::string &event_name,
                                           const std::string &payload_json);

private:
    std::uint64_t generation_;
    std::uint64_t next_sequence_ = 1;
};

} // namespace ipc_v1
} // namespace network_service

#endif // NETWORK_IPC_V1_EVENT_H
