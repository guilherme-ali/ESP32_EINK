#!/usr/bin/env python3
"""Console de diagnostico. Acorde o aparelho antes de executar; nao repete conexoes."""
import argparse
import codecs
import contextlib
import math
from pathlib import Path
import sys
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", default="status",
                        choices=["listen", "status", "battery", "boot-metrics", "sleep", "feedback-start", "feedback-cold", "feedback-stop", "power-selftest 1", "power-selftest 30", "sync", "sync-one", "wifi", "repair-drive", "stayawake 0", "stayawake 1",
                                 "phone-pair", "phone-stop", "assistant-start", "assistant-release", "assistant-cancel",
                                  "assistant-ptt", "assistant-models", "assistant-enable",
                                  "benchmark-route auto", "benchmark-route wifi", "benchmark-route phone",
                                  "benchmark-playback full", "benchmark-playback ready",
                                   "benchmark-media default", "benchmark-media legacy", "benchmark-media pcm", "benchmark-media opus64",
                                  "benchmark-media opus48", "benchmark-media opus32"])
    parser.add_argument("--port", default="COM7")
    parser.add_argument("--timeout", type=float,
                        help="Padrao: 600 s no ensaio de 30 repeticoes, 120 s no ensaio simples e 30 s nos demais")
    parser.add_argument("--log", type=Path, help="Registra texto serial em arquivo novo; diretorio deve existir")
    parser.add_argument("--capture-seconds", type=float, default=2.5,
                        help="Duracao de captura de assistant-ptt (0 < segundos <= 30)")
    args = parser.parse_args()
    if args.timeout is None:
        args.timeout = 600 if args.command == "power-selftest 30" else 120 if args.command == "power-selftest 1" else 30
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout precisa ser positivo e finito")
    if not 0 < args.capture_seconds <= 30:
        parser.error("--capture-seconds precisa estar entre 0 e 30 segundos")
    if args.log and (not args.log.parent.is_dir() or args.log.exists()):
        parser.error("--log exige diretorio existente e arquivo ainda inexistente")
    connection = serial.Serial()
    connection.port = args.port
    connection.baudrate = 115200
    connection.timeout = 0.25
    connection.dtr = False
    connection.rts = False
    try:
        connection.open()
    except serial.SerialException as error:
        print(f"Acorde o ESP32 e confira a porta {args.port}: {error}", file=sys.stderr)
        return 1
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    with connection, (args.log.open("x", encoding="utf-8") if args.log else contextlib.nullcontext()) as log:
        time.sleep(0.5)
        if args.command != "listen":
            connection.reset_input_buffer()
            first_cmd = "assistant-start" if args.command == "assistant-ptt" else args.command
            connection.write((first_cmd + "\n").encode())
        deadline = time.monotonic() + args.timeout
        ptt_release_at = time.monotonic() + args.capture_seconds if args.command == "assistant-ptt" else None
        done = False
        last_data = time.monotonic()
        if args.command == "sync":
            markers = ("[Sync] IA:",)
        elif args.command == "sync-one":
            markers = ("[Sync] Arquivos confirmados", "[Sync] Drive:", "[Sync] Transcricao:", "[Sync] Markdown:")
        elif args.command == "assistant-ptt":
            markers = ("[AssistantDone]",)
        elif args.command == "listen":
            markers = ("[Diag]", "[Status]", "[Sync] IA:", "[Sync] Arquivos confirmados", "[Sync] Drive:", "[Sync] Transcricao:", "[Sync] Markdown:", "[AssistantDone]")
        elif args.command == "status":
            markers = ("[DiagDone] status",)
        elif args.command.startswith("power-selftest "):
            markers = ("[PowerTestDone]",)
        elif args.command in ("feedback-start", "feedback-cold"):
            markers = ("[ListeningFeedback]",)
        elif args.command == "wifi":
            markers = ("RTC sincronizado",)
        else:
            markers = ("[Diag]",)
        received = ""
        while time.monotonic() < deadline:
            if ptt_release_at is not None and time.monotonic() >= ptt_release_at:
                connection.write(b"assistant-release\n")
                ptt_release_at = None
            try:
                data = connection.read(max(1, connection.in_waiting))
            except serial.SerialException as error:
                if args.command == "sleep" and "[DiagDone] sleep requested=1" in received:
                    print("[Console] USB desconectou apos pedido de sono; confirmar o wake na proxima inicializacao.")
                    break
                print(f"Falha serial durante {args.command}: {error}", file=sys.stderr)
                return 1
            if data:
                text = decoder.decode(data)
                if log:
                    log.write(text)
                    log.flush()
                print(text, end="", flush=True)
                last_data = time.monotonic()
                received = (received + text)[-8192:]
                ack_finishes = args.command not in ("feedback-start", "feedback-cold")
                if (ack_finishes and "[DiagDone] " + args.command in received) or any(marker in received for marker in markers):
                    done = True
            elif done and time.monotonic() - last_data >= 1:
                break
        print(decoder.decode(b"", final=True), end="", flush=True)
        if args.command.startswith("power-selftest ") and "[PowerTestResult] success=1" not in received.splitlines():
            print("Ensaio de energia/captura falhou ou ficou incompleto.", file=sys.stderr)
            return 2
        if args.command in ("feedback-start", "feedback-cold"):
            expected = "[FeedbackTest] cold_started=1" if args.command == "feedback-cold" else "[FeedbackTest] started=1"
            after_ack = received.split(expected, 1)[-1] if expected in received else ""
            if expected not in received.splitlines() or not any(
                    line.startswith("[ListeningFeedback] ") and "ok=1" in line.split() for line in after_ack.splitlines()):
                print("Feedback visual nao terminou com sucesso.", file=sys.stderr)
                return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
