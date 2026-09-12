#include "platform/dhcp_lease_store.h"

#include <cstdio>
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

bool is_safe_generation(const std::string &value) {
    if (value.empty() || value.size() > 80) return false;
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

std::string DhcpLeaseStore::path_for(const std::string &iface,
                                     const std::string &generation) {
    return "/tmp/network_service_dhcp_" + iface + "_" + generation + ".lease";
}

std::string DhcpLeaseStore::generation_path_for(const std::string &iface) {
    return "/tmp/network_service_dhcp_" + iface + ".generation";
}

bool DhcpLeaseStore::activate_generation(const std::string &iface,
                                         const std::string &generation,
                                         std::string &error) {
    error.clear();
    if (!is_safe_iface(iface) || !is_safe_generation(generation)) {
        error = "invalid DHCP lease generation";
        return false;
    }

    const std::string path = generation_path_for(iface);
    const std::string tmp = path + "." + std::to_string(getpid()) + ".tmp";
    std::ofstream out(tmp, std::ios::out | std::ios::trunc);
    if (!out) {
        error = "failed to write DHCP generation marker";
        return false;
    }
    out << generation << '\n';
    out.close();
    if (!out || std::rename(tmp.c_str(), path.c_str()) != 0) {
        (void)unlink(tmp.c_str());
        error = "failed to publish DHCP generation marker";
        return false;
    }
    return true;
}

bool DhcpLeaseStore::active_generation(const std::string &iface,
                                       std::string &generation,
                                       bool &exists,
                                       std::string &error) {
    generation.clear();
    exists = false;
    error.clear();
    if (!is_safe_iface(iface)) {
        error = "invalid DHCP lease iface";
        return false;
    }

    std::ifstream input(generation_path_for(iface));
    if (!input) return true;
    std::getline(input, generation);
    if (!is_safe_generation(generation)) {
        generation.clear();
        error = "invalid DHCP generation marker";
        return false;
    }
    exists = true;
    return true;
}

bool DhcpLeaseStore::read(const std::string &iface,
                          DhcpLeaseFact &fact,
                          bool &exists,
                          std::string &error) {
    fact = DhcpLeaseFact{};
    exists = false;
    error.clear();

    std::string active;
    bool active_exists = false;
    if (!active_generation(iface, active, active_exists, error)) return false;
    if (!active_exists) return true;

    std::ifstream input(path_for(iface, active));
    if (!input) return true;

    fact.iface = iface;
    std::string event;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        if (key == "generation") {
            fact.generation = value;
        } else if (key == "event") {
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

    if (fact.generation != active) {
        fact = DhcpLeaseFact{};
        return true;
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
    exists = true;
    return true;
}

void DhcpLeaseStore::clear(const std::string &iface) {
    if (!is_safe_iface(iface)) return;

    std::string generation;
    bool exists = false;
    std::string ignored;
    if (active_generation(iface, generation, exists, ignored) && exists) {
        (void)unlink(path_for(iface, generation).c_str());
    }
    (void)unlink(generation_path_for(iface).c_str());
}

} // namespace network_service
