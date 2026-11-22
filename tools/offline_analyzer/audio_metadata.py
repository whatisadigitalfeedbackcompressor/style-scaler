#!/usr/bin/env python3
"""
Zero-dependency audio & MIDI metadata reader and canonical genre normalizer for
StyleScaler's offline analysis pipeline (mirrors core/src/AudioMetadata.cpp).

Supports ID3v2 (.mp3), ID3v1, FLAC Vorbis comments (.flac), Ogg/Opus (.ogg/.opus),
WAV RIFF LIST/INFO + id3  (.wav), AIFF NAME/AUTH/ANNO/ID3 (.aif/.aiff),
MP4/M4A ilst atoms (.m4a/.mp4), and SMF MIDI meta events (.mid/.midi).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import re
import struct
from typing import Dict, List, Optional, Tuple


ID3V1_GENRES: Tuple[str, ...] = (
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
    "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep", "Garage Rock", "Psybient",
)

NOISE_TOKENS = {
    "", "unknown", "none", "other", "misc", "miscellaneous", "default",
    "genre", "n a", "na", "unset",
}

GENRE_ALIASES: Tuple[Tuple[str, str], ...] = (
    ("90s hip hop", "90s Hip-Hop"),
    ("1990s hip hop", "90s Hip-Hop"),
    ("boom bap", "90s Hip-Hop"),
    ("boombap", "90s Hip-Hop"),
    ("old school hip hop", "90s Hip-Hop"),
    ("east coast hip hop", "90s Hip-Hop"),
    ("golden age hip hop", "90s Hip-Hop"),
    ("liquid dnb", "Liquid DnB"),
    ("liquid drum and bass", "Liquid DnB"),
    ("liquid drum n bass", "Liquid DnB"),
    ("liquid funk", "Liquid DnB"),
    ("neuro dnb", "Neuro DnB"),
    ("neurofunk", "Neuro DnB"),
    ("neuro funk", "Neuro DnB"),
    ("techstep", "Neuro DnB"),
    ("drum and bass", "Drum & Bass"),
    ("drum n bass", "Drum & Bass"),
    ("dnb", "Drum & Bass"),
    ("d and b", "Drum & Bass"),
    ("jungle", "Drum & Bass"),
    ("uk garage", "UK Garage"),
    ("ukg", "UK Garage"),
    ("2 step", "UK Garage"),
    ("two step", "UK Garage"),
    ("speed garage", "UK Garage"),
    ("future garage", "UK Garage"),
    ("dark trap", "Trap"),
    ("atlanta trap", "Trap"),
    ("uk drill", "Trap"),
    ("drill", "Trap"),
    ("phonk", "Trap"),
    ("trap", "Trap"),
    ("lofi hip hop", "Lo-Fi"),
    ("lo fi hip hop", "Lo-Fi"),
    ("lo fi", "Lo-Fi"),
    ("lofi", "Lo-Fi"),
    ("chillhop", "Lo-Fi"),
    ("hip hop", "Hip-Hop"),
    ("hiphop", "Hip-Hop"),
    ("gangsta rap", "Hip-Hop"),
    ("rap", "Hip-Hop"),
    ("neo soul", "R&B / Neo-Soul"),
    ("contemporary r and b", "R&B / Neo-Soul"),
    ("r and b", "R&B / Neo-Soul"),
    ("rhythm and blues", "R&B / Neo-Soul"),
    ("rnb", "R&B / Neo-Soul"),
    ("soul", "R&B / Neo-Soul"),
    ("tech house", "Tech House"),
    ("deep house", "Deep House"),
    ("progressive house", "Progressive House"),
    ("afro house", "Afro House"),
    ("bass house", "Bass House"),
    ("electro house", "Electro House"),
    ("house", "House"),
    ("melodic techno", "Melodic Techno"),
    ("acid techno", "Techno"),
    ("hard techno", "Techno"),
    ("minimal techno", "Techno"),
    ("techno", "Techno"),
    ("psytrance", "Trance"),
    ("uplifting trance", "Trance"),
    ("trance", "Trance"),
    ("brostep", "Dubstep"),
    ("riddim", "Dubstep"),
    ("dubstep", "Dubstep"),
    ("synthwave", "Synthwave"),
    ("retrowave", "Synthwave"),
    ("outrun", "Synthwave"),
    ("vaporwave", "Synthwave"),
    ("trip hop", "Ambient / Downtempo"),
    ("downtempo", "Ambient / Downtempo"),
    ("ambient", "Ambient / Downtempo"),
    ("chillout", "Ambient / Downtempo"),
    ("idm", "Ambient / Downtempo"),
    ("afrobeats", "Afrobeats / Amapiano"),
    ("afrobeat", "Afrobeats / Amapiano"),
    ("amapiano", "Afrobeats / Amapiano"),
    ("reggaeton", "Reggaeton / Dancehall"),
    ("dancehall", "Reggaeton / Dancehall"),
    ("latin", "Reggaeton / Dancehall"),
    ("synthpop", "Pop"),
    ("synth pop", "Pop"),
    ("electropop", "Pop"),
    ("dance pop", "Pop"),
    ("indie pop", "Pop"),
    ("pop", "Pop"),
    ("nu disco", "Funk / Disco"),
    ("disco", "Funk / Disco"),
    ("funk", "Funk / Disco"),
    ("boogie", "Funk / Disco"),
    ("jazz fusion", "Jazz"),
    ("nu jazz", "Jazz"),
    ("bebop", "Jazz"),
    ("jazz", "Jazz"),
    ("indie rock", "Indie Rock"),
    ("alt rock", "Alternative Rock"),
    ("alternative rock", "Alternative Rock"),
    ("alternative", "Alternative Rock"),
    ("hard rock", "Rock"),
    ("rock", "Rock"),
    ("metal", "Metal"),
    ("heavy metal", "Metal"),
    ("punk rock", "Punk"),
    ("punk", "Punk"),
    ("classical", "Classical"),
    ("soundtrack", "Cinematic / Score"),
    ("cinematic", "Cinematic / Score"),
    ("electronic", "Electronic"),
    ("electronica", "Electronic"),
    ("edm", "Electronic"),
    ("dance", "Dance"),
)


@dataclass
class SongMetadata:
    title: str = ""
    artist: str = ""
    album: str = ""
    raw_genre: str = ""
    canonical_genre: str = ""
    tags: List[str] = field(default_factory=list)
    genre_source: str = "none"
    bpm: float = 0.0

    @property
    def has_genre(self) -> bool:
        return bool(self.canonical_genre)


def _norm_key(s: str) -> str:
    s = s.replace("&", " and ").replace("'", "").replace("`", "")
    s = re.sub(r"[^a-zA-Z0-9]+", " ", s).strip().lower()
    return s


def sanitize_genre_slug(genre: str) -> str:
    out = re.sub(r"[^a-z0-9]+", "_", genre.lower()).strip("_")
    return out or "genre"


def _expand_id3v1_token(tok: str) -> str:
    tok = tok.strip("\x00 \t\r\n")
    if not tok:
        return ""
    if tok.isdigit():
        idx = int(tok)
        return ID3V1_GENRES[idx] if 0 <= idx < len(ID3V1_GENRES) else ""
    m = re.match(r"^\(([^)]+)\)(.*)$", tok)
    if m:
        inside, rest = m.group(1), m.group(2).strip()
        if rest:
            return rest
        if inside == "RX":
            return "Remix"
        if inside == "CR":
            return "Cover"
        if inside.isdigit():
            idx = int(inside)
            return ID3V1_GENRES[idx] if 0 <= idx < len(ID3V1_GENRES) else ""
    return tok


def _split_parts(raw: str) -> List[str]:
    parts = re.split(r"[\x00;,/|]+|(?<=\))(?=\()", raw)
    return [p.strip("\x00 \t\r\n") for p in parts if p.strip("\x00 \t\r\n")]


def canonicalize_genre(raw: str) -> str:
    for part in _split_parts(raw):
        exp = _expand_id3v1_token(part)
        norm = _norm_key(exp)
        if norm in NOISE_TOKENS:
            continue
        for pat, canon in GENRE_ALIASES:
            if norm == pat:
                return canon
        return " ".join(w[:1].upper() + w[1:] for w in exp.split())
    return ""


def extract_genre_tags(raw: str) -> List[str]:
    tags: List[str] = []
    for part in _split_parts(raw):
        exp = _expand_id3v1_token(part)
        norm = _norm_key(exp)
        if norm in NOISE_TOKENS:
            continue
        canon = canonicalize_genre(exp)
        t = (canon or exp).lower()
        if t and t not in tags:
            tags.append(t)
    return tags


def detect_genre_from_path(path: Path) -> str:
    cands = [_norm_key(path.stem)]
    cur = path.parent
    for _ in range(3):
        if cur.name:
            cands.append(_norm_key(cur.name))
        if cur.parent == cur:
            break
        cur = cur.parent

    for cand in cands:
        if not cand:
            continue
        for pat, canon in GENRE_ALIASES:
            if cand == pat or re.search(rf"(?:^|\s){re.escape(pat)}(?:$|\s)", cand):
                return canon
    return ""


def _decode_synchsafe(b: bytes) -> Optional[int]:
    if len(b) < 4 or any(x & 0x80 for x in b[:4]):
        return None
    return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]


def _decode_id3_text(enc: int, payload: bytes) -> str:
    if not payload:
        return ""
    if enc == 0:
        parts = [p.decode("latin-1", errors="ignore").strip() for p in payload.split(b"\x00")]
    elif enc == 3:
        parts = [p.decode("utf-8", errors="ignore").strip() for p in payload.split(b"\x00")]
    elif enc in (1, 2):
        codec = "utf-16" if enc == 1 else "utf-16-be"
        text = payload.decode(codec, errors="ignore")
        parts = [p.strip() for p in text.split("\x00")]
    else:
        return ""
    return "; ".join(p for p in parts if p)


def _merge_genre(meta: SongMetadata, val: str, source: str) -> None:
    val = val.strip("\x00 \t\r\n")
    if not val:
        return
    for t in extract_genre_tags(val):
        if t not in meta.tags:
            meta.tags.append(t)
    canon = canonicalize_genre(val)
    if canon and not meta.canonical_genre:
        meta.raw_genre = val
        meta.canonical_genre = canon
        meta.genre_source = source


def _apply_kv(meta: SongMetadata, key: str, val: str, source: str) -> None:
    k = _norm_key(key)
    v = val.strip("\x00 \t\r\n")
    if not k or not v:
        return
    if k in ("genre", "contentgroup", "style", "subgenre", "tags"):
        _merge_genre(meta, v, source)
    elif k in ("title", "track") and not meta.title:
        meta.title = v
    elif k in ("artist", "albumartist", "performer") and not meta.artist:
        meta.artist = v
    elif k == "album" and not meta.album:
        meta.album = v
    elif k in ("bpm", "tempo", "tbpm") and meta.bpm <= 0.0:
        try:
            b = float(v)
            if 25.0 <= b <= 320.0:
                meta.bpm = b
        except ValueError:
            pass


def _parse_id3v2_bytes(buf: bytes, meta: SongMetadata) -> None:
    if len(buf) < 10 or buf[:3] != b"ID3":
        return
    ver = buf[3]
    if ver not in (2, 3, 4):
        return
    size = _decode_synchsafe(buf[6:10])
    if size is None:
        return
    end = min(len(buf), 10 + size)
    pos = 10
    flags = buf[5]
    if (flags & 0x40) and ver >= 3 and pos + 4 <= end:
        if ver == 4:
            ext = _decode_synchsafe(buf[pos:pos + 4]) or 4
            pos += ext
        else:
            ext = struct.unpack(">I", buf[pos:pos + 4])[0]
            pos += 4 + ext

    while pos < end:
        if buf[pos] == 0:
            break
        if ver == 2:
            if pos + 6 > end:
                break
            fid = buf[pos:pos + 3].decode("latin-1", errors="ignore")
            fsize = (buf[pos + 3] << 16) | (buf[pos + 4] << 8) | buf[pos + 5]
            hdr = 6
        else:
            if pos + 10 > end:
                break
            fid = buf[pos:pos + 4].decode("latin-1", errors="ignore")
            fsize = _decode_synchsafe(buf[pos + 4:pos + 8]) if ver == 4 else struct.unpack(">I", buf[pos + 4:pos + 8])[0]
            if fsize is None:
                break
            hdr = 10
        if fsize == 0 or pos + hdr + fsize > end:
            break
        payload = buf[pos + hdr:pos + hdr + fsize]
        pos += hdr + fsize
        if not payload:
            continue
        enc = payload[0]
        body = payload[1:]
        if fid in ("TCON", "TCO"):
            _merge_genre(meta, _decode_id3_text(enc, body), "id3v2")
        elif fid in ("TIT2", "TT2") and not meta.title:
            meta.title = _decode_id3_text(enc, body)
        elif fid in ("TPE1", "TP1") and not meta.artist:
            meta.artist = _decode_id3_text(enc, body)
        elif fid in ("TALB", "TAL") and not meta.album:
            meta.album = _decode_id3_text(enc, body)
        elif fid in ("TBPM", "TBP"):
            _apply_kv(meta, "bpm", _decode_id3_text(enc, body), "id3v2")
        elif fid in ("TXXX", "TXX"):
            joined = _decode_id3_text(enc, body)
            parts = [p.strip() for p in joined.split("; ", 1)]
            if len(parts) == 2:
                _apply_kv(meta, parts[0], parts[1], "id3v2")


def _parse_vorbis_comment(payload: bytes, meta: SongMetadata) -> None:
    if len(payload) < 8:
        return
    pos = 0
    vlen = struct.unpack("<I", payload[pos:pos + 4])[0]
    pos += 4 + vlen
    if pos + 4 > len(payload):
        return
    cnt = min(4096, struct.unpack("<I", payload[pos:pos + 4])[0])
    pos += 4
    for _ in range(cnt):
        if pos + 4 > len(payload):
            break
        ilen = struct.unpack("<I", payload[pos:pos + 4])[0]
        pos += 4
        if pos + ilen > len(payload):
            break
        kv = payload[pos:pos + ilen].decode("utf-8", errors="ignore")
        pos += ilen
        if "=" in kv:
            k, v = kv.split("=", 1)
            _apply_kv(meta, k, v, "vorbis")


def read_audio_metadata(path: Path) -> SongMetadata:
    meta = SongMetadata()
    if not path.is_file():
        return meta

    fsize = path.stat().st_size
    ext = path.suffix.lower()

    with path.open("rb") as f:
        hdr = f.read(16)
        if hdr[:3] == b"ID3":
            tag_sz = _decode_synchsafe(hdr[6:10]) or 0
            f.seek(0)
            _parse_id3v2_bytes(f.read(min(fsize, 10 + tag_sz, 8 * 1024 * 1024)), meta)
        elif hdr[:4] == b"fLaC":
            f.seek(4)
            while f.tell() + 4 <= fsize:
                bh = f.read(4)
                if len(bh) < 4:
                    break
                is_last = (bh[0] & 0x80) != 0
                btype = bh[0] & 0x7F
                blen = (bh[1] << 16) | (bh[2] << 8) | bh[3]
                if btype == 4 and blen <= 8 * 1024 * 1024:
                    _parse_vorbis_comment(f.read(blen), meta)
                else:
                    f.seek(blen, 1)
                if is_last:
                    break
        elif hdr[:4] == b"OggS":
            f.seek(0)
            blob = f.read(min(fsize, 256 * 1024))
            for sig in (b"\x03vorbis", b"OpusTags"):
                idx = blob.find(sig)
                if idx != -1:
                    _parse_vorbis_comment(blob[idx + len(sig):], meta)
                    break
        elif hdr[:4] == b"RIFF" and hdr[8:12] == b"WAVE":
            end_lim = min(fsize, 8 + struct.unpack("<I", hdr[4:8])[0])
            pos = 12
            while pos + 8 <= end_lim:
                f.seek(pos)
                ch = f.read(8)
                if len(ch) < 8:
                    break
                cid, csz = ch[:4], struct.unpack("<I", ch[4:8])[0]
                if csz > end_lim - (pos + 8):
                    break
                if cid == b"LIST" and csz >= 4:
                    ltype = f.read(4)
                    if ltype == b"INFO":
                        sub_end = pos + 8 + csz
                        sp = pos + 12
                        while sp + 8 <= sub_end:
                            f.seek(sp)
                            sch = f.read(8)
                            if len(sch) < 8:
                                break
                            scid, scsz = sch[:4], struct.unpack("<I", sch[4:8])[0]
                            if scsz > sub_end - (sp + 8):
                                break
                            if scsz <= 65536:
                                val = f.read(scsz).decode("latin-1", errors="ignore").rstrip("\x00")
                                if scid == b"IGNR":
                                    _apply_kv(meta, "genre", val, "riff_info")
                                elif scid == b"INAM":
                                    _apply_kv(meta, "title", val, "riff_info")
                                elif scid == b"IART":
                                    _apply_kv(meta, "artist", val, "riff_info")
                                elif scid == b"IPRD":
                                    _apply_kv(meta, "album", val, "riff_info")
                                elif scid in (b"IBPM", b"ITMP"):
                                    _apply_kv(meta, "bpm", val, "riff_info")
                            sp += 8 + scsz + (scsz & 1)
                elif cid in (b"id3 ", b"ID3 ") and 10 <= csz <= 8 * 1024 * 1024:
                    _parse_id3v2_bytes(f.read(csz), meta)
                pos += 8 + csz + (csz & 1)
        elif hdr[:4] == b"FORM" and hdr[8:12] in (b"AIFF", b"AIFC"):
            end_lim = min(fsize, 8 + struct.unpack(">I", hdr[4:8])[0])
            pos = 12
            while pos + 8 <= end_lim:
                f.seek(pos)
                ch = f.read(8)
                if len(ch) < 8:
                    break
                cid, csz = ch[:4], struct.unpack(">I", ch[4:8])[0]
                if csz > end_lim - (pos + 8):
                    break
                if cid == b"NAME" and csz <= 65536:
                    _apply_kv(meta, "title", f.read(csz).decode("latin-1", errors="ignore"), "aiff")
                elif cid == b"AUTH" and csz <= 65536:
                    _apply_kv(meta, "artist", f.read(csz).decode("latin-1", errors="ignore"), "aiff")
                elif cid in (b"ID3 ", b"id3 ") and 10 <= csz <= 8 * 1024 * 1024:
                    _parse_id3v2_bytes(f.read(csz), meta)
                pos += 8 + csz + (csz & 1)
        elif hdr[4:8] == b"ftyp":
            # Quick MP4 ilst walker
            def walk_mp4(start: int, stop: int, depth: int = 0) -> None:
                if depth > 6:
                    return
                p = start
                while p + 8 <= stop:
                    f.seek(p)
                    ah = f.read(8)
                    if len(ah) < 8:
                        break
                    asz = struct.unpack(">I", ah[:4])[0]
                    tag = ah[4:8]
                    if asz < 8 or p + asz > stop:
                        break
                    if tag in (b"moov", b"udta"):
                        walk_mp4(p + 8, p + asz, depth + 1)
                    elif tag == b"meta":
                        walk_mp4(p + 12, p + asz, depth + 1)
                    elif tag == b"ilst":
                        ip = p + 8
                        iend = p + asz
                        while ip + 8 <= iend:
                            f.seek(ip)
                            ih = f.read(8)
                            isz = struct.unpack(">I", ih[:4])[0]
                            if isz < 16 or ip + isz > iend:
                                break
                            cp = ip + 8
                            while cp + 8 <= ip + isz:
                                f.seek(cp)
                                ch2 = f.read(8)
                                csz2 = struct.unpack(">I", ch2[:4])[0]
                                if csz2 < 8 or cp + csz2 > ip + isz:
                                    break
                                if ch2[4:8] == b"data" and csz2 > 16 and csz2 <= 65536:
                                    raw = f.read(csz2 - 8)[8:]
                                    it = ih[4:8]
                                    if it == b"\xa9gen":
                                        _merge_genre(meta, raw.decode("utf-8", errors="ignore"), "mp4")
                                    elif it == b"gnre" and len(raw) >= 2:
                                        code = struct.unpack(">H", raw[:2])[0]
                                        if 1 <= code <= len(ID3V1_GENRES):
                                            _merge_genre(meta, ID3V1_GENRES[code - 1], "mp4")
                                    elif it == b"\xa9nam":
                                        _apply_kv(meta, "title", raw.decode("utf-8", errors="ignore"), "mp4")
                                    elif it == b"\xa9ART":
                                        _apply_kv(meta, "artist", raw.decode("utf-8", errors="ignore"), "mp4")
                                    elif it == b"\xa9alb":
                                        _apply_kv(meta, "album", raw.decode("utf-8", errors="ignore"), "mp4")
                                    elif it == b"tmpo" and len(raw) >= 2 and meta.bpm <= 0.0:
                                        tb = struct.unpack(">H", raw[:2])[0]
                                        if 25 <= tb <= 320:
                                            meta.bpm = float(tb)
                                cp += csz2
                            ip += isz
                    p += asz

            walk_mp4(0, fsize)

        # ID3v1 footer fallback for MP3
        if ext == ".mp3" or hdr[:3] == b"ID3":
            if fsize >= 128:
                f.seek(fsize - 128)
                v1 = f.read(128)
                if len(v1) == 128 and v1[:3] == b"TAG":
                    if not meta.title:
                        meta.title = v1[3:33].decode("latin-1", errors="ignore").rstrip("\x00 ")
                    if not meta.artist:
                        meta.artist = v1[33:63].decode("latin-1", errors="ignore").rstrip("\x00 ")
                    if not meta.album:
                        meta.album = v1[63:93].decode("latin-1", errors="ignore").rstrip("\x00 ")
                    g = v1[127]
                    if g < len(ID3V1_GENRES):
                        _merge_genre(meta, ID3V1_GENRES[g], "id3v1")

    if not meta.has_genre:
        pg = detect_genre_from_path(path)
        if pg:
            meta.raw_genre = pg
            meta.canonical_genre = pg
            meta.genre_source = "path"
            if pg.lower() not in meta.tags:
                meta.tags.append(pg.lower())
    if not meta.title:
        meta.title = path.stem
    return meta
