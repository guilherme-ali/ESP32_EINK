"""Focused codec tests: original volume cases plus real ES8311 lifecycle/player."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "test/host"


def main():
    cxx = shutil.which(os.environ.get("CXX", r"C:\Strawberry\c\bin\g++.exe" if os.name == "nt" else "g++"))
    cc = shutil.which(os.environ.get("CC", str(Path(cxx).with_name("gcc.exe")) if os.name == "nt" else "gcc"))
    if not cxx or not cc:
        raise RuntimeError("C/C++ host compiler not found")
    common = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Ilib/es8311",
              "-Itest/host/audio_codec_tests_stubs", "-Itest/host/stubs"]

    def run(args):
        # MinGW ld cannot reliably decode accented absolute output paths.
        rendered = [str(x.relative_to(ROOT)) if isinstance(x, Path) and x.is_relative_to(ROOT)
                    else str(x) for x in args]
        subprocess.run(rendered, cwd=ROOT, check=True, timeout=120)

    # Keep executable cleanup outside Google Drive's sync/locking path.
    build_root = Path(tempfile.gettempdir()) / "kilo" if os.name == "nt" else HOST / ".build"
    build_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="audio-codec-", dir=build_root) as temporary:
        build = Path(temporary)
        suffix = ".exe" if os.name == "nt" else ""
        original = build / ("volume" + suffix)
        run([cxx, "-std=c++17", *common, "src/audio/codec.cpp", HOST / "audio_codec_volume_tests.cpp",
             "-o", original])
        run([original])
        objects = []
        for name in ("es8311", "esp_codec_dev_vol"):
            obj = build / (name + ".o")
            run([cc, "-std=c11", *common, "-c", ROOT / f"lib/es8311/{name}.c", "-o", obj])
            objects.append(obj)
        lifecycle = build / ("lifecycle" + suffix)
        run([cxx, "-std=c++17", *common, "-DAUDIO_CODEC_TEST_HOOKS", "-DARDUINO_USB_CDC_ON_BOOT=1",
             "src/audio/codec.cpp", "src/audio/tone_test.cpp", "src/audio/player.cpp",
             HOST / "audio_codec_lifecycle_tests.cpp", *objects,
             "-Wl,--wrap=calloc", "-Wl,--wrap=free", "-o", lifecycle])
        run([lifecycle])


if __name__ == "__main__":
    main()
