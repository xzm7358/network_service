#include "platform/wpa_text_codec.h"

#include <cstddef>

namespace network_service {

namespace {

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool is_valid_utf8(const std::string &value) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
    std::size_t pos = 0;
    while (pos < value.size()) {
        const unsigned char lead = bytes[pos];
        if (lead <= 0x7f) {
            ++pos;
            continue;
        }

        std::size_t width = 0;
        unsigned int codepoint = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            width = 2;
            codepoint = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            width = 3;
            codepoint = lead & 0x0f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            width = 4;
            codepoint = lead & 0x07;
        } else {
            return false;
        }
        if (pos + width > value.size()) return false;

        for (std::size_t offset = 1; offset < width; ++offset) {
            const unsigned char continuation = bytes[pos + offset];
            if ((continuation & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if ((width == 3 && codepoint < 0x800) ||
            (width == 4 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint > 0x10ffff) {
            return false;
        }
        pos += width;
    }
    return true;
}

} // namespace

std::string decode_wpa_printable_text(const std::string &encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());

    for (std::size_t pos = 0; pos < encoded.size(); ++pos) {
        const char ch = encoded[pos];
        if (ch != '\\' || pos + 1 >= encoded.size()) {
            decoded.push_back(ch);
            continue;
        }

        const char escape = encoded[pos + 1];
        switch (escape) {
        case '\\': decoded.push_back('\\'); ++pos; break;
        case '"': decoded.push_back('"'); ++pos; break;
        case 'n': decoded.push_back('\n'); ++pos; break;
        case 'r': decoded.push_back('\r'); ++pos; break;
        case 't': decoded.push_back('\t'); ++pos; break;
        case 'e': decoded.push_back(static_cast<char>(0x1b)); ++pos; break;
        case 'x': {
            if (pos + 3 < encoded.size()) {
                const int high = hex_value(encoded[pos + 2]);
                const int low = hex_value(encoded[pos + 3]);
                if (high >= 0 && low >= 0) {
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    pos += 3;
                    break;
                }
            }
            // A malformed escape did not come from printf_encode(). Preserve it
            // literally rather than silently changing an externally supplied SSID.
            decoded.push_back('\\');
            break;
        }
        default:
            if (escape >= '0' && escape <= '7') {
                unsigned int octal = 0;
                std::size_t digits = 0;
                while (digits < 3 && pos + 1 + digits < encoded.size()) {
                    const char digit = encoded[pos + 1 + digits];
                    if (digit < '0' || digit > '7') break;
                    octal = (octal << 3) | static_cast<unsigned int>(digit - '0');
                    ++digits;
                }
                decoded.push_back(static_cast<char>(octal & 0xff));
                pos += digits;
            } else {
                decoded.push_back('\\');
            }
            break;
        }
    }

    return is_valid_utf8(decoded) ? decoded : encoded;
}

std::string encode_wpa_ssid_hex(const std::string &ssid) {
    static const char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(ssid.size() * 2);
    for (unsigned char ch : ssid) {
        encoded.push_back(kHex[ch >> 4]);
        encoded.push_back(kHex[ch & 0x0f]);
    }
    return encoded;
}

} // namespace network_service
