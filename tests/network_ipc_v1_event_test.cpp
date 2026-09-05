#include "ipc/network_ipc_v1_codec.h"
#include "ipc/network_ipc_v1_event.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

network_service::ipc_v1::Frame decode_one(const std::vector<std::uint8_t> &encoded) {
    network_service::ipc_v1::FrameDecoder decoder;
    require(decoder.feed(encoded) == network_service::ipc_v1::DecodeStatus::FrameReady,
            "encoded event must decode as one frame");
    require(decoder.has_frame(), "decoded event frame missing");
    return decoder.take_frame();
}

} // namespace

int main() {
    using namespace network_service::ipc_v1;

    EventSequencer sequencer(77);
    require(sequencer.generation() == 77, "generation must be preserved");
    require(sequencer.next_sequence() == 1, "first sequence must be one");

    const Frame first = decode_one(sequencer.encode_event("network.events.subscribed", "{}"));
    require(first.header.type == MessageType::Event, "first frame must be EVENT");
    require(first.payload ==
                "{\"event\":\"network.events.subscribed\",\"generation\":77,\"seq\":1,\"payload\":{}}",
            "first EVENT envelope mismatch");
    require(sequencer.next_sequence() == 2, "sequence must advance after first event");

    const Frame second = decode_one(sequencer.encode_event("network.state.changed", "{\"online\":true}"));
    require(second.header.type == MessageType::Event, "second frame must be EVENT");
    require(second.payload ==
                "{\"event\":\"network.state.changed\",\"generation\":77,\"seq\":2,\"payload\":{\"online\":true}}",
            "second EVENT envelope mismatch");
    require(sequencer.next_sequence() == 3, "sequence must remain monotonic");

    EventSequencer normalized(0);
    require(normalized.generation() == 1, "zero generation must normalize to one");

    std::cout << "Network IPC v1 event sequencer: PASS" << std::endl;
    return 0;
}
