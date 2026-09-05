#include "ipc/network_ipc_v1_outbound.h"

#include <cassert>
#include <cstdint>
#include <vector>

using namespace network_service::ipc_v1;

namespace {

std::vector<std::uint8_t> bytes(std::size_t size, std::uint8_t value) {
    return std::vector<std::uint8_t>(size, value);
}

} // namespace

int main() {
    {
        OutboundQueue queue(2, 10);
        assert(queue.enqueue(bytes(4, 1)) == OutboundEnqueueResult::Accepted);
        assert(queue.enqueue(bytes(6, 2)) == OutboundEnqueueResult::Accepted);
        assert(queue.frame_count() == 2);
        assert(queue.queued_bytes() == 10);
        assert(queue.enqueue(bytes(1, 3)) == OutboundEnqueueResult::Overflow);
        assert(queue.frame_count() == 2);
        assert(queue.queued_bytes() == 10);
    }

    {
        OutboundQueue queue(8, 5);
        assert(queue.enqueue(bytes(6, 1)) == OutboundEnqueueResult::FrameTooLarge);
        assert(queue.empty());
        assert(queue.enqueue({}) == OutboundEnqueueResult::InvalidFrame);
    }

    {
        OutboundQueue queue(4, 16);
        assert(queue.enqueue(bytes(10, 7)) == OutboundEnqueueResult::Accepted);
        assert(queue.front_size() == 10);
        assert(queue.queued_bytes() == 10);
        assert(queue.consume(3));
        assert(queue.front_size() == 7);
        assert(queue.queued_bytes() == 7);
        assert(!queue.consume(8));
        assert(queue.front_size() == 7);
        assert(queue.queued_bytes() == 7);
        assert(queue.consume(7));
        assert(queue.empty());
        assert(queue.queued_bytes() == 0);
    }

    {
        OutboundQueue queue(3, 12);
        assert(queue.enqueue(bytes(4, 1)) == OutboundEnqueueResult::Accepted);
        assert(queue.enqueue(bytes(4, 2)) == OutboundEnqueueResult::Accepted);
        assert(queue.enqueue(bytes(4, 3)) == OutboundEnqueueResult::Accepted);
        assert(queue.enqueue(bytes(1, 4)) == OutboundEnqueueResult::Overflow);
        queue.clear();
        assert(queue.empty());
        assert(queue.frame_count() == 0);
        assert(queue.queued_bytes() == 0);
    }

    return 0;
}
