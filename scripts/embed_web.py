"""Compacta (gzip) as paginas de data/<placa>/ e gera include/<placa>/DmkWebAssets.h.
Assim o painel web vai DENTRO do firmware: uma gravacao so, e a OTA atualiza
firmware + paginas juntos. Roda sozinho antes de cada compilacao (PlatformIO).
Uso manual: python scripts/embed_web.py [serve|controle]"""
import gzip, sys
from pathlib import Path

MIME = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
        ".js": "application/javascript; charset=utf-8", ".json": "application/json",
        ".svg": "image/svg+xml", ".ico": "image/x-icon", ".png": "image/png"}

def build(root: Path, role: str) -> Path:
    src = root / "data" / role
    out = root / "include" / role / "DmkWebAssets.h"
    files = sorted(p for p in src.rglob("*") if p.is_file() and p.suffix in MIME)
    lines = ["#pragma once", "// GERADO por scripts/embed_web.py a partir de data/" + role + " - nao edite.",
             "#include <Arduino.h>", "", "struct DmkWebAsset { const char *path; const char *mime; const uint8_t *data; uint32_t len; };", ""]
    table, total_raw, total_gz = [], 0, 0
    for i, f in enumerate(files):
        raw = f.read_bytes()
        gz = gzip.compress(raw, compresslevel=9, mtime=0)
        total_raw += len(raw); total_gz += len(gz)
        rel = "/" + f.relative_to(src).as_posix()
        body = ",".join(str(b) for b in gz)
        lines.append(f"static const uint8_t DMK_WEB_{i}[] PROGMEM = {{{body}}};")
        table.append(f'  {{"{rel}", "{MIME[f.suffix]}", DMK_WEB_{i}, {len(gz)}}},')
    lines += ["", "static const DmkWebAsset DMK_WEB_ASSETS[] = {"] + table + ["};",
              f"static const size_t DMK_WEB_ASSET_COUNT = {len(files)};", "",
              "static const DmkWebAsset *dmkFindWebAsset(const String &path) {",
              "  for (size_t i = 0; i < DMK_WEB_ASSET_COUNT; i++)",
              "    if (path == DMK_WEB_ASSETS[i].path) return &DMK_WEB_ASSETS[i];",
              "  return nullptr;", "}", ""]
    text = "\n".join(lines)
    if not out.exists() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8")
    print(f"[DMK WEB] {role}: {len(files)} arquivos, {total_raw//1024} KB -> {total_gz//1024} KB gzip (no firmware)")
    return out

if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    roles = sys.argv[1:] or ["serve", "controle"]
    for r in roles:
        build(root, r)
