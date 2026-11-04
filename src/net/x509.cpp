#include "bonk/net/x509.hpp"

#include <ctime>

#include "bonk/core/strings.hpp"
#include "bonk/crypto/crypto.hpp"

namespace bonk::net {
namespace {

Error x509_error(std::string_view message) { return Error::make("x509.malformed", message); }

/// A cursor over DER: tag | length | value.
struct Cursor {
  std::string_view data;
  std::size_t position = 0;

  [[nodiscard]] bool eof() const { return position >= data.size(); }
  [[nodiscard]] std::uint8_t peek_tag() const { return position < data.size() ? static_cast<std::uint8_t>(data[position]) : 0; }

  bool read_tag(std::uint8_t expected, std::string_view& value, std::uint8_t& tag) {
    if (position + 2 > data.size()) return false;
    tag = static_cast<std::uint8_t>(data[position]);
    if (expected != 0 && tag != expected) return false;
    std::size_t length = static_cast<std::uint8_t>(data[position + 1]);
    position += 2;
    if ((length & 0x80) != 0) {
      const std::size_t count = length & 0x7F;
      if (count == 0 || count > 4 || position + count > data.size()) return false;
      length = 0;
      for (std::size_t i = 0; i < count; ++i) {
        length = (length << 8) | static_cast<std::uint8_t>(data[position + i]);
      }
      position += count;
    }
    if (position + length > data.size()) return false;
    value = data.substr(position, length);
    position += length;
    return true;
  }

  bool read(std::uint8_t expected, std::string_view& value) {
    std::uint8_t tag = 0;
    return read_tag(expected, value, tag);
  }
};

std::string oid_to_string(std::string_view oid) {
  if (oid.empty()) return {};
  std::string out = std::to_string(static_cast<std::uint8_t>(oid[0]) / 40);
  out += '.';
  out += std::to_string(static_cast<std::uint8_t>(oid[0]) % 40);
  std::uint32_t value = 0;
  for (std::size_t i = 1; i < oid.size(); ++i) {
    const auto byte = static_cast<std::uint8_t>(oid[i]);
    value = (value << 7) | (byte & 0x7F);
    if ((byte & 0x80) == 0) {
      out += '.';
      out += std::to_string(value);
      value = 0;
    }
  }
  return out;
}

/// Reads a PrintableString / UTF8String / IA5String / BMPString into UTF-8.
std::string read_string(std::string_view value, std::uint8_t tag) {
  if (tag == 0x1E) {  // BMPString (UTF-16BE)
    std::string out;
    for (std::size_t i = 0; i + 1 < value.size(); i += 2) {
      const auto code_unit = static_cast<std::uint16_t>((static_cast<std::uint8_t>(value[i]) << 8) |
                                                        static_cast<std::uint8_t>(value[i + 1]));
      if (code_unit < 0x80) {
        out.push_back(static_cast<char>(code_unit));
      } else if (code_unit < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_unit >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_unit & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xE0 | (code_unit >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_unit >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_unit & 0x3F)));
      }
    }
    return out;
  }
  return std::string(value);
}

/// ASN.1 Time → "YYYY-MM-DDTHH:MM:SSZ".
std::string read_time(std::string_view value, std::uint8_t tag) {
  std::string digits;
  for (const char c : value) {
    if (c >= '0' && c <= '9') digits.push_back(c);
    else break;
  }
  std::string year;
  std::size_t offset = 0;
  if (tag == 0x17) {  // UTCTime: 2-digit year
    if (digits.size() < 10) return std::string(value);
    const int short_year = std::stoi(digits.substr(0, 2));
    year = (short_year >= 50 ? "19" : "20") + digits.substr(0, 2);
    offset = 2;
  } else {
    if (digits.size() < 12) return std::string(value);
    year = digits.substr(0, 4);
    offset = 4;
  }
  return year + "-" + digits.substr(offset, 2) + "-" + digits.substr(offset + 2, 2) + "T" +
         digits.substr(offset + 4, 2) + ":" + digits.substr(offset + 6, 2) + ":" + digits.substr(offset + 8, 2) +
         "Z";
}

struct NameInfo {
  std::string common_name;
};

NameInfo parse_name(std::string_view name_contents) {
  // `name_contents` is the *content* of the Name SEQUENCE (a list of RDN SETs).
  NameInfo info;
  Cursor list{name_contents};
  while (!list.eof()) {
    std::string_view set;
    if (!list.read(0x31, set)) break;
    Cursor sets{set};
    while (!sets.eof()) {
      std::string_view sequence;
      if (!sets.read(0x30, sequence)) break;
      Cursor pair{sequence};
      std::string_view oid;
      if (!pair.read(0x06, oid)) break;
      std::uint8_t tag = 0;
      std::string_view value;
      if (!pair.read_tag(0, value, tag)) break;
      if (oid_to_string(oid) == "2.5.4.3") info.common_name = read_string(value, tag);
    }
  }
  return info;
}

/// Finds the subjectAltName extension and extracts dNSName entries.
void collect_dns_names(std::string_view extensions_der, std::vector<std::string>& dns_names) {
  Cursor outer{extensions_der};
  std::string_view list;
  if (!outer.read(0x30, list)) return;
  Cursor items{list};
  while (!items.eof()) {
    std::string_view extension;
    if (!items.read(0x30, extension)) break;
    Cursor fields{extension};
    std::string_view oid;
    if (!fields.read(0x06, oid)) break;
    if (oid_to_string(oid) != "2.5.29.17") continue;
    std::uint8_t tag = 0;
    std::string_view value;
    bool found = false;
    while (!fields.eof()) {
      if (!fields.read_tag(0, value, tag)) break;
      if (tag == 0x04) {  // OCTET STRING holds the GeneralNames
        found = true;
        break;
      }
    }
    if (!found) continue;

    Cursor names_outer{value};
    std::string_view names;
    if (!names_outer.read(0x30, names)) continue;
    Cursor names_cursor{names};
    while (!names_cursor.eof()) {
      std::uint8_t name_tag = 0;
      std::string_view name_value;
      if (!names_cursor.read_tag(0, name_value, name_tag)) break;
      if (name_tag == 0x82) dns_names.emplace_back(name_value);  // [2] dNSName
    }
  }
}

}  // namespace

Result<Certificate> parse_certificate(std::string_view der) {
  Certificate certificate;
  Cursor outer{der};
  std::string_view body;
  if (!outer.read(0x30, body)) return Result<Certificate>::failure(x509_error("certificate is not a SEQUENCE"));

  Cursor certificate_cursor{body};
  std::string_view tbs;
  if (!certificate_cursor.read(0x30, tbs)) return Result<Certificate>::failure(x509_error("missing tbsCertificate"));

  Cursor tbs_cursor{tbs};
  std::uint8_t tag = static_cast<std::uint8_t>(tbs_cursor.peek_tag());
  if (tag == 0xA0) {  // [0] EXPLICIT version
    std::string_view version;
    if (!tbs_cursor.read(0xA0, version)) return Result<Certificate>::failure(x509_error("bad version field"));
  }

  std::string_view serial;
  if (!tbs_cursor.read(0x02, serial)) return Result<Certificate>::failure(x509_error("missing serial"));
  certificate.serial_hex = crypto::to_hex(std::string_view(serial.data(), serial.size()));

  std::string_view algorithm;
  if (!tbs_cursor.read(0x30, algorithm)) return Result<Certificate>::failure(x509_error("missing signature algorithm"));

  std::string_view issuer;
  if (!tbs_cursor.read(0x30, issuer)) return Result<Certificate>::failure(x509_error("missing issuer"));
  certificate.issuer_common_name = parse_name(issuer).common_name;

  std::string_view validity;
  if (!tbs_cursor.read(0x30, validity)) return Result<Certificate>::failure(x509_error("missing validity"));
  {
    Cursor validity_cursor{validity};
    std::uint8_t time_tag = 0;
    std::string_view not_before;
    if (validity_cursor.read_tag(0, not_before, time_tag)) certificate.not_before = read_time(not_before, time_tag);
    std::string_view not_after;
    if (validity_cursor.read_tag(0, not_after, time_tag)) certificate.not_after = read_time(not_after, time_tag);
  }

  std::string_view subject;
  if (!tbs_cursor.read(0x30, subject)) return Result<Certificate>::failure(x509_error("missing subject"));
  certificate.subject_common_name = parse_name(subject).common_name;

  std::string_view spki;
  if (!tbs_cursor.read(0x30, spki)) return Result<Certificate>::failure(x509_error("missing subjectPublicKeyInfo"));

  // Optional issuerUniqueID [1], subjectUniqueID [2], extensions [3].
  while (!tbs_cursor.eof()) {
    std::uint8_t extension_tag = 0;
    std::string_view value;
    if (!tbs_cursor.read_tag(0, value, extension_tag)) break;
    if (extension_tag == 0xA3) {
      Cursor explicit_cursor{value};
      std::string_view extensions;
      if (explicit_cursor.read(0x30, extensions)) collect_dns_names(extensions, certificate.dns_names);
    }
  }

  if (certificate.dns_names.empty() && !certificate.subject_common_name.empty()) {
    certificate.dns_names.push_back(certificate.subject_common_name);
  }
  return Result<Certificate>::success(std::move(certificate));
}

bool certificate_matches_host(const Certificate& certificate, std::string_view host) {
  std::string lowered(host);
  for (char& c : lowered) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  for (const std::string& name : certificate.dns_names) {
    std::string pattern = name;
    for (char& c : pattern) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (pattern == lowered) return true;
    if (pattern.starts_with("*.")) {
      const std::string_view suffix(pattern.data() + 1, pattern.size() - 1);  // ".example.com"
      if (lowered.size() > suffix.size() && lowered.ends_with(suffix)) {
        const std::string_view label = std::string_view(lowered).substr(0, lowered.size() - suffix.size());
        if (!label.empty() && label.find('.') == std::string_view::npos) return true;  // one label only
      }
    }
  }
  return false;
}

std::string current_utc_timestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
  return buffer;
}

bool certificate_is_valid_at(const Certificate& certificate, std::string_view now_iso8601) {
  const std::string now = now_iso8601.empty() ? current_utc_timestamp() : std::string(now_iso8601);
  if (!certificate.not_before.empty() && now < certificate.not_before) return false;
  if (!certificate.not_after.empty() && now > certificate.not_after) return false;
  return true;
}

}  // namespace bonk::net
