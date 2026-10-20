#!/usr/bin/env python3
"""Um ensaio limitado ESP speaker -> microfone PC; nunca reproduz audio no PC.

Niveis em dBFS sao relativos ao microfone, nao SPL calibrado. A THD inclui
alto-falante, sala e microfone e NAO isola o amplificador. Nenhum ganho do SO,
NVS ou controle de volume e ajustado automaticamente. Requer ESP acordado.
"""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import struct
import sys
import time
import uuid

import numpy as np

MAX_SECONDS = 10
NOISE_SECONDS = 0.5
WINDOW_SECONDS = 0.25
HOP_SECONDS = 0.05
BAND_HZ = 15
LIMITATION = "THD da cadeia alto-falante + sala + microfone; nao isola amplificador; nao e SPL calibrado."
CALLBACK_FLAGS = ("input_overflow", "input_underflow", "output_overflow",
                  "output_underflow", "priming_output")
SAFE_STRINGS = ("dac", "both", "drc", "cold", "capture", "resume", "complete", "profile", "leveler_tail", "dac_headroom",
                "initial_cleanup", "sequence", "parameters", "capture_prepare",
                "capture_read", "capture_cleanup", "playback_prepare", "readback",
                "prefill", "tone_write", "drain")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM7")
    parser.add_argument("--input-device", type=int, default=9)
    parser.add_argument("--samplerate", type=int, default=48000)
    parser.add_argument("--mode", choices=("dac", "both", "drc"), default="dac")
    parser.add_argument("--amplitude-dbfs", type=float, default=-24)
    parser.add_argument("--frequency", type=int, default=1000)
    parser.add_argument("--duration-ms", type=int, default=1500,
                        help="250..2000; tons curtos podem nao fornecer 3 janelas estacionarias")
    parser.add_argument("--volume", type=int, default=100)
    parser.add_argument("--dac-trim-db", type=float, default=0,
                        help="Trim experimental 0..8 dB; firmware valida margem pos-DAC antes de tocar")
    parser.add_argument("--settle-ms", type=int, default=0)
    parser.add_argument("--sequence", choices=("cold", "capture", "resume"), default="cold")
    parser.add_argument("--wasapi-exclusive", action="store_true")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="Diretorio existente explicito, ou novo dentro da pasta do usuario")
    args = parser.parse_args(argv)
    checks = ((math.isfinite(args.amplitude_dbfs) and -48 <= args.amplitude_dbfs <= -3,
               "--amplitude-dbfs precisa ser finito entre -48 e -3"),
              (math.isfinite(args.frequency) and 100 <= args.frequency <= 4000,
               "--frequency precisa ser finita entre 100 e 4000 Hz"),
              (250 <= args.duration_ms <= 2000, "--duration-ms deve estar entre 250 e 2000"),
               (0 <= args.volume <= 100, "--volume deve estar entre 0 e 100"),
               (math.isfinite(args.dac_trim_db) and 0 <= args.dac_trim_db <= 8,
                "--dac-trim-db precisa ser finito entre 0 e 8"),
              (0 <= args.settle_ms <= 1000, "--settle-ms deve estar entre 0 e 1000"),
              (8000 <= args.samplerate <= 192000 and 5 * args.frequency + BAND_HZ < args.samplerate / 2,
               "--samplerate precisa cobrir a quinta harmonica e sua banda abaixo de Nyquist"),
              (args.input_device >= 0, "--input-device precisa ser nao negativo"))
    for valid, message in checks:
        if not valid:
            parser.error(message)
    return args


def tone_command(args):
    trim = f" {args.dac_trim_db:g}" if args.dac_trim_db else ""
    return (f"audio-tone {args.mode} {args.amplitude_dbfs:g} {args.frequency:g} "
            f"{args.duration_ms} {args.volume} {args.settle_ms} {args.sequence}{trim}\n").encode("ascii")


def artifact_directory(output_dir):
    root = output_dir.expanduser().resolve()
    if not root.exists():
        if not root.is_relative_to(Path.home().resolve()):
            raise ValueError("Diretorio novo precisa estar dentro da pasta do usuario")
        root.mkdir(parents=True)
    if not root.is_dir():
        raise ValueError("--output-dir precisa ser um diretorio")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    run = root / f"speaker-{stamp}-{uuid.uuid4().hex}"
    run.mkdir()  # No exist_ok: even an improbable collision must never clobber.
    return run


class CaptureBuffer:
    """Callback copia somente para memoria pre-alocada; nenhuma E/S ou FFT."""

    def __init__(self, samplerate):
        self.samples = np.empty(samplerate * MAX_SECONDS, dtype=np.float32)
        self.count = 0
        self.flags = 0
        self.capacity_exceeded = False
        self.callback_error = False
        self.nonfinite = False
        self.clipped = False

    def callback(self, indata, frames, timing, status):
        try:
            for index, name in enumerate(CALLBACK_FLAGS):
                if getattr(status, name, False):
                    self.flags |= 1 << index
            if status and not self.flags:
                self.flags |= 1 << len(CALLBACK_FLAGS)
            if indata.shape != (frames, 1) or frames < 0:
                self.callback_error = True
                return
            take = min(frames, len(self.samples) - self.count)
            if take != frames:
                self.capacity_exceeded = True
            if take:
                block = indata[:take, 0]
                low, high = float(np.min(block)), float(np.max(block))
                self.nonfinite |= not (math.isfinite(low) and math.isfinite(high))
                self.clipped |= low <= -0.98 or high >= 0.98
                np.copyto(self.samples[self.count:self.count + take], block)
                self.count += take
        except Exception:
            self.callback_error = True

    def problems(self):
        result = [name for index, name in enumerate(CALLBACK_FLAGS) if self.flags & (1 << index)]
        if self.flags & (1 << len(CALLBACK_FLAGS)):
            result.append("unknown_callback_flag")
        for name in ("capacity_exceeded", "callback_error", "nonfinite", "clipped"):
            if getattr(self, name):
                result.append(name)
        return result


def safe_event(value, depth=0):
    """Retem snapshots numericos, nunca texto livre/logs/credenciais do serial."""
    if depth > 8:
        return None
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if not re.fullmatch(r"[A-Za-z0-9_]{1,64}", key):
                continue
            if re.search(r"key|token|secret|passw|ssid|credential|auth", key, re.I):
                continue
            cleaned = safe_event(item, depth + 1)
            if cleaned is not None:
                result[key] = cleaned
        return result
    if isinstance(value, list):
        return [safe_event(item, depth + 1) for item in value[:256]]
    if value is None or isinstance(value, bool):
        return value
    if isinstance(value, (int, float)) and math.isfinite(value):
        return value
    if value in SAFE_STRINGS:
        return value
    return None


class ToneProtocol:
    def __init__(self, args=None):
        self.args = args
        self.ready = None
        self.done = None
        self.errors = []
        self.pending = b""

    def feed(self, chunk):
        self.pending += chunk
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            self.line(line)
        if len(self.pending) > 8192:
            self.pending = b""
            self.errors.append("serial_line_too_long")

    def line(self, line):
        for marker, attr in ((b"[ToneReady] ", "ready"), (b"[ToneDone] ", "done")):
            if not line.startswith(marker):
                continue
            try:
                if len(line) > 8192:
                    raise ValueError()
                fields = json.loads(line[len(marker):])
                if not isinstance(fields, dict):
                    raise ValueError()
            except (ValueError, UnicodeError, RecursionError):
                self.errors.append("malformed_" + attr)
                return
            if getattr(self, attr) is not None:
                self.errors.append("duplicate_" + attr)
            elif attr == "done" and self.ready is None:
                self.errors.append("done_before_ready")
            setattr(self, attr, safe_event(fields))

    def problems(self):
        result = list(self.errors)
        if self.ready is None:
            result.append("tone_ready_missing")
        elif self.args is not None:
            expected = {"mode": self.args.mode, "amplitude_dbfs": self.args.amplitude_dbfs,
                        "sample_rate_hz": 24000, "frequency_hz": self.args.frequency,
                        "duration_ms": self.args.duration_ms, "volume_percent": self.args.volume,
                         "settle_ms": self.args.settle_ms, "sequence": self.args.sequence}
            if self.args.dac_trim_db or "dac_trim_db" in self.ready:
                expected["dac_trim_db"] = self.args.dac_trim_db
            for key, value in expected.items():
                actual = self.ready.get(key)
                # Firmware serializes amplitude as %.3f, not full float precision.
                matches = actual == value
                if key in ("amplitude_dbfs", "dac_trim_db"):
                    matches = (isinstance(actual, (int, float)) and not isinstance(actual, bool)
                               and math.isclose(actual, value, rel_tol=0, abs_tol=0.0006))
                if isinstance(actual, bool) or not matches:
                    result.append("tone_ready_mismatch_" + key)
        if self.done is None:
            result.append("tone_done_missing")
        else:
            if self.done.get("pass") is not True:
                result.append("tone_not_passed")
            if self.done.get("quiescent") is not True:
                result.append("tone_not_quiescent")
        return result


def dbfs(rms):
    return 20 * math.log10(rms) if rms > 0 else None


def analyze(samples, samplerate, frequency, noise_frames):
    """Hann power integration (Parseval), same +/-15 Hz for orders 1..5.

    Selection is a stationary plateau estimate, NOT an acoustic onset/latency.
    Three overlapping windows are descriptive, not independent observations.
    """
    x = np.asarray(samples, dtype=np.float64).reshape(-1)
    result = {"valid": False, "reasons": [], "limitation": LIMITATION,
              "reference": "microphone_dBFS_not_SPL", "window_ms": 250,
              "hop_ms": 50, "band_half_width_hz": BAND_HZ}
    reasons = result["reasons"]
    if not len(x) or not np.all(np.isfinite(x)):
        reasons.append("empty_or_nonfinite_capture")
        return result
    result["waveform"] = {"peak": float(np.max(np.abs(x))),
                          "rms": float(np.sqrt(np.mean(x * x))),
                          "clip_fraction": float(np.mean(np.abs(x) >= 0.98))}
    if result["waveform"]["peak"] >= 0.98:
        reasons.append("microphone_clipping")
    n = round(WINDOW_SECONDS * samplerate)
    hop = round(HOP_SECONDS * samplerate)
    if noise_frames < round(NOISE_SECONDS * samplerate) or noise_frames > len(x):
        reasons.append("pre_noise_missing")
        return result
    if not 100 <= frequency <= 4000 or 5 * frequency + BAND_HZ >= samplerate / 2:
        reasons.append("harmonics_outside_nyquist")
        return result
    noise = x[:round(NOISE_SECONDS * samplerate)]
    noise_rms = float(np.sqrt(np.mean(noise * noise)))
    result["noise_rms"] = noise_rms
    result["noise_dbfs"] = dbfs(noise_rms)
    window = np.hanning(n)
    bins = np.fft.rfftfreq(n, 1 / samplerate)
    masks = [np.abs(bins - order * frequency) <= BAND_HZ for order in range(1, 6)]
    scale = 2 / (n * float(np.sum(window * window)))

    def powers(block):
        spectrum = np.abs(np.fft.rfft((block - np.mean(block)) * window)) ** 2
        return np.array([float(np.sum(spectrum[mask])) * scale for mask in masks])

    starts = np.arange(noise_frames, len(x) - n + 1, hop, dtype=int)
    if len(starts) < 3:
        reasons.append("stationary_windows_missing")
        return result
    energies = np.array([powers(x[start:start + n]) for start in starts])
    fund = energies[:, 0]
    noise_band = float(np.median([powers(noise[start:start + n])[0]
                                 for start in range(0, len(noise) - n + 1, hop)]))
    peak = float(np.max(fund))
    if peak <= max(1e-16, 100 * noise_band):
        reasons.append("no_tone")
        return result
    # A high-amplitude contiguous plateau; edge windows may include fade/zeros.
    active = fund >= peak * 10 ** (-1.5 / 10)
    groups = np.split(np.flatnonzero(active), np.flatnonzero(np.diff(np.flatnonzero(active)) > 1) + 1)
    group = max(groups, key=len)
    interior = group[1:-1]
    if len(interior) < 3:
        reasons.append("stationary_windows_missing")
        return result
    center = float(np.mean(interior))
    central = interior[np.abs(interior - center) <= max(1, len(interior) // 4)]
    best = int(central[np.argmax(fund[central])])
    selected = np.array([best - 1, best, best + 1])
    if not np.all(np.isin(selected, interior)):
        selected = interior[np.argsort(np.abs(interior - center))[:3]]
    estimates = []
    for i in selected:
        block = x[starts[i]:starts[i] + n]
        spectrum = np.abs(np.fft.rfft((block - np.mean(block)) * window))
        allowed = np.flatnonzero(masks[0])
        k = int(allowed[np.argmax(spectrum[allowed])])
        left, mid, right = np.log(np.maximum(spectrum[k - 1:k + 2], 1e-30))
        denominator = left - 2 * mid + right
        delta = float(np.clip(0.5 * (left - right) / denominator, -0.5, 0.5)) if denominator else 0
        estimates.append((k + delta) * samplerate / n)
    measured_frequency = float(np.median(estimates))
    # Harmonic bands follow the observed fundamental, not accumulated clock drift.
    masks = [np.abs(bins - order * measured_frequency) <= BAND_HZ for order in range(1, 6)]
    measured = np.sqrt(np.median([powers(x[starts[i]:starts[i] + n]) for i in selected], axis=0))
    fundamental = float(measured[0])
    # A finite floor avoids Infinity in the JSON when synthetic pre-noise is zero.
    snr = 20 * math.log10(fundamental / max(noise_rms, 1e-15))
    result.update({"fundamental_rms": fundamental, "fundamental_dbfs": dbfs(fundamental),
                   "measured_frequency_hz": measured_frequency,
                   "harmonics": [{"order": order, "rms": float(measured[order - 1]),
                                  "dbfs": dbfs(float(measured[order - 1])),
                                  "relative_db": dbfs(float(measured[order - 1]) / fundamental)}
                                 for order in range(2, 6)],
                   "thd_percent": 100 * float(np.linalg.norm(measured[1:])) / fundamental,
                   "snr_db": snr, "snr_reference": "fundamental_vs_pre_noise_broadband_rms",
                   "selected_window_start_frames": sorted(int(starts[i]) for i in selected),
                   "stationary_window_count": len(selected),
                   "stationary_waveform_rms": float(np.median([
                       np.sqrt(np.mean(x[starts[i]:starts[i] + n] ** 2)) for i in selected]))})
    if snr < 20:
        reasons.append("snr_below_20_db")
    result["valid"] = not reasons
    return result


def capture_once(args, sd, serial_module, clock=time.monotonic, sleep=time.sleep):
    capture = CaptureBuffer(args.samplerate)
    protocol = ToneProtocol(args)
    result = {"command": tone_command(args).decode().strip(), "noise_frames": 0,
              "protocol": protocol, "capture": capture, "errors": []}
    connection = serial_module.Serial()
    connection.port = args.port
    connection.baudrate = 115200
    connection.timeout = 0.05
    connection.write_timeout = 0.5
    connection.dtr = False
    connection.rts = False
    try:
        extra = {}
        if args.wasapi_exclusive:
            device = sd.query_devices(args.input_device, "input")
            hostapi = sd.query_hostapis(device["hostapi"])
            if "WASAPI" not in hostapi["name"].upper():
                raise ValueError("wasapi_input_required")
            extra["extra_settings"] = sd.WasapiSettings(exclusive=True)
        sd.check_input_settings(device=args.input_device, channels=1,
                                dtype="float32", samplerate=args.samplerate, **extra)
        connection.open()
        with sd.InputStream(device=args.input_device, channels=1, dtype="float32",
                            samplerate=args.samplerate, blocksize=0,
                            callback=capture.callback, **extra):
            start = clock()
            deadline = start + MAX_SECONDS - 0.1
            while capture.count < round(NOISE_SECONDS * args.samplerate):
                if clock() - start >= 2 or capture.problems():
                    raise RuntimeError("pre_noise_capture_failed")
                sleep(0.01)
            # Only pre-command reset: never discard Ready/Done after the write.
            connection.reset_input_buffer()
            result["noise_frames"] = capture.count
            command = tone_command(args)
            if connection.write(command) != len(command):
                raise RuntimeError("serial_short_write")
            while clock() < deadline:
                data = connection.read(min(4096, max(1, connection.in_waiting)))
                if data:
                    protocol.feed(data)
                if protocol.done is not None:
                    # Keep a short acoustic tail, not a second requested test.
                    sleep(min(0.25, max(0, deadline - clock())))
                    break
            else:
                result["errors"].append("serial_timeout")
    except KeyboardInterrupt:
        result["errors"].append("interrupted")
    except Exception as error:
        # Exception messages can contain serial/network credentials: type only.
        result["errors"].append("capture_error_" + type(error).__name__)
    finally:
        try:
            connection.close()
        except Exception:
            result["errors"].append("serial_close_error")
    return result


def save_result(run, args, trial):
    capture, protocol = trial["capture"], trial["protocol"]
    raw = capture.samples[:capture.count]
    metrics = analyze(raw, args.samplerate, args.frequency, trial["noise_frames"])
    reasons = trial["errors"] + capture.problems() + protocol.problems() + metrics["reasons"]
    report = {"schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
              "valid": not reasons, "reasons": list(dict.fromkeys(reasons)),
              "limitation": LIMITATION, "command": trial["command"],
              "requested": {key: value for key, value in vars(args).items() if key != "output_dir"},
              "capture": {"frames": capture.count, "samplerate": args.samplerate,
                          "channels": 1, "dtype": "float32", "noise_frames": trial["noise_frames"],
                          "callback_problems": capture.problems()},
              "tone_ready": protocol.ready, "tone_done": protocol.done,
              "metrics": metrics, "wav": "microphone.wav"}
    # WAVEFORMATEX IEEE_FLOAT (3), cbSize=0 and fact sample count. Python's
    # wave module only supports integer PCM, so write the small RIFF header.
    # https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatex
    payload = raw.astype("<f4", copy=False).tobytes()
    with (run / "microphone.wav").open("xb") as output:
        output.write(struct.pack("<4sI4s4sIHHIIHHH4sII4sI", b"RIFF", 50 + len(payload),
                                 b"WAVE", b"fmt ", 18, 3, 1, args.samplerate,
                                 args.samplerate * 4, 4, 32, 0, b"fact", 4,
                                 len(raw), b"data", len(payload)))
        output.write(payload)
    with (run / "result.json").open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2, ensure_ascii=True, allow_nan=False)
        output.write("\n")
    return report


def main(argv=None):
    args = parse_args(argv)
    try:
        run = artifact_directory(args.output_dir)
        # Lazy imports keep synthetic tests and --help independent of hardware.
        import serial
        import sounddevice as sd
        trial = capture_once(args, sd, serial)
        report = save_result(run, args, trial)
    except (OSError, ValueError, ImportError) as error:
        print("Falha de infraestrutura: " + type(error).__name__, file=sys.stderr)
        return 2
    print(str(run / "result.json"))
    print(LIMITATION)
    print("VALIDO" if report["valid"] else "INVALIDO: " + ", ".join(report["reasons"]))
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
