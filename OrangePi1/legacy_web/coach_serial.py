from __future__ import annotations

import json
import re
import threading
import time
from copy import deepcopy
from typing import Any

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None


class CoachSerialBridge:
    """Read ESP32 telemetry and expose a stable Web/AR state snapshot."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._serial = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._tx_lock = threading.Lock()
        self._song_resolver = None
        self._upload_condition = threading.Condition()
        self._pending_upload = None
        self._upload_thread = None
        self._ack_condition = threading.Condition()
        self._score_acks = {}
        self._clock_received = 0.0
        self._state: dict[str, Any] = {
            "connected": False, "port": "", "baud": 115200, "error": "",
            "app_state": "unknown", "resume_state": "unknown",
            "target_note": "", "target_hz": 0.0,
            "target_string_id": 0, "target_finger": -1,
            "target_measure": 0, "target_beat": 0.0,
            "pitch_hz": None, "pitch_cents": None, "pitch_confidence": None,
            "song_active": False, "song_id": "", "song_title": "",
            "song_index": 0, "song_count": 0,
            "tempo_bpm": None, "tolerance_cents": None,
            "practice_mode": "pitch", "metronome_enabled": False,
            "firmware": "", "mode_control_supported": False, "control_rx_confirmed":False,
            "last_control_ack": {}, "last_control_ack_at": 0.0,
            "rhythm_tolerance_ms": 140,
            "rhythm_started": False,
            "rhythm_elapsed_beats": 0.0, "rhythm_elapsed_ms": 0,
            "rhythm_target_ms": 0, "song_clock_beats": 0.0,
            "rhythm_event_seq": 0, "last_rhythm_event": {}, "rhythm_events": [],
            "song_request_seq": 0, "last_song_request": {},
            "control_generation": 0, "time_num": 4, "time_den": 4, "trainer_phase": "idle",
            "last_rhythm_event_at": 0.0,
            "release_required": False, "prev_note": "", "next_note": "",
            "song_notes": [], "last_asr": "", "last_intent": {},
            "last_intent_at": 0.0, "last_intent_seq": 0,
            "voice_phase": "unknown", "voice_source": "", "voice_message": "",
            "voice_wake_model": "", "voice_event_at": 0.0,
            "last_song_action": "", "last_song_action_at": 0.0,
            "last_song_action_seq": 0, "song_run_seq": 0,
            "last_web_command": "", "last_web_command_at": 0.0,
            "last_raw": "", "last_message_at": 0.0, "last_pitch_at": 0.0,
        }

    @property
    def serial_available(self) -> bool:
        return serial is not None

    def list_ports(self) -> list[dict[str, str]]:
        if serial is None:
            return []
        return [{"device": p.device, "description": p.description or "", "hwid": p.hwid or ""}
                for p in serial.tools.list_ports.comports()]

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            result = deepcopy(self._state)
        now = time.time()
        result["pitch_age_s"] = None if not result["last_pitch_at"] else max(0.0, now-result["last_pitch_at"])
        result["message_age_s"] = None if not result["last_message_at"] else max(0.0, now-result["last_message_at"])
        result["serial_available"] = self.serial_available
        result["clock_age_ms"] = max(0.0,(time.monotonic()-self._clock_received)*1000) if self._clock_received else 0.0
        return result

    def connect(self, port: str, baud: int = 115200) -> None:
        if serial is None:
            raise RuntimeError("未安装 pyserial，请重新运行 01_install_web.bat")
        port = (port or "").strip()
        if not port:
            raise ValueError("请选择串口")
        self.disconnect()
        try:
            ser = serial.Serial(port=port, baudrate=int(baud), timeout=0.25)
        except Exception as exc:
            with self._lock:
                self._state["connected"] = False
                self._state["error"] = str(exc)
            raise
        self._serial = ser
        self._stop.clear()
        with self._lock:
            self._state.update({"connected": True, "port": port, "baud": int(baud), "error": "",
                "last_song_request":{}, "control_generation":0, "firmware":"", "control_rx_confirmed":False})
        self._thread = threading.Thread(target=self._read_loop, name="violin-coach-serial", daemon=True)
        self._thread.start()
        # Request score/state without restarting a song. Older firmware ignores
        # this command; V10.5.1 supports reconnecting during a performance.
        try:
            self.send_command("HOST_LIBRARY:ON")
            self.send_command("SYNC")
        except Exception as exc:
            with self._lock:
                self._state["error"] = str(exc)

    def send_command(self, command: str) -> None:
        """Send a control command to ESP32 over the already-open COM port.

        V10.4 supports both fixed commands and commands carrying a value/query:
        NEXT / PREVIOUS / PAUSE / CONTINUE / END
        VOICE (manual one-shot voice capture test)
        TEMPO_UP / TEMPO_DOWN / TEMPO_RESET / TEMPO:<bpm>
        MODE:PITCH / MODE:RHYTHM / MODE:FULL
        METRONOME:ON / METRONOME:OFF
        RHYTHM_TOLERANCE:<milliseconds>
        REVIEW_BEGIN:<zero-based-index> / REVIEW_SEEK:<zero-based-index> / REVIEW_END
        START_SONG:<title-or-slug>
        Plain SEEK is blocked by firmware V10.3.2.
        """
        raw = str(command or "").strip()
        if not raw:
            raise ValueError("控制命令不能为空")
        if "\n" in raw or "\r" in raw:
            raise ValueError("控制命令不能包含换行")

        upper = raw.upper()
        fixed = {
            "NEXT", "PREVIOUS", "PAUSE", "CONTINUE", "END", "VOICE",
            "PITCH", "RHYTHM", "FULL",
            "TEMPO_UP", "TEMPO_DOWN", "TEMPO_RESET", "REVIEW_END", "SYNC", "PHRASE_REPEAT", "PHRASE_SLOW_REPEAT", "PHRASE_NEXT", "RESTART",
        }
        variable_ok = (
            upper.startswith("TEMPO:")
            or upper.startswith("MODE:")
            or upper.startswith("METRONOME:")
            or upper.startswith("RHYTHM_TOLERANCE:")
            or upper.startswith("REVIEW_BEGIN:")
            or upper.startswith("REVIEW_SEEK:")
            or upper.startswith("START_SONG:")
            or upper.startswith("START_NOTE:")
            or upper.startswith("SCORE_")
            or upper.startswith("HOST_LIBRARY:")
            or upper.startswith("RHYTHM_OFFSET:")
        )
        if upper not in fixed and not variable_ok:
            raise ValueError(f"不支持的控制命令：{command}")

        # Keep the song query text untouched; firmware accepts UTF-8 stdin.
        wire_command = raw if upper.startswith(("START_SONG:","SCORE_")) else upper
        ser = self._serial
        if ser is None or not getattr(ser, "is_open", False):
            raise RuntimeError("ESP32 串口尚未连接")

        payload = f"VC_CMD:{wire_command}\n".encode("utf-8")
        with self._tx_lock:
            ser.write(payload)
            ser.flush()

        with self._lock:
            self._state["last_web_command"] = upper.split(":", 1)[0]
            self._state["last_web_command_at"] = time.time()

    def set_song_resolver(self, resolver) -> None:
        self._song_resolver = resolver

    def _queue_score(self, request: dict) -> None:
        with self._upload_condition:
            self._pending_upload=dict(request)
            if self._upload_thread is None:
                self._upload_thread=threading.Thread(target=self._upload_loop,daemon=True,name="violin-score-upload")
                self._upload_thread.start()
            self._upload_condition.notify_all()

    def _upload_loop(self) -> None:
        while True:
            with self._upload_condition:
                while self._pending_upload is None:
                    self._upload_condition.wait()
                req,self._pending_upload=self._pending_upload,None
            rid = int(req["request_id"])
            try:
                if self._song_resolver is None:
                    raise RuntimeError("未配置曲库解析器")
                song = self._song_resolver(str(req.get("query", "")), strict=True)
                if song is None:
                    raise ValueError("曲库没有这个准确标题或别名，请在曲库检查；没有猜测换成其他曲目")
                self._upload_score(rid, song)
            except Exception as exc:
                with self._lock:
                    current = self._state.get("last_song_request", {})
                    if int(current.get("request_id", -1)) != rid:
                        continue
                    self._state["error"] = f"换曲失败：{exc}"
                try:
                    self.send_command(f"SCORE_ABORT:{rid}")
                except Exception:
                    pass

    def _score_line(self, rid: int, stage: str, command: str, index: int = -1) -> None:
        key = (rid, stage, index)
        for attempt in range(3):
            with self._lock:
                current = self._state.get("last_song_request", {})
                if current.get("request_id") != rid or current.get("phase") not in {"loading", "applied", "unchanged"}:
                    raise RuntimeError("该换曲请求已被新操作取消")
            with self._ack_condition:
                self._score_acks.pop(key, None)
            self.send_command(command)
            # COMMIT includes a score snapshot; allow up to 8 s at 115200 baud.
            deadline = time.monotonic() + (8 if stage == "COMMIT" else 2)
            with self._ack_condition:
                while key not in self._score_acks and time.monotonic() < deadline:
                    self._ack_condition.wait(timeout=max(.001, deadline-time.monotonic()))
                ack = self._score_acks.pop(key, None)
            if ack is not None:
                if not ack:
                    raise RuntimeError(f"板子拒绝乐谱 {stage}，索引 {index}；原曲保留")
                return
        raise TimeoutError(f"板子未确认 {stage}，请检查串口连接")

    def _upload_score(self, rid: int, song: dict) -> None:
        notes = song.get("notes") or []
        if not 1 <= len(notes) <= 256:
            raise ValueError("乐谱音符数必须为1–256")
        title = str(song["title"]).encode("utf-8")
        slug = str(song["slug"]).encode("utf-8")
        if len(slug)>=40 and song.get("id") is not None:
            slug=f"db-{int(song['id'])}".encode("ascii")
        if len(title) >= 72 or len(slug) >= 40:
            raise ValueError("曲名或标识过长，请在曲库缩短")
        begin=f"SCORE_BEGIN:{rid}:{len(notes)}:{float(song['tempo_bpm']):.6g}:" \
            f"{float(song.get('tolerance_cents',30)):.6g}:{int(song.get('hold_ms',320))}:" \
            f"{int(song.get('time_num',4))}:{int(song.get('time_den',4))}"
        title_line=f"SCORE_TITLE:{rid}:{title.hex()}"
        id_line=f"SCORE_ID:{rid}:{slug.hex()}"
        self._score_line(rid,"BEGIN",begin)
        self._score_line(rid,"TITLE",title_line)
        self._score_line(rid,"ID",id_line)
        checksum=2166136261
        for byte in (begin+title_line+id_line).encode():
            checksum=((checksum^byte)*16777619)&0xffffffff
        beat_cursor = 0.0
        measure_quarters = int(song.get("time_num",4))*4/int(song.get("time_den",4))
        for i, n in enumerate(notes):
            beats = float(n.get("beats",1))
            measure = int(n.get("measure",0)) or int(beat_cursor/measure_quarters)+1
            beat = float(n.get("beat",0)) or beat_cursor % measure_quarters+1
            cmd = f"SCORE_NOTE:{rid}:{i}:{n['note']}:{beats:.6g}:{int(n.get('string_id',0))}:" \
                  f"{int(n.get('finger',-1))}:{measure}:{beat:.6g}"
            if len(("VC_CMD:"+cmd+"\n").encode()) >= 192:
                raise ValueError("乐谱串口帧过长")
            self._score_line(rid, "NOTE", cmd, i)
            for byte in cmd.encode():
                checksum = ((checksum ^ byte)*16777619) & 0xffffffff
            beat_cursor += beats
        self._score_line(rid, "COMMIT", f"SCORE_COMMIT:{rid}:{checksum:08x}")

    def disconnect(self) -> None:
        self._stop.set()
        ser, self._serial = self._serial, None
        if ser is not None:
            try: ser.close()
            except Exception: pass
        old_thread=self._thread
        if old_thread is not None and old_thread is not threading.current_thread():
            old_thread.join(timeout=1)
        self._thread=None
        with self._lock:
            self._state["connected"] = False

    def _read_loop(self) -> None:
        while not self._stop.is_set():
            ser = self._serial
            if ser is None: break
            try:
                raw = ser.readline()
                if not raw: continue
                line = raw.decode("utf-8", errors="ignore").strip()
                if line: self._handle_line(line)
            except Exception as exc:
                with self._lock:
                    self._state["connected"] = False
                    self._state["error"] = str(exc)
                break

    def _handle_line(self, line: str) -> None:
        with self._lock:
            self._state["last_raw"] = line[-800:]
            self._state["last_message_at"] = time.time()
        obj = self._extract_json(line)
        if isinstance(obj, dict):
            self._handle_json(obj)
            return
        # Legacy firmware compatibility.
        m = re.search(r"ACTUAL_PITCH.*?hps=([-+]?\d+(?:\.\d+)?)", line, re.I)
        if m: self._set_pitch(float(m.group(1)), None, None)
        m = re.search(r"MIC_DIAG:.*?target=([A-G](?:#)?\d|REST)", line, re.I)
        if m:
            with self._lock:
                if not self._state.get("firmware"):
                    self._state["target_note"] = m.group(1).upper()

    @staticmethod
    def _extract_json(line: str) -> dict[str, Any] | None:
        for start, ch in enumerate(line):
            if ch != "{": continue
            try:
                obj = json.loads(line[start:])
                if isinstance(obj, dict): return obj
            except Exception:
                pass
        return None

    def _handle_json(self, obj: dict[str, Any]) -> None:
        kind = str(obj.get("vc") or obj.get("type") or "")
        if not kind and "hz" in obj:
            self._set_pitch(self._float(obj.get("hz")), None, None); return
        if kind == "score_ack":
            key=(self._int(obj.get("request_id"),0),str(obj.get("stage","")),self._int(obj.get("index"),-1))
            with self._ack_condition:
                self._score_acks[key]=bool(obj.get("ok",False))
                self._ack_condition.notify_all()
            return
        if kind == "hello":
            try:
                self.send_command("HOST_LIBRARY:ON")
            except Exception:
                pass
            with self._lock:
                self._state["firmware"] = str(obj.get("firmware", ""))
                self._state["mode_control_supported"] = bool(obj.get("mode_control", False))
            return
        if kind == "song_request":
            with self._lock:
                request_id = self._int(obj.get("request_id"), 0)
                previous = self._state.get("last_song_request") or {}
                if request_id < self._int(previous.get("request_id"), 0):
                    return
                self._state["song_request_seq"] += 1
                self._state["last_song_request"] = dict(obj, received_at=time.time())
                if obj.get("phase") in {"loading","applied","unchanged"}:
                    self._state["error"]=""
            if obj.get("phase")=="loading" and obj.get("transport")=="usb":
                self._queue_score(obj)
            return
        if kind == "control_ack":
            with self._lock:
                if obj.get("command")=="HOST_LIBRARY" and obj.get("ok") and obj.get("value")=="USB":
                    self._state["control_rx_confirmed"]=True
                ack = {
                    "command": str(obj.get("command", "")),
                    "ok": bool(obj.get("ok", False)),
                    "value": str(obj.get("value", "")),
                    "firmware": str(obj.get("firmware", "")),
                }
                self._state["last_control_ack"] = ack
                self._state["last_control_ack_at"] = time.time()
                if ack["firmware"]:
                    self._state["firmware"] = ack["firmware"]
                    self._state["mode_control_supported"] = True
                if ack["command"] == "MODE" and ack["ok"]:
                    value = ack["value"].lower()
                    if value in {"pitch", "rhythm", "full", "follow"}:
                        self._state["practice_mode"] = value
            return
        if kind == "pitch":
            self._set_pitch(self._float(obj.get("hz")), self._float(obj.get("cents")), self._float(obj.get("confidence"))); return
        if kind == "target":
            with self._lock:
                self._state["app_state"] = str(obj.get("state", self._state["app_state"]))
                if self._state["target_note"] != str(obj.get("note", "")):
                    self._state.update(pitch_hz=None, pitch_cents=None, pitch_confidence=None, last_pitch_at=0.0)
                self._state["target_note"] = str(obj.get("note", ""))
                self._state["target_hz"] = self._float(obj.get("hz")) or 0.0
                if "song_active" in obj:
                    self._state["song_active"] = bool(obj["song_active"])
                self._apply_target_fields(obj)
                self._apply_song_fields(obj)
            return
        if kind == "state":
            with self._lock:
                self._state["app_state"] = str(obj.get("state", "unknown"))
                self._state["resume_state"] = str(obj.get("resume_state", "unknown"))
                if "firmware" in obj:
                    self._state["firmware"] = str(obj.get("firmware", ""))
                    self._state["mode_control_supported"] = bool(self._state["firmware"])
                if self._state["target_note"] != str(obj.get("target", "")):
                    self._state.update(pitch_hz=None, pitch_cents=None, pitch_confidence=None, last_pitch_at=0.0)
                self._state["target_note"] = str(obj.get("target", ""))
                if "target_hz" in obj:
                    self._state["target_hz"] = self._float(obj.get("target_hz")) or 0.0
                self._state["song_active"] = bool(obj.get("song_active", False))
                self._state["release_required"] = bool(obj.get("release_required", False))
                self._state["prev_note"] = str(obj.get("prev_note", ""))
                self._state["next_note"] = str(obj.get("next_note", ""))
                self._apply_target_fields(obj)
                self._apply_song_fields(obj)
            return
        if kind == "rhythm_tick":
            with self._lock:
                self._apply_song_fields(obj)
                self._state["rhythm_started"] = bool(obj.get("started", False))
                self._state["rhythm_elapsed_ms"] = self._int(obj.get("elapsed_ms"), 0)
                self._state["rhythm_target_ms"] = self._int(obj.get("target_ms"), 0)
                self._state["song_clock_beats"] = self._float(obj.get("clock_beats")) or 0.0
            return
        if kind in {"song", "song_action", "song_definition"}:
            if kind == "song_definition":
                with self._lock:
                    self._state["song_notes"] = []
                    self._state["song_count"] = self._int(obj.get("count"), 0)
                    self._state["tempo_bpm"] = self._float(obj.get("tempo"))
                    self._state["song_index"] = 0
                return
            action = str(obj.get("action", ""))
            with self._lock:
                self._state["last_song_action"] = action
                self._state["last_song_action_at"] = time.time()
                self._state["last_song_action_seq"] = int(
                    self._state.get("last_song_action_seq", 0)
                ) + 1
                if action == "start":
                    self._state["song_run_seq"] = int(
                        self._state.get("song_run_seq", 0)
                    ) + 1
                    self._state["song_notes"] = []
                    self._state["song_index"] = 0
                    self._state["song_active"] = True
                    self._state["song_id"] = str(obj.get("id", ""))
                    self._state["song_title"] = str(obj.get("title", ""))
                    self._state["song_count"] = int(obj.get("count", 0) or 0)
                    self._state["tempo_bpm"] = self._float(obj.get("tempo_bpm"))
                    self._state["tolerance_cents"] = self._float(obj.get("tolerance_cents"))
                    if "practice_mode" in obj:
                        self._state["practice_mode"] = str(obj.get("practice_mode") or "pitch")
                    if "metronome_enabled" in obj:
                        self._state["metronome_enabled"] = bool(obj.get("metronome_enabled"))
                elif action in {"done", "stop"}:
                    self._state["song_active"] = False
                elif action == "sync":
                    self._state["song_notes"] = []
                    self._state["song_id"] = str(obj.get("id", ""))
                    self._state["song_title"] = str(obj.get("title", ""))
                    self._state["song_count"] = self._int(obj.get("count"), 0)
                self._apply_song_fields(obj)
            return
        if kind == "song_note":
            try: index = int(obj.get("index", 0))
            except Exception: return
            event = {
                "index": index, "note": str(obj.get("note", "")),
                "beats": self._float(obj.get("beats")) or 1.0,
                "string_id": self._int(obj.get("string_id"), 0),
                "finger": self._int(obj.get("finger"), -1),
                "measure": self._int(obj.get("measure"), 0),
                "beat": self._float(obj.get("beat")) or 0.0,
            }
            with self._lock:
                if index < 0 or index >= 256:
                    return
                # Retain wire indices even if messages arrive with a gap.
                notes = {n["index"]: n for n in self._state["song_notes"]}
                notes[index] = event
                self._state["song_notes"] = [notes[i] for i in sorted(notes)]
            return
        if kind == "rhythm_note":
            event = {
                "phrase_run": self._int(obj.get("phrase_run"),0),
                "onset_detected": bool(obj.get("onset_detected", self._int(obj.get("onset_ms"), -1) >= 0)),
                "timing_status": str(obj.get("timing_status", "")),
                "boundary_reason": str(obj.get("boundary_reason", "")),
                "index": self._int(obj.get("index"), 0),
                "note": str(obj.get("note", "")),
                "measure": self._int(obj.get("measure"), 0),
                "beat": self._float(obj.get("beat")) or 0.0,
                "beats": self._float(obj.get("beats")) or 1.0,
                "target_ms": self._int(obj.get("target_ms"), 0),
                "onset_ms": self._int(obj.get("onset_ms"), -1),
                "sounding_ms": self._int(obj.get("sounding_ms"), 0),
                "correct_ms": self._int(obj.get("correct_ms"), 0),
                "mean_cents": self._float(obj.get("mean_cents")),
                "mean_abs_cents": self._float(obj.get("mean_abs_cents")),
                "stability_cents": self._float(obj.get("stability_cents")),
                "pitch_samples": self._int(obj.get("pitch_samples"), 0),
                "mode": str(obj.get("mode", "")),
            }
            with self._lock:
                self._state["last_rhythm_event"] = event
                self._state["last_rhythm_event_at"] = time.time()
                self._state["rhythm_event_seq"] = int(
                    self._state.get("rhythm_event_seq", 0)
                ) + 1
                event["seq"] = self._state["rhythm_event_seq"]
                event["run_seq"] = self._state["song_run_seq"]
                self._state["rhythm_events"].append(dict(event))
                self._state["rhythm_events"] = self._state["rhythm_events"][-256:]
            return
        if kind == "voice":
            with self._lock:
                self._state["voice_phase"] = str(obj.get("phase", "unknown"))
                self._state["voice_source"] = str(obj.get("source", ""))
                self._state["voice_message"] = str(obj.get("message", ""))
                self._state["voice_wake_model"] = str(obj.get("wake_model", ""))
                self._state["voice_event_at"] = time.time()
            return
        if kind == "asr":
            with self._lock: self._state["last_asr"] = str(obj.get("text", ""))
            return
        if kind == "intent":
            with self._lock:
                self._state["last_intent"] = {
                    "kind": str(obj.get("kind", "")), "note": str(obj.get("note", "")),
                    "song": str(obj.get("song", "")), "string_id": self._int(obj.get("string_id"), 0),
                    "tempo_bpm": self._float(obj.get("tempo_bpm")),
                }
                self._state["last_intent_at"] = time.time()
                self._state["last_intent_seq"] = int(self._state.get("last_intent_seq", 0)) + 1

    def _apply_target_fields(self, obj: dict[str, Any]) -> None:
        sid = self._int(obj.get("string_id"), self._state.get("target_string_id", 0))
        self._state["target_string_id"] = sid if sid in (1,2,3,4) else 0
        melody_context = str(obj.get("state", self._state.get("app_state", ""))) == "melody"
        if "finger" in obj:
            self._state["target_finger"] = self._int(obj.get("finger"), -1)
        elif not melody_context:
            self._state["target_finger"] = -1
        if "measure" in obj:
            self._state["target_measure"] = self._int(obj.get("measure"), 0)
        elif not melody_context:
            self._state["target_measure"] = 0
        if "beat" in obj:
            self._state["target_beat"] = self._float(obj.get("beat")) or 0.0
        elif not melody_context:
            self._state["target_beat"] = 0.0

    def _apply_song_fields(self, obj: dict[str, Any]) -> None:
        if "control_generation" in obj:
            generation = self._int(obj["control_generation"], 0)
            if generation < self._state["control_generation"]:
                self._state["last_song_request"] = {}  # device reboot
            self._state["control_generation"] = generation
        if "phrase_run" in obj and obj["phrase_run"]!=self._state.get("phrase_run"):
            self._state["rhythm_events"]=[]
        for key in ("follow_retry_reason", "follow_phase", "follow_elapsed_ms", "follow_target_ms", "follow_remaining_prep_ms", "follow_retries", "follow_pitch_ok", "follow_sounding", "follow_armed", "clock_lag_ms", "phrase_run", "rhythm_offset_ms", "metronome_clock_beats", "time_num", "time_den", "trainer_phase", "phrase_first", "phrase_end", "phrase_has_next", "phrase_good", "phrase_early", "phrase_late", "phrase_missed", "phrase_rest_noise", "phrase_mean_abs_ms"):
            if key in obj:
                self._state[key] = obj[key]
        if "metronome_clock_beats" in obj:
            self._clock_received=time.monotonic()
        if "song_title" in obj: self._state["song_title"] = str(obj.get("song_title", ""))
        if "song_id" in obj: self._state["song_id"] = str(obj.get("song_id", ""))
        if "song_index" in obj: self._state["song_index"] = self._int(obj.get("song_index"), 0)
        if "song_count" in obj: self._state["song_count"] = self._int(obj.get("song_count"), 0)
        if "tempo_bpm" in obj: self._state["tempo_bpm"] = self._float(obj.get("tempo_bpm"))
        if "tolerance_cents" in obj: self._state["tolerance_cents"] = self._float(obj.get("tolerance_cents"))
        if "practice_mode" in obj or "mode" in obj:
            self._state["practice_mode"] = str(obj.get("practice_mode") or obj.get("mode") or "pitch")
        if "metronome_enabled" in obj: self._state["metronome_enabled"] = bool(obj.get("metronome_enabled"))
        if "rhythm_tolerance_ms" in obj: self._state["rhythm_tolerance_ms"] = self._int(obj.get("rhythm_tolerance_ms"), 140)
        if "rhythm_started" in obj: self._state["rhythm_started"] = bool(obj.get("rhythm_started"))
        if "rhythm_elapsed_beats" in obj: self._state["rhythm_elapsed_beats"] = self._float(obj.get("rhythm_elapsed_beats")) or 0.0
        if "rhythm_elapsed_ms" in obj: self._state["rhythm_elapsed_ms"] = self._int(obj.get("rhythm_elapsed_ms"), 0)
        if "rhythm_target_ms" in obj: self._state["rhythm_target_ms"] = self._int(obj.get("rhythm_target_ms"), 0)
        if "song_clock_beats" in obj: self._state["song_clock_beats"] = self._float(obj.get("song_clock_beats")) or 0.0

    def _set_pitch(self, hz: float | None, cents: float | None, confidence: float | None) -> None:
        if hz is None or not (20.0 <= hz <= 5000.0): return
        with self._lock:
            self._state["pitch_hz"] = hz
            import math
            target = float(self._state.get("target_hz") or 0)
            self._state["pitch_cents"] = 1200 * math.log2(hz / target) if target > 0 else cents
            self._state["pitch_confidence"] = confidence
            self._state["last_pitch_at"] = time.time()

    @staticmethod
    def _float(value: Any) -> float | None:
        try: return None if value is None else float(value)
        except (TypeError, ValueError): return None

    @staticmethod
    def _int(value: Any, default: int = 0) -> int:
        try: return int(value)
        except (TypeError, ValueError): return default


coach_serial_bridge = CoachSerialBridge()
