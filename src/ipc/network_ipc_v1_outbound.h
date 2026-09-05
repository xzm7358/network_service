#ifndef NETWORK_IPC_V1_OUTBOUND_H
#define NETWORK_IPC_V1_OUTBOUND_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "ipc/network_ipc_v1_codec.h"

namespace network_service {
namespace ipc_v1 {

constexpr std::size_t kDefaultOutboundMaxFrames = 32;
constexpr std::size_t kDefaultOutboundMaxBytes =
    4 * (kHeaderSize + kMaxPayloadBytes);

enum class OutboundEnqueueResult {
    Accepted = 0,
    InvalidFrame,
    FrameTooLarge,
    Overflow,
};

class OutboundQueue {
public:
    explicit OutboundQueue(std::size_t max_frames = kDefaultOutboundMaxFrames,
                           std::size_t max_bytes = kDefaultOutboundMaxBytes);

    OutboundEnqueueResult enqueue(std::vector<std::uint8_t> frame);

    bool empty() const;
    std::size_t frame_count() const;
    std::size_t queued_bytes() const;
    std::size_t max_frames() const;
    std::size_t max_bytes() const;

    const std::uint8_t *front_data() const;
    std::size_t front_size() const;
    bool consume(std::size_t bytes);
    void clear();

private:
    struct PendingFrame {
        std::vector<std::uint8_t> bytes;
        std::size_t offset = 0;
    };

    std::size_t max_frames_;
    std::size_t max_bytes_;
    std::size_t queued_bytes_ = 0;
    std::deque<PendingFrame> frames_;
};

} // namespace ipc_v1
} // namespace network_service

#endif // NETWORK_IPC_V1_OUTBOUND_H
