#include "platform/dhcp_lease_store.h"

#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace network_service {
namespace {

bool is_safe_iface(const std::string &value) {
    if (value.empty() || value.size() > 15) return false;
    for (char ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

std::string first_token(const std::string &value) {
    std::istringstream input(value);
    std::string out;
    input >> out;
    return out;
}

DhcpLeaseEvent parse_event(const std::string &value) {
    if (value == "bound") return DhcpLeaseEvent::Bound;
    if (value == "renew") return DhcpLeaseEvent::Renew;
    if (value == "deconfig") return DhcpLeaseEvent::Deconfig;
    return DhcpLeaseEvent::None;
}

} // namespace

bool DhcpLeaseFact::configured() const {
    return event == DhcpLeaseEvent::Bound || event == DhcpLeaseEvent::Renew;
}

std::string DhcpLeaseFact::fingerprint() const {
    std::ostringstream os;
    os << static_cast<int>(event) << '\x1f'
       << iface << '\x1f'
       << ip4 << '\x1f'
       << netmask4 << '\x1f'
       << gateway4 << '\x1f'
       << dns4;
    return os.str();
}

std::string DhcpLeaseStore::path_for(const std::string &iface) {
    return "/tmp/network_service_dhcp_" + iface + ".lease";
}

bool DhcpLeaseStore::read(const std::string &iface,
                          DhcpLeaseFact &fact,
                          bool &exists,
                          std::string &error) {
    fact = DhcpLeaseFact{};
    exists = false;
    error.clear();

    if (!is_safe_iface(iface)) {
        error = "invalid DHCP lease iface";
        return false;
    }

    std::ifstream input(path_for(iface));
    if (!input) {
        return true;
    }
    exists = true;
    fact.iface = iface;

    std::string event;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        if (key == "event") {
            event = value;
        } else if (key == "ip") {
            fact.ip4 = value;
        } else if (key == "subnet") {
            fact.netmask4 = value;
        } else if (key == "router") {
            fact.gateway4 = first_token(value);
        } else if (key == "dns") {
            fact.dns4 = first_token(value);
        }
    }

    fact.event = parse_event(event);
    if (fact.event == DhcpLeaseEvent::None) {
        error = "invalid DHCP lease event";
        return false;
    }
    if (fact.configured() && (fact.ip4.empty() || fact.netmask4.empty())) {
        error = "configured DHCP lease is missing ip/subnet";
        return false;
    }
    return true;
}

void DhcpLeaseStore::clear(const std::string &iface) {
    if (!is_safe_iface(iface)) return;
    (void)unlink(path_for(iface).c_str());
}

} // namespace network_service
