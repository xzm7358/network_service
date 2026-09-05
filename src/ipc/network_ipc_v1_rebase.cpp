#include "ipc/network_ipc_v1_rebase.h"

#include <limits>
#include <sstream>

#include "ipc/network_ipc_v1_codec.h"

namespace network_service {
namespace ipc_v1 {

void RebaseTracker::invalidate() {
    has_baseline_ = false;
    generation_ = 0;
    last_sequence_ = 0;
}

bool RebaseTracker::rebase(std::uint64_t generation, std::uint64_t snapshot_seq) {
    if (generation == 0) {
        invalidate();
        return false;
    }
    has_baseline_ = true;
    generation_ = generation;
    last_sequence_ = snapshot_seq;
    return true;
}

EventDecision RebaseTracker::observe_event(std::uint64_t generation, std::uint64_t seq) {
    if (!has_baseline_ || generation == 0 || seq == 0) {
        invalidate();
        return EventDecision::RebaseRequired;
    }
    if (generation != generation_) {
        invalidate();
        return EventDecision::RebaseRequired;
    }
    if (seq <= last_sequence_) {
        return EventDecision::IgnoreStale;
    }
    if (last_sequence_ != std::numeric_limits<std::uint64_t>::max() &&
        seq == last_sequence_ + 1) {
        last_sequence_ = seq;
        return EventDecision::Accept;
    }
    invalidate();
    return EventDecision::RebaseRequired;
}

bool RebaseTracker::has_baseline() const {
    return has_baseline_;
}

std::uint64_t RebaseTracker::generation() const {
    return generation_;
}

std::uint64_t RebaseTracker::last_sequence() const {
    return last_sequence_;
}

std::vector<std::uint8_t> encode_snapshot_response(std::uint64_t request_id,
                                                   std::uint64_t generation,
                                                   std::uint64_t snapshot_seq,
                                                   const std::string &snapshot_json) {
    if (request_id == 0 || generation == 0 || snapshot_json.empty()) return {};

    std::ostringstream os;
    os << "{\"requestId\":" << request_id
       << ",\"status\":200,\"result\":{\"generation\":" << generation
       << ",\"snapshotSeq\":" << snapshot_seq
       << ",\"snapshot\":" << snapshot_json << "}}";

    CodecError error = CodecError::None;
    std::vector<std::uint8_t> encoded = encode_frame(MessageType::Response, os.str(), error);
    if (error != CodecError::None) return {};
    return encoded;
}

} // namespace ipc_v1
} // namespace network_service
