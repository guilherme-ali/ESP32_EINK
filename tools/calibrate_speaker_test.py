"""Somente sinteticos, arquivos temporarios e doubles; nenhum dispositivo real."""
import importlib.util
import contextlib
import io
import json
import math
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import numpy as np

SPEC = importlib.util.spec_from_file_location("calibrate_speaker", Path(__file__).with_name("calibrate_speaker.py"))
speaker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(speaker)
TEMP_ROOT = Path(tempfile.gettempdir()) / "kilo"
RATE = 48000


def arguments(*extra):
    return speaker.parse_args(["--output-dir", str(TEMP_ROOT), *extra])


def ready_fields(args=None):
    args = args or arguments()
    return {"mode": args.mode, "amplitude_dbfs": args.amplitude_dbfs,
            "sample_rate_hz": 24000, "frequency_hz": args.frequency,
            "duration_ms": args.duration_ms, "volume_percent": args.volume,
            "settle_ms": args.settle_ms, "sequence": args.sequence,
            "regs": {"0x00": 130, "0x01": 55, "0x0D": 49,
                     "0x0E": 114, "0x12": 0, "0x13": 16,
                     "0x31": 0, "0x32": 186, "0x34": 0}}


def event(tag, fields):
    return (f"[{tag}] " + json.dumps(fields) + "\n").encode()


def fixture(frequency=1000, amplitude=0.1, harmonics=(), noise=0.00001,
            duration=1.5, delay=0.2, clip=None):
    count = round((0.5 + delay + duration + 0.4) * RATE)
    x = np.random.default_rng(23).normal(0, noise, count)
    start = round((0.5 + delay) * RATE)
    frames = round(duration * RATE)
    t = np.arange(frames) / RATE
    tone = amplitude * np.sin(2 * np.pi * frequency * t)
    for order, relative in harmonics:
        tone += amplitude * relative * np.sin(2 * np.pi * order * frequency * t + 0.3)
    fade = round(0.02 * RATE)
    tone[:fade] *= np.linspace(0, 1, fade)
    tone[-fade:] *= np.linspace(1, 0, fade)
    if clip is not None:
        tone = np.clip(tone, -clip, clip)
    x[start:start + frames] += tone
    return x.astype(np.float32)


class SpectrumTests(unittest.TestCase):
    def measure(self, x, frequency=1000, noise_frames=RATE // 2):
        return speaker.analyze(x, RATE, frequency, noise_frames)

    def test_hann_integrated_sine_rms_not_peak(self):
        result = self.measure(fixture())
        self.assertTrue(result["valid"], result)
        self.assertAlmostEqual(result["fundamental_rms"], 0.1 / math.sqrt(2), delta=1e-5)
        self.assertAlmostEqual(result["fundamental_dbfs"], -23.0103, delta=0.01)
        self.assertLess(result["thd_percent"], 0.02)
        self.assertGreaterEqual(result["stationary_window_count"], 3)
        self.assertEqual(result["window_ms"], 250)
        self.assertEqual(result["hop_ms"], 50)
        self.assertNotIn("latency_ms", result)
        self.assertIn("nao isola", result["limitation"])

    def test_known_second_through_fifth_harmonic_mixture(self):
        harmonics = [(2, 0.10), (3, 0.05), (4, 0.02), (5, 0.01)]
        result = self.measure(fixture(harmonics=harmonics))
        self.assertTrue(result["valid"], result)
        expected = 100 * math.sqrt(sum(relative ** 2 for _, relative in harmonics))
        self.assertAlmostEqual(result["thd_percent"], expected, delta=0.01)
        for item, (order, relative) in zip(result["harmonics"], harmonics):
            self.assertEqual(item["order"], order)
            self.assertAlmostEqual(item["rms"] / result["fundamental_rms"], relative, delta=1e-4)
            self.assertAlmostEqual(item["relative_db"], 20 * math.log10(relative), delta=0.03)

    def test_off_bin_997_hz_and_harmonics_have_same_normalization(self):
        for actual, requested in ((997, 997), (997, 1000), (1002, 1000)):
            with self.subTest(actual=actual, requested=requested):
                result = self.measure(fixture(frequency=actual, harmonics=[(5, 0.1)]), requested)
                self.assertTrue(result["valid"], result)
                self.assertAlmostEqual(result["measured_frequency_hz"], actual, delta=0.1)
                self.assertAlmostEqual(result["thd_percent"], 10, delta=0.03)
                self.assertAlmostEqual(result["fundamental_dbfs"], -23.0103, delta=0.01)

    def test_four_khz_fifth_harmonic_is_twenty_khz(self):
        result = self.measure(fixture(frequency=4000, harmonics=[(5, 0.02)]), 4000)
        self.assertTrue(result["valid"], result)
        self.assertAlmostEqual(result["thd_percent"], 2, delta=0.01)

    def test_zeros_latency_and_fades_do_not_bias_plateau(self):
        for delay in (0, 0.2, 1.1):
            result = self.measure(fixture(delay=delay))
            self.assertTrue(result["valid"], result)
            for start in result["selected_window_start_frames"]:
                self.assertGreater(start / RATE, 0.5 + delay + 0.02)
                self.assertLess((start / RATE) + 0.25, 0.5 + delay + 1.5 - 0.02)
            self.assertAlmostEqual(result["fundamental_dbfs"], -23.0103, delta=0.01)

    def test_clipped_speaker_tone_has_distortion_without_mic_clip(self):
        result = self.measure(fixture(amplitude=0.8, clip=0.25))
        self.assertTrue(result["valid"], result)
        self.assertGreater(result["thd_percent"], 20)
        self.assertEqual(result["waveform"]["clip_fraction"], 0)

    def test_microphone_peak_point_98_invalidates_even_outside_plateau(self):
        x = fixture()
        x[-1] = 0.99
        result = self.measure(x)
        self.assertFalse(result["valid"])
        self.assertIn("microphone_clipping", result["reasons"])
        self.assertGreater(result["waveform"]["clip_fraction"], 0)

    def test_low_snr_invalid_even_with_detectable_fundamental(self):
        result = self.measure(fixture(noise=0.012))
        self.assertFalse(result["valid"])
        self.assertIn("snr_below_20_db", result["reasons"])
        self.assertLess(result["snr_db"], 20)
        self.assertAlmostEqual(result["noise_rms"], 0.012, delta=0.0003)

    def test_silence_noise_wrong_frequency_or_ambient_tone_are_not_tone(self):
        rng = np.random.default_rng(4)
        ambient = 0.1 * np.sin(2 * np.pi * 1000 * np.arange(3 * RATE) / RATE)
        for x in (np.zeros(3 * RATE), rng.normal(0, 0.001, 3 * RATE),
                  fixture(frequency=1200), ambient):
            with self.subTest(kind=float(np.max(np.abs(x)))):
                result = self.measure(x)
                self.assertFalse(result["valid"], result)
                self.assertTrue(set(result["reasons"]) & {"no_tone", "stationary_windows_missing"})

    def test_short_tone_does_not_invent_three_stationary_windows(self):
        result = self.measure(fixture(duration=0.25))
        self.assertFalse(result["valid"])
        self.assertIn("stationary_windows_missing", result["reasons"])

    def test_noise_missing_nonfinite_or_nyquist_are_invalid(self):
        for x, frequency, noise_frames, reason in (
                (fixture(), 1000, 0, "pre_noise_missing"),
                (np.full(RATE, np.nan), 1000, RATE // 2, "empty_or_nonfinite_capture"),
                (fixture(), 5000, RATE // 2, "harmonics_outside_nyquist"),
                (np.array([]), 1000, 0, "empty_or_nonfinite_capture")):
            result = self.measure(x, frequency, noise_frames)
            self.assertFalse(result["valid"])
            self.assertIn(reason, result["reasons"])


class ProtocolTests(unittest.TestCase):
    def test_trim_command_requires_matching_firmware_echo(self):
        args = arguments("--amplitude-dbfs", "-6", "--dac-trim-db", "4")
        self.assertTrue(speaker.tone_command(args).endswith(b"cold 4\n"))
        ready = ready_fields(args)
        ready["dac_trim_db"] = 4.0
        ready["regs"]["0x32"] = 194
        protocol = speaker.ToneProtocol(args)
        protocol.feed(event("ToneReady", ready))
        protocol.feed(event("ToneDone", {"pass": True, "quiescent": True, "framecount": 36000}))
        self.assertEqual(protocol.problems(), [])
        for value in (None, 2.0, False):
            bad = dict(ready)
            if value is None:
                bad.pop("dac_trim_db")
            else:
                bad["dac_trim_db"] = value
            protocol = speaker.ToneProtocol(args)
            protocol.feed(event("ToneReady", bad))
            protocol.feed(event("ToneDone", {"pass": True, "quiescent": True}))
            self.assertIn("tone_ready_mismatch_dac_trim_db", protocol.problems())

    def test_zero_trim_keeps_legacy_command_and_validates_new_echo(self):
        args = arguments()
        self.assertTrue(speaker.tone_command(args).endswith(b"cold\n"))
        ready = ready_fields(args)
        ready["dac_trim_db"] = 2
        protocol = speaker.ToneProtocol(args)
        protocol.feed(event("ToneReady", ready))
        protocol.feed(event("ToneDone", {"pass": True, "quiescent": True}))
        self.assertIn("tone_ready_mismatch_dac_trim_db", protocol.problems())

    def test_trim_bounds(self):
        for value in ("-1", "9", "nan", "inf"):
            with self.subTest(value=value), self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                arguments("--dac-trim-db", value)

    def test_processed_tone_profile_echoes_without_changing_register_contract(self):
        args = arguments("--mode", "drc")
        protocol = speaker.ToneProtocol(args)
        protocol.feed(event("ToneReady", ready_fields(args)))
        protocol.feed(event("ToneDone", {"pass": True, "quiescent": True, "framecount": 36000}))
        self.assertEqual(protocol.problems(), [])
        self.assertEqual(protocol.ready["mode"], "drc")
        self.assertEqual(protocol.ready["regs"]["0x32"], 186)

    def test_fragmented_json_and_logs_only_store_sanitized_events(self):
        protocol = speaker.ToneProtocol(arguments())
        fields = ready_fields()
        fields.update({"api_key": "supersecret", "note": "private transcript", "token": 123})
        payload = (b"[Network] token=supersecret\n" + event("ToneReady", fields)
                   + event("ToneDone", {"pass": True, "quiescent": True, "framecount": 36000}))
        for byte in payload:
            protocol.feed(bytes([byte]))
        self.assertEqual(protocol.problems(), [])
        stored = json.dumps([protocol.ready, protocol.done])
        self.assertNotIn("supersecret", stored)
        self.assertNotIn("private transcript", stored)
        self.assertNotIn("token", stored)
        self.assertEqual(protocol.ready["regs"]["0x32"], 186)

    def test_missing_ready_done_pass_or_quiescent_rejects(self):
        for ready, done, reason in (
                (None, None, "tone_ready_missing"),
                (ready_fields(), None, "tone_done_missing"),
                (None, {"pass": True, "quiescent": True}, "done_before_ready"),
                (ready_fields(), {"pass": False, "quiescent": True}, "tone_not_passed"),
                (ready_fields(), {"pass": 1, "quiescent": True}, "tone_not_passed"),
                (ready_fields(), {"pass": "true", "quiescent": True}, "tone_not_passed"),
                (ready_fields(), {"pass": True, "quiescent": False}, "tone_not_quiescent"),
                (ready_fields(), {"pass": True}, "tone_not_quiescent")):
            protocol = speaker.ToneProtocol()
            if ready is not None:
                protocol.feed(event("ToneReady", ready))
            if done is not None:
                protocol.feed(event("ToneDone", done))
            self.assertIn(reason, protocol.problems())

    def test_wire_precision_and_safe_firmware_reason_are_preserved(self):
        args = arguments("--amplitude-dbfs", "-24.123456")
        protocol = speaker.ToneProtocol(args)
        fields = ready_fields(args)
        fields["amplitude_dbfs"] = -24.123
        protocol.feed(event("ToneReady", fields))
        protocol.feed(event("ToneDone", {"pass": True, "quiescent": True, "reason": "complete"}))
        self.assertEqual(protocol.problems(), [])
        self.assertEqual(protocol.done["reason"], "complete")
        self.assertEqual(protocol.ready["regs"]["0x34"], 0)

    def test_malformed_duplicate_oversized_and_mismatched_events_reject(self):
        for line, reason in ((b"[ToneReady] []\n", "malformed_ready"),
                             (b"[ToneDone] {bad}\n", "malformed_done"),
                             (b"x" * 9000, "serial_line_too_long")):
            protocol = speaker.ToneProtocol()
            protocol.feed(line)
            self.assertIn(reason, protocol.problems())
        protocol = speaker.ToneProtocol(arguments())
        fields = ready_fields()
        fields["frequency_hz"] = 2000
        protocol.feed(event("ToneReady", fields) * 2)
        self.assertIn("duplicate_ready", protocol.problems())
        self.assertIn("tone_ready_mismatch_frequency_hz", protocol.problems())


class FakeClock:
    def __init__(self):
        self.now = 0.0
        self.stream = None
        self.command_at = None
        self.status = False
        self.record = []

    def __call__(self):
        return self.now

    def advance(self, seconds):
        if self.stream is not None:
            count = round(seconds * RATE)
            times = self.now + np.arange(count) / RATE
            data = np.full(count, 0.000001, dtype=np.float32)
            if self.command_at is not None:
                elapsed = times - self.command_at - 0.2
                inside = (elapsed >= 0) & (elapsed < 1.5)
                data[inside] += 0.1 * np.sin(2 * np.pi * 1000 * elapsed[inside])
            self.stream.callback(data[:, None], count, None, self.status)
        self.now += seconds


class FakeStream:
    def __init__(self, clock, **kwargs):
        self.clock = clock
        self.callback = kwargs["callback"]
        self.kwargs = kwargs

    def __enter__(self):
        self.clock.stream = self
        self.clock.record.append("stream_start")
        return self

    def __exit__(self, *args):
        self.clock.stream = None
        self.clock.record.append("stream_stop")


class FakeSerial:
    def __init__(self, clock, args, *, timeout=False, disconnect=False, done=None):
        self.clock = clock
        self.args = args
        self.never_done = timeout
        self.disconnect = disconnect
        self.done_fields = {"pass": True, "quiescent": True, "framecount": 36000} if done is None else done
        self.sent_ready = False
        self.sent_done = False
        self.writes = []
        self.reset_counts = []
        self.closed = False

    @property
    def in_waiting(self):
        return 4096

    def open(self):
        if self.dtr is not False or self.rts is not False:
            raise AssertionError("DTR/RTS devem ser false ANTES de abrir")
        self.clock.record.append("serial_open")

    def reset_input_buffer(self):
        self.reset_counts.append(self.clock.now)
        if self.writes:
            raise AssertionError("reset serial apos comando")
        self.clock.record.append("reset")

    def write(self, data):
        self.writes.append(data)
        self.clock.command_at = self.clock.now
        self.clock.record.append("command")
        return len(data)

    def read(self, count):
        self.clock.advance(0.05)
        if self.disconnect:
            raise OSError("private USB error")
        if not self.sent_ready:
            self.sent_ready = True
            return event("ToneReady", ready_fields(self.args))
        if not self.never_done and not self.sent_done and self.clock.now - self.clock.command_at >= 2:
            self.sent_done = True
            return event("ToneDone", self.done_fields)
        return b""

    def close(self):
        self.closed = True


def fake_trial(*options, **serial_options):
    args = arguments(*options)
    clock = FakeClock()
    serial = FakeSerial(clock, args, **serial_options)
    streams = []

    def stream(**kwargs):
        instance = FakeStream(clock, **kwargs)
        streams.append(instance)
        return instance

    sd = SimpleNamespace(InputStream=stream, check_input_settings=Mock(),
                         query_devices=Mock(return_value={"hostapi": 1}),
                         query_hostapis=Mock(return_value={"name": "Windows WASAPI"}),
                         WasapiSettings=Mock(return_value="exclusive_settings"))
    module = SimpleNamespace(Serial=Mock(return_value=serial))
    trial = speaker.capture_once(args, sd, module, clock=clock, sleep=clock.advance)
    return args, trial, clock, serial, sd, streams


class CaptureContractTests(unittest.TestCase):
    def test_one_command_mono_input_only_preroll_then_reset_no_playback(self):
        args, trial, clock, serial, sd, streams = fake_trial()
        self.assertEqual(trial["errors"], [])
        self.assertEqual(serial.writes, [b"audio-tone dac -24 1000 1500 100 0 cold\n"])
        self.assertEqual(len(serial.reset_counts), 1)
        self.assertGreaterEqual(serial.reset_counts[0], 0.5 - 1e-8)
        self.assertGreaterEqual(trial["noise_frames"], RATE // 2)
        self.assertEqual(clock.record, ["serial_open", "stream_start", "reset", "command", "stream_stop"])
        self.assertEqual(streams[0].kwargs["device"], 9)
        self.assertEqual(streams[0].kwargs["channels"], 1)
        self.assertEqual(streams[0].kwargs["dtype"], "float32")
        self.assertLessEqual(trial["capture"].count, RATE * 10)
        self.assertTrue(serial.closed)
        self.assertEqual(trial["protocol"].problems(), [])
        sd.query_devices.assert_not_called()
        self.assertFalse(hasattr(sd, "OutputStream"))

    def test_dac_both_and_each_sequence_are_single_bounded_requests(self):
        for mode in ("dac", "both"):
            for sequence in ("cold", "capture", "resume"):
                args, trial, _, serial, _, _ = fake_trial("--mode", mode, "--sequence", sequence)
                self.assertEqual(len(serial.writes), 1)
                self.assertEqual(serial.writes[0], speaker.tone_command(args))
                self.assertEqual(trial["errors"], [])

    def test_usb_disconnect_or_timeout_preserves_raw_and_invalidates(self):
        for options, reason in (({"timeout": True}, "serial_timeout"),
                                ({"disconnect": True}, "capture_error_OSError")):
            _, trial, clock, serial, _, _ = fake_trial(**options)
            self.assertIn(reason, trial["errors"])
            self.assertGreater(trial["capture"].count, 0)
            self.assertLessEqual(trial["capture"].count, RATE * 10)
            self.assertLessEqual(clock.now, 10)
            self.assertTrue(serial.closed)
            self.assertEqual(len(serial.writes), 1)
            self.assertIn("tone_done_missing", trial["protocol"].problems())

    def test_wasapi_exclusive_is_local_input_setting_only(self):
        _, trial, _, _, sd, streams = fake_trial("--wasapi-exclusive")
        self.assertEqual(trial["errors"], [])
        sd.WasapiSettings.assert_called_once_with(exclusive=True)
        self.assertEqual(streams[0].kwargs["extra_settings"], "exclusive_settings")
        self.assertEqual(sd.check_input_settings.call_args.kwargs["extra_settings"], "exclusive_settings")

    def test_non_wasapi_exclusive_fails_before_open_or_command(self):
        args = arguments("--wasapi-exclusive")
        sd = SimpleNamespace(query_devices=Mock(return_value={"hostapi": 0}),
                             query_hostapis=Mock(return_value={"name": "MME"}))
        serial = FakeSerial(FakeClock(), args)
        trial = speaker.capture_once(args, sd, SimpleNamespace(Serial=lambda: serial))
        self.assertIn("capture_error_ValueError", trial["errors"])
        self.assertEqual(serial.writes, [])

    def test_callback_status_clipping_nonfinite_shape_and_capacity_latch(self):
        for name in speaker.CALLBACK_FLAGS:
            with self.subTest(name=name):
                buffer = speaker.CaptureBuffer(100)
                status = SimpleNamespace(**{name: True})
                buffer.callback(np.zeros((10, 1), np.float32), 10, None, status)
                buffer.callback(np.zeros((10, 1), np.float32), 10, None, False)
                self.assertIn(name, buffer.problems())
        buffer = speaker.CaptureBuffer(100)
        address = buffer.samples.ctypes.data
        buffer.callback(np.ones((1100, 1), np.float32), 1100, None, False)
        self.assertEqual(buffer.count, 1000)
        self.assertIn("capacity_exceeded", buffer.problems())
        self.assertIn("clipped", buffer.problems())
        self.assertEqual(buffer.samples.ctypes.data, address)
        buffer.callback(np.zeros((1, 2), np.float32), 1, None, False)
        self.assertIn("callback_error", buffer.problems())
        nonfinite = speaker.CaptureBuffer(100)
        nonfinite.callback(np.full((1, 1), np.nan, np.float32), 1, None, False)
        self.assertIn("nonfinite", nonfinite.problems())


class ArgumentsAndArtifactsTests(unittest.TestCase):
    def test_safety_bounds_and_finite_arguments(self):
        for option, value in (("--amplitude-dbfs", "nan"), ("--amplitude-dbfs", "inf"),
                              ("--amplitude-dbfs", "-49"), ("--amplitude-dbfs", "0"),
                              ("--volume", "101"), ("--volume", "-1"),
                              ("--duration-ms", "249"), ("--duration-ms", "2001"),
                              ("--frequency", "99"), ("--frequency", "4001"),
                              ("--frequency", "nan"), ("--settle-ms", "1001"),
                              ("--input-device", "-1"), ("--samplerate", "8000")):
            with self.subTest(option=option, value=value), patch("sys.stderr", io.StringIO()):
                with self.assertRaises(SystemExit) as stopped:
                    arguments(option, value)
                self.assertEqual(stopped.exception.code, 2)
        for value in ("-48", "-3"):
            self.assertEqual(arguments("--amplitude-dbfs", value).amplitude_dbfs, float(value))

    def test_output_dir_is_required_and_new_outside_home_refused(self):
        with patch("sys.stderr", io.StringIO()), self.assertRaises(SystemExit):
            speaker.parse_args([])
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temp:
            outside = Path(temp) / "missing"
            with patch.object(speaker.Path, "home", return_value=Path(temp) / "another"):
                with self.assertRaises(ValueError):
                    speaker.artifact_directory(outside)
            self.assertFalse(outside.exists())
            with patch.object(speaker.Path, "home", return_value=Path(temp)):
                self.assertTrue(speaker.artifact_directory(outside).is_dir())

    def test_run_directory_collision_refuses_to_reuse(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temp, \
                patch.object(speaker, "datetime") as date, \
                patch.object(speaker.uuid, "uuid4", return_value=SimpleNamespace(hex="fixed")):
            date.now.return_value.strftime.return_value = "fixedUTC"
            speaker.artifact_directory(Path(temp))
            with self.assertRaises(FileExistsError):
                speaker.artifact_directory(Path(temp))

    def test_raw_float_wav_roundtrip_unique_artifacts_and_no_clobber(self):
        args, trial, _, _, _, _ = fake_trial()
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temp:
            run = speaker.artifact_directory(Path(temp))
            second = speaker.artifact_directory(Path(temp))
            self.assertNotEqual(run, second)
            report = speaker.save_result(run, args, trial)
            self.assertTrue(report["valid"], report)
            wav = (run / "microphone.wav").read_bytes()
            self.assertEqual(wav[:4], b"RIFF")
            self.assertEqual(struct.unpack_from("<I", wav, 4)[0], len(wav) - 8)
            self.assertEqual(wav[8:16], b"WAVEfmt ")
            self.assertEqual(struct.unpack_from("<IHH", wav, 16), (18, 3, 1))
            rate = struct.unpack_from("<I", wav, 24)[0]
            self.assertEqual(struct.unpack_from("<IHHH", wav, 28), (RATE * 4, 4, 32, 0))
            self.assertEqual(wav[38:42], b"fact")
            self.assertEqual(struct.unpack_from("<II", wav, 42), (4, trial["capture"].count))
            self.assertEqual(wav[50:54], b"data")
            self.assertEqual(struct.unpack_from("<I", wav, 54)[0], len(wav) - 58)
            x = np.frombuffer(wav, dtype="<f4", offset=58)
            self.assertEqual(rate, RATE)
            np.testing.assert_array_equal(x, trial["capture"].samples[:trial["capture"].count])
            stored = json.loads((run / "result.json").read_text(encoding="utf-8"))
            self.assertEqual(stored["tone_ready"]["regs"]["0x32"], 186)
            self.assertTrue(stored["tone_done"]["quiescent"])
            with self.assertRaises(FileExistsError):
                speaker.save_result(run, args, trial)

    def test_invalid_callback_or_done_can_never_be_accepted_by_fft(self):
        for invalid in ("callback", "done", "quiescent"):
            args, trial, _, _, _, _ = fake_trial()
            if invalid == "callback":
                trial["capture"].flags |= 1
            else:
                trial["protocol"].done["pass" if invalid == "done" else "quiescent"] = False
            with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temp:
                report = speaker.save_result(Path(temp), args, trial)
                self.assertTrue(report["metrics"]["valid"])
                self.assertFalse(report["valid"])
                self.assertTrue(report["reasons"])

    def test_partial_capture_on_failure_is_still_saved(self):
        args, trial, _, _, _, _ = fake_trial(disconnect=True)
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temp:
            report = speaker.save_result(Path(temp), args, trial)
            self.assertFalse(report["valid"])
            self.assertIn("capture_error_OSError", report["reasons"])
            self.assertTrue((Path(temp) / "microphone.wav").is_file())
            self.assertNotIn("private USB error", (Path(temp) / "result.json").read_text())

    def test_main_success_invalid_and_infrastructure_exit_codes_without_hardware(self):
        for invalid, expected in ((False, 0), (True, 1)):
            args, trial, _, _, _, _ = fake_trial()
            if invalid:
                trial["errors"].append("serial_timeout")
            with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as temp, \
                    patch.dict("sys.modules", {"serial": Mock(), "sounddevice": Mock()}), \
                    patch.object(speaker, "capture_once", return_value=trial) as captured, \
                    patch("sys.stdout", io.StringIO()):
                code = speaker.main(["--output-dir", temp])
                self.assertEqual(code, expected)
                captured.assert_called_once()
                self.assertEqual(len(list(Path(temp).glob("speaker-*/result.json"))), 1)
        with patch.object(speaker, "artifact_directory", side_effect=OSError("private detail")), \
                patch("sys.stderr", io.StringIO()) as errors:
            self.assertEqual(speaker.main(["--output-dir", str(args.output_dir)]), 2)
            self.assertNotIn("private detail", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
