#ifndef NETWORK_STATE_CHANGE_DETECTOR_H
#define NETWORK_STATE_CHANGE_DETECTOR_H

#include <string>

#include "network_service_types.h"

namespace network_service {

struct NetworkStateChangeSet {
    bool eth = false;
    bool wifi = false;
    bool route = false;
    bool dns = false;

    bool any() const;
    std::string payload_json() const;
};

class NetworkStateChangeDetector {
public:
    NetworkStateChangeSet observe(const NetworkSnapshot &snapshot);
    void reset();
    bool initialized() const;

private:
    bool initialized_ = false;
    NetworkSnapshot previous_;
};

} // namespace network_service

#endif // NETWORK_STATE_CHANGE_DETECTOR_H
