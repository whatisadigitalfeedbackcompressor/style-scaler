#pragma once

#include "style_scaler/StyleProfile.h"
#include "style_scaler/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace style_scaler {

struct SongMetadata {
    std::string title;
    std::string artist;
    std::string album;
    std::string raw_genre;
    std::string canonical_genre;        // Canonicalized genre from tag, path, or heuristic
    std::vector<std::string> tags;      // Lowercase deduplicated genre/style tags
    std::string genre_source = "none";  // "id3v2", "id3v1", "vorbis", "mp4", "riff_info", "aiff", "midi_meta", "path", "heuristic", "none"
    double bpm = 0.0;                   // Embedded tempo (0.0 if absent)
    std::string key_text;               // Raw embedded key string (e.g. "Cm", "8A")
    std::optional<int> key_pc;
    std::optional<Mode> mode;
    std::string comment;

    bool hasGenre() const noexcept { return !canonical_genre.empty(); }
};

class AudioMetadataReader {
public:
    // Reads file header/trailer metadata using bounded streaming I/O (safe on malformed/huge files).
    static SongMetadata readFile(const std::filesystem::path& path);

    // Parses metadata directly from an in-memory buffer given an extension hint (e.g. ".mp3", ".wav").
    static SongMetadata readFromMemory(const uint8_t* data, size_t size, std::string_view extLower = {});

    // Normalizes raw genre strings: decodes "(13)" ID3v1 prefixes, splits multi-tag delimiters,
    // maps known aliases to canonical StyleScaler genres, or cleanly title-cases unknown genres.
    static std::string canonicalizeGenre(std::string_view rawGenre);

    // Splits raw genre metadata into normalized lowercase tags.
    static std::vector<std::string> extractGenreTags(std::string_view rawGenre);

    // Inspects parent folders and filename stem for canonical genre keywords.
    static std::string detectGenreFromPath(const std::filesystem::path& path);

    // Parses key strings like "Cm", "F# minor", "BbMaj", or Camelot notation "8A" / "9B".
    static bool parseKeyString(std::string_view text, std::optional<int>& outPc, std::optional<Mode>& outMode);

    // Maps an ID3v1 0..191 numeric genre byte to a canonical StyleScaler genre name.
    static std::string lookupId3v1Genre(uint8_t code);

    // Infers a genre label from musical StyleProfile features when no tag or path hint exists.
    static std::string inferGenreFromProfileHeuristic(const StyleProfile& profile);

    // Produces a filesystem-safe lowercase underscore slug from any genre string.
    static std::string sanitizeGenreSlug(std::string_view genre);
};

} // namespace style_scaler
