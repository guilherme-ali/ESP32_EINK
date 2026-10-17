"""Runner isolado dos módulos assistant. Não executa PlatformIO nem baixa dependências."""
from pathlib import Path
import ast
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "test/host"
BUILD = HOST / ".build"


def rel(path):
    try:
        return Path(path).resolve().relative_to(ROOT)
    except Exception:
        return Path(path)


def run(args, cwd=ROOT):
    rendered = [str(rel(a)) if isinstance(a, Path) and cwd == ROOT else str(a) for a in args]
    print("+", " ".join(rendered), flush=True)
    subprocess.run(rendered, cwd=cwd, check=True, timeout=180)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    assert HOST.is_dir()
    files = list((ROOT / "src/assistant").glob("*")) + list(HOST.glob("assistant_*"))
    for path in files:
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        assert text.endswith("\n"), f"Sem newline final: {path}"
        assert all(line == line.rstrip() for line in text.splitlines()), f"Whitespace: {path}"
        if path.suffix == ".py":
            ast.parse(text, filename=str(path))
    print("Assistant whitespace (inclui arquivos novos) e sintaxe Python: OK", flush=True)
    assert "i2sConfig.tx_desc_auto_clear = true;" in (ROOT / "src/audio/codec.cpp").read_text(encoding="utf-8"), "I2S deve limpar DMA em underflow"
    print("Contrato I2S: auto-clear de DMA habilitado (nao simula hardware)", flush=True)
    BUILD.mkdir(exist_ok=True)
    cjson = Path(os.environ.get("CJSON_DIR", str(BUILD / "v1.7.17")))
    if not (cjson / "cJSON.c").is_file():
        raise RuntimeError("Forneça CJSON_DIR com cJSON.c/cJSON.h oficiais ou use o cache existente do projeto.")
    cxx = shutil.which(os.environ.get("CXX", "g++"))
    if not cxx:
        raise RuntimeError("Compilador C++ host não encontrado.")
    obj = BUILD / "assistant_cJSON.o"
    run([cxx, "-x", "c", "-std=c89", "-O1", "-DCJSON_HIDE_SYMBOLS", "-c", cjson / "cJSON.c", "-o", obj])
    flags = ["-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-DCJSON_HIDE_SYMBOLS", "-I" + str(rel(cjson))]
    common = [ROOT / "src/assistant" / name for name in ("ws_codec.cpp", "live_protocol.cpp")]
    protocol = BUILD / ("assistant_protocol_tests.exe" if os.name == "nt" else "assistant_protocol_tests")
    run([cxx, *flags, *common, HOST / "assistant_protocol_tests.cpp", obj, "-o", protocol])
    run([protocol])
    run([sys.executable, HOST / "assistant_schema_test.py"])
    media = BUILD / ("media_protocol_tests.exe" if os.name == "nt" else "media_protocol_tests")
    run([cxx, *flags, ROOT / "src/assistant/media_protocol.cpp", HOST / "media_protocol_tests.cpp", "-o", media])
    run([media])
    phone = BUILD / ("phone_voice_tests.exe" if os.name == "nt" else "phone_voice_tests")
    run([cxx, *flags, "-DARDUINO_ARCH_ESP32", "-DESP32", "-DASSISTANT_HOST_TEST", "-I" + str(rel(HOST)),
         *common, ROOT / "src/assistant/media_protocol.cpp", ROOT / "src/assistant/phone_voice.cpp",
         HOST / "phone_voice_tests.cpp", obj, "-o", phone])
    run([phone])
    if os.name == "nt":
        runtime = BUILD / "assistant_runtime_tests.exe"
        run([cxx, *flags, "-pthread", "-DASSISTANT_HOST_TEST", "-I" + str(rel(HOST)), *common,
             ROOT / "src/assistant/transport.cpp", ROOT / "src/assistant/assistant_job.cpp",
             HOST / "assistant_host_platform.cpp", HOST / "assistant_runtime_tests.cpp", obj,
             "-lbcrypt", "-o", runtime])
        run([runtime])
    else:
        print("Runtime: adapter Windows/CNG; protocolo e schema são portáteis.")
    lint = shutil.which("cppcheck")
    if lint:
        config = Path(lint).resolve().parent.parent / "share/Cppcheck/cfg"
        run([lint, "--enable=warning,performance,portability", "--error-exitcode=1", "--std=c++11",
             "--inline-suppr", "--quiet", "--suppress=missingIncludeSystem", "--suppress=normalCheckLevelMaxBranches",
             "-DASSISTANT_HOST_TEST", "-DCJSON_HIDE_SYMBOLS", "-I" + str(HOST), "-I" + str(cjson),
             *common, ROOT / "src/assistant/transport.cpp", ROOT / "src/assistant/assistant_job.cpp"],
            cwd=config if (config / "std.cfg").is_file() else ROOT)
    run(["git", "-c", "core.safecrlf=false", "diff", "--check", "--", "src/assistant", "test/host/assistant_*"])
    print("Assistant: testes e verificações concluídos.")


if __name__ == "__main__":
    main()
