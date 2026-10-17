#!/usr/bin/env python3
"""Console de diagnostico. Acorde o aparelho antes de executar; nao repete conexoes."""
import argparse
import codecs
import sys
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", default="status",
                        choices=["listen", "status", "sync", "sync-one", "wifi", "repair-drive", "stayawake 0", "stayawake 1",
                                 "phone-pair", "phone-stop", "assistant-start", "assistant-release", "assistant-cancel",
                                  "assistant-ptt", "assistant-models", "assistant-enable",
                                  "benchmark-route auto", "benchmark-route wifi", "benchmark-route phone",
                                  "benchmark-playback full", "benchmark-playback ready",
                                  "benchmark-media legacy", "benchmark-media pcm", "benchmark-media opus64",
                                  "benchmark-media opus48", "benchmark-media opus32"])
    parser.add_argument("--port", default="COM7")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--capture-seconds", type=float, default=2.5,
                        help="Duracao de captura de assistant-ptt (0 < segundos <= 30)")
    args = parser.parse_args()
    if not 0 < args.capture_seconds <= 30:
        parser.error("--capture-seconds precisa estar entre 0 e 30 segundos")
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
    with connection:
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
        elif args.command == "wifi":
            markers = ("RTC sincronizado",)
        else:
            markers = ("[Diag]",)
        received = ""
        while time.monotonic() < deadline:
            if ptt_release_at is not None and time.monotonic() >= ptt_release_at:
                connection.write(b"assistant-release\n")
                ptt_release_at = None
            data = connection.read(max(1, connection.in_waiting))
            if data:
                text = decoder.decode(data)
                print(text, end="", flush=True)
                last_data = time.monotonic()
                received = (received + text)[-8192:]
                if "[DiagDone] " + args.command in received or any(marker in received for marker in markers):
                    done = True
            elif done and time.monotonic() - last_data >= 1:
                break
        print(decoder.decode(b"", final=True), end="", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
