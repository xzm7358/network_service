#include "platform/ethernet_apply.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace network_service {

namespace {

static bool is_safe_iface(const std::string &value) {
    if (value.empty() || value.size() > 15) return false;
    for (char ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

static bool is_safe_ipv4ish(const std::string &value, bool allow_empty) {
    if (value.empty()) return allow_empty;
    if (value.size() > 63) return false;
    for (char ch : value) {
        if (!((ch >= '0' && ch <= '9') || ch == '.')) return false;
    }
    return true;
}

static bool run_command(const std::string &cmd, std::string &error) {
    int rc = system(cmd.c_str());
    if (rc != 0) {
        std::ostringstream os;
        os << "command failed rc=" << rc << ": " << cmd;
        error = os.str();
        return false;
    }
    return true;
}

static std::string dhcp_pidfile(const std::string &iface) {
    return "/tmp/smart_hmi_udhcpc_" + iface + ".pid";
}

static std::string dhcp_script_path() {
    return "/tmp/smart_hmi_udhcpc_network_service.script";
}

static void stop_dhcp_for_iface(const std::string &iface) {
    std::ifstream f(dhcp_pidfile(iface));
    int pid = -1;
    if (f >> pid) {
        if (pid > 1) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "kill %d 2>/dev/null", pid);
            (void)system(cmd);
        }
    }
    std::string rm = "rm -f " + dhcp_pidfile(iface);
    (void)system(rm.c_str());
}

static bool ensure_dhcp_script(std::string &error) {
    const std::string path = dhcp_script_path();
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) {
        error = "failed to write udhcpc script";
        return false;
    }
    f << "#!/bin/sh\n"
      << "case \"$1\" in\n"
      << "  deconfig)\n"
      << "    ifconfig \"$interface\" 0.0.0.0 2>/dev/null\n"
      << "    ;;\n"
      << "  bound|renew)\n"
      << "    ifconfig \"$interface\" \"$ip\" netmask \"$subnet\" up\n"
      << "    if [ -n \"$router\" ]; then\n"
      << "      route del default dev \"$interface\" 2>/dev/null\n"
      << "      for r in $router; do route add default gw \"$r\" dev \"$interface\" metric 10 2>/dev/null; break; done\n"
      << "    fi\n"
      << "    if [ -n \"$dns\" ]; then\n"
      << "      : > /etc/resolv.conf\n"
      << "      for d in $dns; do echo nameserver \"$d\" >> /etc/resolv.conf; done\n"
      << "    fi\n"
      << "    ;;\n"
      << "esac\n"
      << "exit 0\n";
    f.close();
    chmod(path.c_str(), 0755);
    return true;
}

static bool validate_config_common(const EthernetConfig &config, std::string &error) {
    if (!is_safe_iface(config.iface)) {
        error = "invalid iface";
        return false;
    }
    if (!is_safe_ipv4ish(config.ip4, true) ||
        !is_safe_ipv4ish(config.netmask4, true) ||
        !is_safe_ipv4ish(config.gateway4, true) ||
        !is_safe_ipv4ish(config.dns4, true)) {
        error = "invalid IPv4 field";
        return false;
    }
    return true;
}

} // namespace

bool apply_ethernet_static(const EthernetConfig &config, std::string &error) {
    if (!validate_config_common(config, error)) return false;
    if (config.ip4.empty() || config.netmask4.empty()) {
        error = "static IP and netmask are required";
        return false;
    }

    stop_dhcp_for_iface(config.iface);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ifconfig %s %s netmask %s up",
             config.iface.c_str(), config.ip4.c_str(), config.netmask4.c_str());
    if (!run_command(cmd, error)) return false;

    snprintf(cmd, sizeof(cmd), "route del default dev %s 2>/dev/null", config.iface.c_str());
    (void)system(cmd);

    if (!config.gateway4.empty()) {
        snprintf(cmd, sizeof(cmd), "route add default gw %s dev %s metric %d",
                 config.gateway4.c_str(), config.iface.c_str(), config.route_metric);
        if (!run_command(cmd, error)) return false;
    }

    if (config.dns_enabled && !config.dns4.empty()) {
        std::ofstream resolv("/etc/resolv.conf", std::ios::out | std::ios::trunc);
        if (resolv) {
            resolv << "nameserver " << config.dns4 << "\n";
        }
    }
    return true;
}

bool apply_ethernet_dhcp(const EthernetConfig &config, std::string &error) {
    if (!validate_config_common(config, error)) return false;
    if (!ensure_dhcp_script(error)) return false;

    stop_dhcp_for_iface(config.iface);

    char cmd[384];
    snprintf(cmd, sizeof(cmd),
             "udhcpc -i %s -t 15 -n -p %s -s %s &",
             config.iface.c_str(), dhcp_pidfile(config.iface).c_str(), dhcp_script_path().c_str());
    return run_command(cmd, error);
}

} // namespace network_service
