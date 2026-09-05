#include "ipc/network_ipc_v1_rebase.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "ipc/network_ipc_v1_codec.h"

using namespace network_service::ipc_v1;

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

Frame decode_one(const std::vector<std::uint8_t> &encoded) {
    FrameDecoder decoder;
    require(decoder.feed(encoded) != DecodeStatus::Error, "encoded frame must decode");
    require(decoder.has_frame(), "encoded frame must be complete");
    return decoder.take_frame();
}

} // namespace

int main() {
    RebaseTracker tracker;
    require(!tracker.has_baseline(), "tracker starts invalidated");
    require(tracker.observe_event(10, 1) == EventDecision::RebaseRequired,
            "event before snapshot requires rebase");
    require(!tracker.has_baseline(), "pre-baseline event keeps tracker invalid");

    require(tracker.rebase(10, 7), "valid snapshot establishes baseline");
    require(tracker.has_baseline(), "tracker has baseline after rebase");
    require(tracker.generation() == 10, "baseline generation preserved");
    require(tracker.last_sequence() == 7, "snapshot watermark preserved");

    require(tracker.observe_event(10, 7) == EventDecision::IgnoreStale,
            "duplicate watermark event is stale");
    require(tracker.observe_event(10, 6) == EventDecision::IgnoreStale,
            "older event is stale");
    require(tracker.last_sequence() == 7, "stale event does not advance watermark");

    require(tracker.observe_event(10, 8) == EventDecision::Accept,
            "exact next sequence is accepted");
    require(tracker.last_sequence() == 8, "accepted event advances sequence");

    require(tracker.observe_event(10, 10) == EventDecision::RebaseRequired,
            "sequence gap requires rebase");
    require(!tracker.has_baseline(), "gap invalidates baseline");

    require(tracker.rebase(10, 10), "tracker can rebase after gap");
    require(tracker.observe_event(11, 11) == EventDecision::RebaseRequired,
            "generation change requires rebase");
    require(!tracker.has_baseline(), "generation change invalidates baseline");

    require(!tracker.rebase(0, 0), "zero generation cannot establish baseline");

    require(tracker.rebase(12, std::numeric_limits<std::uint64_t>::max()),
            "UINT64_MAX watermark is representable");
    require(tracker.observe_event(12, std::numeric_limits<std::uint64_t>::max()) ==
                EventDecision::IgnoreStale,
            "max sequence duplicate is stale");

    const Frame response = decode_one(encode_snapshot_response(
        99,
        42,
        7,
        R"({"eth":{"up":true},"wifi":{"up":false}})"));
    require(response.header.type == MessageType::Response, "snapshot response uses RESPONSE type");
    require(response.payload.find("\"requestId\":99") != std::string::npos,
            "snapshot response correlates requestId");
    require(response.payload.find("\"generation\":42") != std::string::npos,
            "snapshot response carries generation");
    require(response.payload.find("\"snapshotSeq\":7") != std::string::npos,
            "snapshot response carries watermark");
    require(response.payload.find("\"snapshot\":{\"eth\"") != std::string::npos,
            "snapshot response embeds authoritative snapshot object");

    require(encode_snapshot_response(0, 42, 0, "{}").empty(),
            "zero requestId is rejected");
    require(encode_snapshot_response(1, 0, 0, "{}").empty(),
            "zero generation is rejected");

    std::cout << "network_ipc_v1_rebase_test: PASS" << std::endl;
    return 0;
}
