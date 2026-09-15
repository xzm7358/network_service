#ifndef NETWORK_SERVICE_WPA_TEXT_CODEC_H
#define NETWORK_SERVICE_WPA_TEXT_CODEC_H

#include <string>

namespace network_service {

// wpa_supplicant renders SSID octets through printf_encode() in text replies.
// Decode that representation for JSON/UI consumers. If the decoded octets are
// not valid UTF-8, retain the printable source representation so IPC JSON never
// contains invalid UTF-8.
std::string decode_wpa_printable_text(const std::string &encoded);

// SET_NETWORK accepts an unquoted hexadecimal SSID. This preserves the exact
// UTF-8/special-character byte sequence without control-command quoting rules.
std::string encode_wpa_ssid_hex(const std::string &ssid);

} // namespace network_service

#endif // NETWORK_SERVICE_WPA_TEXT_CODEC_H
