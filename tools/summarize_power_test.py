"""Resume o ensaio de captura. setup_us NAO mede wake nem audio analogico valido."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics


def summarize(text, repetitions):
    rows = [json.loads(line[len("[PowerTest] "):]) for line in text.splitlines()
            if line.startswith("[PowerTest] ")]
    expected = {(rep, case) for rep in range(1, repetitions + 1) for case in range(1, 9)}
    seen = {(row["rep"], row["case"]) for row in rows}
    if len(rows) != len(expected) or seen != expected:
        raise ValueError("Matriz incompleta, duplicada ou com casos inesperados")
    if "[PowerTestResult] success=1" not in text.splitlines():
        raise ValueError("Veredito final de sucesso ausente")
    groups = defaultdict(list)
    for row in rows:
        if (not all(row[key] is True for key in ("pass", "cleanup", "wav", "quiescent")) or
                any(row[key] for key in ("overflow", "failed", "session_failed", "storage_full", "timeout")) or
                row["bytes"] <= 0 or row["bytes"] != row["captured_samples"] * 2):
            raise ValueError("Falha de captura, integridade, limpeza ou estado ocioso")
        groups[(row["in_hz"], row["out_hz"])].append(row["setup_us"])
    rates = []
    for (input_hz, output_hz), values in sorted(groups.items()):
        ordered = sorted(values)
        rates.append(dict(input_hz=input_hz, output_hz=output_hz, trials=len(values),
                          codec_setup_us=dict(min=min(values), median=statistics.median(values),
                                              p95=ordered[math.ceil(len(values) * .95) - 1], max=max(values))))
    return dict(passed=True, cases=len(rows), repetitions=repetitions, rates=rates,
                heap_internal=dict(min=min(row["heap_internal"] for row in rows),
                                   max=max(row["heap_internal"] for row in rows)),
                psram=dict(min=min(row["psram"] for row in rows), max=max(row["psram"] for row in rows)),
                limits="Apenas preparo do codec; nao mede wake, prontidao analogica, qualidade acustica, corrente ou autonomia")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--repetitions", type=int, default=30)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 30:
        parser.error("--repetitions precisa estar entre 1 e 30")
    result = summarize(args.log.read_text(encoding="utf-8"), args.repetitions)
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        with args.output.open("x", encoding="utf-8") as output:
            output.write(rendered)
    print(rendered, end="")


if __name__ == "__main__":
    main()
