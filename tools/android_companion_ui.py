#!/usr/bin/env python3
"""Inspeção/tap de controles do app de teste e associação CDM, via ADB autorizado."""
import argparse
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

PACKAGES = {"br.com.ideiarec.companion", "com.android.companiondevicemanager"}


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--tap", help="Texto exato, sem distinção de maiúsculas")
    args = parser.parse_args()
    base = ["adb", "-s", args.serial]
    subprocess.run([*base, "shell", "uiautomator", "dump", "/data/local/tmp/ideiarec-ui.xml"],
                   check=True, timeout=15, capture_output=True)
    raw = subprocess.check_output([*base, "exec-out", "cat", "/data/local/tmp/ideiarec-ui.xml"], timeout=10)
    root = ET.fromstring(raw)
    parents = {child: parent for parent in root.iter() for child in parent}
    candidates = []
    for node in root.iter("node"):
        if node.get("package") not in PACKAGES or node.get("password") == "true":
            continue
        label = node.get("text", "")
        if label:
            print(f"{node.get('bounds')}: {label}")
        if args.tap and label.casefold() == args.tap.casefold():
            target = node
            while target is not None and target.get("clickable") != "true":
                target = parents.get(target)
            if target is not None and target.get("package") in PACKAGES:
                candidates.append(target)
    if args.tap:
        if len(candidates) != 1:
            raise SystemExit("Controle ausente/ambíguo ou outra tela por cima; nenhum tap executado.")
        values = list(map(int, re.findall(r"\d+", candidates[0].get("bounds", ""))))
        if len(values) != 4:
            raise SystemExit("Bounds inválidos; nenhum tap executado.")
        x = (values[0] + values[2]) // 2
        y = (values[1] + values[3]) // 2
        subprocess.run([*base, "shell", "input", "tap", str(x), str(y)], check=True, timeout=10)
        print("Controle acionado.")


if __name__ == "__main__":
    main()
