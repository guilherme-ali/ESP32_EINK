#!/usr/bin/env python3
"""Benchmark acustico automatico; energia nao comprova fala nem qualidade perceptiva."""
import argparse
import codecs
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import time
import uuid
import wave


MEDIA_PROFILES = ("legacy", "pcm", "opus64", "opus48", "opus32")
TIMING_SANITY_TOLERANCE_SECONDS = 0.75
TIMING_SANITY_EARLY_TOLERANCE_SECONDS = 0.1


def open_serial(serial_module, device, timeout):
    connection = serial_module.Serial()
    connection.port = device
    connection.baudrate = 115200
    connection.timeout = timeout
    connection.write_timeout = 2
    connection.dsrdtr = False
    connection.rtscts = False
    # Configure control lines before open: do not reboot the ESP32 via DTR.
    connection.dtr = False
    connection.rts = False
    connection.open()
    return connection


def load_prompt(path, np, target_rate):
    with wave.open(str(path), "rb") as wav:
        if wav.getsampwidth() != 2 or wav.getnchannels() not in (1, 2):
            raise ValueError("O WAV precisa ser PCM16 mono/estereo")
        rate, channels = wav.getframerate(), wav.getnchannels()
        if not 0 < wav.getnframes() <= 30 * rate:
            raise ValueError("O WAV precisa ter entre 0 e 30 segundos")
        frames = wav.readframes(wav.getnframes())
    samples = np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0
    if channels == 2:
        samples = samples.reshape(-1, 2).mean(axis=1)
    if rate != target_rate:
        new_x = np.arange(round(len(samples) * target_rate / rate), dtype=np.float64) * rate / target_rate
        samples = np.interp(new_x, np.arange(len(samples)), samples).astype(np.float32)
    # Trim digital silence only, not quiet phonemes at an absolute amplitude floor.
    active = np.flatnonzero(np.abs(samples) >= 0.5 / 32768.0)
    if not len(active):
        raise ValueError("O WAV nao contem amostras audiveis")
    samples = samples[:active[-1] + 1].copy()
    if float(np.max(np.abs(samples))) > 0.98:
        raise ValueError("O WAV clipa ou esta acima de -0.2 dBFS")
    return samples.reshape(-1, 1), hashlib.sha256(path.read_bytes()).hexdigest(), rate


def parse_fields(line):
    return {key: value[:160] for key, value in
            re.findall(r"([A-Za-z0-9_.]{1,64})=([^\s]+)", line[:8192])[:64]}


def parse_done(line):
    fields = parse_fields(line)
    allowed = ("turn", "boot_id", "state", "captured_ms", "audio_rx_ms", "audio_tx_ms", "rebuffer", "buffered")
    return {key: fields[key] for key in allowed if key in fields}


def adc_frame(timestamp, adc_times, adc_offsets, adc_sizes, block_count, sample_rate, begin=0):
    """Diagnostic only: DAC/ADC mapping is NOT the acoustic measurement."""
    if timestamp is None or not math.isfinite(timestamp):
        return None
    for i in range(min(block_count, len(adc_times), len(adc_offsets), len(adc_sizes))):
        adc, size = float(adc_times[i]), int(adc_sizes[i])
        if math.isfinite(adc) and size > 0 and adc <= timestamp < adc + size / sample_rate:
            return round(int(adc_offsets[i]) + (timestamp - adc) * sample_rate) - begin
    return None


def onset_metrics(recording, begin, end, sample_rate, prompt, np, prompt_output_frame=None):
    """Align known stimulus and detect sustained energy in ONE saved WAV's coordinates.

    A callback frame is only a bounded alignment-search hint, never an acoustic
    endpoint. Reverberation/overlap and earlier transients require manual review.
    Reported uncertainty is algorithmic resolution, NOT physical calibration.
    """
    result = {"prompt_start_frame": None, "prompt_end_frame": None,
              "response_onset_frame": None, "candidate_onset_frame": None, "latency_ms": None,
              "onset_threshold": None, "noise_rms": None, "noise_end_frame": None,
              "alignment_score": None, "alignment_competing_score": None,
              "timing_uncertainty_ms": None, "uncertainty_kind": "algorithmic_resolution_only",
              "onset_method": "waveform_correlation_then_sustained_energy",
              "detection_status": "invalid", "detection_reason": "missing_prompt"}
    if prompt is None:
        return result
    signal = np.asarray(recording[begin:end], dtype=np.float64).reshape(-1)
    reference = np.asarray(prompt, dtype=np.float64).reshape(-1)
    length = len(reference)
    if length < round(0.1 * sample_rate) or len(signal) < length:
        result["detection_reason"] = "insufficient_samples"
        return result
    if not np.all(np.isfinite(signal)) or not np.all(np.isfinite(reference)):
        result["detection_reason"] = "nonfinite_samples"
        return result
    low, high = 0, len(signal) - length
    if prompt_output_frame is not None:
        hint = prompt_output_frame - begin
        low = max(low, hint - round(0.3 * sample_rate))
        high = min(high, hint + round(0.3 * sample_rate))
    elif len(signal) > length + 10 * sample_rate:
        result["detection_reason"] = "alignment_hint_missing"
        return result
    if high < low:
        result["detection_reason"] = "alignment_outside_capture"
        return result
    search = signal[low:high + length]
    reference = reference - reference.mean()
    reference_energy = float(reference @ reference)
    if reference_energy <= 0:
        result["detection_reason"] = "silent_prompt"
        return result
    # FFT correlation and sliding, mean-corrected energy; no scipy dependency.
    fft_size = 1 << (len(search) + length - 2).bit_length()
    correlation = np.fft.irfft(np.fft.rfft(search, fft_size) *
                              np.fft.rfft(reference[::-1], fft_size), fft_size)
    sums = np.concatenate(([0.0], np.cumsum(search)))
    squares = np.concatenate(([0.0], np.cumsum(search * search)))
    energy = squares[length:] - squares[:-length] - (sums[length:] - sums[:-length]) ** 2 / length
    scores = np.divide(np.abs(correlation[length - 1:len(search)]),
                       np.sqrt(np.maximum(energy, 0) * reference_energy),
                       out=np.zeros_like(energy), where=energy > reference_energy * 1e-12)
    best = int(np.argmax(scores))
    score = float(scores[best])
    competitors = scores.copy()
    exclusion = max(1, round(0.15 * sample_rate))
    competitors[max(0, best - exclusion):best + exclusion + 1] = 0
    second = float(np.max(competitors))
    result.update(alignment_score=score, alignment_competing_score=second)
    if score < 0.35 or (len(competitors) > 0 and second >= score * 0.90):
        result.update(detection_status="ambiguous", detection_reason="prompt_alignment")
        return result
    start, finish = low + best, low + best + length
    result.update(prompt_start_frame=start, prompt_end_frame=finish)
    noise_end = max(0, start - round(0.05 * sample_rate))
    result["noise_end_frame"] = noise_end
    if noise_end < round(0.2 * sample_rate):
        result["detection_reason"] = "pre_stimulus_noise_missing"
        return result
    baseline = signal[:noise_end]
    dc = float(np.mean(baseline))
    noise_rms = float(np.sqrt(np.mean((baseline - dc) ** 2)))
    # Require multiple PCM16 steps; sub-LSB capture artifacts are not evidence of sound.
    threshold = max(3.0 / 32768.0, 3.0 * noise_rms)
    result.update(noise_rms=noise_rms, onset_threshold=threshold)
    window, hop = max(1, round(0.010 * sample_rate)), max(1, round(0.005 * sample_rate))
    required = math.ceil(0.030 * sample_rate / hop)
    tail_observed = False
    tail_end = 0
    streak, candidate, transient = 0, None, False
    streak_peak = 0.0
    for offset in range(finish, len(signal) - window + 1, hop):
        segment = signal[offset:offset + window] - dc
        rms = float(np.sqrt(np.mean(segment * segment)))
        peak = float(np.max(np.abs(segment)))
        active = rms >= threshold
        if active:
            if streak == 0:
                candidate = offset
                streak_peak = 0.0
            streak += 1
            streak_peak = max(streak_peak, peak)
            if streak >= required:
                if candidate == finish:
                    tail_observed = True
                    tail_end = offset
                elif transient:
                    result.update(candidate_onset_frame=candidate, detection_status="ambiguous",
                                  detection_reason="earlier_transient")
                    return result
                elif tail_observed and (candidate - tail_end) < round(0.150 * sample_rate):
                    result.update(candidate_onset_frame=finish, detection_status="ambiguous",
                                  detection_reason="prompt_tail_or_overlap")
                    return result
                else:
                    result.update(candidate_onset_frame=candidate, response_onset_frame=candidate,
                                  latency_ms=1000.0 * (candidate - finish) / sample_rate,
                                  timing_uncertainty_ms=1000.0 * (window + hop + 1) / sample_rate,
                                  detection_status="ok", detection_reason="sustained_energy_not_verified_speech")
                    return result
        elif streak:
            if candidate == finish:
                tail_end = offset
            elif streak_peak >= 6.0 * threshold:
                transient = True
                if result["candidate_onset_frame"] is None:
                    result["candidate_onset_frame"] = candidate
            streak = 0
            candidate = None
            streak_peak = 0.0
    if tail_observed:
        result.update(candidate_onset_frame=finish, detection_status="ambiguous",
                      detection_reason="prompt_tail_or_overlap")
    elif transient or streak:
        result.update(detection_status="ambiguous", detection_reason="transient_or_truncated_onset")
    else:
        result.update(detection_status="no_response", detection_reason="no_sustained_energy")
    return result


def timing_sanity(onset, firmware, capture, release_lateness_seconds):
    """Veto only: compare elapsed times relative to release, never align ESP/PC clocks."""
    result = {"timing_sanity_valid": False, "timing_sanity_reason": "timing_reference_missing",
               "timing_sanity_tolerance_ms": 1000 * TIMING_SANITY_TOLERANCE_SECONDS,
              "timing_sanity_early_tolerance_ms": 1000 * TIMING_SANITY_EARLY_TOLERANCE_SECONDS,
              "acoustic_onset_after_release_ms_diagnostic": None,
              "firmware_first_i2s_after_release_ms_diagnostic": None,
              "acoustic_firmware_delta_ms_diagnostic": None}
    if onset.get("detection_status") != "ok":
        result["timing_sanity_reason"] = "acoustic_onset_unavailable"
        return result
    try:
        release_us, first_us = int(firmware["release_us"]), int(firmware["first_i2s_us"])
        if release_us <= 0 or first_us < release_us:
            raise ValueError("invalid_firmware_timestamps")
        frame = onset["response_onset_frame"]
        adc_onset = None
        for i in range(capture.blocks):
            offset, size = int(capture.adc_offsets[i]), int(capture.adc_sizes[i])
            adc = float(capture.adc_times[i])
            if offset <= frame < offset + size and math.isfinite(adc):
                adc_onset = adc + (frame - offset) / capture.sample_rate
                break
        if adc_onset is None or capture.prompt_end_dac is None or release_lateness_seconds is None:
            return result
        if not all(math.isfinite(value) for value in
                   (adc_onset, capture.prompt_end_dac, release_lateness_seconds)) or release_lateness_seconds < 0:
            return result
        # ADC and DAC share PortAudio's clock. Only elapsed ESP times are used.
        acoustic = adc_onset - capture.prompt_end_dac - release_lateness_seconds
        first = (first_us - release_us) / 1e6
        delta = acoustic - first
        result.update(acoustic_onset_after_release_ms_diagnostic=1000 * acoustic,
                      firmware_first_i2s_after_release_ms_diagnostic=1000 * first,
                      acoustic_firmware_delta_ms_diagnostic=1000 * delta)
        reason = ""
        # DMA and leading silence can delay sound, not make it precede I2S.
        if delta < -TIMING_SANITY_EARLY_TOLERANCE_SECONDS:
            reason = "acoustic_onset_before_firmware_playback"
        elif delta > TIMING_SANITY_TOLERANCE_SECONDS:
            reason = "acoustic_onset_late_vs_firmware_playback"
        result.update(timing_sanity_valid=not reason, timing_sanity_reason=reason)
    except (KeyError, TypeError, ValueError, OverflowError):
        pass
    return result


def wasapi_extra_settings(sd, devices, enabled):
    if not enabled:
        return None
    settings = []
    for device, kind in zip(devices, ("input", "output")):
        info = sd.query_devices(device, kind)
        hostapi = sd.query_hostapis(info["hostapi"])["name"]
        settings.append(sd.WasapiSettings(exclusive=True) if hostapi == "Windows WASAPI" else None)
    if not all(settings):
        raise ValueError("--wasapi-exclusive exige dispositivos de entrada e saida WASAPI")
    return tuple(settings)


class AudioCapture:
    """One owner per trial. Never reset counters/buffers while PortAudio runs."""

    def __init__(self, prompt, sample_rate, max_frames, blocksize, np):
        self.np, self.prompt, self.sample_rate = np, prompt, sample_rate
        self.recording = np.zeros((max_frames, 1), dtype=np.float32)
        count = math.ceil(max_frames / blocksize) + 2
        self.adc_times = np.full(count, np.nan, dtype=np.float64)
        self.adc_offsets = np.zeros(count, dtype=np.int64)
        self.adc_sizes = np.zeros(count, dtype=np.int32)
        self.frames = self.blocks = self.prompt_offset = 0
        self.armed = False  # Main thread only enables once; no multi-field reset.
        self.prompt_output_frame = self.prompt_start_dac = self.prompt_end_dac = None
        self.input_overflow = self.output_underflow = self.input_underflow = self.output_overflow = self.truncated = 0
        self.error = ""

    def callback(self, indata, outdata, frames, time_info, status):
        outdata.fill(0)
        try:
            start = self.frames
            count = min(frames, len(self.recording) - start)
            if count < frames:
                self.truncated += 1
            if count:
                self.np.copyto(self.recording[start:start + count], indata[:count, :1])
                block = self.blocks
                if block < len(self.adc_times):
                    self.adc_times[block] = getattr(time_info, "inputBufferAdcTime", None)
                    self.adc_offsets[block], self.adc_sizes[block] = start, count
                    self.blocks = block + 1
                else:
                    self.truncated += 1
                self.frames = start + count
            if getattr(status, "input_overflow", False):
                self.input_overflow += 1
            if getattr(status, "output_underflow", False):
                self.output_underflow += 1
            if getattr(status, "input_underflow", False):
                self.input_underflow += 1
            if getattr(status, "output_overflow", False):
                self.output_overflow += 1
            if self.armed and not self.error and not self.truncated:
                offset = self.prompt_offset
                take = min(frames, len(self.prompt) - offset)
                if take > 0:
                    dac = getattr(time_info, "outputBufferDacTime", None)
                    if dac is None or not math.isfinite(dac):
                        self.error = "missing_dac_timestamp"
                        return
                    if self.prompt_output_frame is None:
                        self.prompt_output_frame = start
                        self.prompt_start_dac = dac
                    self.np.copyto(outdata[:take, :1], self.prompt[offset:offset + take])
                    self.prompt_offset = offset + take
                    if offset + take == len(self.prompt):
                        self.prompt_end_dac = dac + take / self.sample_rate
        except Exception as error:
            self.error = str(error)[:240]
            outdata.fill(0)


SUMMARY_FIELDS = ["run_id", "trial_id", "trial", "repeat", "route_requested", "route_actual", "playback_policy", "media_requested",
                  "media_expected", "media_actual", "media_valid", "media_failure_reason", "media_tx_wire_bytes", "media_rx_wire_bytes",
                  "wav", "turn_id", "boot_id", "status", "failure_reason", "timed_out", "detection_status", "detection_reason",
                  "latency_ms", "candidate_latency_ms_diagnostic", "timing_uncertainty_ms",
                  "timing_sanity_valid", "timing_sanity_reason", "timing_sanity_tolerance_ms", "timing_sanity_early_tolerance_ms",
                  "acoustic_onset_after_release_ms_diagnostic", "firmware_first_i2s_after_release_ms_diagnostic",
                  "acoustic_firmware_delta_ms_diagnostic", "release_lateness_seconds",
                  "wasapi_exclusive", "input_overflow_callbacks", "output_underflow_callbacks",
                  "valid", "recording", "backlog_release_bytes", "backlog_max_bytes", "input_bytes", "network_connect_us",
                  "ws_upgrade_us", "setup_complete_us", "activity_end_us", "first_audio_us", "first_pcm_us",
                  "turn_complete_us", "response_ready_us", "first_i2s_us", "network_done_us",
                  "ble_mtu", "ble_interval_1.25ms", "ble_tx_phy", "ble_rx_phy", "ble_tx_octets", "ble_rx_octets"]


class RunLog:
    def __init__(self, base, metadata):
        self.run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:12]
        self.directory = base / self.run_id
        self.directory.mkdir(parents=True, exist_ok=False)
        self.events = (self.directory / "events.jsonl").open("x", encoding="utf-8")
        self.summary = (self.directory / "summary.csv").open("x", newline="", encoding="utf-8")
        self.writer = csv.DictWriter(self.summary, fieldnames=SUMMARY_FIELDS)
        self.writer.writeheader()
        self.summary.flush()
        self.sequence = 0
        self.completed_trials = 0
        self.trial_id = None
        (self.directory / "run.json").write_text(json.dumps({"run_id": self.run_id, **metadata}, indent=2,
                                                        ensure_ascii=False), encoding="utf-8")
        self.event("run_start", metadata=metadata)

    def event(self, kind, **fields):
        self.sequence += 1
        event = {"run_id": self.run_id, "trial_id": self.trial_id, "event_id": self.sequence,
                 "host_perf_ns": time.perf_counter_ns(), "event": kind, **fields}
        self.events.write(json.dumps(event, ensure_ascii=False, allow_nan=False) + "\n")
        self.events.flush()

    def save_trial(self, result):
        result.update(run_id=self.run_id, trial_id=self.trial_id)
        with (self.directory / f"{self.trial_id}.json").open("x", encoding="utf-8") as file:
            json.dump(result, file, indent=2, ensure_ascii=False, allow_nan=False)
        row = {key: result.get(key, "") for key in SUMMARY_FIELDS}
        for key in SUMMARY_FIELDS:
            if key in result.get("firmware", {}):
                row[key] = result["firmware"][key]
            if key.startswith("ble_"):
                row[key] = result.get("ble_link", {}).get(key[4:], "")
        for column, key in (("backlog_release_bytes", "backlog_release"), ("backlog_max_bytes", "backlog_max"),
                            ("network_connect_us", "net_connect_us")):
            row[column] = result.get("firmware", {}).get(key, "")
        self.writer.writerow(row)
        self.summary.flush()
        self.completed_trials += 1
        self.event("trial_result", result=result)

    def close(self):
        self.events.close()
        self.summary.close()


class SerialSession:
    """Bounded serial parsing, fresh command barriers, sanitized JSONL telemetry."""

    def __init__(self, port, log):
        self.port, self.log = port, log
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.pending = ""
        self.discard_line = False
        self.sequence = self.command_id = 0

    def read(self):
        text = self.decoder.decode(self.port.read(min(4096, max(1, self.port.in_waiting))))
        events = []
        for part in text.splitlines(keepends=True):
            self.pending += part
            complete = part.endswith("\n")
            if len(self.pending) > 8192:
                self.pending = ""
                self.discard_line = True
                self.log.event("serial_line_truncated")
            if not complete:
                continue
            line, self.pending = self.pending.strip(), ""
            if self.discard_line:
                self.discard_line = False
                continue
            match = re.match(r"^\[(AssistantMetrics|AssistantMedia|AssistantDone|Assistant|AssistantAudio|PlaybackSource|PlaybackProcessed|ListeningFeedback|BLE|BenchmarkRoute|BenchmarkPlayback|BenchmarkMedia|DiagDone)\] (.*)$", line)
            if not match:
                continue
            tag, body = match.groups()
            fields = parse_done(line) if tag == "AssistantDone" else parse_fields(line)
            if tag == "AssistantMedia":
                allowed = ("turn", "boot_id", "media", "tx_bytes", "rx_bytes", "encode_us", "decode_us", "stack_free_min_bytes")
                fields = {key: fields[key] for key in allowed if key in fields}
            elif tag in ("PlaybackSource", "PlaybackProcessed", "ListeningFeedback", "AssistantAudio"):
                allowed = {"samples", "rms_dbfs", "max100ms_dbfs", "peak_dbfs", "fullscale", "volume_requested",
                           "volume_cached", "reg32", "reg32_valid", "stack_min_bytes", "state", "request_us",
                           "refresh_begin_us", "refresh_done_us", "ok", "error", "latency_samples", "guards"}
                fields = {key: value for key, value in fields.items()
                          if (key in allowed and re.fullmatch(r"-?\d+(?:\.\d+)?", value)) or
                          (key == "kind" and value in ("assistant", "wav")) or
                          (key == "scope" and value == "offered") or
                          (key == "stage" and value == "capture")}
            elif tag in ("Assistant", "AssistantMetrics", "BLE"):
                fields = {key: value for key, value in fields.items() if re.fullmatch(r"-?\d+", value) or
                          (key == "route" and value in ("wifi", "ble", "phone", "none")) or
                          (key == "boot_id" and re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value))}
            value = body[:80] if tag in ("BenchmarkRoute", "BenchmarkPlayback", "BenchmarkMedia", "DiagDone") else ""
            self.sequence += 1
            event = {"serial_seq": self.sequence, "tag": tag, "fields": fields, "value": value}
            events.append(event)
            self.log.event("serial", **event)
        return events

    def drain(self, observer=None):
        # No historical ACK survives a new command, including incomplete lines.
        deadline = time.monotonic() + 0.1
        while self.port.in_waiting and time.monotonic() < deadline:
            for event in self.read():
                if observer:
                    observer(event)
        if self.port.in_waiting:
            raise RuntimeError("serial_drain_overrun")
        self.pending = ""
        self.discard_line = False

    def send(self, command):
        self.command_id += 1
        data = (command + "\n").encode("ascii")
        sent_ns = time.perf_counter_ns()
        written = self.port.write(data)
        # File flushing must not delay the release write or pretend to be its timestamp.
        self.log.event("command", command_id=self.command_id, command=command,
                       host_send_perf_ns=sent_ns, serial_write_bytes=written)
        if written != len(data):
            raise RuntimeError("serial_short_write")
        return sent_ns

    def command(self, command, expected=None, timeout=8, observer=None):
        self.drain(observer)
        marker = self.sequence
        self.send(command)
        deadline, observations, acknowledged = time.monotonic() + timeout, [], expected is None
        while time.monotonic() < deadline:
            barrier_acknowledged = None
            for event in self.read():
                if observer:
                    observer(event)
                if event["serial_seq"] <= marker:
                    continue
                if len(observations) >= 128:
                    raise RuntimeError("command_event_limit")
                observations.append(event)
                if barrier_acknowledged is not None:
                    continue
                acknowledged |= expected == (event["tag"], event["value"])
                if event["tag"] == "DiagDone" and event["value"] == command:
                    barrier_acknowledged = acknowledged
            # A read can also contain the turn's Metrics/Done AFTER the command
            # barrier. Observe the whole batch before returning, or they are lost.
            if barrier_acknowledged is not None:
                if not barrier_acknowledged:
                    raise RuntimeError(f"command_rejected: {command}")
                self.log.event("command_done", command_id=self.command_id, command=command)
                return observations
        raise TimeoutError(f"command_timeout: {command}")


class TurnTracker:
    def __init__(self, previous_turn=None):
        self.previous_turn = previous_turn
        self.turn_id = None
        self.boot_id = None  # Do not invent a device boot ID from a host run ID.
        self.metrics, self.done = {}, {}
        self.media = {}
        self.media_conflict = False

    def ready(self, events):
        for event in events:
            fields = event["fields"]
            if event["tag"] != "Assistant" or fields.get("state") not in ("1", "2"):
                continue
            if fields.get("released") != "0" or fields.get("finished") != "0":
                continue
            if int(fields.get("captured_ms", "0")) < 50:
                continue
            turn = fields.get("turn", "")
            if not turn.isdecimal() or not 0 < int(turn) <= 0xffffffff:
                raise RuntimeError("turn_correlation_missing: firmware precisa informar turn no status e Done")
            if self.previous_turn is not None and int(turn) <= self.previous_turn:
                raise RuntimeError("stale_turn_or_device_reboot")
            self.turn_id = int(turn)
            self.boot_id = fields.get("boot_id")
            # Completion can share the same serial read as the first ready
            # status. Replay after binding the ID, not before it is known.
            for observation in events:
                self.observe(observation)
            return True
        return False

    def observe(self, event):
        fields = event["fields"]
        if self.turn_id is None or fields.get("turn") != str(self.turn_id):
            return
        if self.boot_id is not None and fields.get("boot_id") not in (None, self.boot_id):
            raise RuntimeError("device_boot_changed")
        if event["tag"] == "AssistantMetrics":
            self.metrics = fields
        elif event["tag"] == "AssistantMedia":
            if self.media and self.media.get("media") != fields.get("media"):
                self.media_conflict = True
            self.media = fields
        elif event["tag"] == "AssistantDone":
            self.done = fields

    def media_result(self, route, requested):
        expected = "legacy" if route == "wifi" else "opus48" if requested == "default" else requested
        actual = self.media.get("media")
        reason = ""
        if not self.media:
            reason = "media_telemetry_missing"
        elif self.media_conflict:
            reason = "media_telemetry_conflict"
        elif actual not in MEDIA_PROFILES:
            reason = "media_profile_missing_or_unknown"
        elif actual != expected:
            reason = "media_profile_mismatch"
        result = {"assistant_media": self.media, "media_expected": expected, "media_actual": actual,
                  "media_wire_bytes_scope": "esp32_media_payload_only_not_total_network_traffic"}
        for source, output in (("tx_bytes", "media_tx_wire_bytes"), ("rx_bytes", "media_rx_wire_bytes")):
            value = self.media.get(source)
            count = None
            if value is not None:
                if re.fullmatch(r"[0-9]{1,20}", value) and int(value) <= 0xffffffffffffffff:
                    count = int(value)
                elif not reason:
                    reason = "media_wire_bytes_invalid"
            result[output] = count
        result.update(media_valid=not reason, media_failure_reason=reason)
        return result


def restore_defaults(session):
    errors = []
    try:
        session.command("assistant-cancel", timeout=3)
    except Exception as error:
        errors.append(str(error))
    # Cancel ACK is not worker completion: require actual fresh override ACKs.
    for command, expected in (("benchmark-route auto", ("BenchmarkRoute", "auto")),
                              ("benchmark-playback ready", ("BenchmarkPlayback", "ready")),
                              ("benchmark-media default", ("BenchmarkMedia", "default"))):
        last_error = f"restore_timeout: {command}"
        deadline = time.monotonic() + 10
        for _ in range(50):
            if time.monotonic() >= deadline:
                break
            try:
                session.command(command, expected, timeout=min(3, max(0.01, deadline - time.monotonic())))
                last_error = None
                break
            except Exception as error:
                last_error = str(error)
                time.sleep(0.2)
        if last_error:
            errors.append(last_error)
    session.log.event("restore", confirmed=not errors, errors=errors)
    return errors


def run_trial(args, session, log, np, sd, devices, prompt_info, route, trial, repeat, previous_turn):
    path, prompt, digest, original_rate = prompt_info
    result = {"trial": trial, "repeat": repeat, "route_requested": route, "playback_policy": args.playback,
              "capture_path": "adopted" if getattr(args, "early_capture", False) else "direct",
              "media_requested": args.media,
              "wav": str(path), "wav_sha256": digest, "wav_original_rate": original_rate,
              "sample_rate": args.samplerate, "status": "error", "failure_reason": "incomplete_trial",
              "valid": False, "timed_out": False, "latency_ms": None,
              "detection_status": "invalid", "detection_reason": "not_analyzed", "firmware": {}, "assistant_done": {}}
    capture = stream = None
    tracker = TurnTracker(previous_turn)
    interrupted = False
    try:
        session.command(f"benchmark-route {route}", ("BenchmarkRoute", route), timeout=8)
        session.command(f"benchmark-playback {args.playback}", ("BenchmarkPlayback", args.playback))
        session.command(f"benchmark-media {args.media}", ("BenchmarkMedia", args.media))
        idle = session.command("status", timeout=15)
        if any(event["tag"] == "Assistant" and "state" in event["fields"] for event in idle):
            raise RuntimeError("assistant_not_idle_before_start")
        blocksize = round(args.samplerate * 0.01)
        capture = AudioCapture(prompt, args.samplerate, math.ceil((args.timeout + args.tail_seconds + 20) * args.samplerate),
                               blocksize, np)
        extra_settings = wasapi_extra_settings(sd, devices, getattr(args, "wasapi_exclusive", False))
        stream_options = {} if extra_settings is None else {"extra_settings": extra_settings}
        stream = sd.Stream(device=devices, samplerate=args.samplerate, channels=(1, 1), dtype="float32",
                           blocksize=blocksize, latency="low", callback=capture.callback, **stream_options)
        result.update(input_device=str(stream.device[0]), output_device=str(stream.device[1]),
                      wasapi_exclusive=getattr(args, "wasapi_exclusive", False),
                      stream_latency_seconds=stream.latency, stream_blocksize=stream.blocksize,
                      stream_sample_rate=stream.samplerate)
        stream.start()
        deadline = time.monotonic() + args.timeout
        start_command = "assistant-early-start" if getattr(args, "early_capture", False) else "assistant-start"
        session.command(start_command, timeout=min(15, args.timeout), observer=tracker.observe)
        while time.monotonic() < deadline:
            events = session.command("status", timeout=min(3, max(0.01, deadline - time.monotonic())),
                                     observer=tracker.observe)
            if tracker.ready(events):
                break
            if capture.error or capture.truncated:
                raise RuntimeError("audio_callback_failed_before_prompt")
            time.sleep(0.05)
        if tracker.turn_id is None:
            result["timed_out"] = True
            raise TimeoutError("capture_ready_timeout")
        if tracker.done:
            raise RuntimeError("assistant_finished_before_stimulus")
        # This baseline contains no stimulus; never substitute prompt audio for it.
        while capture.frames < round(0.5 * args.samplerate) and time.monotonic() < deadline:
            time.sleep(0.005)
        capture.armed = True
        log.event("stimulus_armed", turn_id=tracker.turn_id)
        released = False
        while time.monotonic() < deadline:
            # Poll serial in short bounded reads, never wait for status ACK during playback.
            if capture.prompt_end_dac is not None and not released and stream.time >= capture.prompt_end_dac:
                before_ns = time.perf_counter_ns()
                release_stream_time = stream.time
                after_ns = time.perf_counter_ns()
                result["release_host_perf_ns"] = session.send("assistant-release")
                result["release_clock_query_duration_ms"] = (after_ns - before_ns) / 1e6
                result["release_lateness_seconds"] = (release_stream_time - capture.prompt_end_dac +
                    (result["release_host_perf_ns"] - after_ns) / 1e9)
                released = True
            for event in session.read():
                tracker.observe(event)
            if capture.error or capture.truncated:
                raise RuntimeError("audio_callback_failed")
            if tracker.done:
                break
            time.sleep(0.003)
        if not tracker.done:
            result["timed_out"] = True
            capture.armed = False
            session.command("assistant-cancel", timeout=3, observer=tracker.observe)
        tail_end = time.monotonic() + args.tail_seconds
        while time.monotonic() < tail_end:
            for event in session.read():
                tracker.observe(event)
        if tracker.done:
            events = session.command("status", timeout=15, observer=tracker.observe)
            for event in events:
                if event["tag"] == "BLE":
                    result["ble_link"] = event["fields"]
        result.update(status="completed", failure_reason="")
    except KeyboardInterrupt:
        interrupted = True
        result.update(status="interrupted", failure_reason="keyboard_interrupt")
    except Exception as error:
        result.update(status="error", timed_out=result["timed_out"] or isinstance(error, TimeoutError),
                      failure_reason=f"{type(error).__name__}: {error}"[:320])
    finally:
        if capture is not None:
            capture.armed = False
        if stream is not None:
            try:
                stream.stop()
            except Exception as error:
                result.update(status="error", failure_reason=result["failure_reason"] + f"; audio_stop_failed: {error}"[:240])
            finally:
                try:
                    stream.close()
                except Exception as error:
                    result.update(status="error", failure_reason=result["failure_reason"] + f"; audio_close_failed: {error}"[:240])
                    capture = None  # Cannot safely inspect buffers if the callback might still run.
        result.update(turn_id=tracker.turn_id, boot_id=tracker.boot_id, firmware=tracker.metrics, assistant_done=tracker.done)
        result.update(tracker.media_result(route, args.media))
        result["route_actual"] = tracker.metrics.get("route", "missing")
        result["route_valid"] = result["route_actual"] == ("ble" if route == "phone" else "wifi")
        try:
            if capture is not None:
                pcm16 = (np.clip(capture.recording[:capture.frames, 0], -1, 1) * 32767).astype("<i2")
                wav_path = log.directory / f"{log.trial_id}.wav"
                with wav_path.open("xb") as file:
                    with wave.open(file, "wb") as wav:
                        wav.setnchannels(1)
                        wav.setsampwidth(2)
                        wav.setframerate(args.samplerate)
                        wav.writeframes(pcm16.tobytes())
                result.update(recording=str(wav_path), recording_frames=capture.frames,
                              frame_origin="saved_wav_start", input_overflow_callbacks=capture.input_overflow,
                              output_underflow_callbacks=capture.output_underflow,
                              input_underflow_callbacks=capture.input_underflow, output_overflow_callbacks=capture.output_overflow,
                              truncated_callbacks=capture.truncated,
                              callback_error=capture.error, prompt_start_dac_time=capture.prompt_start_dac,
                              prompt_end_dac_time=capture.prompt_end_dac,
                              prompt_dac_end_adc_frame_diagnostic=adc_frame(capture.prompt_end_dac, capture.adc_times,
                                  capture.adc_offsets, capture.adc_sizes, capture.blocks, args.samplerate))
                # Analyze exactly the quantized samples saved above, not a different float recording.
                saved_samples = pcm16.astype(np.float32).reshape(-1, 1) / 32768.0
                onset = onset_metrics(saved_samples, 0, len(saved_samples), args.samplerate, prompt, np,
                                      prompt_output_frame=capture.prompt_output_frame)
                result.update(onset)
                sanity = timing_sanity(onset, tracker.metrics, capture, result.get("release_lateness_seconds"))
                if result.get("release_clock_query_duration_ms", math.inf) > 10:
                    sanity.update(timing_sanity_valid=False, timing_sanity_reason="release_clock_query_too_slow")
                result.update(sanity)
                if result["status"] == "completed":
                    failures = []
                    if result["timed_out"]:
                        failures.append("turn_timeout")
                    if not result["route_valid"]:
                        failures.append("route_mismatch_or_metrics_missing")
                    if not result["media_valid"]:
                        failures.append(result["media_failure_reason"])
                    if tracker.done.get("state") != "5":
                        failures.append("assistant_failed_or_done_missing")
                    if (capture.input_overflow or capture.output_underflow or capture.input_underflow or
                            capture.output_overflow or capture.truncated or capture.error):
                        failures.append("audio_capture_invalid")
                    if capture.prompt_end_dac is None:
                        failures.append("stimulus_not_completed")
                    if onset["detection_status"] != "ok":
                        failures.append(onset["detection_reason"])
                    elif not sanity["timing_sanity_valid"]:
                        failures.append(sanity["timing_sanity_reason"])
                    result.update(valid=not failures, status="valid" if not failures else "invalid",
                                   failure_reason=",".join(failures))
        except Exception as error:
            result.update(status="error", valid=False,
                          failure_reason=result["failure_reason"] + f"; recording_analysis_failed: {error}"[:240])
        if not result["valid"] and result.get("latency_ms") is not None:
            result["candidate_latency_ms_diagnostic"] = result["latency_ms"]
            result["latency_ms"] = None
        log.save_trial(result)
        print(f"[Trial {trial}] status={result['status']} latency_ms={result.get('latency_ms')} reason={result['failure_reason']}", flush=True)
    if interrupted:
        raise KeyboardInterrupt
    return result


def exit_code(results, run_error=False, restore_errors=()):
    if run_error or restore_errors or any(result.get("status") == "error" for result in results):
        return 2
    return 0 if results and all(result.get("valid") for result in results) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wav", action="append", type=Path, help="WAV PCM16 fixo, ate 30 s; repetir para corpus")
    parser.add_argument("--routes", default="wifi,phone", help="Rotas exclusivas; ordem invertida a cada repeticao")
    parser.add_argument("--playback", choices=("full", "ready"), default="ready")
    parser.add_argument("--media", choices=("default", *MEDIA_PROFILES), default="default",
                         help="default usa o perfil normal do BOOT (opus48); Wi-Fi ignora compressao")
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--early-capture", action="store_true",
                        help="Testa adocao da captura antecipada (debug USB); nao simula wake fisico")
    parser.add_argument("--port", default="COM7")
    parser.add_argument("--input-device", help="ID ou substring do microfone local")
    parser.add_argument("--output-device", help="ID ou substring da saida local, preferencialmente cabeada")
    parser.add_argument("--wasapi-exclusive", action="store_true", help="Exige entrada/saida WASAPI em modo exclusivo")
    parser.add_argument("--samplerate", type=int, default=48000)
    parser.add_argument("--prompt-distance-m", type=float, default=1.0, help="Somente metadado; nao corrige o alinhamento")
    parser.add_argument("--prompt-distance-uncertainty-m", type=float, default=0.3, help="Somente metadado")
    parser.add_argument("--timeout", type=float, default=190)
    parser.add_argument("--tail-seconds", type=float, default=0.8)
    parser.add_argument("--results", type=Path, default=Path("test/results/voice-benchmark"),
                        help="Diretorio base; cada execucao cria subdiretorio exclusivo com JSONL/CSV/WAV")
    parser.add_argument("--list-devices", action="store_true")
    args = parser.parse_args()
    if not args.list_devices:
        if not args.wav or not 1 <= args.repeats <= 1000 or not math.isfinite(args.timeout) or not 10 <= args.timeout <= 600:
            parser.error("Informe --wav, 1 <= --repeats <= 1000 e 10 <= --timeout <= 600")
        if not 8000 <= args.samplerate <= 96000 or not math.isfinite(args.tail_seconds) or not 0.1 <= args.tail_seconds <= 10:
            parser.error("Use 8000 <= --samplerate <= 96000 e 0.1 <= --tail-seconds <= 10")
        if any(not math.isfinite(value) or value < 0 for value in
               (args.prompt_distance_m, args.prompt_distance_uncertainty_m)):
            parser.error("Distancias precisam ser finitas e nao negativas")
    routes = [part.strip().lower() for part in args.routes.split(",")]
    if any(route not in ("wifi", "phone") for route in routes):
        parser.error("--routes aceita apenas wifi,phone")
    if "phone" in routes and args.playback == "full" and args.media not in ("legacy", "pcm"):
        parser.error("--playback full no BLE exige --media legacy ou pcm; Opus usa reproducao progressiva")
    try:
        import numpy as np
        import serial
        import sounddevice as sd
    except ImportError as error:
        parser.error(f"Dependencia ausente ({error}); use tools/requirements-benchmark.txt no Python de bancada")
    if args.list_devices:
        print(sd.query_devices())
        return 0
    metadata = {"arguments": {key: [str(path) for path in value] if key == "wav" else
                             str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
                "python": sys.version, "numpy": np.__version__, "sounddevice": sd.__version__, "pyserial": serial.VERSION,
                "harness_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                "utc_start": datetime.now(timezone.utc).isoformat(),
                "measurement": "same_recording_stimulus_correlation_and_energy_onset",
                "media_validation": "AssistantMedia do mesmo turn obrigatorio: wifi=legacy, phone=perfil solicitado; bytes=payload midia ESP32 TX/RX, nao trafego total",
                "limitations": "Nao valida qualidade perceptiva; resolucao algoritmica nao e calibracao fisica; boot_id pode faltar."}
    log = RunLog(args.results, metadata)
    results, session, port, run_error, interrupted = [], None, None, False, False
    restore_errors = []
    try:
        def select_device(value):
            return int(value) if value is not None and value.isdecimal() else value

        devices = (select_device(args.input_device), select_device(args.output_device))
        extra_settings = wasapi_extra_settings(sd, devices, args.wasapi_exclusive)
        input_options = {} if extra_settings is None else {"extra_settings": extra_settings[0]}
        output_options = {} if extra_settings is None else {"extra_settings": extra_settings[1]}
        sd.check_input_settings(device=devices[0], channels=1, dtype="float32", samplerate=args.samplerate, **input_options)
        sd.check_output_settings(device=devices[1], channels=1, dtype="float32", samplerate=args.samplerate, **output_options)
        log.event("audio_configuration", portaudio=sd.get_portaudio_version(),
                  input=sd.query_devices(devices[0], "input"), output=sd.query_devices(devices[1], "output"))
        prompts = [(path, *load_prompt(path, np, args.samplerate)) for path in args.wav]
        port = open_serial(serial, args.port, 0.005)
        session = SerialSession(port, log)
        previous_turn, trial, abort = None, 0, False
        for repeat in range(args.repeats):
            for prompt_info in prompts:
                for route in routes if repeat % 2 == 0 else reversed(routes):
                    trial += 1
                    log.trial_id = f"trial-{trial:04d}"
                    log.event("trial_start", route=route, repeat=repeat + 1, wav=str(prompt_info[0]), wav_sha256=prompt_info[2])
                    result = run_trial(args, session, log, np, sd, devices, prompt_info, route, trial, repeat + 1, previous_turn)
                    results.append(result)
                    previous_turn = result.get("turn_id") or previous_turn
                    if result["status"] == "error" or result["timed_out"]:
                        abort = True
                        break
                if abort:
                    break
            if abort:
                break
    except KeyboardInterrupt:
        interrupted = True
        log.event("run_interrupted")
    except Exception as error:
        run_error = True
        log.event("run_error", error=f"{type(error).__name__}: {error}"[:320])
        print(f"[Error] {error}", file=sys.stderr)
    finally:
        log.trial_id = None
        if session is not None:
            try:
                restore_errors = restore_defaults(session)
            except Exception as error:
                restore_errors = [f"restore_failed: {error}"[:320]]
        if port is not None:
            try:
                port.close()
            except Exception as error:
                restore_errors.append(f"serial_close_failed: {error}"[:320])
        code = 130 if interrupted else exit_code(results, run_error, restore_errors)
        log.event("run_end", exit_code=code, trials=log.completed_trials, restore_errors=restore_errors)
        log.close()
        if restore_errors:
            print("[Error] Restauracao NAO confirmada: " + "; ".join(restore_errors), file=sys.stderr)
        print(f"Resultados: {log.directory.resolve()} (exit={code})", flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
