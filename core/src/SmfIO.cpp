#include "style_scaler/SmfIO.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <tuple>

namespace style_scaler {

namespace {

void writeBe16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void writeBe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void writeVlq(std::vector<uint8_t>& buf, uint32_t value) {
    uint8_t bytes[5]{};
    int count = 0;
    bytes[count++] = static_cast<uint8_t>(value & 0x7F);
    while ((value >>= 7) > 0) {
        bytes[count++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
    }
    for (int i = count - 1; i >= 0; --i) {
        buf.push_back(bytes[i]);
    }
}

void appendTrackChunk(std::vector<uint8_t>& fileBytes, const std::vector<uint8_t>& trackBody) {
    fileBytes.push_back('M');
    fileBytes.push_back('T');
    fileBytes.push_back('r');
    fileBytes.push_back('k');
    writeBe32(fileBytes, static_cast<uint32_t>(trackBody.size()));
    fileBytes.insert(fileBytes.end(), trackBody.begin(), trackBody.end());
}

uint16_t readBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool readVlq(const uint8_t* data, size_t size, size_t& offset, uint32_t& outValue) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        if (offset >= size) return false;
        const uint8_t b = data[offset++];
        v = (v << 7) | static_cast<uint32_t>(b & 0x7F);
        if ((b & 0x80) == 0) {
            outValue = v;
            return true;
        }
    }
    return false;
}

struct RawMidiMsg {
    uint64_t tick = 0;
    int order = 0; // 0=note-off, 1=note-on
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

std::vector<uint8_t> buildNoteTrack(const std::string& name, const std::vector<NoteEvent>& events, uint16_t ppq) {
    std::vector<uint8_t> body;

    // Track Name Meta Event (0xFF 0x03)
    if (!name.empty()) {
        writeVlq(body, 0);
        body.push_back(0xFF);
        body.push_back(0x03);
        writeVlq(body, static_cast<uint32_t>(name.size()));
        body.insert(body.end(), name.begin(), name.end());
    }

    std::vector<RawMidiMsg> msgs;
    msgs.reserve(events.size() * 2);

    for (const auto& ev : events) {
        const int ch = ev.is_drum ? 9 : std::clamp(ev.channel - 1, 0, 15);
        const uint8_t pitch = static_cast<uint8_t>(std::clamp(ev.pitch, 0, 127));
        const uint8_t vel = static_cast<uint8_t>(std::clamp(ev.velocity, 1, 127));
        const uint64_t onTick = static_cast<uint64_t>(std::max<long long>(0LL, std::llround(ev.onset_beats * ppq)));
        const uint64_t durTicks = std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(ev.duration_beats * ppq)));
        const uint64_t offTick = onTick + durTicks;

        msgs.push_back(RawMidiMsg{onTick, 1, static_cast<uint8_t>(0x90 | ch), pitch, vel});
        msgs.push_back(RawMidiMsg{offTick, 0, static_cast<uint8_t>(0x80 | ch), pitch, 0});
    }

    // Sort chronologically; Note-Off before Note-On at identical tick to avoid choking retriggered notes
    std::sort(msgs.begin(), msgs.end(), [](const RawMidiMsg& a, const RawMidiMsg& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.order != b.order) return a.order < b.order;
        if (a.status != b.status) return a.status < b.status;
        return a.data1 < b.data1;
    });

    // Track overlapping identical (channel, pitch) notes so an early Note-Off doesn't prematurely silence a retrigger
    std::array<std::array<int, 128>, 16> activeCount{};
    uint64_t prevTick = 0;

    for (const auto& m : msgs) {
        const int ch = m.status & 0x0F;
        const bool isNoteOn = (m.status & 0xF0) == 0x90;
        if (isNoteOn) {
            if (activeCount[static_cast<size_t>(ch)][m.data1] > 0) {
                // Close sounding instance at current delta before re-attack
                const uint32_t delta = static_cast<uint32_t>(m.tick - prevTick);
                writeVlq(body, delta);
                body.push_back(static_cast<uint8_t>(0x80 | ch));
                body.push_back(m.data1);
                body.push_back(0);
                prevTick = m.tick;
                activeCount[static_cast<size_t>(ch)][m.data1] = 0;
            }
            activeCount[static_cast<size_t>(ch)][m.data1] += 1;
        } else {
            if (activeCount[static_cast<size_t>(ch)][m.data1] <= 0) {
                continue;
            }
            activeCount[static_cast<size_t>(ch)][m.data1] -= 1;
            if (activeCount[static_cast<size_t>(ch)][m.data1] > 0) {
                continue;
            }
        }

        const uint32_t delta = static_cast<uint32_t>(m.tick - prevTick);
        writeVlq(body, delta);
        body.push_back(m.status);
        body.push_back(m.data1);
        body.push_back(m.data2);
        prevTick = m.tick;
    }

    // End of Track
    writeVlq(body, 0);
    body.push_back(0xFF);
    body.push_back(0x2F);
    body.push_back(0x00);

    return body;
}

} // namespace

std::expected<void, std::string> SmfWriter::writeType1(
    const std::filesystem::path& path,
    double bpm,
    int timeSigNum,
    int timeSigDen,
    const std::vector<NoteEvent>& events,
    const std::array<std::string, 3>& channelTrackNames) {

    const double clampedBpm = std::clamp(bpm, 20.0, 350.0);
    const uint32_t usPerQuarter = static_cast<uint32_t>(std::llround(60000000.0 / clampedBpm));
    const int num = std::max(1, timeSigNum);
    int denPow = 0;
    int denVal = std::max(1, timeSigDen);
    while ((1 << denPow) < denVal && denPow < 6) {
        ++denPow;
    }

    // Group events by channel (1, 2, 3, and any extra channels)
    std::map<int, std::vector<NoteEvent>> perChannel;
    for (const auto& ev : events) {
        const int ch = ev.is_drum ? 10 : std::clamp(ev.channel, 1, 16);
        perChannel[ch].push_back(ev);
    }

    std::vector<std::pair<std::string, std::vector<NoteEvent>>> tracks;
    for (const auto& [ch, chEvents] : perChannel) {
        std::string name;
        if (ch >= 1 && ch <= 3) {
            name = channelTrackNames[static_cast<size_t>(ch - 1)];
        } else if (ch == 10) {
            name = "Drums";
        } else {
            name = "Channel " + std::to_string(ch);
        }
        tracks.emplace_back(std::move(name), chEvents);
    }

    // Conductor track (Track 0)
    std::vector<uint8_t> conductor;
    // Time Signature (0xFF 0x58 0x04 nn dd cc bb)
    writeVlq(conductor, 0);
    conductor.push_back(0xFF);
    conductor.push_back(0x58);
    conductor.push_back(0x04);
    conductor.push_back(static_cast<uint8_t>(num));
    conductor.push_back(static_cast<uint8_t>(denPow));
    conductor.push_back(24);
    conductor.push_back(8);

    // Tempo (0xFF 0x51 0x03 tttttt)
    writeVlq(conductor, 0);
    conductor.push_back(0xFF);
    conductor.push_back(0x51);
    conductor.push_back(0x03);
    conductor.push_back(static_cast<uint8_t>((usPerQuarter >> 16) & 0xFF));
    conductor.push_back(static_cast<uint8_t>((usPerQuarter >> 8) & 0xFF));
    conductor.push_back(static_cast<uint8_t>(usPerQuarter & 0xFF));

    // End of Track
    writeVlq(conductor, 0);
    conductor.push_back(0xFF);
    conductor.push_back(0x2F);
    conductor.push_back(0x00);

    std::vector<uint8_t> fileBytes;
    fileBytes.reserve(1024 + events.size() * 12);

    // Header Chunk: MThd
    fileBytes.push_back('M');
    fileBytes.push_back('T');
    fileBytes.push_back('h');
    fileBytes.push_back('d');
    writeBe32(fileBytes, 6);
    writeBe16(fileBytes, 1); // Format 1
    writeBe16(fileBytes, static_cast<uint16_t>(1 + tracks.size()));
    writeBe16(fileBytes, kDefaultPpq);

    appendTrackChunk(fileBytes, conductor);
    for (const auto& [name, chEvents] : tracks) {
        appendTrackChunk(fileBytes, buildNoteTrack(name, chEvents, kDefaultPpq));
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream ofs(path, std::ios::binary);
    auto u8Path = [](const std::filesystem::path& p) {
        const auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    };
    if (!ofs) {
        return std::unexpected("Unable to open MIDI file for writing: " + u8Path(path));
    }
    ofs.write(reinterpret_cast<const char*>(fileBytes.data()), static_cast<std::streamsize>(fileBytes.size()));
    if (!ofs) {
        return std::unexpected("Failed writing MIDI file: " + u8Path(path));
    }
    return {};
}

std::expected<void, std::string> SmfWriter::writeFilteredType1(
    const std::filesystem::path& path,
    double bpm,
    int timeSigNum,
    int timeSigDen,
    const std::vector<NoteEvent>& events,
    int channelFilter) {
    if (channelFilter <= 0) {
        return writeType1(path, bpm, timeSigNum, timeSigDen, events);
    }
    std::vector<NoteEvent> filtered;
    filtered.reserve(events.size());
    for (const auto& ev : events) {
        if (ev.channel == channelFilter) {
            filtered.push_back(ev);
        }
    }
    return writeType1(path, bpm, timeSigNum, timeSigDen, filtered);
}

std::expected<SmfReadResult, std::string> SmfReader::readFile(const std::filesystem::path& path) {
    auto u8Path = [](const std::filesystem::path& p) {
        const auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    };
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return std::unexpected("Unable to open MIDI file: " + u8Path(path));
    }
    const auto sz = ifs.tellg();
    if (sz <= 0) {
        return std::unexpected("Empty MIDI file: " + u8Path(path));
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    if (!ifs.read(reinterpret_cast<char*>(data.data()), sz)) {
        return std::unexpected("Failed reading MIDI file: " + u8Path(path));
    }
    return readMemory(data.data(), data.size());
}

std::expected<SmfReadResult, std::string> SmfReader::readMemory(const uint8_t* data, size_t size) {
    if (size < 14 || data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd') {
        return std::unexpected("Invalid MIDI header (missing MThd)");
    }

    const uint32_t hdrLen = readBe32(data + 4);
    if (hdrLen < 6 || 8 + hdrLen > size) {
        return std::unexpected("Truncated MIDI header");
    }

    const uint16_t numTracks = readBe16(data + 10);
    const uint16_t division = readBe16(data + 12);
    if ((division & 0x8000) != 0 || division == 0) {
        return std::unexpected("SMPTE timecode MIDI files are not supported");
    }
    const double ppq = static_cast<double>(division);

    SmfReadResult result;
    bool foundTempo = false;
    bool foundTimeSig = false;

    size_t pos = 8 + hdrLen;
    struct ActiveNote {
        uint64_t onTick = 0;
        int velocity = 100;
        int program = 0;
    };

    for (uint16_t trk = 0; trk < numTracks && pos + 8 <= size; ++trk) {
        if (data[pos] != 'M' || data[pos + 1] != 'T' || data[pos + 2] != 'r' || data[pos + 3] != 'k') {
            return std::unexpected("Expected MTrk chunk header");
        }
        const uint32_t trkLen = readBe32(data + pos + 4);
        pos += 8;
        if (pos + trkLen > size) {
            return std::unexpected("Truncated MTrk chunk");
        }

        const size_t trkEnd = pos + trkLen;
        uint64_t tick = 0;
        uint8_t runningStatus = 0;
        std::array<int, 16> channelPrograms{};
        std::array<std::array<std::vector<ActiveNote>, 128>, 16> activeNotes;

        while (pos < trkEnd) {
            uint32_t delta = 0;
            if (!readVlq(data, trkEnd, pos, delta)) break;
            tick += delta;
            if (pos >= trkEnd) break;

            uint8_t status = data[pos];
            if (status < 0x80) {
                if (runningStatus == 0) break;
                status = runningStatus;
            } else {
                ++pos;
                if (status < 0xF0) {
                    runningStatus = status;
                }
            }

            if (status == 0xFF) {
                // Meta event
                if (pos >= trkEnd) break;
                const uint8_t metaType = data[pos++];
                uint32_t metaLen = 0;
                if (!readVlq(data, trkEnd, pos, metaLen)) break;
                if (pos + metaLen > trkEnd) break;

                if (metaType == 0x51 && metaLen == 3 && !foundTempo) {
                    const uint32_t usPerQuarter =
                        (static_cast<uint32_t>(data[pos]) << 16) |
                        (static_cast<uint32_t>(data[pos + 1]) << 8) |
                        static_cast<uint32_t>(data[pos + 2]);
                    if (usPerQuarter > 0) {
                        result.bpm = 60000000.0 / static_cast<double>(usPerQuarter);
                        foundTempo = true;
                    }
                } else if (metaType == 0x58 && metaLen >= 2 && !foundTimeSig) {
                    result.time_sig_num = std::max(1, static_cast<int>(data[pos]));
                    result.time_sig_den = 1 << std::clamp(static_cast<int>(data[pos + 1]), 0, 6);
                    foundTimeSig = true;
                }

                pos += metaLen;
                if (metaType == 0x2F) break;
                continue;
            }

            if (status == 0xF0 || status == 0xF7) {
                uint32_t syxLen = 0;
                if (!readVlq(data, trkEnd, pos, syxLen)) break;
                pos = std::min(trkEnd, pos + syxLen);
                continue;
            }

            const uint8_t msgType = status & 0xF0;
            const int ch = status & 0x0F;

            // Single data-byte messages: 0xC0 (Program Change), 0xD0 (Channel Pressure)
            if (msgType == 0xC0 || msgType == 0xD0) {
                if (pos >= trkEnd) break;
                const uint8_t d1 = data[pos++];
                if (msgType == 0xC0) {
                    channelPrograms[static_cast<size_t>(ch)] = d1;
                }
                continue;
            }

            // Two data-byte messages
            if (pos + 1 >= trkEnd) break;
            const uint8_t d1 = data[pos++];
            const uint8_t d2 = data[pos++];

            if (msgType == 0x90 && d2 > 0) {
                activeNotes[static_cast<size_t>(ch)][d1 & 0x7F].push_back(
                    ActiveNote{tick, static_cast<int>(d2), channelPrograms[static_cast<size_t>(ch)]});
            } else if (msgType == 0x80 || (msgType == 0x90 && d2 == 0)) {
                auto& stack = activeNotes[static_cast<size_t>(ch)][d1 & 0x7F];
                if (!stack.empty()) {
                    const ActiveNote an = stack.front();
                    stack.erase(stack.begin());
                    const double onsetBeats = static_cast<double>(an.onTick) / ppq;
                    const double durBeats = std::max(1.0 / ppq, static_cast<double>(tick - an.onTick) / ppq);
                    NoteEvent ev;
                    ev.onset_beats = onsetBeats;
                    ev.duration_beats = durBeats;
                    ev.pitch = d1 & 0x7F;
                    ev.velocity = an.velocity;
                    ev.channel = ch + 1;
                    ev.is_drum = (ch == 9);
                    ev.program = an.program;
                    result.notes.push_back(ev);
                }
            }
        }

        // Close any lingering un-terminated notes at track end
        for (int ch = 0; ch < 16; ++ch) {
            for (int p = 0; p < 128; ++p) {
                for (const auto& an : activeNotes[static_cast<size_t>(ch)][static_cast<size_t>(p)]) {
                    NoteEvent ev;
                    ev.onset_beats = static_cast<double>(an.onTick) / ppq;
                    ev.duration_beats = std::max(0.25, static_cast<double>(tick - an.onTick) / ppq);
                    ev.pitch = p;
                    ev.velocity = an.velocity;
                    ev.channel = ch + 1;
                    ev.is_drum = (ch == 9);
                    ev.program = an.program;
                    result.notes.push_back(ev);
                }
            }
        }

        pos = trkEnd;
    }

    std::sort(result.notes.begin(), result.notes.end(), [](const NoteEvent& a, const NoteEvent& b) {
        if (a.onset_beats != b.onset_beats) return a.onset_beats < b.onset_beats;
        if (a.channel != b.channel) return a.channel < b.channel;
        return a.pitch < b.pitch;
    });

    return result;
}

} // namespace style_scaler
