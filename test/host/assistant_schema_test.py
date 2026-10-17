"""Verifica o contrato embarcado contra o schema real, sem gerar/editar arquivos."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
source = (ROOT / "src/assistant/tool_schema.h").read_text(encoding="utf-8")
embedded = source.split('R"schema(', 1)[1].split(')schema"', 1)[0]
assert json.loads(embedded) == json.loads((ROOT / "gateway/tool-schema.json").read_text(encoding="utf-8"))
print("Assistant schema: oito declarações idênticas ao gateway OK")
