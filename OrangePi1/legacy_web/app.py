from __future__ import annotations

import csv
import io
import json
import math
import re
import sqlite3
import unicodedata
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from difflib import SequenceMatcher
from pathlib import Path
from typing import Iterable

from flask import (
    Flask,
    abort,
    flash,
    jsonify,
    redirect,
    render_template,
    request,
    Response,
    url_for,
)

from coach_serial import coach_serial_bridge
from teacher_engine import TeacherEngine

BASE_DIR = Path(__file__).resolve().parent
DB_PATH = BASE_DIR / "songs.db"
MAX_SONG_NOTES = 256
NOTE_MIN_MIDI = 48   # C3
NOTE_MAX_MIDI = 108  # C8

TEMPLATE_DIR = BASE_DIR / "templates"
STATIC_DIR = BASE_DIR / "static"
COACH_BUILD = "V10.8.2-SINGLE"

app = Flask(
    __name__,
    template_folder=str(TEMPLATE_DIR),
    static_folder=str(STATIC_DIR),
    static_url_path="/static",
)
app.secret_key = "violin-song-library-local-dev-key"
app.config["TEMPLATES_AUTO_RELOAD"] = True
app.jinja_env.auto_reload = True

teacher_engine = TeacherEngine(BASE_DIR / "practice_history.db")
teacher_engine.set_command_sender(coach_serial_bridge.send_command)



def db_connect() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


def init_db() -> None:
    with db_connect() as conn:
        conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS songs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                slug TEXT UNIQUE NOT NULL,
                title TEXT NOT NULL,
                aliases TEXT NOT NULL DEFAULT '[]',
                tempo_bpm REAL NOT NULL DEFAULT 90,
                tolerance_cents REAL NOT NULL DEFAULT 30,
                hold_ms INTEGER NOT NULL DEFAULT 320,
                source TEXT NOT NULL DEFAULT '',
                created_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS song_notes (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                song_id INTEGER NOT NULL,
                position INTEGER NOT NULL,
                note TEXT NOT NULL,
                beats REAL NOT NULL DEFAULT 1,
                comment TEXT NOT NULL DEFAULT '',
                measure INTEGER NOT NULL DEFAULT 0,
                beat REAL NOT NULL DEFAULT 0,
                string_id INTEGER NOT NULL DEFAULT 0,
                finger INTEGER NOT NULL DEFAULT -1,
                FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE
            );

            CREATE INDEX IF NOT EXISTS idx_song_notes_song_position
                ON song_notes(song_id, position);
            """
        )

        song_columns = {row[1] for row in conn.execute("PRAGMA table_info(songs)")}
        for column in ("time_num", "time_den"):
            if column not in song_columns:
                conn.execute(f"ALTER TABLE songs ADD COLUMN {column} INTEGER NOT NULL DEFAULT 4")

        # In-place migration for the user's existing songs.db.
        existing = {row[1] for row in conn.execute("PRAGMA table_info(song_notes)")}
        migrations = {
            "measure": "INTEGER NOT NULL DEFAULT 0",
            "beat": "REAL NOT NULL DEFAULT 0",
            "string_id": "INTEGER NOT NULL DEFAULT 0",
            "finger": "INTEGER NOT NULL DEFAULT -1",
        }
        for column, declaration in migrations.items():
            if column not in existing:
                conn.execute(f"ALTER TABLE song_notes ADD COLUMN {column} {declaration}")


SHARP_NAMES = [
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B",
]

STRING_ID_TO_NAME = {1: "E", 2: "A", 3: "D", 4: "G"}
STRING_OPEN_MIDI = {1: 76, 2: 69, 3: 62, 4: 55}


def choose_default_string_id(note: str) -> int:
    if not note or note == "REST":
        return 0
    midi = note_to_midi(note)
    for sid in (1, 2, 3, 4):
        if midi >= STRING_OPEN_MIDI[sid]:
            return sid
    return 4


def infer_first_position_finger(note: str, string_id: int) -> int:
    if not note or note == "REST" or string_id not in STRING_OPEN_MIDI:
        return -1
    delta = note_to_midi(note) - STRING_OPEN_MIDI[string_id]
    if delta == 0: return 0
    if delta in (1, 2): return 1
    if delta in (3, 4): return 2
    if delta == 5: return 3
    if delta in (6, 7): return 4
    return -1


def note_to_midi(note: str) -> int:
    raw = note.strip().replace("♯", "#").replace("♭", "b")
    match = re.fullmatch(r"([A-Ga-g])([#b]?)(-?\d+)", raw)
    if not match:
        raise ValueError(f"无效音名：{note}")

    letter, accidental, octave_text = match.groups()
    letter = letter.upper()
    octave = int(octave_text)

    base = {
        "C": 0,
        "D": 2,
        "E": 4,
        "F": 5,
        "G": 7,
        "A": 9,
        "B": 11,
    }[letter]

    if accidental == "#":
        base += 1
    elif accidental == "b":
        base -= 1

    return (octave + 1) * 12 + base


def midi_to_note(midi: int) -> str:
    if midi < 0 or midi > 127:
        raise ValueError("MIDI超出0~127")
    return f"{SHARP_NAMES[midi % 12]}{midi // 12 - 1}"


def normalize_note(note: str) -> str:
    raw = note.strip()
    if raw.upper() in {"REST", "R", "休止", "休止符"}:
        return "REST"

    midi = note_to_midi(raw)
    if midi < NOTE_MIN_MIDI or midi > NOTE_MAX_MIDI:
        raise ValueError(
            f"{note} 超出本工程支持范围 C3~C8。"
            "标准小提琴通常从G3开始。"
        )
    return midi_to_note(midi)


def normalize_title(text: str) -> str:
    text = unicodedata.normalize("NFKC", text or "")
    text = re.sub(r"[\s\-_，。！？、,.!?;；:：'\"“”‘’]+", "", text)
    for prefix in ["练习歌曲", "练习乐曲", "练习", "播放", "演奏", "来一首", "歌曲", "乐曲"]:
        if text.startswith(prefix):
            text = text[len(prefix):]
            break
    return text.casefold()


def make_slug(title: str) -> str:
    normalized = unicodedata.normalize("NFKC", title).strip()
    ascii_part = re.sub(r"[^A-Za-z0-9]+", "-", normalized).strip("-").lower()
    if ascii_part:
        base = ascii_part[:32]
    else:
        codepoints = "-".join(f"{ord(ch):x}" for ch in normalized[:8])
        base = f"song-{codepoints}" if codepoints else "song"

    slug = base
    suffix = 2
    with db_connect() as conn:
        while conn.execute(
            "SELECT 1 FROM songs WHERE slug = ?", (slug,)
        ).fetchone():
            slug = f"{base}-{suffix}"
            suffix += 1
    return slug


def parse_aliases(text: str) -> list[str]:
    if not text:
        return []
    items = re.split(r"[|｜,，;；\n]+", text)
    result = []
    seen = set()
    for item in items:
        item = item.strip()
        if item and item not in seen:
            seen.add(item)
            result.append(item)
    return result


def validate_song(
    title: str,
    tempo_bpm: float,
    tolerance_cents: float,
    hold_ms: int,
    notes: list[dict],
) -> None:
    if not title.strip():
        raise ValueError("曲名不能为空")
    if not math.isfinite(tempo_bpm) or tempo_bpm < 20 or tempo_bpm > 300:
        raise ValueError("tempo_bpm应在20~300")
    if not math.isfinite(tolerance_cents) or tolerance_cents < 3 or tolerance_cents > 100:
        raise ValueError("tolerance_cents应在3~100")
    if hold_ms < 100 or hold_ms > 3000:
        raise ValueError("hold_ms应在100~3000")
    if not notes:
        raise ValueError("曲目至少需要一个音符")
    if len(notes) > MAX_SONG_NOTES:
        raise ValueError(f"单首曲目最多{MAX_SONG_NOTES}个事件")

    for index, event in enumerate(notes, start=1):
        event["note"] = normalize_note(str(event["note"]))
        beats = float(event.get("beats", 1))
        if not math.isfinite(beats) or beats <= 0 or beats > 16:
            raise ValueError(f"第{index}个音符beats必须在0~16")
        event["beats"] = beats
        event["comment"] = str(event.get("comment", "")).strip()
        if event["note"] == "REST":
            event["string_id"] = 0
            event["finger"] = -1
        else:
            sid = int(event.get("string_id", 0) or 0)
            if sid not in (1, 2, 3, 4):
                sid = choose_default_string_id(event["note"])
            event["string_id"] = sid
            finger = int(event.get("finger", -1) if event.get("finger", -1) is not None else -1)
            if finger not in (-1, 0, 1, 2, 3, 4): finger = -1
            if finger == -1: finger = infer_first_position_finger(event["note"], sid)
            event["finger"] = finger
        event["measure"] = int(event.get("measure", 0) or 0)
        event["beat"] = float(event.get("beat", 0) or 0)


def save_song(
    *,
    title: str,
    aliases: Iterable[str],
    tempo_bpm: float,
    tolerance_cents: float,
    hold_ms: int,
    notes: list[dict],
    source: str = "",
    replace_title: bool = True,
    time_num: int = 4,
    time_den: int = 4,
) -> int:
    title = title.strip()
    aliases = [x.strip() for x in aliases if x.strip()]
    validate_song(title, tempo_bpm, tolerance_cents, hold_ms, notes)
    if not 1 <= int(time_num) <= 12 or int(time_den) not in (2,4,8,16):
        raise ValueError("拍号需为 1~12 / 2、4、8、16")

    now = datetime.now(timezone.utc).isoformat(timespec="seconds")

    with db_connect() as conn:
        existing = None
        if replace_title:
            existing = conn.execute(
                "SELECT id, slug FROM songs WHERE title = ?",
                (title,),
            ).fetchone()

        if existing:
            song_id = existing["id"]
            conn.execute(
                """
                UPDATE songs
                SET aliases=?, tempo_bpm=?, tolerance_cents=?,
                    hold_ms=?, source=?, created_at=?
                WHERE id=?
                """,
                (
                    json.dumps(aliases, ensure_ascii=False),
                    float(tempo_bpm),
                    float(tolerance_cents),
                    int(hold_ms),
                    source,
                    now,
                    song_id,
                ),
            )
            conn.execute("DELETE FROM song_notes WHERE song_id=?", (song_id,))
        else:
            slug = make_slug(title)
            cursor = conn.execute(
                """
                INSERT INTO songs(
                    slug, title, aliases, tempo_bpm,
                    tolerance_cents, hold_ms, source, created_at
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    slug,
                    title,
                    json.dumps(aliases, ensure_ascii=False),
                    float(tempo_bpm),
                    float(tolerance_cents),
                    int(hold_ms),
                    source,
                    now,
                ),
            )
            song_id = cursor.lastrowid

        conn.execute("UPDATE songs SET time_num=?, time_den=? WHERE id=?", (int(time_num), int(time_den), song_id))

        conn.executemany(
            """
            INSERT INTO song_notes(song_id, position, note, beats, comment, measure, beat, string_id, finger)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                (
                    song_id,
                    index,
                    event["note"],
                    float(event["beats"]),
                    event.get("comment", ""),
                    int(event.get("measure", 0) or 0),
                    float(event.get("beat", 0) or 0),
                    int(event.get("string_id", 0) or 0),
                    int(event.get("finger", -1)),
                )
                for index, event in enumerate(notes, start=1)
            ],
        )

    return int(song_id)


def enrich_loaded_notes(notes: list[dict], time_num: int = 4, time_den: int = 4) -> list[dict]:
    """Backfill score/string metadata for old DB rows without changing their notes."""
    measure = 1
    beat = 1.0
    measure_quarters = float(time_num) * 4.0 / float(time_den)
    result = []
    for index, raw in enumerate(notes):
        event = dict(raw)
        event["index"] = index
        beats = float(event.get("beats", 1) or 1)
        stored_measure = int(event.get("measure", 0) or 0)
        stored_beat = float(event.get("beat", 0) or 0)
        if stored_measure > 0:
            measure = stored_measure
        event["measure"] = stored_measure if stored_measure > 0 else measure
        event["beat"] = stored_beat if stored_beat > 0 else beat
        if event.get("note") == "REST":
            event["string_id"] = 0
            event["finger"] = -1
        else:
            sid = int(event.get("string_id", 0) or 0)
            if sid not in (1,2,3,4): sid = choose_default_string_id(event["note"])
            event["string_id"] = sid
            finger = int(event.get("finger", -1) if event.get("finger") is not None else -1)
            if finger not in (-1,0,1,2,3,4): finger = -1
            if finger == -1: finger = infer_first_position_finger(event["note"], sid)
            event["finger"] = finger
        result.append(event)
        beat = event["beat"] + beats
        measure = event["measure"]
        while beat >= measure_quarters + 0.9999:
            measure += 1
            beat -= measure_quarters
    return result


def load_song_by_id(song_id: int) -> dict | None:
    with db_connect() as conn:
        song = conn.execute(
            "SELECT * FROM songs WHERE id=?", (song_id,)
        ).fetchone()
        if not song:
            return None
        notes = conn.execute(
            """
            SELECT position, note, beats, comment, measure, beat, string_id, finger
            FROM song_notes
            WHERE song_id=?
            ORDER BY position
            """,
            (song_id,),
        ).fetchall()

    return {
        "id": song["id"],
        "slug": song["slug"],
        "title": song["title"],
        "aliases": json.loads(song["aliases"] or "[]"),
        "tempo_bpm": song["tempo_bpm"],
        "tolerance_cents": song["tolerance_cents"],
        "hold_ms": song["hold_ms"],
        "source": song["source"],
        "created_at": song["created_at"],
        "time_num": song["time_num"], "time_den": song["time_den"],
        "notes": enrich_loaded_notes([dict(row) for row in notes], song["time_num"], song["time_den"]),
    }


def all_songs() -> list[dict]:
    with db_connect() as conn:
        rows = conn.execute(
            """
            SELECT s.*,
                   (SELECT COUNT(*) FROM song_notes n WHERE n.song_id=s.id) AS note_count
            FROM songs s
            ORDER BY s.id
            """
        ).fetchall()

    result = []
    for row in rows:
        item = dict(row)
        item["aliases"] = json.loads(item["aliases"] or "[]")
        result.append(item)
    return result


def resolve_song(query: str, *, strict: bool = False) -> dict | None:
    q = normalize_title(query)
    if not q:
        return None

    candidates = all_songs()

    # 1. 标题/别名精确命中
    for song in candidates:
        names = [song["slug"], song["title"], *song["aliases"]]
        for name in names:
            if normalize_title(name) == q:
                return load_song_by_id(song["id"])

    if strict:
        return None

    # 2. 包含关系
    for song in candidates:
        names = [song["title"], *song["aliases"]]
        for name in names:
            n = normalize_title(name)
            if len(q) >= 2 and (q in n or n in q):
                return load_song_by_id(song["id"])

    # 3. ASR轻微错字的模糊匹配
    best = (0.0, None)
    for song in candidates:
        names = [song["title"], *song["aliases"]]
        for name in names:
            n = normalize_title(name)
            score = SequenceMatcher(None, q, n).ratio()
            if score > best[0]:
                best = (score, song)

    if best[1] and best[0] >= 0.62:
        return load_song_by_id(best[1]["id"])

    return None


def parse_csv_song(raw_bytes: bytes) -> dict:
    text = raw_bytes.decode("utf-8-sig")
    reader = csv.DictReader(io.StringIO(text))
    required = {"title", "order", "note", "beats"}
    headers = {h.strip() for h in (reader.fieldnames or []) if h}
    missing = required - headers
    if missing:
        raise ValueError("CSV缺少列：" + ", ".join(sorted(missing)))

    rows = list(reader)
    if not rows:
        raise ValueError("CSV没有数据")

    first = rows[0]
    title = (first.get("title") or "").strip()
    aliases = parse_aliases(first.get("aliases") or "")
    tempo_bpm = float(first.get("tempo_bpm") or 90)
    tolerance_cents = float(first.get("tolerance_cents") or 30)
    hold_ms = int(float(first.get("hold_ms") or 320))
    source = (first.get("source") or "CSV upload").strip()

    notes = []
    for row_index, row in enumerate(rows, start=2):
        raw_note = (row.get("note") or "").strip()
        if not raw_note:
            continue
        try:
            order = int(float(row.get("order") or len(notes) + 1))
            beats = float(row.get("beats") or 1)
        except ValueError as exc:
            raise ValueError(f"CSV第{row_index}行order/beats不是数字") from exc

        notes.append(
            {
                "order": order,
                "note": raw_note,
                "beats": beats,
                "comment": (row.get("comment") or "").strip(),
                "measure": int(row.get("measure") or 0),
                "beat": float(row.get("beat") or 0),
            }
        )

    notes.sort(key=lambda x: x["order"])

    return {
        "title": title,
        "aliases": aliases,
        "tempo_bpm": tempo_bpm,
        "tolerance_cents": tolerance_cents,
        "hold_ms": hold_ms,
        "source": source,
        "time_num": int(first.get("time_num") or 4),
        "time_den": int(first.get("time_den") or 4),
        "notes": notes,
    }


def xml_local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def parse_musicxml(raw_bytes: bytes, title_override: str = "", aliases_text: str = "") -> dict:
    """Parse the first monophonic violin part while preserving score metadata.

    MusicXML <technical><string> uses 1=highest string, which exactly matches
    this project: 1=E, 2=A, 3=D, 4=G.
    """
    root = ET.fromstring(raw_bytes)
    title = title_override.strip()
    if not title:
        for path in [".//{*}work-title", ".//{*}movement-title"]:
            node = root.find(path)
            if node is not None and (node.text or "").strip():
                title = node.text.strip(); break
    if not title: title = "未命名MusicXML曲目"

    part = root.find(".//{*}part")
    if part is None: raise ValueError("MusicXML中未找到part")

    divisions = 1.0
    tempo_bpm = 90.0
    notes: list[dict] = []

    # Prefer explicit <sound tempo>, otherwise metronome/per-minute.
    tempo_node = root.find(".//{*}sound[@tempo]")
    if tempo_node is not None:
        try: tempo_bpm = float(tempo_node.attrib.get("tempo", 90))
        except ValueError: pass
    else:
        per_minute = root.find(".//{*}metronome/{*}per-minute")
        if per_minute is not None:
            try: tempo_bpm = float(per_minute.text or 90)
            except ValueError: pass

    time_num, time_den, seen_meter = 4, 4, None
    tie_open = False
    for measure_pos, measure_el in enumerate(part.findall("{*}measure"), start=1):
        try: measure_number = int(measure_el.attrib.get("number", measure_pos))
        except ValueError: measure_number = measure_pos
        beat_cursor = 1.0

        attributes = measure_el.find("{*}attributes")
        if attributes is not None:
            meter = attributes.find("{*}time")
            if meter is not None:
                num_el, den_el = meter.find("{*}beats"), meter.find("{*}beat-type")
                if num_el is not None and den_el is not None:
                    current = (int(num_el.text or 4), int(den_el.text or 4))
                    if seen_meter is not None and current != seen_meter:
                        raise ValueError("此版短句跟拍暂不支持中途变拍号，请先拆为两首曲目")
                    seen_meter = current
                    time_num, time_den = current
            div_el = attributes.find("{*}divisions")
            if div_el is not None:
                try: divisions = max(1.0, float(div_el.text or 1))
                except ValueError: divisions = 1.0

        for element in measure_el.findall("{*}note"):
            if element.find("{*}chord") is not None: continue
            if element.find("{*}grace") is not None: continue
            voice_el = element.find("{*}voice")
            voice = (voice_el.text or "").strip() if voice_el is not None else "1"
            if voice not in ("", "1"): continue

            duration_el = element.find("{*}duration")
            try: duration = float(duration_el.text) if duration_el is not None else divisions
            except (TypeError, ValueError): duration = divisions
            beats = max(0.0625, duration / divisions)

            is_rest = element.find("{*}rest") is not None
            if is_rest:
                note = "REST"
            else:
                pitch = element.find("{*}pitch")
                if pitch is None: continue
                step_el, alter_el, octave_el = pitch.find("{*}step"), pitch.find("{*}alter"), pitch.find("{*}octave")
                if step_el is None or octave_el is None: continue
                step = (step_el.text or "").strip()
                try: alter = int(float(alter_el.text or 0)) if alter_el is not None else 0
                except ValueError: alter = 0
                if abs(alter) > 1: raise ValueError("暂不支持双升/双降MusicXML")
                accidental = "#" if alter == 1 else "b" if alter == -1 else ""
                note = normalize_note(f"{step}{accidental}{int(octave_el.text or 4)}")

            string_id = 0
            finger = -1
            technical = element.find(".//{*}technical")
            if technical is not None:
                string_el = technical.find("{*}string")
                fingering_el = technical.find("{*}fingering")
                if string_el is not None:
                    try:
                        parsed = int(string_el.text or 0)
                        if parsed in (1,2,3,4): string_id = parsed
                    except ValueError: pass
                if fingering_el is not None:
                    try:
                        parsed = int((fingering_el.text or "").strip())
                        if parsed in (0,1,2,3,4): finger = parsed
                    except ValueError: pass

            if note != "REST":
                if string_id == 0: string_id = choose_default_string_id(note)
                if finger == -1: finger = infer_first_position_finger(note, string_id)

            tie_types = {e.attrib.get("type") for e in element.findall("{*}tie")}
            tie_types.update(e.attrib.get("type") for e in element.findall(".//{*}tied"))
            if "stop" in tie_types and tie_open and notes and notes[-1]["note"] == note:
                notes[-1]["beats"] += beats
                tie_open = "start" in tie_types
                beat_cursor += beats
                continue
            if "stop" in tie_types:
                raise ValueError("MusicXML 延音线缺少匹配的前一音，请检查第一声部")
            tie_open = "start" in tie_types
            notes.append({
                "note": note, "beats": beats, "comment": "MusicXML",
                "measure": measure_number, "beat": beat_cursor,
                "string_id": string_id, "finger": finger,
            })
            beat_cursor += beats

    if not notes: raise ValueError("MusicXML未解析到第一声部的单旋律音符")
    return {
        "title": title, "aliases": parse_aliases(aliases_text),
        "tempo_bpm": tempo_bpm, "tolerance_cents": 30.0, "hold_ms": 320,
        "source": "MusicXML upload", "notes": notes,
        "time_num": time_num, "time_den": time_den,
    }


def parse_manual_sequence(text: str) -> list[dict]:
    tokens = re.split(r"[\s,，;；]+", text.strip())
    notes, measure, beat = [], 1, 1.0
    for token in tokens:
        if not token: continue
        if ":" in token:
            note_text, beats_text = token.split(":", 1); beats = float(beats_text)
        else:
            note_text, beats = token, 1.0
        note = normalize_note(note_text)
        sid = 0 if note == "REST" else choose_default_string_id(note)
        notes.append({
            "note": note, "beats": beats, "comment": "",
            "measure": measure, "beat": beat,
            "string_id": sid, "finger": -1 if sid == 0 else infer_first_position_finger(note, sid),
        })
        beat += beats
        if beat > 4.0001:
            measure += 1; beat = 1.0
    return notes


def seed_defaults() -> None:
    if all_songs():
        return

    twinkle_notes = [
        ("C4",1),("C4",1),("G4",1),("G4",1),("A4",1),("A4",1),("G4",2),
        ("F4",1),("F4",1),("E4",1),("E4",1),("D4",1),("D4",1),("C4",2),
        ("G4",1),("G4",1),("F4",1),("F4",1),("E4",1),("E4",1),("D4",2),
        ("G4",1),("G4",1),("F4",1),("F4",1),("E4",1),("E4",1),("D4",2),
        ("C4",1),("C4",1),("G4",1),("G4",1),("A4",1),("A4",1),("G4",2),
        ("F4",1),("F4",1),("E4",1),("E4",1),("D4",1),("D4",1),("C4",2),
    ]
    save_song(
        title="小星星",
        aliases=["一闪一闪亮晶晶", "小行星", "小猩猩"],
        tempo_bpm=90,
        tolerance_cents=30,
        hold_ms=320,
        notes=[
            {"note": note, "beats": beats, "comment": ""}
            for note, beats in twinkle_notes
        ],
        source="Built-in example",
    )

    # 1=G示例版本。网页中明确标注用户可按自己的谱重新上传覆盖。
    find_friend = [
        "D5","D5","D5","D5",
        "D5","E5","D5",
        "D5","G5","F#5","E5",
        "D5","E5","D5",
        "D5","D5","B4","B4",
        "D5","D5","B4",
        "A4","C5","B4","A4",
        "G4","A4","G4",
    ]
    save_song(
        title="找朋友",
        aliases=["找朋友儿歌", "找呀找呀找朋友"],
        tempo_bpm=100,
        tolerance_cents=30,
        hold_ms=300,
        notes=[
            {"note": note, "beats": 1, "comment": "示例转写"}
            for note in find_friend
        ],
        source="示例转写（1=G）；请按你最终采用的谱面校对；https://www.ccguitar.cn/cchtml/9988828.htm",
    )


@app.route("/")
def index():
    return render_template("index.html", songs=all_songs())



@app.after_request
def _disable_dev_cache(response):
    # V10.1: avoid browser serving an older coach.js/coach.css while iterating.
    if request.path in {"/coach", "/control", "/hud", "/coach-v104"} or request.path.startswith("/static/"):
        response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
        response.headers["Pragma"] = "no-cache"
        response.headers["Expires"] = "0"
    return response

@app.get("/coach")
def coach_dashboard():
    return redirect(url_for("control_dashboard"))


@app.get("/control")
@app.get("/coach-v104")
def control_dashboard():
    app.jinja_env.cache.clear()
    response = app.make_response(render_template("control.html"))
    response.headers["X-Violin-Coach-Build"] = COACH_BUILD
    return response


@app.get("/hud")
def hud_dashboard():
    app.jinja_env.cache.clear()
    response = app.make_response(render_template("hud.html"))
    response.headers["X-Violin-Coach-Build"] = COACH_BUILD
    return response


@app.get("/api/coach/version")
def api_coach_version():
    return jsonify({
        "ok": True,
        "build": COACH_BUILD,
        "base_dir": str(BASE_DIR),
        "control_html": str(TEMPLATE_DIR / "control.html"),
        "hud_html": str(TEMPLATE_DIR / "hud.html"),
        "teacher_db": str(BASE_DIR / "practice_history.db"),
        "control_exists": (TEMPLATE_DIR / "control.html").exists(),
        "hud_exists": (TEMPLATE_DIR / "hud.html").exists(),
        "teacher_engine": True,
    })


def build_coach_state() -> dict:
    state = coach_serial_bridge.snapshot()
    # Enrich the serial snapshot from the song DB so HUD always has the full score.
    song = None
    if state.get("song_id"):
        with db_connect() as conn:
            row = conn.execute("SELECT id FROM songs WHERE slug=?", (state["song_id"],)).fetchone()
        if row:
            song = load_song_by_id(row["id"])
    if song is None and state.get("song_title"):
        song = resolve_song(state["song_title"],strict=True)
    serial_notes = state.get("song_notes") or []
    count = int(state.get("song_count") or 0)
    complete_serial_score = count > 0 and len(serial_notes) == count and all(
        int(note.get("index", -1)) == index
        for index, note in enumerate(serial_notes)
    )
    # Firmware may use its local fallback or a different arrangement. Its full
    # transmitted score is authoritative for the targets it actually accepts.
    if complete_serial_score:
        state["score_notes"] = serial_notes
        state["score_source"] = "ESP32 serial"
    elif song:
        state["score_notes"] = song["notes"]
        state["score_source"] = song.get("source", "")
        state["song_db_id"] = song.get("id")
        if not state.get("song_count"):
            state["song_count"] = len(song["notes"])
    else:
        state["score_notes"] = state.get("song_notes", [])
        state["score_source"] = "ESP32 serial"

    state["teacher"] = teacher_engine.observe(state)
    single = state.get("app_state") == "single" or (
        state.get("app_state") in {"paused", "voice", "cloud"} and state.get("resume_state") == "single")
    state["single_active"] = single
    if single:
        # Keep the bridge's cached score for returning to song practice; present only the live note.
        state.update(song_title="单音练习", song_count=0, song_index=0,
                     score_notes=[], song_notes=[], practice_mode="pitch", rhythm_events=[])
    return state


@app.get("/api/coach/ports")
def api_coach_ports():
    return jsonify(
        {
            "ok": True,
            "serial_available": coach_serial_bridge.serial_available,
            "ports": coach_serial_bridge.list_ports(),
        }
    )


@app.get("/api/coach/state")
def api_coach_state():
    return jsonify({"ok": True, "state": build_coach_state()})


@app.post("/api/coach/connect")
def api_coach_connect():
    payload = request.get_json(silent=True) or {}
    port = str(payload.get("port", "")).strip()
    try:
        baud = int(payload.get("baud", 115200))
        coach_serial_bridge.connect(port, baud)
    except Exception as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    return jsonify({"ok": True, "state": coach_serial_bridge.snapshot()})


@app.post("/api/coach/disconnect")
def api_coach_disconnect():
    coach_serial_bridge.disconnect()
    return jsonify({"ok": True, "state": coach_serial_bridge.snapshot()})


@app.post("/api/coach/command")
def api_coach_command():
    payload = request.get_json(silent=True) or {}
    command = str(payload.get("command", "")).strip()
    try:
        coach_serial_bridge.send_command(command)
    except Exception as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    return jsonify({"ok": True, "command": command, "state": build_coach_state()})


@app.post("/api/coach/start-song")
def api_coach_start_song():
    payload = request.get_json(silent=True) or {}
    query = str(payload.get("query", "")).strip()
    if not query:
        return jsonify({"ok": False, "error": "请选择曲目"}), 400
    song = resolve_song(query, strict=True)
    if song is None:
        return jsonify({"ok": False, "error": "曲库没有这个标题或别名；请先在曲库页确认"}), 404
    query = song["title"]
    if len(query.encode("utf-8")) > 71:
        return jsonify({"ok": False, "error": "曲目标题过长，请缩短到23个汉字以内"}), 400
    baseline = coach_serial_bridge.snapshot().get("song_request_seq", 0)
    try:
        coach_serial_bridge.send_command(f"START_SONG:{query}")
    except Exception as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    return jsonify({"ok": True, "accepted": True, "query": query, "baseline_seq": baseline})


@app.get("/api/coach/diagnostics")
def api_coach_diagnostics():
    state=coach_serial_bridge.snapshot()
    keys=("connected","control_rx_confirmed","port","baud","firmware","error","message_age_s","song_id","song_title",
          "song_active","song_index","song_count","practice_mode","tempo_bpm","trainer_phase",
          "last_control_ack","last_song_request","voice_phase","voice_message","last_asr","last_intent",
          "rhythm_offset_ms","rhythm_events","clock_age_ms","clock_lag_ms",
          "follow_phase","follow_elapsed_ms","follow_target_ms","follow_remaining_prep_ms",
          "follow_retry_reason","follow_retries","follow_pitch_ok","follow_sounding","follow_armed")
    report={"web_build":COACH_BUILD,"firmware_matches":state.get("firmware")==COACH_BUILD,
            "ports":coach_serial_bridge.list_ports(),"state":{k:state.get(k) for k in keys},
            "songs":[{"title":x["title"],"slug":x["slug"],"note_count":x["note_count"]} for x in all_songs()]}
    response=Response(json.dumps(report,ensure_ascii=False,indent=2),mimetype="application/json")
    response.headers["Content-Disposition"]='attachment; filename="violin_diagnostics_V10_8_0.json"'
    return response


@app.get("/api/teacher/state")
def api_teacher_state():
    return jsonify({"ok": True, "teacher": teacher_engine.snapshot()})


@app.get("/api/teacher/history")
def api_teacher_history():
    limit = request.args.get("limit", 12, type=int)
    return jsonify({"ok": True, "sessions": teacher_engine.history.recent_sessions(limit or 12)})


@app.post("/api/teacher/review/start")
def api_teacher_review_start():
    ok, message = teacher_engine.start_review()
    return jsonify({"ok": ok, "message": message, "teacher": teacher_engine.snapshot()}), (200 if ok else 400)


@app.post("/api/teacher/review/skip")
def api_teacher_review_skip():
    ok, message = teacher_engine.skip_review()
    return jsonify({"ok": ok, "message": message, "teacher": teacher_engine.snapshot()}), (200 if ok else 400)


@app.route("/song/<int:song_id>")
def song_detail(song_id: int):
    song = load_song_by_id(song_id)
    if not song:
        abort(404)
    return render_template("song_detail.html", song=song)


@app.route("/song/<int:song_id>/timing", methods=["GET", "POST"])
def song_timing(song_id: int):
    song = load_song_by_id(song_id)
    if not song:
        abort(404)
    if request.method == "POST":
        try:
            num = int(request.form.get("time_num", 4))
            den = int(request.form.get("time_den", 4))
            tempo = float(request.form.get("tempo_bpm", 90))
            updated = []
            for i, original in enumerate(song["notes"]):
                event = dict(original)
                event["beats"] = float(request.form.get(f"beats_{i}", ""))
                event["measure"] = int(request.form.get(f"measure_{i}", ""))
                event["beat"] = float(request.form.get(f"beat_{i}", ""))
                if event["measure"] < 1 or not 1 <= event["beat"] <= 49:
                    raise ValueError(f"第{i+1}音小节或位置不合法")
                updated.append(event)
            validate_song(song["title"], tempo, song["tolerance_cents"], song["hold_ms"], updated)
            if not 1 <= num <= 12 or den not in (2,4,8,16):
                raise ValueError("拍号不合法")
            backup_dir = BASE_DIR / "_score_backups"
            backup_dir.mkdir(exist_ok=True)
            stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S_%f")
            backup = backup_dir / f"songs_before_timing_{stamp}.db"
            with db_connect() as source, sqlite3.connect(backup) as dest:
                source.backup(dest)
            # Preserve this id/slug even if the DB happens to contain duplicate titles.
            with db_connect() as conn:
                conn.execute("UPDATE songs SET time_num=?,time_den=?,tempo_bpm=? WHERE id=?", (num,den,tempo,song_id))
                for i, event in enumerate(updated,1):
                    conn.execute("UPDATE song_notes SET beats=?,measure=?,beat=? WHERE song_id=? AND position=?", (event["beats"],event["measure"],event["beat"],song_id,i))
            flash("节拍已保存，并已备份原曲库。请结束当前练习，再启动这首曲目载入新谱。", "success")
            return redirect(url_for("song_timing",song_id=song_id))
        except (ValueError, TypeError) as exc:
            flash(str(exc), "error")
    return render_template("song_timing.html", song=load_song_by_id(song_id))


@app.route("/upload", methods=["GET", "POST"])
def upload_song():
    if request.method == "GET":
        return render_template("upload.html")

    try:
        upload = request.files.get("score_file")
        manual_sequence = (request.form.get("manual_sequence") or "").strip()

        if upload and upload.filename:
            filename = upload.filename.lower()
            raw = upload.read()

            if filename.endswith(".csv"):
                song = parse_csv_song(raw)
            elif filename.endswith((".musicxml", ".xml")):
                song = parse_musicxml(
                    raw,
                    title_override=request.form.get("title") or "",
                    aliases_text=request.form.get("aliases") or "",
                )
                song["tempo_bpm"] = float(request.form.get("tempo_bpm") or song["tempo_bpm"])
                song["tolerance_cents"] = float(
                    request.form.get("tolerance_cents") or song["tolerance_cents"]
                )
                song["hold_ms"] = int(float(request.form.get("hold_ms") or song["hold_ms"]))
            else:
                raise ValueError("只支持CSV、MusicXML(.musicxml/.xml)")

        elif manual_sequence:
            song = {
                "title": (request.form.get("title") or "").strip(),
                "aliases": parse_aliases(request.form.get("aliases") or ""),
                "tempo_bpm": float(request.form.get("tempo_bpm") or 90),
                "tolerance_cents": float(request.form.get("tolerance_cents") or 30),
                "hold_ms": int(float(request.form.get("hold_ms") or 320)),
                "source": "Web manual input",
                "notes": parse_manual_sequence(manual_sequence),
            }
        else:
            raise ValueError("请选择曲谱文件，或填写手动音符序列")

        song_id = save_song(**song)
        flash(f"已保存：{song['title']}", "success")
        return redirect(url_for("song_detail", song_id=song_id))

    except Exception as exc:
        flash(str(exc), "error")
        return render_template("upload.html"), 400


@app.post("/song/<int:song_id>/delete")
def delete_song(song_id: int):
    with db_connect() as conn:
        conn.execute("DELETE FROM songs WHERE id=?", (song_id,))
    flash("曲目已删除", "success")
    return redirect(url_for("index"))


@app.get("/template.csv")
def template_csv():
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(
        [
            "title",
            "aliases",
            "tempo_bpm",
            "tolerance_cents",
            "hold_ms",
            "order",
            "note",
            "beats",
            "comment",
            "source",
        ]
    )
    writer.writerow(
        [
            "我的练习曲",
            "我的曲子|练习曲一",
            "90",
            "30",
            "320",
            "1",
            "C4",
            "1",
            "第一音",
            "Excel编辑后另存为CSV UTF-8",
        ]
    )
    writer.writerow(["","","","","","2","C#4","0.5","升C示例",""])
    writer.writerow(["","","","","","3","REST","0.5","休止符",""])
    writer.writerow(["","","","","","4","D4","1","第四音",""])

    return Response(
        output.getvalue().encode("utf-8-sig"),
        mimetype="text/csv",
        headers={
            "Content-Disposition":
                'attachment; filename="violin_song_template.csv"'
        },
    )


@app.get("/hotwords.txt")
def hotwords_txt():
    lines = [
        "你好小智",
        "暂停练习",
        "继续练习",
        "下一音",
        "上一音",
        "下一根弦",
        "上一根弦",
        "结束练习",
        "速度快一点",
        "速度慢一点",
        "恢复原速",
        "开始强化",
        "练习弱点",
        "跳过强化",
        "不用强化",
        "音准模式",
        "节奏模式",
        "完整模式",
        "完整演奏",
        "打开节拍器",
        "关闭节拍器",
        "速度六十",
        "速度八十",
        "速度九十",
        "速度一百",
        "速度一百二十",
        "第一弦",
        "第二弦",
        "第三弦",
        "第四弦",
        "升半音",
        "降半音",
    ]

    number_words = {
        3: "three",
        4: "four",
        5: "five",
        6: "six",
        7: "seven",
        8: "eight",
    }

    chinese_numbers = {
        3: "三",
        4: "四",
        5: "五",
        6: "六",
        7: "七",
        8: "八",
    }

    for octave in range(3, 9):
        for letter in "CDEFGAB":
            lines.append(f"{letter} {number_words[octave]}")
            lines.append(f"练习{letter}{chinese_numbers[octave]}")
            lines.append(f"{letter} sharp {number_words[octave]}")
            lines.append(f"练习升{letter}{chinese_numbers[octave]}")

    # Explicit string + pitch phrases, e.g. “二弦A4 / 三弦A4”.
    string_words = {1: "一弦", 2: "二弦", 3: "三弦", 4: "四弦"}
    for string_id, string_word in string_words.items():
        open_midi = STRING_OPEN_MIDI[string_id]
        for midi in range(open_midi, min(open_midi + 25, NOTE_MAX_MIDI + 1)):
            note = midi_to_note(midi)
            lines.append(f"{string_word}{note}")
            lines.append(f"练习{string_word}{note}")

    for song in all_songs():
        lines.append(song["title"])
        lines.append("练习" + song["title"])
        lines.extend(song["aliases"])

    deduplicated = []
    seen = set()
    for line in lines:
        line = line.strip()
        if line and line not in seen:
            seen.add(line)
            deduplicated.append(line)

    text = "\n".join(deduplicated) + "\n"

    return Response(
        text.encode("utf-8"),
        mimetype="text/plain; charset=utf-8",
        headers={
            "Content-Disposition":
                'attachment; filename="xfyun_hotwords.txt"'
        },
    )


@app.get("/api/health")
def api_health():
    return jsonify(
        {
            "ok": True,
            "service": "Violin Song Library",
            "song_count": len(all_songs()),
            "note_range": "C3-C8",
            "max_song_notes": MAX_SONG_NOTES,
        }
    )


@app.get("/api/songs")
def api_songs():
    return jsonify({"ok": True, "songs": all_songs()})


@app.get("/api/songs/resolve")
def api_resolve_song():
    query = request.args.get("q", "")
    song = resolve_song(query, strict=request.args.get("strict") == "1")
    if not song:
        return jsonify({"ok": False, "error": "song_not_found"}), 404
    return jsonify({"ok": True, "song": song})


@app.get("/api/songs/<slug>")
def api_song(slug: str):
    with db_connect() as conn:
        row = conn.execute(
            "SELECT id FROM songs WHERE slug=?", (slug,)
        ).fetchone()
    if not row:
        return jsonify({"ok": False, "error": "song_not_found"}), 404
    return jsonify({"ok": True, "song": load_song_by_id(row["id"])})


init_db()
seed_defaults()
coach_serial_bridge.set_song_resolver(resolve_song)

if __name__ == "__main__":
    import os

    if os.environ.get("VIOLIN_AUTO_SERIAL", "0") == "1":
        requested_port = os.environ.get("VIOLIN_SERIAL_PORT", "").strip()
        try:
            if requested_port:
                coach_serial_bridge.connect(requested_port, 115200)
                print(f"AUTO SERIAL: connected {requested_port}")
            else:
                ports = coach_serial_bridge.list_ports()
                preferred = next(
                    (p["device"] for p in ports if any(k in p["device"].lower() for k in ("ttyusb", "ttyacm"))),
                    ports[0]["device"] if ports else "",
                )
                if preferred:
                    coach_serial_bridge.connect(preferred, 115200)
                    print(f"AUTO SERIAL: connected {preferred}")
                else:
                    print("AUTO SERIAL: no serial port found; /control can connect later")
        except Exception as exc:
            print(f"AUTO SERIAL failed: {exc}")

    control_path = TEMPLATE_DIR / "control.html"
    hud_path = TEMPLATE_DIR / "hud.html"
    print("=" * 72)
    print(f"Violin Coach {COACH_BUILD}")
    print(f"PID      : {os.getpid()}")
    print(f"APP      : {Path(__file__).resolve()}")
    print(f"BASE_DIR : {BASE_DIR}")
    print(f"CONTROL  : {control_path} -> {'OK' if control_path.exists() else 'MISSING'}")
    print(f"HUD      : {hud_path} -> {'OK' if hud_path.exists() else 'MISSING'}")
    print(f"STATIC   : {STATIC_DIR}")
    print("Control       : http://127.0.0.1:8040/control")
    print("Projection HUD: http://127.0.0.1:8040/hud")
    print("Version JSON  : http://127.0.0.1:8040/api/coach/version")
    print("=" * 72)
    app.run(host="0.0.0.0", port=8040, debug=False, use_reloader=False)
