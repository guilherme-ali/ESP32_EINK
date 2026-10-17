#!/usr/bin/env python3
"""Compila o protocolo sem android.jar/Gradle e executa JUnitCore com javac/java 17."""
import hashlib
import os
from pathlib import Path
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parent
CENTRAL = "https://repo.maven.apache.org/maven2/"
ARTIFACTS = [
    ("junit/junit/4.13.2/junit-4.13.2.jar", "8ac9e16d933b6fb43bc7f576336b8f4d7eb5ba12"),
    ("org/hamcrest/hamcrest-core/1.3/hamcrest-core-1.3.jar", "42a25dc3219429f0e5d060061f71acb49bf010a0"),
]


def main():
    parent = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode"
    if not parent.is_dir():
        raise SystemExit("Pasta temporária preaprovada ausente")
    cache = parent / "ideiarec-protocol-tests"
    cache.mkdir(exist_ok=True)
    jars = []
    for artifact, pinned_sha1 in ARTIFACTS:
        jar = cache / artifact.rsplit("/", 1)[1]
        if not jar.exists():
            # Maven Central publica SHA-1 oficial para estas versões históricas, não SHA-256.
            official = urllib.request.urlopen(CENTRAL + artifact + ".sha1", timeout=30).read().decode().strip()
            if official != pinned_sha1:
                raise SystemExit("Checksum oficial Maven divergente")
            urllib.request.urlretrieve(CENTRAL + artifact, jar)
        with jar.open("rb") as stream:
            if hashlib.file_digest(stream, "sha1").hexdigest() != pinned_sha1:
                raise SystemExit(f"Checksum inválido: {jar}")
        jars.append(str(jar))
    classes = cache / "classes"
    classes.mkdir(exist_ok=True)
    sources = list((ROOT / "app/src/main/java/br/com/ideiarec/companion/protocol").glob("*.java"))
    sources += list((ROOT / "app/src/test/java/br/com/ideiarec/companion/protocol").glob("*.java"))
    cp = os.pathsep.join(jars)
    subprocess.run(["javac", "--release", "17", "-encoding", "UTF-8", "-cp", cp, "-d", str(classes),
                    *map(str, sources)], check=True)
    tests = ["br.com.ideiarec.companion.protocol.CodecPolicyWindowTest", "br.com.ideiarec.companion.protocol.RelayEngineTest"]
    subprocess.run(["java", "-cp", str(classes) + os.pathsep + cp, "org.junit.runner.JUnitCore", *tests], check=True)


if __name__ == "__main__":
    main()
