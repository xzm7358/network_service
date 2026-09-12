#ifndef NETWORK_IPC_V1_REBASE_H
#define NETWORK_IPC_V1_REBASE_H

#include <cstdint>
#include <string>
#include <vector>

namespace network_service {
namespace ipc_v1 {

enum class EventDecision {
    Accept = 0,
    IgnoreStale,
    RebaseRequired,
};

class RebaseTracker {
public:
    void invalidate();
    bool rebase(std::uint64_t generation, std::uint64_t snapshot_seq);
    EventDecision observe_event(std::uint64_t generation, std::uint64_t seq);

    bool has_baseline() const;
    std::uint64_t generation() const;
    std::uint64_t last_sequence() const;

private:
    bool has_baseline_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t last_sequence_ = 0;
};

std::vector<std::uint8_t> encode_snapshot_response(std::uint64_t request_id,
                                                   std::uint64_t generation,
                                                   std::uint64_t snapshot_seq,
                                                   const std::string &snapshot_json);

} // namespace ipc_v1
} // namespace network_service

#endif // NETWORK_IPC_V1_REBASE_H
