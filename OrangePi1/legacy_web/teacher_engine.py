from __future__ import annotations

import json
import math
import sqlite3
import statistics
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable


def _finite(value: Any) -> float | None:
    try:
        number = float(value)
        return number if math.isfinite(number) else None
    except Exception:
        return None


@dataclass
class NotePerformance:
    index: int
    note: str = ""
    measure: int = 0
    beat: float = 0.0
    beats: float = 1.0
    string_id: int = 0
    finger: int = -1
    entered_at: float = 0.0
    completed_at: float = 0.0

    cents_samples: list[float] = field(default_factory=list)
    valid_samples: int = 0
    outside_samples: int = 0
    failure_count: int = 0
    manual_skip: bool = False
    _bad_latch: bool = False

    rhythm_mode: str = ""
    rhythm_target_ms: int = 0
    rhythm_onset_ms: int = -1
    rhythm_onset_detected: bool = False
    rhythm_timing_status: str = ""
    rhythm_clock_driven: bool = False
    rhythm_sounding_ms: int = 0
    rhythm_correct_ms: int = 0
    rhythm_mean_cents: float | None = None
    rhythm_mean_abs_cents: float | None = None
    rhythm_stability_cents: float | None = None
    rhythm_pitch_samples: int = 0
    rhythm_tolerance_ms: int = 140

    def add_cents(self, cents: float, tolerance: float) -> None:
        if not math.isfinite(cents):
            return
        self.valid_samples += 1
        self.cents_samples.append(float(cents))
        if len(self.cents_samples) > 180:
            del self.cents_samples[: len(self.cents_samples) - 180]

        bad = abs(cents) > max(8.0, tolerance)
        if bad:
            self.outside_samples += 1
            if not self._bad_latch:
                self.failure_count += 1
                self._bad_latch = True
        elif abs(cents) <= max(5.0, tolerance * 0.60):
            self._bad_latch = False

    def apply_rhythm_event(self, event: dict[str, Any], tolerance_ms: int = 140) -> None:
        self.rhythm_onset_detected = bool(event.get("onset_detected", int(event.get("onset_ms", -1)) >= 0))
        self.rhythm_timing_status = str(event.get("timing_status", ""))
        self.rhythm_clock_driven = event.get("boundary_reason") == "score_clock"
        self.rhythm_mode = str(event.get("mode") or self.rhythm_mode or "")
        self.rhythm_target_ms = max(0, int(event.get("target_ms") or 0))
        self.rhythm_onset_ms = int(event.get("onset_ms") if event.get("onset_ms") is not None else -1)
        self.rhythm_sounding_ms = max(0, int(event.get("sounding_ms") or 0))
        self.rhythm_correct_ms = max(0, int(event.get("correct_ms") or 0))
        self.rhythm_mean_cents = _finite(event.get("mean_cents"))
        self.rhythm_mean_abs_cents = _finite(event.get("mean_abs_cents"))
        self.rhythm_stability_cents = _finite(event.get("stability_cents"))
        self.rhythm_pitch_samples = max(0, int(event.get("pitch_samples") or 0))
        self.rhythm_tolerance_ms = max(60, min(300, int(tolerance_ms or 140)))

    def metrics(self, now: float | None = None, session_mode: str = "pitch") -> dict[str, Any]:
        now = now or time.time()
        samples = self.cents_samples
        mean = statistics.fmean(samples) if samples else self.rhythm_mean_cents
        mean_abs = statistics.fmean(abs(x) for x in samples) if samples else self.rhythm_mean_abs_cents
        std = statistics.pstdev(samples) if len(samples) >= 2 else (
            0.0 if samples else self.rhythm_stability_cents
        )
        end = self.completed_at or now
        hit_ms = max(0.0, (end - self.entered_at) * 1000.0) if self.entered_at else None

        pitch_weakness = 0.0
        if mean_abs is not None:
            pitch_weakness += mean_abs * 1.20
        if std is not None:
            pitch_weakness += std * 0.80
        pitch_weakness += self.failure_count * 12.0
        if session_mode == "pitch" and hit_ms is not None and hit_ms > 700:
            pitch_weakness += min(35.0, (hit_ms - 700.0) / 80.0)
        if self.manual_skip:
            pitch_weakness += 45.0

        pitch_score = max(0.0, 100.0 - min(100.0, pitch_weakness * 1.25))

        rhythm_score = None
        rhythm_weakness = 0.0
        duration_error_ms = None
        duration_ratio = None
        onset_error_ms = None

        if self.rhythm_target_ms > 0:
            target = float(self.rhythm_target_ms)
            tol = float(self.rhythm_tolerance_ms)
            is_rest = self.note == "REST"

            if is_rest:
                # For a rest, silence is the "correct duration".
                bad_sound = min(target, float(self.rhythm_sounding_ms))
                duration_ratio = max(0.0, 1.0 - bad_sound / target)
                duration_error_ms = -bad_sound
                rhythm_weakness = min(100.0, (bad_sound / target) * 120.0)
                rhythm_score = max(0.0, 100.0 - rhythm_weakness)
            else:
                onset_error_ms = float(self.rhythm_onset_ms) if self.rhythm_onset_detected else None

                # Rhythm-only mode grades bow sounding time. Full mode grades time
                # occupied by the correct pitch, so pitch+rhythm are both meaningful.
                effective_ms = (
                    float(self.rhythm_sounding_ms)
                    if (self.rhythm_mode or session_mode) == "rhythm"
                    else float(self.rhythm_correct_ms)
                )
                duration_ratio = effective_ms / target if target > 0 else None
                duration_error_ms = effective_ms - target

                if onset_error_ms is None:
                    onset_penalty = 55.0
                else:
                    onset_penalty = min(45.0, abs(onset_error_ms) / max(1.0, tol) * 18.0)

                ratio_error = abs(effective_ms - target) / target
                duration_penalty = min(55.0, ratio_error * 90.0)

                # In full mode, missing correct-pitch occupancy is intentionally
                # reflected here; in rhythm mode any clear sound can satisfy duration.
                rhythm_weakness = min(100.0, onset_penalty + duration_penalty)
                rhythm_score = max(0.0, 100.0 - rhythm_weakness)

        if self.rhythm_clock_driven and self.note != "REST":
            onset_error_ms = float(self.rhythm_onset_ms) if self.rhythm_onset_detected else None
            rhythm_score = 0.0 if onset_error_ms is None else max(0.0, 100.0 - max(0.0, abs(onset_error_ms) - self.rhythm_tolerance_ms) * 0.25)
            rhythm_weakness = 100.0 - rhythm_score
            duration_error_ms = duration_ratio = None  # envelope occupancy is not bow duration

        mode = self.rhythm_mode or session_mode or "pitch"
        if mode == "rhythm" and rhythm_score is not None:
            weakness = rhythm_weakness * 1.10 + pitch_weakness * 0.25
        elif mode == "full" and rhythm_score is not None:
            weakness = pitch_weakness * 0.80 + rhythm_weakness * 0.85
        else:
            weakness = pitch_weakness

        return {
            "index": self.index,
            "note": self.note,
            "measure": self.measure,
            "beat": self.beat,
            "beats": self.beats,
            "string_id": self.string_id,
            "finger": self.finger,
            "valid_samples": self.valid_samples,
            "outside_samples": self.outside_samples,
            "mean_cents": None if mean is None else round(mean, 2),
            "mean_abs_cents": None if mean_abs is None else round(mean_abs, 2),
            "stability_cents": None if std is None else round(std, 2),
            "hit_time_ms": None if hit_ms is None else round(hit_ms),
            "failure_count": self.failure_count,
            "manual_skip": self.manual_skip,
            "practice_mode": mode,
            "pitch_score": round(pitch_score, 1),
            "rhythm_target_ms": self.rhythm_target_ms or None,
            "rhythm_onset_ms": self.rhythm_onset_ms if self.rhythm_onset_detected else None,
            "rhythm_sounding_ms": self.rhythm_sounding_ms if self.rhythm_target_ms else None,
            "rhythm_correct_ms": self.rhythm_correct_ms if self.rhythm_target_ms else None,
            "duration_error_ms": None if duration_error_ms is None else round(duration_error_ms),
            "duration_ratio": None if duration_ratio is None else round(duration_ratio, 3),
            "rhythm_score": None if rhythm_score is None else round(rhythm_score, 1),
            "weakness_score": round(weakness, 2),
        }


class TeacherHistory:
    def __init__(self, db_path: Path):
        self.db_path = Path(db_path)
        self._init_db()

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        return conn

    def _init_db(self) -> None:
        with self._connect() as conn:
            conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS practice_sessions (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    song_id TEXT NOT NULL DEFAULT '',
                    song_title TEXT NOT NULL DEFAULT '',
                    started_at REAL NOT NULL,
                    ended_at REAL NOT NULL,
                    practice_mode TEXT NOT NULL DEFAULT 'pitch',
                    summary_json TEXT NOT NULL DEFAULT '{}'
                );
                CREATE TABLE IF NOT EXISTS note_performance (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    session_id INTEGER NOT NULL,
                    note_index INTEGER NOT NULL,
                    note TEXT NOT NULL DEFAULT '',
                    measure INTEGER NOT NULL DEFAULT 0,
                    beat REAL NOT NULL DEFAULT 0,
                    beats REAL NOT NULL DEFAULT 1,
                    string_id INTEGER NOT NULL DEFAULT 0,
                    finger INTEGER NOT NULL DEFAULT -1,
                    valid_samples INTEGER NOT NULL DEFAULT 0,
                    outside_samples INTEGER NOT NULL DEFAULT 0,
                    mean_cents REAL,
                    mean_abs_cents REAL,
                    stability_cents REAL,
                    hit_time_ms REAL,
                    failure_count INTEGER NOT NULL DEFAULT 0,
                    manual_skip INTEGER NOT NULL DEFAULT 0,
                    rhythm_target_ms REAL,
                    rhythm_onset_ms REAL,
                    rhythm_sounding_ms REAL,
                    rhythm_correct_ms REAL,
                    duration_error_ms REAL,
                    rhythm_score REAL,
                    pitch_score REAL,
                    weakness_score REAL NOT NULL DEFAULT 0,
                    FOREIGN KEY(session_id) REFERENCES practice_sessions(id) ON DELETE CASCADE
                );
                CREATE INDEX IF NOT EXISTS idx_practice_song ON practice_sessions(song_title, ended_at DESC);
                CREATE INDEX IF NOT EXISTS idx_note_perf_session ON note_performance(session_id, note_index);
                """
            )

            # In-place migration from V10.2/V10.3 databases.
            session_cols = {row[1] for row in conn.execute("PRAGMA table_info(practice_sessions)")}
            if "practice_mode" not in session_cols:
                conn.execute(
                    "ALTER TABLE practice_sessions ADD COLUMN practice_mode TEXT NOT NULL DEFAULT 'pitch'"
                )

            note_cols = {row[1] for row in conn.execute("PRAGMA table_info(note_performance)")}
            migrations = {
                "beats": "REAL NOT NULL DEFAULT 1",
                "rhythm_target_ms": "REAL",
                "rhythm_onset_ms": "REAL",
                "rhythm_sounding_ms": "REAL",
                "rhythm_correct_ms": "REAL",
                "duration_error_ms": "REAL",
                "rhythm_score": "REAL",
                "pitch_score": "REAL",
            }
            for name, declaration in migrations.items():
                if name not in note_cols:
                    conn.execute(f"ALTER TABLE note_performance ADD COLUMN {name} {declaration}")

    def save_session(
        self,
        *,
        song_id: str,
        song_title: str,
        practice_mode: str,
        started_at: float,
        ended_at: float,
        summary: dict[str, Any],
        records: list[dict[str, Any]],
    ) -> int:
        with self._connect() as conn:
            cur = conn.execute(
                """
                INSERT INTO practice_sessions(
                    song_id,song_title,started_at,ended_at,practice_mode,summary_json
                ) VALUES(?,?,?,?,?,?)
                """,
                (
                    song_id,
                    song_title,
                    started_at,
                    ended_at,
                    practice_mode,
                    json.dumps(summary, ensure_ascii=False),
                ),
            )
            session_id = int(cur.lastrowid)
            conn.executemany(
                """
                INSERT INTO note_performance(
                    session_id,note_index,note,measure,beat,beats,string_id,finger,
                    valid_samples,outside_samples,mean_cents,mean_abs_cents,
                    stability_cents,hit_time_ms,failure_count,manual_skip,
                    rhythm_target_ms,rhythm_onset_ms,rhythm_sounding_ms,
                    rhythm_correct_ms,duration_error_ms,rhythm_score,pitch_score,
                    weakness_score
                ) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
                """,
                [
                    (
                        session_id,
                        int(r.get("index", 0)),
                        str(r.get("note", "")),
                        int(r.get("measure", 0) or 0),
                        float(r.get("beat", 0) or 0),
                        float(r.get("beats", 1) or 1),
                        int(r.get("string_id", 0) or 0),
                        int(r.get("finger", -1) if r.get("finger") is not None else -1),
                        int(r.get("valid_samples", 0) or 0),
                        int(r.get("outside_samples", 0) or 0),
                        r.get("mean_cents"),
                        r.get("mean_abs_cents"),
                        r.get("stability_cents"),
                        r.get("hit_time_ms"),
                        int(r.get("failure_count", 0) or 0),
                        1 if r.get("manual_skip") else 0,
                        r.get("rhythm_target_ms"),
                        r.get("rhythm_onset_ms"),
                        r.get("rhythm_sounding_ms"),
                        r.get("rhythm_correct_ms"),
                        r.get("duration_error_ms"),
                        r.get("rhythm_score"),
                        r.get("pitch_score"),
                        float(r.get("weakness_score", 0) or 0),
                    )
                    for r in records
                ],
            )
        return session_id

    def recent_sessions(self, limit: int = 12) -> list[dict[str, Any]]:
        limit = max(1, min(int(limit), 100))
        with self._connect() as conn:
            rows = conn.execute(
                "SELECT * FROM practice_sessions ORDER BY ended_at DESC LIMIT ?", (limit,)
            ).fetchall()
        result = []
        for row in rows:
            item = dict(row)
            try:
                item["summary"] = json.loads(item.pop("summary_json") or "{}")
            except Exception:
                item["summary"] = {}
            result.append(item)
        return result


class TeacherEngine:
    """Pedagogical layer above the unchanged HPS/YIN detector.

    V10.4 adds rhythm scoring from ESP32 `vc=rhythm_note` events while preserving
    V10.3.2's protected review protocol. Pitch mode remains the fast one-note-at-a-time
    trainer; rhythm/full modes follow the score timeline.
    """

    def __init__(self, db_path: Path):
        self._lock = threading.RLock()
        self.history = TeacherHistory(db_path)
        self._sender: Callable[[str], None] | None = None
        self.reset_all()

    def set_command_sender(self, sender: Callable[[str], None]) -> None:
        self._sender = sender

    def reset_all(self) -> None:
        with getattr(self, "_lock", threading.RLock()):
            self.session_active = False
            self.session_started_at = 0.0
            self.session_id = None
            self.song_id = ""
            self.song_title = ""
            self.practice_mode = "pitch"
            self.score_notes: list[dict[str, Any]] = []
            self.original_tempo = 90.0
            self.records: dict[int, NotePerformance] = {}
            self.current_index: int | None = None
            self.current_entered_at = 0.0
            self.last_song_active = False
            self.last_song_index = 0
            self.last_song_action_seq = -1
            self.session_song_run_seq = -1
            self.last_rhythm_event_seq = -1
            self.last_intent_seq = -1
            self.pending_voice_action: tuple[str, float] | None = None
            self.teacher_text = "准备好以后开始练习"
            self.teacher_subtext = "可选择音准、节奏或完整演奏模式。"
            self.teacher_level = "neutral"
            self.rhythm_feedback_text = ""
            self.rhythm_feedback_subtext = ""
            self.rhythm_feedback_level = "neutral"
            self.rhythm_feedback_until = 0.0
            self.summary: dict[str, Any] = {}
            self.phase = "idle"
            self.review_deadline = 0.0
            self.review_segment: dict[str, Any] | None = None
            self.review_round = 0
            self.review_rounds_total = 2
            self.review_speed_ratio = 0.70
            self.review_last_restart_at = 0.0
            self._review_start_index = -1
            self._review_end_index = -1
            self._review_last_index = -1
            self._last_state: dict[str, Any] = {}

    @staticmethod
    def _cents_from_state(state: dict[str, Any]) -> float | None:
        value = _finite(state.get("pitch_cents"))
        if value is not None:
            return value
        actual = _finite(state.get("pitch_hz"))
        target = _finite(state.get("target_hz"))
        if actual and target and actual > 0 and target > 0:
            return 1200.0 * math.log2(actual / target)
        return None

    def _record_for(self, index: int, state: dict[str, Any], now: float) -> NotePerformance:
        rec = self.records.get(index)
        if rec is None:
            event = self.score_notes[index] if 0 <= index < len(self.score_notes) else {}
            rec = NotePerformance(
                index=index,
                note=str(event.get("note") or state.get("target_note") or ""),
                measure=int(event.get("measure", state.get("target_measure", 0)) or 0),
                beat=float(event.get("beat", state.get("target_beat", 0)) or 0),
                beats=float(event.get("beats", 1) or 1),
                string_id=int(event.get("string_id", state.get("target_string_id", 0)) or 0),
                finger=int(
                    event.get("finger", state.get("target_finger", -1))
                    if event.get("finger", state.get("target_finger", -1)) is not None
                    else -1
                ),
                entered_at=now,
            )
            self.records[index] = rec
        return rec

    def _start_session(self, state: dict[str, Any], now: float) -> None:
        self.session_active = True
        self.session_started_at = now
        self.session_id = None
        self.song_id = str(state.get("song_id") or "")
        self.song_title = str(state.get("song_title") or "当前乐曲")
        self.practice_mode = str(state.get("practice_mode") or "pitch")
        self.score_notes = list(state.get("score_notes") or state.get("song_notes") or [])
        self.original_tempo = float(state.get("tempo_bpm") or 90.0)
        self.records = {}
        self.current_index = int(state.get("song_index") or 0)
        self.current_entered_at = now
        self.session_song_run_seq = int(state.get("song_run_seq") or 0)
        existing = [int(e.get("seq") or 0) for e in state.get("rhythm_events", []) if int(e.get("run_seq", -1)) == self.session_song_run_seq]
        self.last_rhythm_event_seq = min(existing)-1 if existing else int(state.get("rhythm_event_seq") or 0)
        self.summary = {}
        self.phase = "playing"
        self.review_segment = None
        self.review_round = 0
        self._review_start_index = -1
        self._review_end_index = -1
        self._record_for(self.current_index, state, now)

    def _finalize_index(self, index: int | None, now: float, *, manual_skip: bool = False) -> None:
        if index is None:
            return
        rec = self.records.get(index)
        if rec is None:
            return
        if not rec.completed_at:
            rec.completed_at = now
        if manual_skip:
            rec.manual_skip = True

    def _consume_rhythm_event(self, state: dict[str, Any], now: float) -> None:
        events = list(state.get("rhythm_events") or [])
        if not events:
            event = dict(state.get("last_rhythm_event") or {})
            if event:
                events = [dict(event, seq=int(state.get("rhythm_event_seq") or 0))]
        for event in events:
            seq = int(event.get("seq") or 0)
            if seq <= self.last_rhythm_event_seq:
                continue
            self.last_rhythm_event_seq = seq
            if "run_seq" in event and int(event["run_seq"]) != self.session_song_run_seq:
                continue
            index = int(event.get("index") or 0)
            rec = self._record_for(index, state, now)
            rec.apply_rhythm_event(event, int(state.get("rhythm_tolerance_ms") or 140))
            self._set_rhythm_feedback(rec, event)

    def _set_rhythm_feedback(self, rec: NotePerformance, event: dict[str, Any]) -> None:
        target = max(1, rec.rhythm_target_ms)
        tol = max(60, rec.rhythm_tolerance_ms)
        mode = rec.rhythm_mode or self.practice_mode
        note = rec.note

        if event.get("boundary_reason") == "score_clock" and note != "REST":
            status = str(event.get("timing_status", ""))
            self.rhythm_feedback_text = {"good":"起音跟上拍点", "early":"起音稍早", "late":"起音稍晚", "missed":"此音未检测到独立起弓"}.get(status,"继续跟拍")
            self.rhythm_feedback_subtext = "漏拉不会停谱；同音需重新起弓，连奏以稳定换音为参考。"
            self.rhythm_feedback_level = "good" if status == "good" else "warn"
            self.rhythm_feedback_until = time.time() + 0.85
            return
        if note == "REST":
            noise = rec.rhythm_sounding_ms
            if noise <= tol:
                text = "休止很好，静音干净。"
                sub = f"目标休止 {target} ms。"
                level = "good"
            else:
                text = "休止符里还有声音，收弓再干净一点。"
                sub = f"休止中检测到约 {noise} ms 的声音。"
                level = "warn"
        else:
            onset = rec.rhythm_onset_ms
            effective = rec.rhythm_sounding_ms if mode == "rhythm" else rec.rhythm_correct_ms
            duration_error = effective - target

            if abs(onset) > tol * 1.5:
                if onset < 0:
                    text = f"{note} 起音早了约 {abs(onset)} ms。"
                    sub = "稍微等到拍点再起弓。"
                else:
                    text = f"{note} 起音晚了约 {onset} ms。"
                    sub = "眼睛提前看下一音，在拍点上起弓。"
                level = "warn"
            elif duration_error < -max(tol, int(target * 0.18)):
                text = f"{note} 时值偏短。"
                sub = f"目标 {target} ms，当前有效约 {effective} ms。"
                level = "warn"
            elif abs(duration_error) <= max(tol, int(target * 0.15)):
                text = "节奏很好，起音和时值都比较稳。"
                sub = f"{note} · 目标 {target} ms · 误差 {duration_error:+d} ms。"
                level = "good"
            else:
                text = f"{note} 时值偏长或换音偏慢。"
                sub = f"目标 {target} ms，当前有效约 {effective} ms。"
                level = "warn"

        self.rhythm_feedback_text = text
        self.rhythm_feedback_subtext = sub
        self.rhythm_feedback_level = level
        self.rhythm_feedback_until = time.time() + 0.85

    def _build_summary(self, now: float) -> dict[str, Any]:
        for index, event in enumerate(self.score_notes):
            if index not in self.records:
                self.records[index] = NotePerformance(
                    index=index,
                    note=str(event.get("note") or ""),
                    measure=int(event.get("measure", 0) or 0),
                    beat=float(event.get("beat", 0) or 0),
                    beats=float(event.get("beats", 1) or 1),
                    string_id=int(event.get("string_id", 0) or 0),
                    finger=int(event.get("finger", -1) if event.get("finger") is not None else -1),
                    entered_at=0.0,
                )

        metrics = [
            self.records[i].metrics(now, self.practice_mode)
            for i in sorted(self.records)
        ]
        ranked = sorted(metrics, key=lambda x: float(x.get("weakness_score") or 0), reverse=True)
        top_notes = [x for x in ranked if x.get("note") and x.get("note") != "REST"][:3]

        pitch_ranked = sorted(
            [x for x in metrics if x.get("mean_abs_cents") is not None],
            key=lambda x: float(x.get("pitch_score") or 100),
        )
        rhythm_ranked = sorted(
            [x for x in metrics if x.get("rhythm_score") is not None],
            key=lambda x: float(x.get("rhythm_score") or 100),
        )

        by_measure: dict[int, list[dict[str, Any]]] = {}
        for item in metrics:
            m = int(item.get("measure") or 0)
            if m > 0:
                by_measure.setdefault(m, []).append(item)
        measure_scores = []
        for measure, items in by_measure.items():
            scores = [float(x.get("weakness_score") or 0) for x in items]
            rhythm_scores = [float(x["rhythm_score"]) for x in items if x.get("rhythm_score") is not None]
            pitch_scores = [float(x["pitch_score"]) for x in items if x.get("pitch_score") is not None]
            measure_scores.append({
                "measure": measure,
                "score": round(statistics.fmean(scores), 2) if scores else 0.0,
                "rhythm_score": round(statistics.fmean(rhythm_scores), 1) if rhythm_scores else None,
                "pitch_score": round(statistics.fmean(pitch_scores), 1) if pitch_scores else None,
                "note_count": len(items),
            })
        measure_scores.sort(key=lambda x: x["score"], reverse=True)

        valid_abs = [float(x["mean_abs_cents"]) for x in metrics if x.get("mean_abs_cents") is not None]
        overall_abs = statistics.fmean(valid_abs) if valid_abs else None
        rhythm_values = [float(x["rhythm_score"]) for x in metrics if x.get("rhythm_score") is not None]
        pitch_values = [float(x["pitch_score"]) for x in metrics if x.get("pitch_score") is not None]
        stability_values = [
            max(0.0, 100.0 - float(x["stability_cents"]) * 3.0)
            for x in metrics
            if x.get("stability_cents") is not None
        ]

        pitch_score = statistics.fmean(pitch_values) if pitch_values else None
        rhythm_score = statistics.fmean(rhythm_values) if rhythm_values else None
        stability_score = statistics.fmean(stability_values) if stability_values else None

        if self.practice_mode == "rhythm":
            overall_parts = [x for x in [rhythm_score, pitch_score] if x is not None]
            overall_score = (
                (rhythm_score * 0.80 + pitch_score * 0.20)
                if rhythm_score is not None and pitch_score is not None
                else (statistics.fmean(overall_parts) if overall_parts else None)
            )
        elif self.practice_mode == "full":
            parts = [x for x in [pitch_score, rhythm_score, stability_score] if x is not None]
            overall_score = statistics.fmean(parts) if parts else None
        else:
            parts = [x for x in [pitch_score, stability_score] if x is not None]
            overall_score = statistics.fmean(parts) if parts else None

        return {
            "song_title": self.song_title,
            "song_id": self.song_id,
            "practice_mode": self.practice_mode,
            "started_at": self.session_started_at,
            "ended_at": now,
            "duration_s": round(max(0.0, now - self.session_started_at), 1),
            "note_count": len(metrics),
            "mean_abs_cents": None if overall_abs is None else round(overall_abs, 2),
            "pitch_score": None if pitch_score is None else round(pitch_score, 1),
            "rhythm_score": None if rhythm_score is None else round(rhythm_score, 1),
            "stability_score": None if stability_score is None else round(stability_score, 1),
            "overall_score": None if overall_score is None else round(overall_score, 1),
            "manual_skips": sum(1 for x in metrics if x.get("manual_skip")),
            "failure_count": sum(int(x.get("failure_count") or 0) for x in metrics),
            "top_notes": top_notes,
            "top_pitch_notes": pitch_ranked[:3],
            "top_rhythm_notes": rhythm_ranked[:3],
            "top_measures": measure_scores[:3],
            "records": metrics,
        }

    def _choose_review_segment(self) -> dict[str, Any] | None:
        if not self.summary:
            return None
        records = self.summary.get("records") or []
        if not records:
            return None
        worst_note_score = max(float(r.get("weakness_score") or 0.0) for r in records)
        if worst_note_score < 12.0:
            return None

        top_measures = self.summary.get("top_measures") or []
        if top_measures:
            worst_measure = int(top_measures[0]["measure"])
            candidate_indices = [
                int(r["index"]) for r in records
                if int(r.get("measure") or 0) in {worst_measure, worst_measure + 1}
            ]
        else:
            worst = max(records, key=lambda r: float(r.get("weakness_score") or 0))
            idx = int(worst["index"])
            candidate_indices = list(range(max(0, idx - 2), min(len(self.score_notes), idx + 5)))
            worst_measure = int(worst.get("measure") or 0)

        if not candidate_indices:
            return None
        start = max(0, min(candidate_indices))
        end = min(len(self.score_notes) - 1, max(candidate_indices))
        if end - start + 1 > 12:
            end = start + 11
        return {
            "start_index": start,
            "end_index": end,
            "measure": worst_measure,
            "rounds": self.review_rounds_total,
            "speed_ratio": self.review_speed_ratio,
        }

    def _finish_session(self, now: float, *, completed: bool = True) -> None:
        if not self.session_active:
            return
        self._finalize_index(self.current_index, now)
        self.summary = self._build_summary(now)
        try:
            self.session_id = self.history.save_session(
                song_id=self.song_id,
                song_title=self.song_title,
                practice_mode=self.practice_mode,
                started_at=self.session_started_at,
                ended_at=now,
                summary={k: v for k, v in self.summary.items() if k != "records"},
                records=self.summary.get("records") or [],
            )
        except Exception:
            self.session_id = None

        self.session_active = False
        self.review_segment = self._choose_review_segment() if completed else None
        if self.review_segment:
            self.phase = "review_done"
            self.review_deadline = 0.0
            m = self.review_segment.get("measure")
            self.teacher_text = (
                f"本曲完成。第 {m} 小节最需要巩固，可点“开始强化”慢速复习。"
                if m else "本曲完成。可点“开始强化”复习弱点。"
            )
            self.teacher_subtext = "保留完成位置，只有明确选择强化才会回到弱点段。"
            self.teacher_level = "review"
        elif completed:
            self.phase = "review_done"
            self.teacher_text = "本曲完成，表现稳定。"
            self.teacher_subtext = "音准和节奏总结已经保存。"
            self.teacher_level = "good"
        else:
            self.phase = "idle"
            self.teacher_text = "练习已结束，本次记录已保存。"
            self.teacher_subtext = "手动结束不会自动进入弱点强化。"
            self.teacher_level = "neutral"

    def _send(self, command: str) -> bool:
        if not self._sender:
            return False
        try:
            self._sender(command)
            return True
        except Exception:
            return False

    def start_review(self) -> tuple[bool, str]:
        with self._lock:
            if not self.review_segment:
                return False, "当前没有可强化的弱点片段"
            start = int(self.review_segment["start_index"])
            end = int(self.review_segment["end_index"])
            tempo = max(40.0, min(220.0, self.original_tempo * self.review_speed_ratio))
            if not self._send(f"TEMPO:{tempo:.1f}"):
                return False, "ESP32 尚未连接，无法开始强化"
            if not self._send(f"REVIEW_BEGIN:{start}"):
                return False, "无法跳转到弱点片段"
            self.phase = "review"
            self.review_round = 1
            self._review_start_index = start
            self._review_end_index = end
            self._review_last_index = start
            self.review_last_restart_at = time.time()
            self.teacher_text = f"弱点强化第 1/{self.review_rounds_total} 轮：慢速练习第 {self.review_segment.get('measure') or '?'} 小节附近。"
            self.teacher_subtext = "拉完这段会自动再练一轮；也可以随时说“跳过强化”。"
            self.teacher_level = "review"
            return True, "弱点强化已开始"

    def skip_review(self) -> tuple[bool, str]:
        with self._lock:
            if self.phase == "review":
                self._send("REVIEW_END")
                self._send(f"TEMPO:{self.original_tempo:.1f}")
                self._send("PAUSE")
            self.phase = "review_skipped"
            self.review_deadline = 0.0
            self.teacher_text = "已跳过弱点强化。"
            self.teacher_subtext = "可以选择下一首曲目，或稍后从历史记录重新练习。"
            self.teacher_level = "neutral"
            return True, "已跳过强化"

    def _handle_voice_intent(self, state: dict[str, Any]) -> None:
        seq = int(state.get("last_intent_seq", -1) or -1)
        if seq == self.last_intent_seq:
            return
        self.last_intent_seq = seq
        intent = state.get("last_intent") or {}
        kind = str(intent.get("kind") or "")
        if kind in {"review_start", "review_skip"}:
            self.pending_voice_action = (kind, time.time() + 0.75)

    def _teacher_feedback(self, state: dict[str, Any], now: float) -> None:
        if self.phase in {"review_countdown", "review_done", "review_skipped"}:
            return

        trainer_phase = state.get("trainer_phase")
        if self.practice_mode in {"rhythm", "full"} and trainer_phase in {"count_in","playing","phrase_end"}:
            if trainer_phase == "count_in":
                self.teacher_text = "预备拍：听完一小节节拍，再从绿色目标音起弓。"
                self.teacher_subtext = "跟拍中谱子按时间推进，音不准或漏拉也会继续。"
                self.teacher_level = "neutral"
            elif trainer_phase == "phrase_end":
                self.teacher_text = f"本段结束：跟上 {state.get('phrase_good',0)}，早 {state.get('phrase_early',0)}，晚 {state.get('phrase_late',0)}，未检测到 {state.get('phrase_missed',0)}。"
                self.teacher_subtext = "重练本段 / 慢速重练 / 下一段。" if state.get("phrase_has_next") else "已到最后一段，可重练、明确重新开始或结束。"
                self.teacher_level = "neutral"
            else:
                self.teacher_text = "跟着拍点看谱起弓，注意下一音。"
                self.teacher_subtext = "时间线持续推进；起音检测为初版，弱弓和同音连弓可能显示未检测到。"
                self.teacher_level = "neutral"
            return

        if now < self.rhythm_feedback_until and self.practice_mode in {"rhythm", "full"}:
            self.teacher_text = self.rhythm_feedback_text
            self.teacher_subtext = self.rhythm_feedback_subtext
            self.teacher_level = self.rhythm_feedback_level
            return

        if state.get("release_required"):
            self.teacher_text = "这是重复音：先释放一点，再重新起弓。"
            self.teacher_subtext = "系统已经识别到前一个音。"
            self.teacher_level = "warn"
            return

        note = str(state.get("target_note") or "")
        if note == "REST":
            self.teacher_text = "休止：保持安静，眼睛提前看下一音。"
            self.teacher_subtext = "跟着节拍准备下一次起弓。"
            self.teacher_level = "neutral"
            return
        if not note:
            self.teacher_text = "选择曲目后开始练习。"
            self.teacher_subtext = "音准 / 节奏 / 完整三种模式都可用。"
            self.teacher_level = "neutral"
            return

        try:
            fresh = state.get("pitch_age_s") is not None and float(state.get("pitch_age_s")) < 0.8
        except Exception:
            fresh = False
        cents = self._cents_from_state(state) if fresh else None

        if self.practice_mode in {"rhythm", "full"}:
            elapsed = float(state.get("rhythm_elapsed_ms") or 0)
            target_ms = float(state.get("rhythm_target_ms") or 0)
            started = bool(state.get("rhythm_started"))

            if not started:
                if state.get("release_required"):
                    self.teacher_text = f"{note} 是重复音：重新起弓后才开始计时。"
                    self.teacher_subtext = "一直保持上一音不会让乐谱自动前进。"
                    self.teacher_level = "warn"
                else:
                    self.teacher_text = f"等待 {note} 起音：你不拉，乐谱不会往后走。"
                    self.teacher_subtext = f"跟着节拍起弓；起音后才开始计算约 {target_ms:.0f} ms 的时值。"
                    self.teacher_level = "neutral"
                return

            if cents is None:
                self.teacher_text = f"{note} 已经开始计时，保持到目标时值。"
                self.teacher_subtext = f"当前约 {elapsed:.0f} / {target_ms:.0f} ms。"
                self.teacher_level = "warn"
                return

        if cents is None:
            self.teacher_text = f"目标 {note}：听你的起音。"
            self.teacher_subtext = "先把音拉出来，不要为了追绿点而急着滑动手指。"
            self.teacher_level = "neutral"
            return

        rec = self.records.get(int(state.get("song_index") or 0))
        recent = rec.cents_samples[-10:] if rec else []
        stability = statistics.pstdev(recent) if len(recent) >= 3 else None
        abs_c = abs(cents)

        if abs_c <= 5.0 and (stability is None or stability <= 7.0):
            if self.practice_mode in {"rhythm", "full"}:
                self.teacher_text = "音准到位，继续保持到节拍线。"
                self.teacher_subtext = f"当前误差 {cents:+.1f} cents。"
            else:
                self.teacher_text = "很好，音准稳定。保持这个手指位置。"
                self.teacher_subtext = f"当前误差 {cents:+.1f} cents。"
            self.teacher_level = "good"
        elif abs_c <= 12.0:
            self.teacher_text = "音高基本正确，先稳住，不要过度修正。"
            self.teacher_subtext = f"当前误差 {cents:+.1f} cents。"
            self.teacher_level = "good"
        elif abs_c <= float(state.get("tolerance_cents") or 30):
            direction = "向琴枕方向" if cents > 0 else "向琴桥方向"
            self.teacher_text = f"{'偏高' if cents > 0 else '偏低'}，手指{direction}微调一点。"
            self.teacher_subtext = f"当前误差 {cents:+.1f} cents。"
            self.teacher_level = "warn"
        else:
            direction = "向琴枕方向" if cents > 0 else "向琴桥方向"
            self.teacher_text = "音高差得较多，确认弦和指位。"
            self.teacher_subtext = f"误差 {cents:+.1f} cents；手指需要{direction}修正。"
            self.teacher_level = "bad"

        if stability is not None and stability > 16 and abs_c <= 18:
            self.teacher_text = "音高在目标附近，但还不够稳定。先把手指固定住。"
            self.teacher_subtext = f"最近波动约 {stability:.1f} cents。"
            self.teacher_level = "warn"

    def observe(self, state: dict[str, Any]) -> dict[str, Any]:
        now = time.time()
        with self._lock:
            self._last_state = dict(state)
            single = state.get("app_state") == "single" or (
                state.get("app_state") in {"paused", "voice", "cloud"} and state.get("resume_state") == "single")
            if single:
                if self.session_active:
                    self._finish_session(now, completed=False)
                self.pending_voice_action = None
                self.review_deadline = 0.0
                self.review_segment = None
                self.phase = "idle"
                self.teacher_text = "单音练习：" + str(state.get("target_note") or "等待目标音")
                self.teacher_subtext = "保持当前音；说新的目标音即可切换，不会自动进入下一音。"
                self.teacher_level = "neutral"
                return self.snapshot()
            self._handle_voice_intent(state)

            if self.pending_voice_action and now >= self.pending_voice_action[1]:
                action = self.pending_voice_action[0]
                self.pending_voice_action = None
                if action == "review_start":
                    self.start_review()
                elif action == "review_skip":
                    self.skip_review()

            song_active = bool(state.get("song_active"))
            index = int(state.get("song_index") or 0)
            score_notes = list(state.get("score_notes") or state.get("song_notes") or [])

            run = int(state.get("song_run_seq") or 0)
            if song_active and run != self.session_song_run_seq:
                if self.session_active:
                    self._finish_session(now, completed=False)
                self.pending_voice_action = None
                self.review_deadline = 0.0
                self.review_segment = None
                self.phase = "idle"
                self._start_session(state, now)

            if song_active and not self.session_active and self.phase != "review":
                self._start_session(state, now)
            elif song_active and self.session_active:
                if score_notes:
                    self.score_notes = score_notes
                if self.current_index is None:
                    self.current_index = index
                    self.current_entered_at = now
                elif index != self.current_index:
                    manual_skip = False
                    try:
                        command_age = now - float(state.get("last_web_command_at") or 0.0)
                        manual_skip = (
                            str(state.get("last_web_command") or "") == "NEXT"
                            and 0 <= command_age < 1.2
                        )
                    except Exception:
                        pass
                    self._finalize_index(self.current_index, now, manual_skip=manual_skip)
                    self.current_index = index
                    self.current_entered_at = now
                    self._record_for(index, state, now)

                if self.phase != "review":
                    rec = self._record_for(index, state, now)
                    try:
                        fresh = (
                            state.get("pitch_age_s") is not None
                            and float(state.get("pitch_age_s")) < 0.75
                        )
                    except Exception:
                        fresh = False
                    cents = self._cents_from_state(state) if fresh else None
                    if cents is not None and str(state.get("target_note") or "") != "REST":
                        rec.add_cents(cents, float(state.get("tolerance_cents") or 30.0))

            # Consume a completed rhythm event once even though /control and /hud
            # may both poll the same backend.
            self._consume_rhythm_event(state, now)

            # V10.3.2 stability rule retained: only a new explicit done/stop event
            # from the same run can finish a session.
            action_seq = int(state.get("last_song_action_seq") or 0)
            if action_seq != self.last_song_action_seq:
                self.last_song_action_seq = action_seq
                action = str(state.get("last_song_action") or "")
                run_seq = int(state.get("song_run_seq") or 0)
                count = int(state.get("song_count") or 0)
                same_run = self.session_active and run_seq == self.session_song_run_seq
                truly_at_end = count > 0 and index >= count - 1

                if same_run and action == "done" and truly_at_end:
                    self._finish_session(now, completed=True)
                elif same_run and action == "stop":
                    self._finish_session(now, completed=False)

            self.last_song_active = song_active
            self.last_song_index = index

            if self.phase == "review_countdown" and self.review_deadline and now >= self.review_deadline:
                self.start_review()

            if self.phase == "review":
                if index != self._review_last_index:
                    self._review_last_index = index
                reached_end = index > self._review_end_index
                ended_song = not song_active and self._review_end_index >= 0
                if (reached_end or ended_song) and now - self.review_last_restart_at > 0.45:
                    if self.review_round < self.review_rounds_total:
                        self.review_round += 1
                        self._send(f"REVIEW_SEEK:{self._review_start_index}")
                        self.review_last_restart_at = now
                        self.teacher_text = f"弱点强化第 {self.review_round}/{self.review_rounds_total} 轮：再来一次。"
                        self.teacher_subtext = "保持慢速；节奏模式继续跟拍，音准模式继续站稳每个音。"
                        self.teacher_level = "review"
                    else:
                        self._send("REVIEW_END")
                        self._send(f"TEMPO:{self.original_tempo:.1f}")
                        self._send("PAUSE")
                        self.phase = "review_done"
                        self.teacher_text = "弱点强化完成。"
                        self.teacher_subtext = "已经恢复原速度。"
                        self.teacher_level = "good"

            self._teacher_feedback(state, now)
            return self.snapshot_unlocked(now)

    def snapshot_unlocked(self, now: float | None = None) -> dict[str, Any]:
        now = now or time.time()
        countdown = None
        if self.phase == "review_countdown" and self.review_deadline:
            countdown = max(0.0, self.review_deadline - now)
        current_record = None
        if self.current_index is not None and self.current_index in self.records:
            current_record = self.records[self.current_index].metrics(now, self.practice_mode)
        return {
            "phase": self.phase,
            "practice_mode": self.practice_mode,
            "teacher_text": self.teacher_text,
            "teacher_subtext": self.teacher_subtext,
            "teacher_level": self.teacher_level,
            "session_active": self.session_active,
            "session_id": self.session_id,
            "review_countdown_s": None if countdown is None else round(countdown, 1),
            "review_segment": self.review_segment,
            "review_round": self.review_round,
            "review_rounds_total": self.review_rounds_total,
            "summary": self.summary,
            "current_record": current_record,
        }

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return self.snapshot_unlocked()
