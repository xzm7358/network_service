#include "ipc/network_ipc_v1_outbound.h"

#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <utility>

namespace network_service {
namespace ipc_v1 {

OutboundQueue::OutboundQueue(std::size_t max_frames, std::size_t max_bytes)
    : max_frames_(max_frames), max_bytes_(max_bytes) {}

OutboundEnqueueResult OutboundQueue::enqueue(std::vector<std::uint8_t> frame) {
    if (frame.empty()) return OutboundEnqueueResult::InvalidFrame;
    if (max_frames_ == 0 || max_bytes_ == 0 || frame.size() > max_bytes_) {
        return OutboundEnqueueResult::FrameTooLarge;
    }
    if (frames_.size() >= max_frames_ || queued_bytes_ > max_bytes_ - frame.size()) {
        return OutboundEnqueueResult::Overflow;
    }

    queued_bytes_ += frame.size();
    frames_.push_back({std::move(frame), 0});
    return OutboundEnqueueResult::Accepted;
}

bool OutboundQueue::empty() const {
    return frames_.empty();
}

std::size_t OutboundQueue::frame_count() const {
    return frames_.size();
}

std::size_t OutboundQueue::queued_bytes() const {
    return queued_bytes_;
}

std::size_t OutboundQueue::max_frames() const {
    return max_frames_;
}

std::size_t OutboundQueue::max_bytes() const {
    return max_bytes_;
}

const std::uint8_t *OutboundQueue::front_data() const {
    if (frames_.empty()) return nullptr;
    const PendingFrame &front = frames_.front();
    return front.bytes.data() + front.offset;
}

std::size_t OutboundQueue::front_size() const {
    if (frames_.empty()) return 0;
    const PendingFrame &front = frames_.front();
    return front.bytes.size() - front.offset;
}

bool OutboundQueue::consume(std::size_t bytes) {
    if (frames_.empty() || bytes == 0 || bytes > front_size()) return false;

    PendingFrame &front = frames_.front();
    front.offset += bytes;
    queued_bytes_ -= bytes;
    if (front.offset == front.bytes.size()) {
        frames_.pop_front();
    }
    return true;
}

void OutboundQueue::clear() {
    frames_.clear();
    queued_bytes_ = 0;
}

OutboundWriter::OutboundWriter(std::size_t max_frames,
                               std::size_t max_bytes,
                               int write_stall_timeout_ms)
    : queue_(max_frames, max_bytes),
      write_stall_timeout_ms_(write_stall_timeout_ms < 0 ? 0 : write_stall_timeout_ms) {}

OutboundEnqueueResult OutboundWriter::enqueue(std::vector<std::uint8_t> frame) {
    return queue_.enqueue(std::move(frame));
}

OutboundFlushResult OutboundWriter::flush(int fd, int wake_read_fd) {
    while (!queue_.empty()) {
        int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        const ssize_t written = send(fd, queue_.front_data(), queue_.front_size(), flags);
        if (written > 0) {
            if (!queue_.consume(static_cast<std::size_t>(written))) {
                return OutboundFlushResult::Disconnected;
            }
            continue;
        }
        if (written == 0) return OutboundFlushResult::Disconnected;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return OutboundFlushResult::Disconnected;
        }

        struct pollfd fds[2];
        fds[0].fd = fd;
        fds[0].events = POLLOUT;
        fds[0].revents = 0;
        nfds_t count = 1;
        if (wake_read_fd >= 0) {
            fds[1].fd = wake_read_fd;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            count = 2;
        }

        const int ret = poll(fds, count, write_stall_timeout_ms_);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return OutboundFlushResult::Disconnected;
        }
        if (ret == 0) return OutboundFlushResult::SlowClient;
        if (count > 1 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            return OutboundFlushResult::Interrupted;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return OutboundFlushResult::Disconnected;
        }
    }
    return OutboundFlushResult::Drained;
}

const OutboundQueue &OutboundWriter::queue() const {
    return queue_;
}

} // namespace ipc_v1
} // namespace network_service
