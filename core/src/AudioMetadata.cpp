#include "style_scaler/AudioMetadata.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace style_scaler {
namespace {

constexpr size_t kMaxTagChunkBytes = 8u * 1024u * 1024u; // Safety cap per metadata chunk

class ByteSource {
public:
    ByteSource(const uint8_t* ptr, size_t len) noexcept
        : mem_(ptr), size_(static_cast<uint64_t>(len)) {}

    explicit ByteSource(const std::filesystem::path& path) {
        file_.open(path, std::ios::binary | std::ios::ate);
        if (file_.is_open()) {
            const auto pos = file_.tellg();
            if (pos > 0) {
                size_ = static_cast<uint64_t>(pos);
            }
            file_.seekg(0, std::ios::beg);
        }
    }

    bool valid() const noexcept { return mem_ != nullptr || file_.is_open(); }
    uint64_t size() const noexcept { return size_; }

    bool readAt(uint64_t offset, void* dst, size_t len) const {
        if (len == 0) return true;
        if (offset > size_ || len > (size_ - offset)) return false;
        if (mem_ != nullptr) {
            std::memcpy(dst, mem_ + offset, len);
            return true;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file_) return false;
        file_.read(static_cast<char*>(dst), static_cast<std::streamsize>(len));
        return file_.gcount() == static_cast<std::streamsize>(len);
    }

    std::vector<uint8_t> readSlice(uint64_t offset, size_t len) const {
        if (len == 0 || offset > size_) return {};
        const size_t clamped = static_cast<size_t>(std::min<uint64_t>(len, size_ - offset));
        std::vector<uint8_t> out(clamped);
        if (!readAt(offset, out.data(), clamped)) return {};
        return out;
    }

    uint16_t readU16BE(uint64_t offset, bool& ok) const {
        uint8_t b[2]{};
        ok = readAt(offset, b, 2);
        return ok ? static_cast<uint16_t>((uint16_t(b[0]) << 8) | uint16_t(b[1])) : 0;
    }

    uint32_t readU32BE(uint64_t offset, bool& ok) const {
        uint8_t b[4]{};
        ok = readAt(offset, b, 4);
        return ok ? ((uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) |
                     (uint32_t(b[2]) << 8) | uint32_t(b[3]))
                  : 0;
    }

    uint32_t readU32LE(uint64_t offset, bool& ok) const {
        uint8_t b[4]{};
        ok = readAt(offset, b, 4);
        return ok ? (uint32_t(b[0]) | (uint32_t(b[1]) << 8) |
                     (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24))
                  : 0;
    }

private:
    const uint8_t* mem_ = nullptr;
    mutable std::ifstream file_;
    uint64_t size_ = 0;
};

uint16_t getU16BE(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

uint16_t getU16LE(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

uint32_t getU32BE(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t getU32LE(const uint8_t* p) noexcept {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool decodeSynchsafe32(const uint8_t* p, uint32_t& out) noexcept {
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80u) {
        return false;
    }
    out = (uint32_t(p[0]) << 21) | (uint32_t(p[1]) << 14) | (uint32_t(p[2]) << 7) | uint32_t(p[3]);
    return true;
}

std::string toLowerAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string pathToUtf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string trimAscii(std::string_view sv) {
    size_t a = 0;
    while (a < sv.size() && (std::isspace(static_cast<unsigned char>(sv[a])) || sv[a] == '\0')) ++a;
    size_t b = sv.size();
    while (b > a && (std::isspace(static_cast<unsigned char>(sv[b - 1])) || sv[b - 1] == '\0')) --b;
    return std::string(sv.substr(a, b - a));
}

std::string normalizeMatchKey(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    bool prevSpace = false;
    for (unsigned char c : sv) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            prevSpace = false;
        } else if (c == '&') {
            if (!prevSpace && !out.empty()) out.push_back(' ');
            out.append("and");
            prevSpace = false;
        } else if (c == '\'' || c == '`') {
            continue;
        } else if (!prevSpace && !out.empty()) {
            out.push_back(' ');
            prevSpace = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string titleCaseClean(std::string_view sv) {
    std::string cleaned = trimAscii(sv);
    std::string out;
    out.reserve(cleaned.size());
    bool capNext = true;
    bool prevSpace = false;
    for (unsigned char c : cleaned) {
        if (std::isspace(c)) {
            if (!prevSpace && !out.empty()) {
                out.push_back(' ');
                prevSpace = true;
            }
            capNext = true;
        } else {
            out.push_back(capNext ? static_cast<char>(std::toupper(c)) : static_cast<char>(c));
            capNext = (c == '-' || c == '/' || c == '(');
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool isNoiseGenreToken(std::string_view key) {
    return key.empty() || key == "unknown" || key == "none" || key == "other" ||
           key == "misc" || key == "miscellaneous" || key == "default" ||
           key == "genre" || key == "n a" || key == "na" || key == "unset";
}

struct GenreAlias {
    const char* pattern;
    const char* canonical;
};

// Ordered from most-specific to broadest so exact subgenres win over umbrella terms.
constexpr GenreAlias kGenreAliases[] = {
    {"90s hip hop", "90s Hip-Hop"},
    {"1990s hip hop", "90s Hip-Hop"},
    {"boom bap", "90s Hip-Hop"},
    {"boombap", "90s Hip-Hop"},
    {"old school hip hop", "90s Hip-Hop"},
    {"east coast hip hop", "90s Hip-Hop"},
    {"golden age hip hop", "90s Hip-Hop"},

    {"liquid dnb", "Liquid DnB"},
    {"liquid drum and bass", "Liquid DnB"},
    {"liquid drum n bass", "Liquid DnB"},
    {"liquid funk", "Liquid DnB"},

    {"neuro dnb", "Neuro DnB"},
    {"neurofunk", "Neuro DnB"},
    {"neuro funk", "Neuro DnB"},
    {"techstep", "Neuro DnB"},

    {"drum and bass", "Drum & Bass"},
    {"drum n bass", "Drum & Bass"},
    {"dnb", "Drum & Bass"},
    {"d and b", "Drum & Bass"},
    {"jungle", "Drum & Bass"},

    {"uk garage", "UK Garage"},
    {"ukg", "UK Garage"},
    {"2 step", "UK Garage"},
    {"two step", "UK Garage"},
    {"speed garage", "UK Garage"},
    {"future garage", "UK Garage"},

    {"dark trap", "Trap"},
    {"atlanta trap", "Trap"},
    {"uk drill", "Trap"},
    {"drill", "Trap"},
    {"phonk", "Trap"},
    {"trap", "Trap"},

    {"lofi hip hop", "Lo-Fi"},
    {"lo fi hip hop", "Lo-Fi"},
    {"lo fi", "Lo-Fi"},
    {"lofi", "Lo-Fi"},
    {"chillhop", "Lo-Fi"},

    {"hip hop", "Hip-Hop"},
    {"hiphop", "Hip-Hop"},
    {"gangsta rap", "Hip-Hop"},
    {"rap", "Hip-Hop"},

    {"neo soul", "R&B / Neo-Soul"},
    {"contemporary r and b", "R&B / Neo-Soul"},
    {"r and b", "R&B / Neo-Soul"},
    {"rhythm and blues", "R&B / Neo-Soul"},
    {"rnb", "R&B / Neo-Soul"},
    {"soul", "R&B / Neo-Soul"},

    {"tech house", "Tech House"},
    {"deep house", "Deep House"},
    {"progressive house", "Progressive House"},
    {"afro house", "Afro House"},
    {"bass house", "Bass House"},
    {"electro house", "Electro House"},
    {"house", "House"},

    {"melodic techno", "Melodic Techno"},
    {"acid techno", "Techno"},
    {"hard techno", "Techno"},
    {"minimal techno", "Techno"},
    {"techno", "Techno"},

    {"psytrance", "Trance"},
    {"uplifting trance", "Trance"},
    {"trance", "Trance"},

    {"brostep", "Dubstep"},
    {"riddim", "Dubstep"},
    {"dubstep", "Dubstep"},

    {"synthwave", "Synthwave"},
    {"retrowave", "Synthwave"},
    {"outrun", "Synthwave"},
    {"vaporwave", "Synthwave"},

    {"trip hop", "Ambient / Downtempo"},
    {"downtempo", "Ambient / Downtempo"},
    {"ambient", "Ambient / Downtempo"},
    {"chillout", "Ambient / Downtempo"},
    {"idm", "Ambient / Downtempo"},

    {"afrobeats", "Afrobeats / Amapiano"},
    {"afrobeat", "Afrobeats / Amapiano"},
    {"amapiano", "Afrobeats / Amapiano"},

    {"reggaeton", "Reggaeton / Dancehall"},
    {"dancehall", "Reggaeton / Dancehall"},
    {"latin", "Reggaeton / Dancehall"},

    {"synthpop", "Pop"},
    {"synth pop", "Pop"},
    {"electropop", "Pop"},
    {"dance pop", "Pop"},
    {"indie pop", "Pop"},
    {"pop", "Pop"},

    {"nu disco", "Funk / Disco"},
    {"disco", "Funk / Disco"},
    {"funk", "Funk / Disco"},
    {"boogie", "Funk / Disco"},

    {"jazz fusion", "Jazz"},
    {"nu jazz", "Jazz"},
    {"bebop", "Jazz"},
    {"jazz", "Jazz"},

    {"pop rock", "Pop / Rock"},
    {"pop punk", "Pop / Rock"},
    {"olivia rodrigo", "Pop / Rock"},
    {"michael jackson", "R&B"},
    {"victoria monet", "Funk / Disco"},
    {"indie rock", "Indie Rock"},
    {"alt rock", "Alternative Rock"},
    {"alternative rock", "Alternative Rock"},
    {"alternative", "Alternative Rock"},
    {"hard rock", "Rock"},
    {"rock", "Rock"},
    {"metal", "Metal"},
    {"heavy metal", "Metal"},
    {"punk rock", "Punk"},
    {"punk", "Punk"},

    {"classical", "Classical"},
    {"soundtrack", "Cinematic / Score"},
    {"cinematic", "Cinematic / Score"},
    {"electronic", "Electronic"},
    {"electronica", "Electronic"},
    {"edm", "Electronic"},
    {"dance", "Dance"}
};

std::string mapNormalizedGenre(const std::string& norm) {
    if (isNoiseGenreToken(norm)) return {};
    for (const auto& entry : kGenreAliases) {
        if (norm == entry.pattern) {
            return entry.canonical;
        }
    }
    return {};
}

// Decodes ID3v1 prefix forms like "(13)", "(17)Rock", "13", "(RX)" or "(CR)".
std::string expandId3v1Token(std::string_view token) {
    std::string s = trimAscii(token);
    if (s.empty()) return {};

    // Pure digits 0..191
    bool allDigits = true;
    for (unsigned char c : s) {
        if (!std::isdigit(c)) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        const long val = std::strtol(s.c_str(), nullptr, 10);
        if (val >= 0 && val <= 191) {
            return AudioMetadataReader::lookupId3v1Genre(static_cast<uint8_t>(val));
        }
        return {};
    }

    // "(13)" or "(13)Pop" or "(RX)"
    if (s.front() == '(') {
        const size_t closePos = s.find(')');
        if (closePos != std::string::npos && closePos > 1) {
            const std::string inside = s.substr(1, closePos - 1);
            const std::string rest = trimAscii(s.substr(closePos + 1));
            if (!rest.empty()) {
                return rest;
            }
            if (inside == "RX") return "Remix";
            if (inside == "CR") return "Cover";
            bool digits = true;
            for (unsigned char c : inside) {
                if (!std::isdigit(c)) {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                const long val = std::strtol(inside.c_str(), nullptr, 10);
                if (val >= 0 && val <= 191) {
                    return AudioMetadataReader::lookupId3v1Genre(static_cast<uint8_t>(val));
                }
            }
        }
    }

    return s;
}

std::vector<std::string> splitRawGenreParts(std::string_view rawGenre) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i < rawGenre.size(); ++i) {
        const char ch = rawGenre[i];
        // Preserve R&B / Drum & Bass; split '/', ';', ',', '|', '\0', or parenthesized ID3 multi-codes like "(13)(7)"
        if (ch == '\0' || ch == ';' || ch == ',' || ch == '|') {
            auto t = trimAscii(cur);
            if (!t.empty()) parts.push_back(std::move(t));
            cur.clear();
        } else if (ch == '/') {
            const std::string lowCur = toLowerAscii(trimAscii(cur));
            // Keep "Drum/Bass" or "R/B" intact only if short compound, usually '/' is ID3v2.3 separator
            (void)lowCur;
            auto t = trimAscii(cur);
            if (!t.empty()) parts.push_back(std::move(t));
            cur.clear();
        } else if (ch == '(' && !cur.empty() && cur.back() == ')') {
            auto t = trimAscii(cur);
            if (!t.empty()) parts.push_back(std::move(t));
            cur.assign(1, ch);
        } else {
            cur.push_back(ch);
        }
    }
    auto t = trimAscii(cur);
    if (!t.empty()) parts.push_back(std::move(t));
    return parts;
}

void appendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp == 0) return;
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string decodeLatin1ToUtf8(const uint8_t* p, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = p[i];
        if (c == 0) break;
        appendUtf8Codepoint(out, c);
    }
    return trimAscii(out);
}

std::string decodeUtf16ToUtf8(const uint8_t* p, size_t len, bool bigEndian) {
    std::string out;
    out.reserve(len);
    size_t i = 0;
    while (i + 1 < len) {
        const uint16_t u = bigEndian ? getU16BE(p + i) : getU16LE(p + i);
        i += 2;
        if (u == 0) break;
        if (u >= 0xD800u && u <= 0xDBFFu && i + 1 < len) {
            const uint16_t u2 = bigEndian ? getU16BE(p + i) : getU16LE(p + i);
            if (u2 >= 0xDC00u && u2 <= 0xDFFFu) {
                i += 2;
                const uint32_t cp = 0x10000u + (((uint32_t(u) - 0xD800u) << 10) | (uint32_t(u2) - 0xDC00u));
                appendUtf8Codepoint(out, cp);
                continue;
            }
        }
        appendUtf8Codepoint(out, u);
    }
    return trimAscii(out);
}

// Decodes ID3 encoded string(s), preserving multiple null-separated strings joined by "; ".
std::string decodeId3StringList(uint8_t encoding, const uint8_t* p, size_t len, size_t* outConsumedBytes = nullptr) {
    if (len == 0) {
        if (outConsumedBytes) *outConsumedBytes = 0;
        return {};
    }

    auto joinNonEmpty = [](const std::vector<std::string>& items) -> std::string {
        std::string res;
        for (const auto& s : items) {
            if (s.empty()) continue;
            if (!res.empty()) res.append("; ");
            res.append(s);
        }
        return res;
    };

    if (encoding == 0 || encoding == 3) {
        // 1-byte code units (Latin-1 or UTF-8)
        std::vector<std::string> items;
        size_t pos = 0;
        while (pos < len) {
            size_t end = pos;
            while (end < len && p[end] != 0) ++end;
            if (encoding == 0) {
                items.push_back(decodeLatin1ToUtf8(p + pos, end - pos));
            } else {
                items.push_back(trimAscii(std::string_view(reinterpret_cast<const char*>(p + pos), end - pos)));
            }
            if (outConsumedBytes && items.size() == 1) {
                *outConsumedBytes = (end < len) ? (end + 1) : end;
                return items.front();
            }
            if (end >= len) break;
            pos = end + 1;
        }
        if (outConsumedBytes) *outConsumedBytes = len;
        return joinNonEmpty(items);
    }

    if (encoding == 1 || encoding == 2) {
        // UTF-16 (1 = BOM required/optional, 2 = UTF-16BE no BOM)
        std::vector<std::string> items;
        size_t pos = 0;
        while (pos + 1 < len) {
            bool bigEndian = (encoding == 2);
            if (encoding == 1 && pos + 1 < len) {
                if (p[pos] == 0xFF && p[pos + 1] == 0xFE) {
                    bigEndian = false;
                    pos += 2;
                } else if (p[pos] == 0xFE && p[pos + 1] == 0xFF) {
                    bigEndian = true;
                    pos += 2;
                } else {
                    bigEndian = false;
                }
            }
            size_t end = pos;
            while (end + 1 < len && !(p[end] == 0 && p[end + 1] == 0)) {
                end += 2;
            }
            items.push_back(decodeUtf16ToUtf8(p + pos, end - pos, bigEndian));
            if (outConsumedBytes && items.size() == 1) {
                *outConsumedBytes = (end + 1 < len) ? (end + 2) : len;
                return items.front();
            }
            if (end + 1 >= len) break;
            pos = end + 2;
        }
        if (outConsumedBytes) *outConsumedBytes = len;
        return joinNonEmpty(items);
    }

    if (outConsumedBytes) *outConsumedBytes = len;
    return {};
}

std::vector<uint8_t> decodeUnsync(const uint8_t* p, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(p[i]);
        if (p[i] == 0xFFu && i + 1 < len && p[i + 1] == 0x00u) {
            ++i;
        }
    }
    return out;
}

void mergeGenreCandidate(SongMetadata& meta, std::string_view raw, const char* sourceName) {
    const std::string trimmed = trimAscii(raw);
    if (trimmed.empty()) return;

    auto newTags = AudioMetadataReader::extractGenreTags(trimmed);
    for (const auto& t : newTags) {
        if (std::find(meta.tags.begin(), meta.tags.end(), t) == meta.tags.end()) {
            meta.tags.push_back(t);
        }
    }

    const std::string canon = AudioMetadataReader::canonicalizeGenre(trimmed);
    if (!canon.empty() && meta.canonical_genre.empty()) {
        meta.raw_genre = trimmed;
        meta.canonical_genre = canon;
        meta.genre_source = sourceName;
    }
}

void applyKeyValueField(SongMetadata& meta, std::string_view keyRaw, std::string_view valRaw, const char* sourceName) {
    const std::string key = normalizeMatchKey(keyRaw);
    const std::string val = trimAscii(valRaw);
    if (key.empty() || val.empty()) return;

    if (key == "genre" || key == "contentgroup" || key == "style" || key == "subgenre" || key == "tags") {
        mergeGenreCandidate(meta, val, sourceName);
    } else if ((key == "title" || key == "track") && meta.title.empty()) {
        meta.title = val;
    } else if ((key == "artist" || key == "albumartist" || key == "performer") && meta.artist.empty()) {
        meta.artist = val;
    } else if (key == "album" && meta.album.empty()) {
        meta.album = val;
    } else if ((key == "bpm" || key == "tempo" || key == "tbpm") && meta.bpm <= 0.0) {
        char* endPtr = nullptr;
        const double parsed = std::strtod(val.c_str(), &endPtr);
        if (endPtr != val.c_str() && parsed >= 25.0 && parsed <= 320.0) {
            meta.bpm = parsed;
        }
    } else if ((key == "initialkey" || key == "initial key" || key == "key") && !meta.key_pc.has_value()) {
        meta.key_text = val;
        (void)AudioMetadataReader::parseKeyString(val, meta.key_pc, meta.mode);
    } else if ((key == "comment" || key == "description") && meta.comment.empty()) {
        meta.comment = val;
    }
}

void parseId3v2Buffer(const uint8_t* buf, size_t len, SongMetadata& meta, uint64_t* outTotalTagBytes = nullptr) {
    if (len < 10 || buf[0] != 'I' || buf[1] != 'D' || buf[2] != '3') return;

    const uint8_t verMajor = buf[3];
    if (verMajor < 2 || verMajor > 4) return;
    const uint8_t flags = buf[5];

    uint32_t payloadSize = 0;
    if (!decodeSynchsafe32(buf + 6, payloadSize)) return;

    const size_t totalNeeded = 10u + static_cast<size_t>(payloadSize);
    if (outTotalTagBytes) *outTotalTagBytes = totalNeeded;

    const size_t availEnd = std::min(len, totalNeeded);
    if (availEnd <= 10u) return;

    std::vector<uint8_t> decoded;
    const uint8_t* tagData = buf + 10;
    size_t tagLen = availEnd - 10u;

    if ((flags & 0x80u) != 0 && verMajor <= 3) {
        decoded = decodeUnsync(tagData, tagLen);
        tagData = decoded.data();
        tagLen = decoded.size();
    }

    size_t pos = 0;
    // Extended header (v2.3 or v2.4)
    if ((flags & 0x40u) != 0 && verMajor >= 3 && tagLen >= 4) {
        uint32_t extSize = 0;
        if (verMajor == 4) {
            if (decodeSynchsafe32(tagData, extSize) && extSize >= 4 && extSize <= tagLen) {
                pos = extSize;
            }
        } else {
            extSize = getU32BE(tagData);
            if (4u + static_cast<size_t>(extSize) <= tagLen) {
                pos = 4u + static_cast<size_t>(extSize);
            }
        }
    }

    while (pos < tagLen) {
        if (tagData[pos] == 0) break; // Padding reached

        std::string frameId;
        uint32_t frameSize = 0;
        uint16_t frameFlags = 0;
        size_t hdrSize = 0;

        if (verMajor == 2) {
            if (pos + 6u > tagLen) break;
            frameId.assign(reinterpret_cast<const char*>(tagData + pos), 3);
            frameSize = (uint32_t(tagData[pos + 3]) << 16) |
                        (uint32_t(tagData[pos + 4]) << 8) |
                        uint32_t(tagData[pos + 5]);
            hdrSize = 6u;
        } else {
            if (pos + 10u > tagLen) break;
            frameId.assign(reinterpret_cast<const char*>(tagData + pos), 4);
            if (verMajor == 4) {
                if (!decodeSynchsafe32(tagData + pos + 4, frameSize)) break;
            } else {
                frameSize = getU32BE(tagData + pos + 4);
            }
            frameFlags = getU16BE(tagData + pos + 8);
            hdrSize = 10u;
        }

        if (frameSize == 0 || pos + hdrSize > tagLen || frameSize > (tagLen - pos - hdrSize)) {
            break;
        }

        const uint8_t* payload = tagData + pos + hdrSize;
        size_t payloadLen = frameSize;
        pos += hdrSize + frameSize;

        std::vector<uint8_t> frameDecoded;
        if (verMajor == 4) {
            if ((frameFlags & 0x0001u) != 0 && payloadLen >= 4) { // Data length indicator
                payload += 4;
                payloadLen -= 4;
            }
            if ((frameFlags & 0x0002u) != 0) { // Frame unsynchronisation
                frameDecoded = decodeUnsync(payload, payloadLen);
                payload = frameDecoded.data();
                payloadLen = frameDecoded.size();
            }
        }

        if (payloadLen == 0) continue;

        if (frameId == "TCON" || frameId == "TCO") {
            const std::string val = decodeId3StringList(payload[0], payload + 1, payloadLen - 1);
            mergeGenreCandidate(meta, val, "id3v2");
        } else if (frameId == "TIT2" || frameId == "TT2") {
            if (meta.title.empty()) meta.title = decodeId3StringList(payload[0], payload + 1, payloadLen - 1);
        } else if (frameId == "TPE1" || frameId == "TP1") {
            if (meta.artist.empty()) meta.artist = decodeId3StringList(payload[0], payload + 1, payloadLen - 1);
        } else if (frameId == "TALB" || frameId == "TAL") {
            if (meta.album.empty()) meta.album = decodeId3StringList(payload[0], payload + 1, payloadLen - 1);
        } else if (frameId == "TBPM" || frameId == "TBP") {
            const std::string s = decodeId3StringList(payload[0], payload + 1, payloadLen - 1);
            applyKeyValueField(meta, "bpm", s, "id3v2");
        } else if (frameId == "TKEY" || frameId == "TKE") {
            const std::string s = decodeId3StringList(payload[0], payload + 1, payloadLen - 1);
            applyKeyValueField(meta, "key", s, "id3v2");
        } else if (frameId == "TXXX" || frameId == "TXX") {
            const uint8_t enc = payload[0];
            size_t consumed = 0;
            const std::string desc = decodeId3StringList(enc, payload + 1, payloadLen - 1, &consumed);
            if (1u + consumed <= payloadLen) {
                const std::string val = decodeId3StringList(enc, payload + 1 + consumed, payloadLen - 1 - consumed);
                applyKeyValueField(meta, desc, val, "id3v2");
            }
        } else if ((frameId == "COMM" || frameId == "COM") && payloadLen >= 4) {
            const uint8_t enc = payload[0];
            size_t consumed = 0;
            const std::string desc = decodeId3StringList(enc, payload + 4, payloadLen - 4, &consumed);
            if (4u + consumed <= payloadLen) {
                const std::string text = decodeId3StringList(enc, payload + 4 + consumed, payloadLen - 4 - consumed);
                if (!text.empty() && meta.comment.empty()) meta.comment = text;
                const std::string descLower = toLowerAscii(desc);
                if (descLower.find("genre") != std::string::npos || descLower.find("style") != std::string::npos) {
                    mergeGenreCandidate(meta, text, "id3v2");
                }
            }
        }
    }
}

void tryParseId3v2At(const ByteSource& src, uint64_t offset, SongMetadata& meta, uint64_t* outTotalBytes = nullptr) {
    uint8_t hdr[10]{};
    if (!src.readAt(offset, hdr, 10)) return;
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') return;

    uint32_t payloadSize = 0;
    if (!decodeSynchsafe32(hdr + 6, payloadSize)) return;
    const size_t totalBytes = static_cast<size_t>(std::min<uint64_t>(kMaxTagChunkBytes, 10ull + payloadSize));
    auto buf = src.readSlice(offset, totalBytes);
    if (buf.size() >= 10) {
        parseId3v2Buffer(buf.data(), buf.size(), meta, outTotalBytes);
    }
}

void parseId3v1Footer(const ByteSource& src, SongMetadata& meta) {
    if (src.size() < 128) return;
    const auto tag = src.readSlice(src.size() - 128, 128);
    if (tag.size() != 128 || tag[0] != 'T' || tag[1] != 'A' || tag[2] != 'G') return;

    auto readFixed = [&](size_t off, size_t len) -> std::string {
        return decodeLatin1ToUtf8(tag.data() + off, len);
    };

    if (meta.title.empty()) meta.title = readFixed(3, 30);
    if (meta.artist.empty()) meta.artist = readFixed(33, 30);
    if (meta.album.empty()) meta.album = readFixed(63, 30);
    if (meta.comment.empty()) meta.comment = readFixed(97, 28);

    const uint8_t genreCode = tag[127];
    if (genreCode < 192) {
        const std::string g = AudioMetadataReader::lookupId3v1Genre(genreCode);
        mergeGenreCandidate(meta, g, "id3v1");
    }
}

void parseVorbisCommentPayload(const uint8_t* p, size_t len, SongMetadata& meta, const char* sourceName) {
    if (len < 8) return;
    size_t pos = 0;
    const uint32_t vendorLen = getU32LE(p + pos);
    pos += 4;
    if (vendorLen > len - pos) return;
    pos += vendorLen;

    if (pos + 4 > len) return;
    const uint32_t count = std::min<uint32_t>(4096u, getU32LE(p + pos));
    pos += 4;

    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 4 > len) break;
        const uint32_t itemLen = getU32LE(p + pos);
        pos += 4;
        if (itemLen > len - pos) break;
        const std::string_view item(reinterpret_cast<const char*>(p + pos), itemLen);
        pos += itemLen;

        const size_t eq = item.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        applyKeyValueField(meta, item.substr(0, eq), item.substr(eq + 1), sourceName);
    }
}

void parseFlac(const ByteSource& src, uint64_t startOffset, SongMetadata& meta) {
    uint8_t magic[4]{};
    if (!src.readAt(startOffset, magic, 4) || std::memcmp(magic, "fLaC", 4) != 0) return;

    uint64_t pos = startOffset + 4;
    for (int iter = 0; iter < 64 && pos + 4 <= src.size(); ++iter) {
        uint8_t blkHdr[4]{};
        if (!src.readAt(pos, blkHdr, 4)) break;
        pos += 4;

        const bool isLast = (blkHdr[0] & 0x80u) != 0;
        const uint8_t blockType = blkHdr[0] & 0x7Fu;
        const uint32_t blockSize = (uint32_t(blkHdr[1]) << 16) |
                                   (uint32_t(blkHdr[2]) << 8) |
                                   uint32_t(blkHdr[3]);

        if (blockSize > src.size() - pos) break;

        if (blockType == 4 && blockSize <= kMaxTagChunkBytes) {
            const auto payload = src.readSlice(pos, blockSize);
            if (!payload.empty()) {
                parseVorbisCommentPayload(payload.data(), payload.size(), meta, "vorbis");
            }
        }
        pos += blockSize;
        if (isLast) break;
    }
}

void parseOgg(const ByteSource& src, SongMetadata& meta) {
    // Reassemble packet stream across first few Ogg pages (<= 256 KB) to locate "\x03vorbis" or "OpusTags".
    std::vector<uint8_t> stream;
    stream.reserve(16384);
    uint64_t offset = 0;

    for (int page = 0; page < 12 && offset + 27 <= src.size() && stream.size() < 256u * 1024u; ++page) {
        uint8_t hdr[27]{};
        if (!src.readAt(offset, hdr, 27) || std::memcmp(hdr, "OggS", 4) != 0) break;
        const uint8_t numSegs = hdr[26];
        if (offset + 27ull + numSegs > src.size()) break;
        const auto lacing = src.readSlice(offset + 27ull, numSegs);
        size_t pageBodySize = 0;
        for (uint8_t seg : lacing) pageBodySize += seg;
        const uint64_t bodyOff = offset + 27ull + numSegs;
        if (pageBodySize > src.size() - bodyOff) break;
        const auto body = src.readSlice(bodyOff, std::min<size_t>(pageBodySize, 256u * 1024u));
        stream.insert(stream.end(), body.begin(), body.end());
        offset = bodyOff + pageBodySize;
    }

    static constexpr const char kVorbisSig[] = "\x03vorbis";
    static constexpr const char kOpusSig[] = "OpusTags";

    auto findMarker = [&](const char* marker, size_t mlen) -> std::optional<size_t> {
        if (stream.size() < mlen) return std::nullopt;
        const auto it = std::search(stream.begin(), stream.end(), marker, marker + mlen);
        if (it == stream.end()) return std::nullopt;
        return static_cast<size_t>(std::distance(stream.begin(), it)) + mlen;
    };

    if (auto off = findMarker(kVorbisSig, 7)) {
        parseVorbisCommentPayload(stream.data() + *off, stream.size() - *off, meta, "vorbis");
    } else if (auto off2 = findMarker(kOpusSig, 8)) {
        parseVorbisCommentPayload(stream.data() + *off2, stream.size() - *off2, meta, "vorbis");
    }
}

void parseWav(const ByteSource& src, SongMetadata& meta) {
    uint8_t hdr[12]{};
    if (!src.readAt(0, hdr, 12)) return;
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return;

    const uint32_t riffDeclared = getU32LE(hdr + 4);
    const uint64_t endLimit = std::min<uint64_t>(src.size(), 8ull + riffDeclared);
    uint64_t pos = 12;

    while (pos + 8 <= endLimit) {
        uint8_t cHdr[8]{};
        if (!src.readAt(pos, cHdr, 8)) break;
        const uint32_t chunkSize = getU32LE(cHdr + 4);
        const uint64_t dataStart = pos + 8;
        const uint64_t paddedSize = static_cast<uint64_t>(chunkSize) + (chunkSize & 1u);
        if (chunkSize > endLimit - dataStart) break;

        if (std::memcmp(cHdr, "LIST", 4) == 0 && chunkSize >= 4) {
            uint8_t listType[4]{};
            if (src.readAt(dataStart, listType, 4) && std::memcmp(listType, "INFO", 4) == 0) {
                uint64_t subPos = dataStart + 4;
                const uint64_t subEnd = dataStart + chunkSize;
                while (subPos + 8 <= subEnd) {
                    uint8_t sHdr[8]{};
                    if (!src.readAt(subPos, sHdr, 8)) break;
                    const uint32_t sSize = getU32LE(sHdr + 4);
                    const uint64_t sData = subPos + 8;
                    const uint64_t sPad = static_cast<uint64_t>(sSize) + (sSize & 1u);
                    if (sSize > subEnd - sData) break;

                    if (sSize > 0 && sSize <= 65536u) {
                        const auto valBytes = src.readSlice(sData, sSize);
                        const std::string val = decodeLatin1ToUtf8(valBytes.data(), valBytes.size());
                        if (std::memcmp(sHdr, "IGNR", 4) == 0) applyKeyValueField(meta, "genre", val, "riff_info");
                        else if (std::memcmp(sHdr, "INAM", 4) == 0) applyKeyValueField(meta, "title", val, "riff_info");
                        else if (std::memcmp(sHdr, "IART", 4) == 0) applyKeyValueField(meta, "artist", val, "riff_info");
                        else if (std::memcmp(sHdr, "IPRD", 4) == 0) applyKeyValueField(meta, "album", val, "riff_info");
                        else if (std::memcmp(sHdr, "ICMT", 4) == 0) applyKeyValueField(meta, "comment", val, "riff_info");
                        else if (std::memcmp(sHdr, "IBPM", 4) == 0 || std::memcmp(sHdr, "ITMP", 4) == 0) {
                            applyKeyValueField(meta, "bpm", val, "riff_info");
                        } else if (std::memcmp(sHdr, "IKEY", 4) == 0) {
                            applyKeyValueField(meta, "key", val, "riff_info");
                        }
                    }
                    if (sPad == 0 || sPad > subEnd - sData) break;
                    subPos = sData + sPad;
                }
            }
        } else if ((std::memcmp(cHdr, "id3 ", 4) == 0 || std::memcmp(cHdr, "ID3 ", 4) == 0) &&
                   chunkSize >= 10 && chunkSize <= kMaxTagChunkBytes) {
            const auto tagBytes = src.readSlice(dataStart, chunkSize);
            if (tagBytes.size() >= 10) {
                parseId3v2Buffer(tagBytes.data(), tagBytes.size(), meta);
            }
        }

        if (paddedSize > src.size() - dataStart) break;
        pos = dataStart + paddedSize;
    }
}

void parseAiff(const ByteSource& src, SongMetadata& meta) {
    uint8_t hdr[12]{};
    if (!src.readAt(0, hdr, 12)) return;
    if (std::memcmp(hdr, "FORM", 4) != 0) return;
    if (std::memcmp(hdr + 8, "AIFF", 4) != 0 && std::memcmp(hdr + 8, "AIFC", 4) != 0) return;

    const uint32_t formDeclared = getU32BE(hdr + 4);
    const uint64_t endLimit = std::min<uint64_t>(src.size(), 8ull + formDeclared);
    uint64_t pos = 12;

    while (pos + 8 <= endLimit) {
        uint8_t cHdr[8]{};
        if (!src.readAt(pos, cHdr, 8)) break;
        const uint32_t chunkSize = getU32BE(cHdr + 4);
        const uint64_t dataStart = pos + 8;
        const uint64_t paddedSize = static_cast<uint64_t>(chunkSize) + (chunkSize & 1u);
        if (chunkSize > endLimit - dataStart) break;

        if (std::memcmp(cHdr, "NAME", 4) == 0 && chunkSize <= 65536u) {
            const auto b = src.readSlice(dataStart, chunkSize);
            applyKeyValueField(meta, "title", decodeLatin1ToUtf8(b.data(), b.size()), "aiff");
        } else if (std::memcmp(cHdr, "AUTH", 4) == 0 && chunkSize <= 65536u) {
            const auto b = src.readSlice(dataStart, chunkSize);
            applyKeyValueField(meta, "artist", decodeLatin1ToUtf8(b.data(), b.size()), "aiff");
        } else if (std::memcmp(cHdr, "ANNO", 4) == 0 && chunkSize <= 65536u) {
            const auto b = src.readSlice(dataStart, chunkSize);
            const std::string anno = decodeLatin1ToUtf8(b.data(), b.size());
            applyKeyValueField(meta, "comment", anno, "aiff");
            const std::string low = toLowerAscii(anno);
            if (const auto gp = low.find("genre:"); gp != std::string::npos) {
                const auto eol = anno.find_first_of("\r\n;", gp + 6);
                mergeGenreCandidate(meta, anno.substr(gp + 6, eol - (gp + 6)), "aiff");
            }
        } else if ((std::memcmp(cHdr, "ID3 ", 4) == 0 || std::memcmp(cHdr, "id3 ", 4) == 0) &&
                   chunkSize >= 10 && chunkSize <= kMaxTagChunkBytes) {
            const auto tagBytes = src.readSlice(dataStart, chunkSize);
            if (tagBytes.size() >= 10) {
                parseId3v2Buffer(tagBytes.data(), tagBytes.size(), meta);
            }
        }

        if (paddedSize > src.size() - dataStart) break;
        pos = dataStart + paddedSize;
    }
}

void parseMp4Ilst(const ByteSource& src, uint64_t start, uint64_t end, SongMetadata& meta) {
    uint64_t pos = start;
    while (pos + 8 <= end) {
        uint8_t aHdr[8]{};
        if (!src.readAt(pos, aHdr, 8)) break;
        const uint32_t atomSize = getU32BE(aHdr);
        if (atomSize < 8 || atomSize > end - pos) break;

        uint64_t childPos = pos + 8;
        const uint64_t atomEnd = pos + atomSize;
        std::string freeformName;

        while (childPos + 8 <= atomEnd) {
            uint8_t cHdr[8]{};
            if (!src.readAt(childPos, cHdr, 8)) break;
            const uint32_t cSize = getU32BE(cHdr);
            if (cSize < 8 || cSize > atomEnd - childPos) break;
            const uint64_t cBody = childPos + 8;
            const uint32_t cBodyLen = cSize - 8;

            if (std::memcmp(aHdr + 4, "----", 4) == 0 && std::memcmp(cHdr + 4, "name", 4) == 0 && cBodyLen > 4) {
                const auto nameBytes = src.readSlice(cBody + 4, std::min<uint32_t>(cBodyLen - 4, 256u));
                freeformName = std::string(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
            } else if (std::memcmp(cHdr + 4, "data", 4) == 0 && cBodyLen > 8 && cBodyLen <= 65536u) {
                const auto dataBox = src.readSlice(cBody, cBodyLen);
                const uint32_t typeInd = getU32BE(dataBox.data());
                const uint8_t* valPtr = dataBox.data() + 8;
                const size_t valLen = dataBox.size() - 8;

                if (aHdr[4] == 0xA9 && std::memcmp(aHdr + 5, "gen", 3) == 0) {
                    mergeGenreCandidate(meta, std::string_view(reinterpret_cast<const char*>(valPtr), valLen), "mp4");
                } else if (std::memcmp(aHdr + 4, "gnre", 4) == 0 && valLen >= 2) {
                    const uint16_t codePlusOne = getU16BE(valPtr);
                    if (codePlusOne >= 1 && codePlusOne <= 192) {
                        mergeGenreCandidate(meta, AudioMetadataReader::lookupId3v1Genre(static_cast<uint8_t>(codePlusOne - 1)), "mp4");
                    }
                } else if (aHdr[4] == 0xA9 && std::memcmp(aHdr + 5, "nam", 3) == 0 && typeInd == 1) {
                    if (meta.title.empty()) meta.title = trimAscii(std::string_view(reinterpret_cast<const char*>(valPtr), valLen));
                } else if (aHdr[4] == 0xA9 && std::memcmp(aHdr + 5, "ART", 3) == 0 && typeInd == 1) {
                    if (meta.artist.empty()) meta.artist = trimAscii(std::string_view(reinterpret_cast<const char*>(valPtr), valLen));
                } else if (aHdr[4] == 0xA9 && std::memcmp(aHdr + 5, "alb", 3) == 0 && typeInd == 1) {
                    if (meta.album.empty()) meta.album = trimAscii(std::string_view(reinterpret_cast<const char*>(valPtr), valLen));
                } else if (std::memcmp(aHdr + 4, "tmpo", 4) == 0 && valLen >= 2 && meta.bpm <= 0.0) {
                    const uint16_t bpmInt = getU16BE(valPtr);
                    if (bpmInt >= 25 && bpmInt <= 320) meta.bpm = static_cast<double>(bpmInt);
                } else if (std::memcmp(aHdr + 4, "----", 4) == 0 && !freeformName.empty()) {
                    applyKeyValueField(meta, freeformName,
                                       std::string_view(reinterpret_cast<const char*>(valPtr), valLen), "mp4");
                }
            }
            childPos += cSize;
        }
        pos = atomEnd;
    }
}

void walkMp4Containers(const ByteSource& src, uint64_t start, uint64_t end, int depth, SongMetadata& meta) {
    if (depth > 6) return;
    uint64_t pos = start;
    while (pos + 8 <= end) {
        uint8_t hdr[8]{};
        if (!src.readAt(pos, hdr, 8)) break;
        const uint32_t size32 = getU32BE(hdr);
        uint64_t atomSize = size32;
        uint64_t hdrLen = 8;
        if (size32 == 1) {
            if (pos + 16 > end) break;
            uint8_t ext[8]{};
            if (!src.readAt(pos + 8, ext, 8)) break;
            atomSize = (uint64_t(getU32BE(ext)) << 32) | getU32BE(ext + 4);
            hdrLen = 16;
        } else if (size32 == 0) {
            atomSize = end - pos;
        }
        if (atomSize < hdrLen || atomSize > end - pos) break;
        const uint64_t bodyStart = pos + hdrLen;
        const uint64_t atomEnd = pos + atomSize;

        if (std::memcmp(hdr + 4, "moov", 4) == 0 || std::memcmp(hdr + 4, "udta", 4) == 0) {
            walkMp4Containers(src, bodyStart, atomEnd, depth + 1, meta);
        } else if (std::memcmp(hdr + 4, "meta", 4) == 0 && bodyStart + 4 <= atomEnd) {
            // MP4 FullBox 'meta' starts with 4-byte version+flags
            walkMp4Containers(src, bodyStart + 4, atomEnd, depth + 1, meta);
        } else if (std::memcmp(hdr + 4, "ilst", 4) == 0) {
            parseMp4Ilst(src, bodyStart, atomEnd, meta);
        }
        pos = atomEnd;
    }
}

void parseMp4(const ByteSource& src, SongMetadata& meta) {
    uint8_t ftyp[8]{};
    if (!src.readAt(0, ftyp, 8) || std::memcmp(ftyp + 4, "ftyp", 4) != 0) return;
    walkMp4Containers(src, 0, src.size(), 0, meta);
}

bool readVlq(const uint8_t* data, size_t len, size_t& pos, uint32_t& out) noexcept {
    out = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= len) return false;
        const uint8_t b = data[pos++];
        out = (out << 7) | (b & 0x7Fu);
        if ((b & 0x80u) == 0) return true;
    }
    return false;
}

void parseMidiMeta(const ByteSource& src, SongMetadata& meta) {
    if (src.size() < 14 || src.size() > 16u * 1024u * 1024u) return;
    const auto bytes = src.readSlice(0, static_cast<size_t>(src.size()));
    if (bytes.size() < 14 || std::memcmp(bytes.data(), "MThd", 4) != 0) return;

    const uint32_t hdrLen = getU32BE(bytes.data() + 4);
    if (hdrLen < 6 || 8u + hdrLen > bytes.size()) return;
    const uint16_t numTracks = getU16BE(bytes.data() + 10);
    size_t pos = 8u + hdrLen;

    static constexpr int kSharpMajRoots[15] = {11, 6, 1, 8, 3, 10, 5, 0, 7, 2, 9, 4, 11, 6, 1}; // index sf+7 -> Cb..C..C#
    static constexpr int kMinRelShift = 9;

    for (uint16_t trk = 0; trk < numTracks && pos + 8 <= bytes.size(); ++trk) {
        if (std::memcmp(bytes.data() + pos, "MTrk", 4) != 0) break;
        const uint32_t trkLen = getU32BE(bytes.data() + pos + 4);
        pos += 8;
        if (trkLen > bytes.size() - pos) break;
        const size_t trkEnd = pos + trkLen;

        uint8_t runningStatus = 0;
        while (pos < trkEnd) {
            uint32_t delta = 0;
            if (!readVlq(bytes.data(), trkEnd, pos, delta)) break;
            if (pos >= trkEnd) break;

            uint8_t status = bytes[pos];
            if (status < 0x80u) {
                if (runningStatus == 0) break;
                status = runningStatus;
            } else {
                ++pos;
                if (status < 0xF0u) runningStatus = status;
            }

            if (status == 0xFFu) {
                if (pos >= trkEnd) break;
                const uint8_t metaType = bytes[pos++];
                uint32_t mLen = 0;
                if (!readVlq(bytes.data(), trkEnd, pos, mLen) || mLen > trkEnd - pos) break;
                const uint8_t* mPtr = bytes.data() + pos;
                pos += mLen;

                if (metaType == 0x01 || metaType == 0x02 || metaType == 0x03 || metaType == 0x06) {
                    const std::string text = decodeLatin1ToUtf8(mPtr, mLen);
                    const std::string low = toLowerAscii(text);
                    for (const char* prefix : {"genre:", "style:", "subgenre:"}) {
                        if (const auto idx = low.find(prefix); idx != std::string::npos) {
                            const size_t startVal = idx + std::strlen(prefix);
                            const size_t endVal = text.find_first_of("\r\n|", startVal);
                            mergeGenreCandidate(meta, text.substr(startVal, endVal - startVal), "midi_meta");
                        }
                    }
                    if (const auto kp = low.find("key:"); kp != std::string::npos && !meta.key_pc.has_value()) {
                        const size_t endK = text.find_first_of("\r\n|", kp + 4);
                        applyKeyValueField(meta, "key", text.substr(kp + 4, endK - (kp + 4)), "midi_meta");
                    }
                } else if (metaType == 0x51 && mLen == 3 && meta.bpm <= 0.0) {
                    const uint32_t usPerQ = (uint32_t(mPtr[0]) << 16) | (uint32_t(mPtr[1]) << 8) | uint32_t(mPtr[2]);
                    if (usPerQ > 0) {
                        const double b = 60'000'000.0 / static_cast<double>(usPerQ);
                        if (b >= 25.0 && b <= 320.0) meta.bpm = b;
                    }
                } else if (metaType == 0x59 && mLen == 2 && !meta.key_pc.has_value()) {
                    const int sf = static_cast<int8_t>(mPtr[0]);
                    const uint8_t mi = mPtr[1];
                    if (sf >= -7 && sf <= 7) {
                        const int majRoot = kSharpMajRoots[sf + 7];
                        meta.key_pc = (mi == 1) ? ((majRoot + kMinRelShift) % 12) : majRoot;
                        meta.mode = (mi == 1) ? Mode::Minor : Mode::Major;
                        meta.key_text = std::string(pitchClassName(*meta.key_pc)) + (mi == 1 ? " minor" : " major");
                    }
                } else if (metaType == 0x2F) {
                    break;
                }
            } else if (status == 0xF0u || status == 0xF7u) {
                uint32_t sLen = 0;
                if (!readVlq(bytes.data(), trkEnd, pos, sLen) || sLen > trkEnd - pos) break;
                pos += sLen;
            } else {
                const uint8_t hi = status & 0xF0u;
                const size_t dataBytes = (hi == 0xC0u || hi == 0xD0u) ? 1u : 2u;
                if (dataBytes > trkEnd - pos) break;
                pos += dataBytes;
            }
        }
        pos = trkEnd;
    }
}

void dispatchMetadataParse(const ByteSource& src, std::string_view extLower, SongMetadata& meta) {
    if (!src.valid() || src.size() < 4) return;

    uint8_t magic[12]{};
    const size_t hdrRead = static_cast<size_t>(std::min<uint64_t>(src.size(), 12));
    if (!src.readAt(0, magic, hdrRead)) return;

    if (hdrRead >= 3 && std::memcmp(magic, "ID3", 3) == 0) {
        uint64_t id3Bytes = 0;
        tryParseId3v2At(src, 0, meta, &id3Bytes);
        if (id3Bytes > 0 && id3Bytes + 4 <= src.size()) {
            uint8_t after[4]{};
            if (src.readAt(id3Bytes, after, 4) && std::memcmp(after, "fLaC", 4) == 0) {
                parseFlac(src, id3Bytes, meta);
            }
        }
        parseId3v1Footer(src, meta);
        return;
    }

    if (hdrRead >= 4 && std::memcmp(magic, "fLaC", 4) == 0) {
        parseFlac(src, 0, meta);
        return;
    }

    if (hdrRead >= 4 && std::memcmp(magic, "OggS", 4) == 0) {
        parseOgg(src, meta);
        return;
    }

    if (hdrRead >= 12 && std::memcmp(magic, "RIFF", 4) == 0 && std::memcmp(magic + 8, "WAVE", 4) == 0) {
        parseWav(src, meta);
        return;
    }

    if (hdrRead >= 12 && std::memcmp(magic, "FORM", 4) == 0 &&
        (std::memcmp(magic + 8, "AIFF", 4) == 0 || std::memcmp(magic + 8, "AIFC", 4) == 0)) {
        parseAiff(src, meta);
        return;
    }

    if (hdrRead >= 8 && std::memcmp(magic + 4, "ftyp", 4) == 0) {
        parseMp4(src, meta);
        return;
    }

    if (hdrRead >= 4 && std::memcmp(magic, "MThd", 4) == 0) {
        parseMidiMeta(src, meta);
        return;
    }

    // Extension fallback (e.g. MP3 without ID3v2 header, only ID3v1 footer)
    if (extLower == ".mp3") {
        parseId3v1Footer(src, meta);
    }
}

bool containsWordBoundary(std::string_view haystack, std::string_view needle) {
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        const bool leftOk = (pos == 0 || haystack[pos - 1] == ' ');
        const size_t end = pos + needle.size();
        const bool rightOk = (end == haystack.size() || haystack[end] == ' ');
        if (leftOk && rightOk) return true;
        ++pos;
    }
    return false;
}

} // namespace

std::string AudioMetadataReader::lookupId3v1Genre(uint8_t code) {
    // Official ID3v1 (0..79) + Winamp extensions (80..191)
    static constexpr const char* kTable[] = {
        "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop",
        "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B", "Rap",
        "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska", "Death Metal", "Pranks",
        "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance",
        "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
        "Alt Rock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
        "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
        "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
        "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi",
        "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock",
        "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin", "Revival",
        "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock",
        "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera",
        "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam",
        "Club", "Tango", "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle",
        "Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass",
        "Club-House", "Hardcore", "Terror", "Indie", "BritPop", "Negerpunk", "Polsk Punk", "Beat",
        "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover", "Contemporary Christian", "Christian Rock", "Merengue", "Salsa",
        "Thrash Metal", "Anime", "JPop", "Synthpop", "Abstract", "Art Rock", "Baroque", "Bhangra",
        "Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM", "Eclectic", "Electro",
        "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient", "Industro-Goth",
        "Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz", "Post-Punk",
        "Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock", "World Music", "Neoclassical", "Audiobook",
        "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep", "Garage Rock", "Psybient"
    };
    constexpr size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);
    if (static_cast<size_t>(code) >= kTableSize) return {};
    const std::string canonical = canonicalizeGenre(kTable[code]);
    return canonical.empty() ? std::string(kTable[code]) : canonical;
}

std::string AudioMetadataReader::canonicalizeGenre(std::string_view rawGenre) {
    const auto parts = splitRawGenreParts(rawGenre);
    for (const auto& part : parts) {
        const std::string expanded = expandId3v1Token(part);
        if (expanded.empty()) continue;
        const std::string norm = normalizeMatchKey(expanded);
        if (isNoiseGenreToken(norm)) continue;
        if (const auto mapped = mapNormalizedGenre(norm); !mapped.empty()) {
            return mapped;
        }
        return titleCaseClean(expanded);
    }
    return {};
}

std::vector<std::string> AudioMetadataReader::extractGenreTags(std::string_view rawGenre) {
    std::vector<std::string> tags;
    for (const auto& part : splitRawGenreParts(rawGenre)) {
        const std::string expanded = expandId3v1Token(part);
        if (expanded.empty()) continue;
        const std::string norm = normalizeMatchKey(expanded);
        if (isNoiseGenreToken(norm)) continue;
        const std::string canon = canonicalizeGenre(expanded);
        const std::string tag = toLowerAscii(canon.empty() ? expanded : canon);
        if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end()) {
            tags.push_back(tag);
        }
    }
    return tags;
}

std::string AudioMetadataReader::detectGenreFromPath(const std::filesystem::path& path) {
    // Inspect filename stem first, then up to 3 enclosing parent folders
    std::vector<std::string> candidates;
    if (path.has_stem()) {
        candidates.push_back(normalizeMatchKey(pathToUtf8(path.stem())));
    }
    auto cur = path.parent_path();
    for (int depth = 0; depth < 3 && !cur.empty() && cur.has_filename(); ++depth) {
        candidates.push_back(normalizeMatchKey(pathToUtf8(cur.filename())));
        cur = cur.parent_path();
    }

    for (const auto& cand : candidates) {
        if (cand.empty()) continue;
        if (const auto exact = mapNormalizedGenre(cand); !exact.empty()) {
            return exact;
        }
        for (const auto& alias : kGenreAliases) {
            if (containsWordBoundary(cand, alias.pattern)) {
                return alias.canonical;
            }
        }
    }
    return {};
}

bool AudioMetadataReader::parseKeyString(std::string_view text, std::optional<int>& outPc, std::optional<Mode>& outMode) {
    const std::string s = trimAscii(text);
    if (s.empty()) return false;

    // Camelot wheel support ("8A" = Am, "9B" = G Maj, etc.)
    if (s.size() >= 2 && s.size() <= 3) {
        const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(s.back())));
        if (letter == 'A' || letter == 'B') {
            bool numOk = true;
            for (size_t i = 0; i + 1 < s.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(s[i]))) numOk = false;
            }
            if (numOk) {
                const int cam = std::stoi(s.substr(0, s.size() - 1));
                if (cam >= 1 && cam <= 12) {
                    // Camelot B: 1B=B(11), 2B=F#(6), 3B=Db(1), 4B=Ab(8), 5B=Eb(3), 6B=Bb(10), 7B=F(5), 8B=C(0)...
                    // i.e. major pc = ((cam - 8) * 7) mod 12; minor pc = (major - 3) mod 12
                    const int majPc = (((cam - 8) * 7) % 12 + 12) % 12;
                    outPc = (letter == 'A') ? ((majPc + 9) % 12) : majPc;
                    outMode = (letter == 'A') ? Mode::Minor : Mode::Major;
                    return true;
                }
            }
        }
    }

    // Parse leading pitch class (1 or 2 chars if #/b)
    size_t rootLen = 1;
    if (s.size() >= 2 && (s[1] == '#' || s[1] == 'b' || s[1] == 'B')) {
        if (s[1] == '#' || s[1] == 'b') rootLen = 2;
    }
    const auto pc = parsePitchClass(s.substr(0, rootLen));
    if (!pc) return false;

    const std::string rest = normalizeMatchKey(s.substr(rootLen));
    Mode m = Mode::Major;
    if (rest.empty() || rest == "maj" || rest == "major" || rest == "ionian") {
        m = Mode::Major;
    } else if (rest == "m" || rest == "min" || rest == "minor" || rest == "aeolian") {
        m = Mode::Minor;
    } else if (auto parsedMode = parseMode(rest)) {
        m = *parsedMode;
    } else {
        return false;
    }

    outPc = *pc;
    outMode = m;
    return true;
}

std::string AudioMetadataReader::inferGenreFromProfileHeuristic(const StyleProfile& p) {
    const double bpm = p.default_bpm;
    if (bpm >= 162.0 && bpm <= 190.0) {
        if (p.harmony.chromaticism >= 0.28 || p.rhythm.syncopation >= 0.55) {
            return "Neuro DnB";
        }
        return "Liquid DnB";
    }
    if (bpm >= 136.0 && bpm < 162.0) {
        if (p.harmony.mode == Mode::Phrygian || p.harmony.mode == Mode::Locrian ||
            (p.harmony.mode == Mode::Minor && p.voicing.extension_richness < 0.35 && p.rhythm.syncopation >= 0.42)) {
            return "Trap";
        }
        if (p.harmony.mode == Mode::Major || p.harmony.mode == Mode::Mixolydian || p.harmony.mode == Mode::Dorian) {
            return "Pop / Rock";
        }
        if (bpm <= 145.0 && p.rhythm.syncopation < 0.30) {
            return "Techno";
        }
        return "Pop / Rock";
    }
    if (bpm >= 129.0 && bpm < 136.0) {
        return (p.rhythm.swing >= 0.14 || p.rhythm.syncopation >= 0.38) ? "UK Garage" : "Techno";
    }
    if (bpm >= 118.0 && bpm < 129.0) {
        return (p.voicing.extension_richness >= 0.55) ? "Deep House" : "House";
    }
    if (bpm >= 100.0 && bpm < 118.0) {
        return (p.harmony.mode == Mode::Dorian || p.voicing.extension_richness >= 0.52)
                   ? "Funk / Disco"
                   : "Pop";
    }
    if (bpm >= 60.0 && bpm < 100.0) {
        if (p.harmony.chromaticism >= 0.30 && p.voicing.extension_richness >= 0.58) {
            return "Jazz";
        }
        if (p.voicing.extension_richness >= 0.56 && p.melody.legato_ratio >= 0.65) {
            return "R&B / Neo-Soul";
        }
        if (p.rhythm.swing >= 0.12 || p.rhythm.syncopation >= 0.35) {
            return "90s Hip-Hop";
        }
        if (p.rhythm.sustain_ratio >= 0.65) {
            return "Lo-Fi";
        }
        return "Hip-Hop";
    }
    return "Electronic";
}

std::string AudioMetadataReader::sanitizeGenreSlug(std::string_view genre) {
    std::string out;
    bool prevUnderscore = false;
    for (unsigned char c : genre) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            prevUnderscore = false;
        } else if (!prevUnderscore && !out.empty()) {
            out.push_back('_');
            prevUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "genre" : out;
}

SongMetadata AudioMetadataReader::readFile(const std::filesystem::path& path) {
    SongMetadata meta;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return meta;

    const std::string extLower = toLowerAscii(pathToUtf8(path.extension()));
    ByteSource src(path);
    dispatchMetadataParse(src, extLower, meta);

    if (!meta.hasGenre()) {
        if (const auto pathGenre = detectGenreFromPath(path); !pathGenre.empty()) {
            meta.raw_genre = pathGenre;
            meta.canonical_genre = pathGenre;
            meta.genre_source = "path";
            const std::string low = toLowerAscii(pathGenre);
            if (std::find(meta.tags.begin(), meta.tags.end(), low) == meta.tags.end()) {
                meta.tags.push_back(low);
            }
        }
    }
    if (meta.title.empty() && path.has_stem()) {
        meta.title = pathToUtf8(path.stem());
    }
    return meta;
}

SongMetadata AudioMetadataReader::readFromMemory(const uint8_t* data, size_t size, std::string_view extLower) {
    SongMetadata meta;
    if (data == nullptr || size == 0) return meta;
    ByteSource src(data, size);
    dispatchMetadataParse(src, toLowerAscii(extLower), meta);
    return meta;
}

} // namespace style_scaler
