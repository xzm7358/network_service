#include "platform/wifi_backend.h"
#include "platform/wpa_text_codec.h"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::cerr << "wifi_backend_text_test: " << message << std::endl;
    return false;
}

} // namespace

int main() {
    const std::string scan_results =
        "bssid / frequency / signal level / flags / ssid\n"
        "d4:da:21:54:bc:2a\t2412\t-50\t[WPA2-PSK-CCMP][WPS][ESS]\t"
        "\\xe5\\xbf\\x85\\xe8\\x83\\x9c\\xe5\\xae\\xa2DNK"
        "\\xe5\\xae\\x98\\xe6\\x96\\xb9\\xe5\\xba\\x97\n";

    const auto records = network_service::parse_wpa_scan_results(scan_results);
    bool ok = true;
    ok = expect(records.size() == 1, "escaped Chinese scan row was not parsed") && ok;
    if (!records.empty()) {
        ok = expect(records[0].ssid == u8"必胜客DNK官方店",
                    "wpa_supplicant byte escapes were exposed as the SSID") && ok;
    }

    ok = expect(network_service::decode_wpa_printable_text(
                    "Cafe\\x20\\\"A\\\"\\\\Lab\\tGuest") ==
                    "Cafe \"A\"\\Lab\tGuest",
                "special-character escapes were not decoded") && ok;
    ok = expect(network_service::decode_wpa_printable_text("literal\\\\xe5") ==
                    "literal\\xe5",
                "a literal backslash escape was decoded twice") && ok;
    ok = expect(network_service::decode_wpa_printable_text("binary\\xff") ==
                    "binary\\xff",
                "invalid UTF-8 must retain its printable representation") && ok;
    ok = expect(network_service::decode_wpa_printable_text("bad\\xG1") ==
                    "bad\\xG1",
                "malformed byte escape was not preserved") && ok;
    ok = expect(network_service::encode_wpa_ssid_hex(u8"中文 Wi-Fi") ==
                    "e4b8ade696872057692d4669",
                "SSID bytes were not encoded exactly for SET_NETWORK") && ok;

    std::string error;
    const std::string oversized_ssid(33, 'x');
    ok = expect(network_service::wifi_create_profile(
                    "wlan0", oversized_ssid, "password", error) == -1 &&
                    error == "ssid exceeds 32-byte limit",
                "profile creation accepted an oversized SSID") && ok;
    error.clear();
    ok = expect(!network_service::wifi_configure_profile(
                    "wlan0", 0, oversized_ssid, "password", error) &&
                    error == "ssid exceeds 32-byte limit",
                "profile update accepted an oversized SSID") && ok;
    return ok ? 0 : 1;
}
