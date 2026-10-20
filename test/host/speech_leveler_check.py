"""Exclusive portable SpeechLeveler checks; no PIO, hardware or full runner."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", action="store_true", help="Also exercise real AssistantJob, CDC 0 and 1")
    parser.add_argument("--player", action="store_true", help="Also exercise real Player/codec/driver, CDC 0 and 1")
    args = parser.parse_args()
    cxx = shutil.which(os.environ.get("CXX", "g++"))
    if not cxx:
        raise RuntimeError("Host C++ compiler not found")
    base = Path(tempfile.gettempdir()) / "kilo"
    assert base.is_dir()
    with tempfile.TemporaryDirectory(prefix="speech-leveler-", dir=base) as tmp:
        exe = Path(tmp) / ("speech_leveler.exe" if os.name == "nt" else "speech_leveler")
        subprocess.run([cxx, "-std=c++11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "test/host/speech_leveler_tests.cpp", "-o", str(exe)], cwd=ROOT, check=True, timeout=120)
        subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=120)
        if args.runtime:
            if os.name != "nt":
                raise RuntimeError("Assistant runtime adapter requires Windows CNG")
            cjson = Path(os.environ.get("CJSON_DIR", str(ROOT / "test/host/.build/v1.7.17")))
            if not (cjson / "cJSON.c").is_file():
                raise RuntimeError("CJSON_DIR must point to the existing official cJSON source")
            obj = Path(tmp) / "cJSON.o"
            subprocess.run([cxx, "-x", "c", "-std=c89", "-O1", "-DCJSON_HIDE_SYMBOLS",
                            "-c", str(cjson / "cJSON.c"), "-o", str(obj)], cwd=ROOT, check=True, timeout=120)
            for cdc in (0, 1):
                runtime = Path(tmp) / f"assistant_cdc{cdc}.exe"
                sources = [f"src/assistant/{name}.cpp" for name in
                           ("ws_codec", "live_protocol", "transport", "assistant_job")]
                subprocess.run([cxx, "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                                "-DCJSON_HIDE_SYMBOLS", "-DASSISTANT_HOST_TEST", f"-DARDUINO_USB_CDC_ON_BOOT={cdc}",
                                "-Itest/host", "-I" + str(cjson), *sources, "test/host/assistant_host_platform.cpp",
                                "test/host/assistant_runtime_tests.cpp", str(obj), "-lbcrypt", "-o", str(runtime)],
                               cwd=ROOT, check=True, timeout=120)
                print(f"Assistant real playback, CDC={cdc}", flush=True)
                subprocess.run([str(runtime)], cwd=ROOT, check=True, timeout=180)
        if args.player:
            cc = shutil.which(os.environ.get("CC", str(Path(cxx).with_name("gcc.exe")) if os.name == "nt" else "gcc"))
            if not cc:
                raise RuntimeError("Host C compiler not found")
            flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Ilib/es8311",
                     "-Itest/host/audio_codec_tests_stubs", "-Itest/host/stubs"]
            objects = []
            for name in ("es8311", "esp_codec_dev_vol"):
                obj = Path(tmp) / f"{name}.o"
                subprocess.run([cc, "-std=c11", *flags, "-c", f"lib/es8311/{name}.c", "-o", str(obj)],
                               cwd=ROOT, check=True, timeout=120)
                objects.append(str(obj))
            for cdc in (0, 1):
                player = Path(tmp) / (f"player_cdc{cdc}" + (".exe" if os.name == "nt" else ""))
                subprocess.run([cxx, "-std=c++17", *flags, "-DAUDIO_CODEC_TEST_HOOKS", f"-DARDUINO_USB_CDC_ON_BOOT={cdc}",
                                "src/audio/codec.cpp", "src/audio/tone_test.cpp", "src/audio/player.cpp",
                                "test/host/audio_codec_lifecycle_tests.cpp", *objects, "-Wl,--wrap=calloc", "-Wl,--wrap=free",
                                "-o", str(player)], cwd=ROOT, check=True, timeout=120)
                print(f"Player real playback, CDC={cdc}", flush=True)
                subprocess.run([str(player)], cwd=ROOT, check=True, timeout=180)


if __name__ == "__main__":
    main()
