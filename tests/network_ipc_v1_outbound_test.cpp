#include "ipc/network_ipc_v1_outbound.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace network_service::ipc_v1;

namespace {

void require(bool condition) {
    if (!condition) std::abort();
}

std::vector<std::uint8_t> bytes(std::size_t size, std::uint8_t value) {
    return std::vector<std::uint8_t>(size, value);
}

void fill_send_buffer(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    require(flags >= 0);
    require(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    std::vector<std::uint8_t> chunk(4096, 0x5a);
    while (true) {
        const ssize_t written = send(fd, chunk.data(), chunk.size(), MSG_DONTWAIT);
        if (written > 0) continue;
        require(written < 0);
        require(errno == EAGAIN || errno == EWOULDBLOCK);
        return;
    }
}

} // namespace

int main() {
    {
        OutboundQueue queue(2, 10);
        require(queue.enqueue(bytes(4, 1)) == OutboundEnqueueResult::Accepted);
        require(queue.enqueue(bytes(6, 2)) == OutboundEnqueueResult::Accepted);
        require(queue.frame_count() == 2);
        require(queue.queued_bytes() == 10);
        require(queue.enqueue(bytes(1, 3)) == OutboundEnqueueResult::Overflow);
        require(queue.frame_count() == 2);
        require(queue.queued_bytes() == 10);
    }

    {
        OutboundQueue queue(8, 5);
        require(queue.enqueue(bytes(6, 1)) == OutboundEnqueueResult::FrameTooLarge);
        require(queue.empty());
        require(queue.enqueue({}) == OutboundEnqueueResult::InvalidFrame);
    }

    {
        OutboundQueue queue(4, 16);
        require(queue.enqueue(bytes(10, 7)) == OutboundEnqueueResult::Accepted);
        require(queue.front_size() == 10);
        require(queue.queued_bytes() == 10);
        require(queue.consume(3));
        require(queue.front_size() == 7);
        require(queue.queued_bytes() == 7);
        require(!queue.consume(8));
        require(queue.front_size() == 7);
        require(queue.queued_bytes() == 7);
        require(queue.consume(7));
        require(queue.empty());
        require(queue.queued_bytes() == 0);
    }

    {
        OutboundQueue queue(3, 12);
        require(queue.enqueue(bytes(4, 1)) == OutboundEnqueueResult::Accepted);
        require(queue.enqueue(bytes(4, 2)) == OutboundEnqueueResult::Accepted);
        require(queue.enqueue(bytes(4, 3)) == OutboundEnqueueResult::Accepted);
        require(queue.enqueue(bytes(1, 4)) == OutboundEnqueueResult::Overflow);
        queue.clear();
        require(queue.empty());
        require(queue.frame_count() == 0);
        require(queue.queued_bytes() == 0);
    }

    {
        int sockets[2] = {-1, -1};
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        int send_buffer = 4096;
        require(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                           &send_buffer, sizeof(send_buffer)) == 0);
        fill_send_buffer(sockets[0]);

        OutboundWriter writer(4, 4096, 20);
        require(writer.enqueue(bytes(128, 9)) == OutboundEnqueueResult::Accepted);
        require(writer.flush(sockets[0], -1) == OutboundFlushResult::SlowClient);
        require(writer.queue().queued_bytes() == 128);

        close(sockets[0]);
        close(sockets[1]);
    }

    {
        int sockets[2] = {-1, -1};
        int wake[2] = {-1, -1};
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        require(pipe(wake) == 0);
        int send_buffer = 4096;
        require(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                           &send_buffer, sizeof(send_buffer)) == 0);
        fill_send_buffer(sockets[0]);
        const char signal = 'x';
        require(write(wake[1], &signal, 1) == 1);

        OutboundWriter writer(4, 4096, 1000);
        require(writer.enqueue(bytes(128, 10)) == OutboundEnqueueResult::Accepted);
        require(writer.flush(sockets[0], wake[0]) == OutboundFlushResult::Interrupted);
        require(writer.queue().queued_bytes() == 128);

        close(wake[0]);
        close(wake[1]);
        close(sockets[0]);
        close(sockets[1]);
    }

    return 0;
}
