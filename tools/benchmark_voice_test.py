import csv
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import wave

import numpy as np

MODULE_PATH = Path(__file__).with_name("benchmark_voice.py")
SPEC = importlib.util.spec_from_file_location("benchmark_voice", MODULE_PATH)
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)
TEMP_ROOT = Path(tempfile.gettempdir()) / "kilo"


def setUpModule():
    TEMP_ROOT.mkdir(exist_ok=True)


def voice(rate, duration, amplitude=0.1, seed=10):
    # Nonperiodic, bandlimited fixture: a pure sine has ambiguous correlation peaks.
    random = np.random.default_rng(seed).normal(size=round(rate * duration))
    samples = np.convolve(random, np.ones(7) / 7, mode="same")
    return (amplitude * samples / np.max(np.abs(samples))).astype(np.float32)


def event(tag, **fields):
    return {"tag": tag, "fields": {key: str(value) for key, value in fields.items()}, "value": ""}


class AcousticMetricsTest(unittest.TestCase):
    rate = 8000

    def fixture(self, amplitude=0.03):
        prompt = voice(self.rate, 0.4)
        recording = np.zeros((2 * self.rate, 1), dtype=np.float32)
        start = round(0.5 * self.rate)
        recording[start:start + len(prompt), 0] = prompt * 0.7
        finish = start + len(prompt)
        response = voice(self.rate, 0.3, amplitude, seed=20)
        onset = finish + round(0.1 * self.rate)
        recording[onset:onset + len(response), 0] = response
        return recording, prompt, start, finish, onset

    def measure(self, recording, prompt, **kwargs):
        return benchmark.onset_metrics(recording, 0, len(recording), self.rate, prompt, np, **kwargs)

    def test_aligns_real_acoustic_delay_not_output_callback_or_dac(self):
        recording, prompt, start, finish, onset = self.fixture()
        result = self.measure(recording, prompt, prompt_output_frame=start - 1600)
        self.assertEqual(result["prompt_start_frame"], start)
        self.assertEqual(result["prompt_end_frame"], finish)
        self.assertAlmostEqual(result["response_onset_frame"], onset, delta=80)
        self.assertAlmostEqual(result["latency_ms"], 100, delta=10)
        self.assertEqual(result["uncertainty_kind"], "algorithmic_resolution_only")

    def test_onset_before_350ms_is_not_discarded(self):
        recording, prompt, _, _, _ = self.fixture()
        result = self.measure(recording, prompt)
        self.assertEqual(result["detection_status"], "ok")
        self.assertLess(result["latency_ms"], 150)

    def test_quiet_voice_uses_noise_not_prompt_energy(self):
        recording, prompt, _, _, _ = self.fixture(amplitude=0.0006)
        result = self.measure(recording, prompt)
        self.assertEqual(result["detection_status"], "ok")
        self.assertLess(result["onset_threshold"], 0.0001)
        self.assertEqual(result["noise_rms"], 0)
        self.assertAlmostEqual(result["latency_ms"], 100, delta=10)

    def test_pre_stimulus_noise_never_includes_prompt(self):
        recording, prompt, start, _, _ = self.fixture()
        noise = np.random.default_rng(30).normal(scale=0.0001, size=start)
        recording[:start, 0] = noise
        result = self.measure(recording, prompt)
        self.assertEqual(result["noise_end_frame"], start - 400)
        self.assertAlmostEqual(result["noise_rms"], 0.0001, delta=0.00001)
        self.assertLess(result["onset_threshold"], 0.0004)

    def test_missing_pre_stimulus_noise_does_not_fallback_to_prompt(self):
        recording, prompt, start, _, _ = self.fixture()
        recording = recording[start - 400:]
        result = self.measure(recording, prompt)
        self.assertEqual(result["detection_reason"], "pre_stimulus_noise_missing")
        self.assertIsNone(result["noise_rms"])
        self.assertIsNone(result["latency_ms"])

    def test_reverberant_prompt_tail_is_ambiguous_not_a_response(self):
        recording, prompt, _, finish, _ = self.fixture()
        recording[finish:finish + 640, 0] = voice(self.rate, 0.08, 0.02)
        result = self.measure(recording, prompt)
        self.assertEqual(result["detection_status"], "ambiguous")
        self.assertEqual(result["detection_reason"], "prompt_tail_or_overlap")
        self.assertIsNone(result["latency_ms"])

    def test_pop_before_quiet_response_requires_review_not_late_success(self):
        recording, prompt, _, finish, _ = self.fixture(amplitude=0.002)
        recording[finish + 200, 0] = 0.8
        result = self.measure(recording, prompt)
        self.assertEqual(result["detection_status"], "ambiguous")
        self.assertEqual(result["detection_reason"], "earlier_transient")
        self.assertIsNone(result["latency_ms"])

    def test_silence_is_no_response_not_zero_latency(self):
        recording, prompt, _, finish, _ = self.fixture()
        recording[finish:] = 0
        result = self.measure(recording, prompt)
        self.assertEqual(result["detection_status"], "no_response")
        self.assertIsNone(result["latency_ms"])

    def test_silence_without_prompt_is_alignment_failure(self):
        _, prompt, _, _, _ = self.fixture()
        result = self.measure(np.zeros((16000, 1)), prompt)
        self.assertEqual(result["detection_status"], "ambiguous")
        self.assertIsNone(result["latency_ms"])

    def test_repeated_prompt_has_ambiguous_alignment(self):
        recording, prompt, start, _, _ = self.fixture()
        recording[start + 7000:start + 7000 + len(prompt), 0] = prompt * 0.7
        result = self.measure(recording, prompt)
        self.assertEqual(result["detection_reason"], "prompt_alignment")
        self.assertIsNone(result["latency_ms"])

    def test_offsets_are_relative_to_saved_wav_slice(self):
        recording, prompt, start, finish, onset = self.fixture()
        prefix = 1234
        padded = np.pad(recording, ((prefix, 400), (0, 0)))
        result = benchmark.onset_metrics(padded, prefix, prefix + len(recording), self.rate, prompt, np,
                                         prompt_output_frame=prefix + start)
        self.assertEqual(result["prompt_start_frame"], start)
        self.assertEqual(result["prompt_end_frame"], finish)
        self.assertAlmostEqual(result["response_onset_frame"], onset, delta=80)

    def test_missing_adc_or_none_dac_end_does_not_invent_time_or_throw(self):
        for timestamp in (None, float("nan"), 10):
            self.assertIsNone(benchmark.adc_frame(timestamp, [], [], [], 0, self.rate))
        recording, prompt, _, _, _ = self.fixture()
        self.assertEqual(self.measure(recording, prompt)["detection_status"], "ok")

    def test_adc_mapping_is_separate_and_relative(self):
        self.assertEqual(benchmark.adc_frame(1.005, [1.0], [2000], [80], 1, 8000, begin=1200), 840)
        self.assertIsNone(benchmark.adc_frame(2.0, [1.0], [0], [80], 1, 8000))

    def test_missing_prompt_empty_capture_and_nonfinite_do_not_throw(self):
        for recording, prompt in ((np.zeros((1000, 1)), None), (np.zeros((0, 1)), np.ones(800)),
                                  (np.full((16000, 1), np.nan), np.ones(800))):
            with self.subTest(shape=recording.shape):
                self.assertIsNone(self.measure(recording, prompt)["latency_ms"])


class PromptAndParsingTest(unittest.TestCase):
    def test_done_parser_does_not_persist_response_text(self):
        parsed = benchmark.parse_done("[AssistantDone] turn=17 state=5 captured_ms=1200 audio_rx_ms=800 audio_tx_ms=800 rebuffer=0 buffered=1 erro=segredo texto=conteudo-privado")
        self.assertEqual(parsed["state"], "5")
        self.assertEqual(parsed["turn"], "17")
        self.assertNotIn("texto", parsed)
        self.assertNotIn("erro", parsed)

    def test_serial_metric_parser_preserves_decimal_key_names(self):
        parsed = benchmark.parse_fields("[BLE] mtu=247 interval_1.25ms=12 tx_phy=2 rx_phy=2")
        self.assertEqual(parsed["interval_1.25ms"], "12")
        self.assertNotIn("interval_1", parsed)

    def test_parser_bounds_values_and_field_count(self):
        fields = benchmark.parse_fields(" ".join(f"key{i}={'a' * 200}" for i in range(100)))
        self.assertLessEqual(len(fields), 64)
        self.assertTrue(all(len(value) <= 160 for value in fields.values()))

    def test_prompt_keeps_quiet_last_phoneme_and_hash(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as folder:
            path = Path(folder) / "quiet.wav"
            samples = np.zeros(8000, dtype="<i2")
            samples[:4000] = 1000
            samples[4000:6000] = 2  # Previously discarded by absolute 0.001 trimming.
            with wave.open(str(path), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(8000)
                wav.writeframes(samples.tobytes())
            prompt, digest, rate = benchmark.load_prompt(path, np, 8000)
            self.assertEqual(len(prompt), 6000)
            self.assertEqual(len(digest), 64)
            self.assertEqual(rate, 8000)


class TurnTrackerTest(unittest.TestCase):
    def ready_event(self, **changes):
        fields = dict(turn=42, state=2, captured_ms=100, released=0, finished=0)
        fields.update(changes)
        return event("Assistant", **fields)

    def test_only_live_unreleased_capture_is_ready(self):
        tracker = benchmark.TurnTracker()
        for changes in ({"state": 5}, {"released": 1}, {"finished": 1}, {"captured_ms": 0}):
            self.assertFalse(tracker.ready([self.ready_event(**changes)]))
        self.assertTrue(tracker.ready([self.ready_event()]))

    def test_old_firmware_missing_turn_is_explicit_failure(self):
        tracker = benchmark.TurnTracker()
        sample = self.ready_event()
        del sample["fields"]["turn"]
        with self.assertRaisesRegex(RuntimeError, "turn_correlation_missing"):
            tracker.ready([sample])

    def test_stale_turn_and_restart_are_not_accepted(self):
        for turn in (1, 41, 42):
            with self.assertRaisesRegex(RuntimeError, "stale_turn"):
                benchmark.TurnTracker(42).ready([self.ready_event(turn=turn)])

    def test_metrics_and_done_must_match_current_turn(self):
        tracker = benchmark.TurnTracker()
        tracker.ready([self.ready_event()])
        tracker.observe(event("AssistantMetrics", turn=41, route="wifi"))
        tracker.observe(event("AssistantDone", turn=41, state=5))
        tracker.observe(event("AssistantDone", state=5))
        self.assertEqual(tracker.metrics, {})
        self.assertEqual(tracker.done, {})
        tracker.observe(event("AssistantMetrics", turn=42, route="ble"))
        tracker.observe(event("AssistantDone", turn=42, state=5))
        self.assertEqual(tracker.metrics["route"], "ble")
        self.assertEqual(tracker.done["state"], "5")
        self.assertIsNone(tracker.boot_id)

    def test_first_ready_status_and_completion_in_same_batch_bind_before_replay(self):
        tracker = benchmark.TurnTracker()
        events = [self.ready_event(), event("AssistantMetrics", turn=42, route="wifi"),
                  event("AssistantMedia", turn=42, media="legacy", tx_bytes=0, rx_bytes=0),
                  event("AssistantDone", turn=42, state=6)]
        self.assertTrue(tracker.ready(events))
        self.assertEqual(tracker.metrics["route"], "wifi")
        self.assertEqual(tracker.done["state"], "6")
        self.assertTrue(tracker.media_result("wifi", "opus64")["media_valid"])

    def test_phone_requires_exact_selected_profile_and_wifi_requires_legacy(self):
        for route in ("wifi", "phone"):
            for requested in benchmark.MEDIA_PROFILES:
                for actual in benchmark.MEDIA_PROFILES:
                    with self.subTest(route=route, requested=requested, actual=actual):
                        tracker = benchmark.TurnTracker()
                        tracker.ready([self.ready_event()])
                        tracker.observe(event("AssistantMedia", turn=42, media=actual, tx_bytes=123, rx_bytes=456))
                        result = tracker.media_result(route, requested)
                        expected = "legacy" if route == "wifi" else requested
                        self.assertEqual(result["media_expected"], expected)
                        self.assertEqual(result["media_actual"], actual)
                        self.assertEqual(result["media_valid"], actual == expected)
                        self.assertEqual(result["media_failure_reason"], "" if actual == expected else "media_profile_mismatch")
                        self.assertEqual(result["media_tx_wire_bytes"], 123)
                        self.assertEqual(result["media_rx_wire_bytes"], 456)

    def test_missing_or_wrong_turn_media_is_never_inferred_from_requested_profile(self):
        tracker = benchmark.TurnTracker()
        tracker.ready([self.ready_event()])
        for sample in (event("AssistantMedia", turn=41, media="opus64"), event("AssistantMedia", media="opus64")):
            tracker.observe(sample)
        for route, requested in (("wifi", "legacy"), ("wifi", "opus64"), ("phone", "opus64")):
            result = tracker.media_result(route, requested)
            self.assertFalse(result["media_valid"])
            self.assertEqual(result["media_failure_reason"], "media_telemetry_missing")
            self.assertIsNone(result["media_actual"])
            self.assertIsNone(result["media_tx_wire_bytes"])

    def test_media_wire_counters_are_optional_but_never_fabricated_or_negative(self):
        tracker = benchmark.TurnTracker()
        tracker.ready([self.ready_event()])
        for fields in ({}, {"tx_bytes": 0, "rx_bytes": 0}, {"tx_bytes": 2**64 - 1, "rx_bytes": 456}):
            tracker.observe(event("AssistantMedia", turn=42, media="pcm", **fields))
            result = tracker.media_result("phone", "pcm")
            self.assertTrue(result["media_valid"])
            self.assertEqual(result["media_tx_wire_bytes"], fields.get("tx_bytes"))
            self.assertEqual(result["media_rx_wire_bytes"], fields.get("rx_bytes"))
        for bad in (-1, "garbage", "1.5", 2**64, "9" * 160):
            tracker.observe(event("AssistantMedia", turn=42, media="pcm", tx_bytes=bad, rx_bytes=42))
            result = tracker.media_result("phone", "pcm")
            self.assertFalse(result["media_valid"])
            self.assertEqual(result["media_failure_reason"], "media_wire_bytes_invalid")
            self.assertIsNone(result["media_tx_wire_bytes"])
            self.assertEqual(result["media_rx_wire_bytes"], 42)

    def test_unknown_and_conflicting_actual_profiles_are_invalid(self):
        tracker = benchmark.TurnTracker()
        tracker.ready([self.ready_event()])
        tracker.observe(event("AssistantMedia", turn=42, media="opus128"))
        self.assertEqual(tracker.media_result("phone", "opus64")["media_failure_reason"], "media_profile_missing_or_unknown")
        tracker.observe(event("AssistantMedia", turn=42, media="opus64"))
        result = tracker.media_result("phone", "opus64")
        self.assertFalse(result["media_valid"])
        self.assertEqual(result["media_failure_reason"], "media_telemetry_conflict")


class FakePort:
    def __init__(self, incoming=b"", responses=None, chunk_size=4096):
        self.incoming = bytearray(incoming)
        self.responses = responses or {}
        self.writes = []
        self.chunk_size = chunk_size

    @property
    def in_waiting(self):
        return len(self.incoming)

    def read(self, count):
        count = min(count, self.chunk_size)
        data = bytes(self.incoming[:count])
        del self.incoming[:count]
        return data

    def write(self, data):
        self.writes.append(data)
        self.incoming.extend(self.responses.get(data, b""))
        return len(data)


class SerialSessionTest(unittest.TestCase):
    def test_old_route_ack_does_not_satisfy_new_command(self):
        port = FakePort(b"[BenchmarkRoute] wifi\n[DiagDone] benchmark-route wifi\n",
                        {b"benchmark-route wifi\n": b"[DiagDone] benchmark-route wifi\n"})
        session = benchmark.SerialSession(port, Mock())
        with self.assertRaisesRegex(RuntimeError, "command_rejected"):
            session.command("benchmark-route wifi", ("BenchmarkRoute", "wifi"))

    def test_fresh_fragmented_ack_and_barrier_are_required(self):
        port = FakePort(responses={b"benchmark-route wifi\n": b"[BenchmarkRoute] wifi\r\n[DiagDone] benchmark-route wifi\r\n"},
                        chunk_size=3)
        session = benchmark.SerialSession(port, Mock())
        observations = session.command("benchmark-route wifi", ("BenchmarkRoute", "wifi"))
        self.assertEqual([item["tag"] for item in observations], ["BenchmarkRoute", "DiagDone"])

    def test_partial_and_oversized_lines_stay_bounded(self):
        port = FakePort(b"x" * 25000 + b"\n[AssistantDone] turn=3 state=5 texto=secret\n")
        session = benchmark.SerialSession(port, Mock())
        events = []
        while port.in_waiting:
            events.extend(session.read())
            self.assertLessEqual(len(session.pending), 8192)
        self.assertEqual(len(events), 1)
        self.assertNotIn("texto", events[0]["fields"])

    def test_serial_events_filter_payload_fields(self):
        port = FakePort(b"[AssistantMetrics] turn=1 route=wifi token=secret input_bytes=99\n")
        events = benchmark.SerialSession(port, Mock()).read()
        self.assertNotIn("token", events[0]["fields"])

    def test_media_parser_preserves_profile_and_wire_bytes_without_payload_fields(self):
        port = FakePort(b"[AssistantMedia] turn=42 media=opus64 tx_bytes=123 rx_bytes=456 token=secret texto=private\n",
                        chunk_size=3)
        log, events = Mock(), []
        session = benchmark.SerialSession(port, log)
        while port.in_waiting:
            events.extend(session.read())
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["tag"], "AssistantMedia")
        self.assertEqual(events[0]["fields"], {"turn": "42", "media": "opus64", "tx_bytes": "123", "rx_bytes": "456"})
        self.assertEqual(log.event.call_args.kwargs["fields"]["media"], "opus64")

    def test_metrics_after_command_barrier_in_same_read_are_not_lost(self):
        port = FakePort(responses={b"status\n": b"[DiagDone] status\n[AssistantMetrics] turn=42 route=wifi\n[AssistantMedia] turn=42 media=legacy tx_bytes=0 rx_bytes=0\n[AssistantDone] turn=42 state=5\n"})
        tracker = benchmark.TurnTracker()
        tracker.turn_id = 42
        benchmark.SerialSession(port, Mock()).command("status", observer=tracker.observe)
        self.assertEqual(tracker.metrics["route"], "wifi")
        self.assertEqual(tracker.done["state"], "5")
        self.assertTrue(tracker.media_result("wifi", "opus64")["media_valid"])

    def test_release_write_precedes_potentially_slow_event_flush(self):
        port, log = FakePort(), Mock()
        log.event.side_effect = lambda *args, **kwargs: self.assertEqual(port.writes, [b"assistant-release\n"])
        sent = benchmark.SerialSession(port, log).send("assistant-release")
        self.assertGreater(sent, 0)
        self.assertEqual(log.event.call_args.kwargs["host_send_perf_ns"], sent)

    def test_missing_command_barrier_is_timeout_not_success(self):
        port = FakePort(responses={b"status\n": b"[Assistant] turn=8 state=2 captured_ms=200\n"})
        with self.assertRaises(TimeoutError):
            benchmark.SerialSession(port, Mock()).command("status", timeout=0.01)


class CaptureTest(unittest.TestCase):
    def test_callback_uses_new_trial_objects_not_reset_shared_counters(self):
        prompt = np.full((100, 1), 0.1, dtype=np.float32)
        first = benchmark.AudioCapture(prompt, 8000, 1000, 80, np)
        second = benchmark.AudioCapture(prompt, 8000, 1000, 80, np)
        timing = SimpleNamespace(inputBufferAdcTime=10, outputBufferDacTime=10.02)
        out = np.empty((80, 1), dtype=np.float32)
        first.callback(np.zeros_like(out), out, 80, timing, SimpleNamespace())
        self.assertEqual(first.frames, 80)
        self.assertEqual(second.frames, 0)
        first.armed = True
        first.callback(np.zeros_like(out), out, 80, timing, SimpleNamespace())
        self.assertEqual(first.prompt_output_frame, 80)
        self.assertEqual(first.prompt_offset, 80)
        self.assertIsNone(first.prompt_end_dac)
        first.callback(np.zeros_like(out), out, 80, timing, SimpleNamespace())
        self.assertAlmostEqual(first.prompt_end_dac, 10.0225)
        self.assertTrue(np.all(out[20:] == 0))
        self.assertEqual(first.frames, 240)

    def test_callback_overflow_and_capacity_exhaustion_are_counted(self):
        capture = benchmark.AudioCapture(np.ones((100, 1)), 8000, 40, 80, np)
        timing = SimpleNamespace(inputBufferAdcTime=0, outputBufferDacTime=0)
        out = np.ones((80, 1), dtype=np.float32)
        capture.callback(out.copy(), out, 80, timing, SimpleNamespace(input_overflow=True, output_underflow=True))
        self.assertEqual(capture.truncated, 1)
        self.assertEqual(capture.input_overflow, 1)
        self.assertEqual(capture.output_underflow, 1)
        self.assertTrue(np.all(out == 0))

    def test_missing_adc_does_not_abort_waveform_capture_but_missing_dac_fails_safely(self):
        for dac in (None, float("nan"), 1.0):
            capture = benchmark.AudioCapture(np.ones((100, 1)), 8000, 1000, 80, np)
            capture.armed = True
            out = np.ones((80, 1), dtype=np.float32)
            capture.callback(out.copy(), out, 80, SimpleNamespace(outputBufferDacTime=dac), SimpleNamespace())
            self.assertEqual(capture.frames, 80)
            self.assertTrue(np.isnan(capture.adc_times[0]))
            if dac == 1.0:
                self.assertEqual(capture.error, "")
            else:
                self.assertEqual(capture.error, "missing_dac_timestamp")
                self.assertIsNone(capture.prompt_end_dac)
                self.assertTrue(np.all(out == 0))


class PersistenceAndFailureTest(unittest.TestCase):
    def test_runs_do_not_overwrite_and_failed_trial_is_csv_and_jsonl(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as folder:
            first, second = benchmark.RunLog(Path(folder), {"fixture": True}), benchmark.RunLog(Path(folder), {})
            try:
                self.assertNotEqual(first.directory, second.directory)
                first.trial_id = "trial-0001"
                first.save_trial({"status": "invalid", "failure_reason": "turn_timeout", "valid": False, "timed_out": True})
                with (first.directory / "summary.csv").open(newline="", encoding="utf-8") as file:
                    rows = list(csv.DictReader(file))
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0]["failure_reason"], "turn_timeout")
                events = [json.loads(line) for line in (first.directory / "events.jsonl").read_text(encoding="utf-8").splitlines()]
                self.assertEqual(events[-1]["event"], "trial_result")
                self.assertEqual(events[-1]["trial_id"], "trial-0001")
                self.assertEqual(events[-1]["run_id"], first.run_id)
                self.assertLess(events[0]["event_id"], events[-1]["event_id"])
                self.assertLessEqual(events[0]["host_perf_ns"], events[-1]["host_perf_ns"])
                with self.assertRaises(FileExistsError):
                    first.save_trial({"valid": True})
            finally:
                first.close()
                second.close()

    def test_timeout_or_restore_error_never_returns_success(self):
        self.assertEqual(benchmark.exit_code([{"valid": True}]), 0)
        self.assertNotEqual(benchmark.exit_code([]), 0)
        self.assertNotEqual(benchmark.exit_code([{"valid": False, "timed_out": True}]), 0)
        self.assertNotEqual(benchmark.exit_code([{"valid": True}], restore_errors=["route unconfirmed"]), 0)
        self.assertEqual(benchmark.exit_code([{"valid": False, "status": "error"}]), 2)

    def test_route_setup_failure_is_still_a_trial_in_csv_and_jsonl(self):
        args = SimpleNamespace(playback="ready", media="legacy", samplerate=8000)
        session = Mock()
        session.command.side_effect = TimeoutError("route_ack_missing")
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as folder:
            log = benchmark.RunLog(Path(folder), {})
            log.trial_id = "trial-0001"
            try:
                result = benchmark.run_trial(args, session, log, np, Mock(), (0, 1),
                                             (Path("fixture.wav"), voice(8000, 0.4), "digest", 8000),
                                             "wifi", 1, 1, None)
                self.assertEqual(result["status"], "error")
                self.assertIn("route_ack_missing", result["failure_reason"])
                self.assertFalse(result["valid"])
                with (log.directory / "summary.csv").open(newline="", encoding="utf-8") as file:
                    self.assertEqual(len(list(csv.DictReader(file))), 1)
                self.assertEqual(log.completed_trials, 1)
            finally:
                log.close()

    def test_restore_retries_busy_and_requires_ack_without_hiding_errors(self):
        session = Mock()
        session.command.side_effect = [[], RuntimeError("busy"), [], [], []]
        with patch.object(benchmark.time, "sleep"):
            self.assertEqual(benchmark.restore_defaults(session), [])
        commands = [call.args[0] for call in session.command.call_args_list]
        self.assertEqual(commands.count("benchmark-route auto"), 2)
        self.assertEqual(commands[-1], "benchmark-media legacy")
        session = Mock()
        session.command.side_effect = TimeoutError("disconnected")
        with patch.object(benchmark.time, "sleep"):
            errors = benchmark.restore_defaults(session)
        self.assertTrue(errors)
        self.assertEqual(session.command.call_count, 151)

    def test_turn_timeout_and_none_prompt_end_still_save_recording_csv_and_jsonl(self):
        class Clock:
            now = 0

            def monotonic(self):
                self.now += 0.01
                return self.now

            def sleep(self, delay):
                self.now += delay

        class Session:
            started = False

            def command(self, command, *args, observer=None, **kwargs):
                if command == "assistant-start":
                    self.started = True
                if command == "status" and self.started:
                    return [event("Assistant", turn=1, state=2, captured_ms=100, released=0, finished=0)]
                if command == "assistant-cancel" and observer:
                    observer(event("AssistantDone", turn=1, state=6))
                return []

            def read(self):
                return []

        class Stream:
            device, latency, blocksize, samplerate = (0, 1), (0.01, 0.01), 80, 8000
            time = 0

            def __init__(self, **kwargs):
                self.capture = kwargs["callback"].__self__

            def start(self):
                self.capture.frames = 4000  # Stub pre-stimulus silence, no real audio.

            def stop(self):
                pass

            def close(self):
                pass

        args = SimpleNamespace(playback="ready", media="legacy", samplerate=8000, timeout=0.2, tail_seconds=0.01)
        clock = Clock()
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as folder:
            log = benchmark.RunLog(Path(folder), {})
            log.trial_id = "trial-0001"
            try:
                with patch.object(benchmark.time, "monotonic", clock.monotonic), patch.object(benchmark.time, "sleep", clock.sleep):
                    result = benchmark.run_trial(args, Session(), log, np, SimpleNamespace(Stream=Stream), (0, 1),
                                                 (Path("fixture.wav"), voice(8000, 0.4).reshape(-1, 1), "digest", 8000),
                                                 "wifi", 1, 1, None)
                self.assertTrue(result["timed_out"])
                self.assertFalse(result["valid"])
                self.assertIsNone(result["latency_ms"])
                self.assertIn("turn_timeout", result["failure_reason"])
                self.assertTrue(Path(result["recording"]).is_file())
                with (log.directory / "summary.csv").open(newline="", encoding="utf-8") as file:
                    self.assertEqual(len(list(csv.DictReader(file))), 1)
                self.assertNotEqual(benchmark.exit_code([result]), 0)
            finally:
                log.close()

    def test_media_profile_gates_synthetic_trial_and_persists_json_csv(self):
        prompt = voice(8000, 0.4).reshape(-1, 1)
        recording = np.zeros((16000, 1), dtype=np.float32)
        recording[4000:7200] = prompt * 0.7
        recording[8000:10400, 0] = voice(8000, 0.3, 0.03, seed=20)

        class Session:
            started = released = delivered = False

            def __init__(self, route, actual, media_turn):
                self.route, self.actual, self.media_turn = route, actual, media_turn

            def command(self, command, *args, **kwargs):
                if command == "assistant-start":
                    self.started = True
                if command == "status" and self.started and not self.delivered:
                    return [event("Assistant", turn=7, state=2, captured_ms=100, released=0, finished=0)]
                return []

            def send(self, command):
                self.released = True
                return 123

            def read(self):
                if self.released and not self.delivered:
                    self.delivered = True
                    events = [event("AssistantMetrics", turn=7, route="wifi" if self.route == "wifi" else "ble")]
                    if self.actual is not None:
                        events.append(event("AssistantMedia", turn=self.media_turn, media=self.actual,
                                            tx_bytes=0 if self.actual == "legacy" else 123,
                                            rx_bytes=0 if self.actual == "legacy" else 456))
                    return events + [event("AssistantDone", turn=7, state=5)]
                return []

        class Stream:
            device, latency, blocksize, samplerate, time = (0, 1), (0.01, 0.01), 80, 8000, 2

            def __init__(self, **kwargs):
                self.capture = kwargs["callback"].__self__

            def start(self):
                self.capture.recording[:len(recording)] = recording
                self.capture.frames = len(recording)
                self.capture.prompt_output_frame = 4000
                self.capture.prompt_end_dac = 0.9

            def stop(self):
                pass

            def close(self):
                pass

        args = SimpleNamespace(playback="ready", media="legacy", samplerate=8000, timeout=10, tail_seconds=0.01)
        analyze = benchmark.onset_metrics
        cases = (("wifi", "legacy", "legacy", 7, False, ""),
                 ("wifi", "opus64", "legacy", 7, False, ""),
                 ("phone", "opus64", "opus64", 7, False, ""),
                 ("phone", "opus64", "pcm", 7, False, "media_profile_mismatch"),
                 ("wifi", "opus64", "opus64", 7, False, "media_profile_mismatch"),
                 ("wifi", "legacy", None, 7, False, "media_telemetry_missing"),
                 ("phone", "opus64", None, 7, False, "media_telemetry_missing"),
                 ("phone", "opus64", "opus64", 6, False, "media_telemetry_missing"),
                 ("wifi", "legacy", "legacy", 7, True, "synthetic_analysis_failure"))
        for route, requested, actual, media_turn, failure, reason in cases:
            with self.subTest(route=route, requested=requested, actual=actual, media_turn=media_turn, analysis_error=failure), tempfile.TemporaryDirectory(dir=TEMP_ROOT) as folder:
                args.media = requested
                log = benchmark.RunLog(Path(folder), {})
                log.trial_id = "trial-0001"
                try:
                    with patch.object(benchmark, "onset_metrics", wraps=analyze,
                                      side_effect=RuntimeError("synthetic_analysis_failure") if failure else None) as detector:
                        result = benchmark.run_trial(args, Session(route, actual, media_turn), log, np, SimpleNamespace(Stream=Stream), (0, 1),
                                                     (Path("fixture.wav"), prompt, "digest", 8000), route, 1, 1, None)
                    if failure:
                        self.assertEqual(result["status"], "error")
                    else:
                        self.assertEqual(result["status"], "invalid" if reason else "valid")
                        self.assertAlmostEqual(result["latency_ms"], 100, delta=10)
                        self.assertIsNone(result["prompt_dac_end_adc_frame_diagnostic"])
                        with wave.open(result["recording"], "rb") as wav:
                            saved = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float32) / 32768
                        np.testing.assert_array_equal(detector.call_args.args[0].reshape(-1), saved)
                    self.assertEqual(result["valid"], not reason)
                    self.assertEqual(benchmark.exit_code([result]), 2 if failure else 1 if reason else 0)
                    if reason:
                        self.assertIn(reason, result["failure_reason"])
                    saved_result = json.loads((log.directory / "trial-0001.json").read_text(encoding="utf-8"))
                    self.assertEqual(saved_result["media_expected"], "legacy" if route == "wifi" else requested)
                    self.assertEqual(saved_result["media_actual"], actual if media_turn == 7 else None)
                    if actual is not None and media_turn == 7:
                        self.assertEqual(saved_result["assistant_media"]["turn"], "7")
                        self.assertEqual(saved_result["media_tx_wire_bytes"], 0 if actual == "legacy" else 123)
                        self.assertEqual(saved_result["media_rx_wire_bytes"], 0 if actual == "legacy" else 456)
                    else:
                        self.assertFalse(saved_result["media_valid"])
                        self.assertIsNone(saved_result["media_tx_wire_bytes"])
                        self.assertIsNone(saved_result["media_rx_wire_bytes"])
                    with (log.directory / "summary.csv").open(newline="", encoding="utf-8") as file:
                        rows = list(csv.DictReader(file))
                    self.assertEqual(len(rows), 1)
                    self.assertEqual(rows[0]["media_expected"], saved_result["media_expected"])
                    self.assertEqual(rows[0]["media_actual"], saved_result["media_actual"] or "")
                    self.assertEqual(rows[0]["media_tx_wire_bytes"], str(saved_result["media_tx_wire_bytes"]) if saved_result["media_tx_wire_bytes"] is not None else "")
                finally:
                    log.close()


if __name__ == "__main__":
    unittest.main()
