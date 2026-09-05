#include "ipc/network_ipc_v1_outbound.h"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace network_service::ipc_v1;

namespace {

std::vector<std::uint8_t> bytes(std::size_t size, std::uint8_t value) {
    return std::vector<std::uint8_t>(size, value);
}

void fill_send_buffer(int fd) {
    std::vector<std::uint8_t> chunk(4096, 0x5a);
    while (true) {
        const ssize_t written = send(fd, chunk.data(), chunk.size(), MSG_DONTWAIT);
        if (written > 0) continue;
        assert(written < 0);
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        return;
    }
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

    {
        int sockets[2] = {-1, -1};
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        int send_buffer = 4096;
        assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                          &send_buffer, sizeof(send_buffer)) == 0);
        fill_send_buffer(sockets[0]);

        OutboundWriter writer(4, 4096, 20);
        assert(writer.enqueue(bytes(128, 9)) == OutboundEnqueueResult::Accepted);
        assert(writer.flush(sockets[0], -1) == OutboundFlushResult::SlowClient);
        assert(writer.queue().queued_bytes() == 128);

        close(sockets[0]);
        close(sockets[1]);
    }

    {
        int sockets[2] = {-1, -1};
        int wake[2] = {-1, -1};
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        assert(pipe(wake) == 0);
        int send_buffer = 4096;
        assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                          &send_buffer, sizeof(send_buffer)) == 0);
        fill_send_buffer(sockets[0]);
        const char signal = 'x';
        assert(write(wake[1], &signal, 1) == 1);

        OutboundWriter writer(4, 4096, 1000);
        assert(writer.enqueue(bytes(128, 10)) == OutboundEnqueueResult::Accepted);
        assert(writer.flush(sockets[0], wake[0]) == OutboundFlushResult::Interrupted);
        assert(writer.queue().queued_bytes() == 128);

        close(wake[0]);
        close(wake[1]);
        close(sockets[0]);
        close(sockets[1]);
    }

    return 0;
}
