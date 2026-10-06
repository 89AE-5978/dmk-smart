#!/usr/bin/env python3
"""DMK SMART - ferramenta de linha de comando usada pelo DMK.bat.

  python scripts/dmk.py preparar          instala/verifica PlatformIO
  python scripts/dmk.py portas            lista portas USB/COM
  python scripts/dmk.py compilar [ENV...] compila (sem ENV = as 6 placas)
  python scripts/dmk.py gravar ENV PORTA [--apagar]
  python scripts/dmk.py monitor PORTA
  python scripts/dmk.py versao SERVE|CONTROLE|AMBOS [patch|minor|major]
  python scripts/dmk.py github usuario/repositorio
  python scripts/dmk.py publicar          envia tag -> GitHub compila e publica
  python scripts/dmk.py publicar-local    publica os .bin de dist/ com o gh CLI
  python scripts/dmk.py manifest --repo R --tag T   (usado pelo GitHub Actions)
  python scripts/dmk.py status            resumo do projeto
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_H = ROOT / "include" / "DmkVersion.h"
REPO_FILE = ROOT / "dmk-github.txt"
DIST = ROOT / "dist"
ENVS = ["SERVE_ESP32", "SERVE_ESP32S3", "SERVE_ESP8266", "CONTROLE_ESP32", "CONTROLE_ESP32S3", "CONTROLE_ESP8266"]

if os.name == "nt":
    os.system("")  # ativa cores ANSI no console do Windows 10+
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

C = {"ok": "\033[92m", "err": "\033[91m", "warn": "\033[93m", "info": "\033[96m", "b": "\033[1m", "0": "\033[0m"}


def say(kind, text):
    icon = {"ok": "✔", "err": "✖", "warn": "!", "info": "›"}.get(kind, "•")
    print(f"{C.get(kind, '')}{icon} {text}{C['0']}")


def die(text, code=1):
    say("err", text)
    sys.exit(code)


def run(cmd, check=True, capture=False, cwd=ROOT):
    shown = " ".join(str(c) for c in cmd)
    if not capture:
        print(f"{C['info']}$ {shown}{C['0']}")
    r = subprocess.run([str(c) for c in cmd], cwd=str(cwd), text=True,
                       capture_output=capture, encoding="utf-8", errors="replace")
    if check and r.returncode != 0:
        if capture:
            print(r.stdout or "", r.stderr or "")
        die(f"Comando falhou ({r.returncode}): {shown}")
    return r


# ----------------------------------------------------------------- PlatformIO
def pio_cmd():
    """Retorna o comando do PlatformIO (venv do projeto > instalado no sistema)."""
    venv = ROOT / ".venv"
    exe = venv / ("Scripts/pio.exe" if os.name == "nt" else "bin/pio")
    if exe.exists():
        return [str(exe)]
    found = shutil.which("pio") or shutil.which("platformio")
    if found:
        return [found]
    return None


def cmd_preparar(_):
    say("info", f"Python {sys.version.split()[0]} em {sys.executable}")
    if sys.version_info < (3, 9):
        die("Instale Python 3.9 ou mais novo (python.org) marcando 'Add to PATH'.")
    venv = ROOT / ".venv"
    py = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if not py.exists():
        say("info", "Criando ambiente isolado .venv (uma vez so)...")
        run([sys.executable, "-m", "venv", str(venv)])
    pio = venv / ("Scripts/pio.exe" if os.name == "nt" else "bin/pio")
    if not pio.exists():
        say("info", "Instalando PlatformIO (alguns minutos na primeira vez)...")
        run([py, "-m", "pip", "install", "--upgrade", "pip"], check=False)
        run([py, "-m", "pip", "install", "--upgrade", "platformio"])
    r = run([pio, "--version"], capture=True)
    say("ok", r.stdout.strip())
    for tool, why in (("git", "publicar versoes no GitHub"), ("gh", "publicar sem GitHub Actions (opcional)")):
        say("ok" if shutil.which(tool) else "warn", f"{tool}: {'encontrado' if shutil.which(tool) else 'nao encontrado'} ({why})")
    say("ok", "Ferramentas prontas. Na primeira compilacao o PlatformIO baixa os compiladores (internet).")


def need_pio():
    p = pio_cmd()
    if not p:
        die("PlatformIO nao instalado. Use a opcao 1 do DMK.bat (Preparar ferramentas).")
    return p


def cmd_portas(_):
    r = run(need_pio() + ["device", "list", "--json-output"], capture=True, check=False)
    try:
        ports = json.loads(r.stdout or "[]")
    except json.JSONDecodeError:
        ports = []
    if not ports:
        say("warn", "Nenhuma porta encontrada. Conecte a placa no USB (cabo de DADOS) e instale o driver CH340/CP210x.")
        return
    for p in ports:
        desc = (p.get("description") or "").replace("n/a", "").strip()
        hwid = p.get("hwid") or ""
        chip = "CH340" if "1A86" in hwid.upper() else "CP210x" if "10C4" in hwid.upper() else "USB nativo" if "303A" in hwid.upper() else ""
        print(f"  {C['b']}{p.get('port')}{C['0']}  {desc} {('· ' + chip) if chip else ''}")


def cmd_compilar(a):
    envs = a.envs or ENVS
    bad = [e for e in envs if e not in ENVS]
    if bad:
        die("Placa desconhecida: " + ", ".join(bad) + ". Use: " + ", ".join(ENVS))
    args = need_pio() + ["run"]
    for e in envs:
        args += ["-e", e]
    run(args)
    say("ok", "Compilado. Arquivos em dist/:")
    for f in sorted(DIST.glob("*.bin")):
        print(f"   {f.name}  ({f.stat().st_size // 1024} KB)")


def cmd_gravar(a):
    if a.env not in ENVS:
        die("Placa desconhecida. Use: " + ", ".join(ENVS))
    pio = need_pio()
    if a.apagar:
        say("warn", "Apagando a memoria da placa (instalacao limpa: Wi-Fi, pareamento e configuracoes serao zerados)...")
        run(pio + ["run", "-e", a.env, "-t", "erase", "--upload-port", a.porta])
    r = run(pio + ["run", "-e", a.env, "-t", "upload", "--upload-port", a.porta], check=False)
    if r.returncode != 0:
        say("warn", "Falhou em 460800 baud. Tentando de novo mais devagar (115200)...")
        env = dict(os.environ, DMK_UPLOAD_SPEED="115200")
        r = subprocess.run(pio + ["run", "-e", a.env, "-t", "upload", "--upload-port", a.porta], cwd=str(ROOT), env=env)
        if r.returncode != 0:
            die("Gravacao falhou. Dica: segure o botao BOOT da placa ao iniciar a gravacao e confira o cabo/driver.")
    say("ok", f"{a.env} gravado em {a.porta}. O painel ja vai junto no firmware.")
    role = a.env.split("_")[0]
    if role == "SERVE":
        say("info", "Primeiro uso: conecte no Wi-Fi 'DMK-SERVE-xxxxxx' (senha 12345678) e abra http://192.168.4.1")
    else:
        say("info", "Controle novo procura o SERVE sozinho por 3 minutos (LED azul piscando rapido).")


def cmd_monitor(a):
    say("info", "Monitor serial (Ctrl+C para sair).")
    subprocess.call(need_pio() + ["device", "monitor", "-p", a.porta, "-b", "115200"], cwd=str(ROOT))


# ------------------------------------------------------------------- versoes
def read_versions():
    text = VERSION_H.read_text(encoding="utf-8")
    return text, dict(re.findall(r'#define DMK_VERSION_(\w+) "([^"]+)"', text))


def bump(v, kind):
    a, b, c = (int(x) for x in v.split("."))
    if kind == "major":
        return f"{a + 1}.0.0"
    if kind == "minor":
        return f"{a}.{b + 1}.0"
    return f"{a}.{b}.{c + 1}"


def cmd_versao(a):
    text, ver = read_versions()
    roles = ["SERVE", "CONTROLE"] if a.placa == "AMBOS" else [a.placa]
    for role in roles:
        new = bump(ver[role], a.tipo)
        text = re.sub(rf'(#define DMK_VERSION_{role} )"[^"]+"', rf'\g<1>"{new}"', text)
        say("ok", f"{role}: {ver[role]} -> {new}")
    VERSION_H.write_text(text, encoding="utf-8")
    say("info", "Agora escolha 'Publicar no GitHub' para gerar a versao para as placas.")


# -------------------------------------------------------------------- github
def repo_name(required=True):
    repo = REPO_FILE.read_text(encoding="utf-8").strip() if REPO_FILE.exists() else ""
    if required and not re.fullmatch(r"[\w.-]+/[\w.-]+", repo or ""):
        die("Configure o GitHub primeiro (DMK.bat > Configurar GitHub).")
    return repo


def git(*args, check=True, capture=False):
    if not shutil.which("git"):
        die("Git nao encontrado. Instale em https://git-scm.com e abra o DMK.bat de novo.")
    return run(["git", *args], check=check, capture=capture)


def cmd_github(a):
    repo = a.repo.strip().replace("https://github.com/", "").rstrip("/").removesuffix(".git")
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
        die("Formato invalido. Use usuario/repositorio (ex.: danilo/dmk-smart).")
    REPO_FILE.write_text(repo + "\n", encoding="utf-8")
    say("ok", f"Repositorio salvo: {repo}")
    if not (ROOT / ".git").exists():
        git("init", "-b", "main")
    remotes = git("remote", capture=True).stdout.split()
    url = f"https://github.com/{repo}.git"
    if "origin" in remotes:
        git("remote", "set-url", "origin", url)
    else:
        git("remote", "add", "origin", url)
    say("info", f"Crie o repositorio vazio em https://github.com/new com o nome '{repo.split('/')[1]}' (publico).")
    say("info", "Depois use 'Publicar no GitHub'. Na primeira vez o Git pede login no navegador.")


def release_tag():
    _, ver = read_versions()
    return f"dmk-s{ver['SERVE']}-c{ver['CONTROLE']}", ver


def cmd_publicar(_):
    repo = repo_name()
    tag, ver = release_tag()
    existing = git("tag", "--list", tag, capture=True).stdout.strip()
    if existing:
        die(f"A versao {tag} ja foi publicada. Use 'Nova versao' antes de publicar de novo.")
    git("add", "-A")
    st = git("status", "--porcelain", capture=True).stdout.strip()
    if st:
        git("commit", "-m", f"DMK Smart SERVE {ver['SERVE']} / CONTROLE {ver['CONTROLE']}")
    git("tag", "-a", tag, "-m", f"SERVE {ver['SERVE']} / CONTROLE {ver['CONTROLE']}")
    git("push", "-u", "origin", "HEAD")
    git("push", "origin", tag)
    say("ok", f"Enviado {tag}. O GitHub esta compilando as 6 placas (5-10 min).")
    say("info", f"Acompanhe: https://github.com/{repo}/actions  ·  Versoes: https://github.com/{repo}/releases")


def build_manifest(repo, tag):
    if not (DIST / "manifest.json").exists():
        die("dist/manifest.json nao existe. Compile antes.")
    doc = json.loads((DIST / "manifest.json").read_text(encoding="utf-8"))
    _, ver = read_versions()
    out = {"system": "DMK SMART", "release": tag,
           "published": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
           "versions": ver, "targets": {}}
    for env, t in sorted(doc.get("targets", {}).items()):
        f = DIST / t["file"]
        if not f.exists():
            continue
        data = f.read_bytes()
        out["targets"][env] = {"version": t["version"], "file": t["file"], "size": len(data),
                               "sha256": hashlib.sha256(data).hexdigest(),
                               "url": f"https://github.com/{repo}/releases/download/{tag}/{t['file']}"}
    missing = [e for e in ENVS if e not in out["targets"]]
    (DIST / "manifest.json").write_text(json.dumps(out, indent=2, ensure_ascii=False), encoding="utf-8")
    notes = [f"## DMK Smart — SERVE {ver['SERVE']} · CONTROLE {ver['CONTROLE']}", "",
             "Atualize pelo painel da placa (Atualizações) ou pelo app **ATUALIZAR-DMK.html**.", "",
             "| Placa | Versão | Tamanho |", "|---|---|---|"]
    for env, t in out["targets"].items():
        notes.append(f"| {env} | {t['version']} | {t['size'] // 1024} KB |")
    news = ROOT / "NOVIDADES.md"
    if news.exists():
        notes += ["", news.read_text(encoding="utf-8")]
    (DIST / "NOTAS.md").write_text("\n".join(notes) + "\n", encoding="utf-8")
    say("ok", f"Manifesto: {len(out['targets'])} placas" + (f" (faltando: {', '.join(missing)})" if missing else ""))
    return out


def cmd_manifest(a):
    build_manifest(a.repo, a.tag)


def cmd_publicar_local(_):
    repo = repo_name()
    if not shutil.which("gh"):
        die("GitHub CLI (gh) nao encontrado. Instale em https://cli.github.com ou use 'Publicar no GitHub'.")
    tag, ver = release_tag()
    cmd_compilar(argparse.Namespace(envs=None))
    build_manifest(repo, tag)
    files = [str(p) for p in sorted(DIST.glob("*.bin"))] + [str(DIST / "manifest.json")]
    run(["gh", "release", "create", tag, *files, "--repo", repo, "--latest",
         "--title", f"DMK Smart SERVE {ver['SERVE']} · CONTROLE {ver['CONTROLE']}", "--notes-file", str(DIST / "NOTAS.md")])
    say("ok", f"Publicado: https://github.com/{repo}/releases/tag/{tag}")


def cmd_status(_):
    _, ver = read_versions()
    repo = repo_name(required=False)
    print(f"  {C['b']}SERVE{C['0']} {ver.get('SERVE')}   {C['b']}CONTROLE{C['0']} {ver.get('CONTROLE')}")
    print(f"  GitHub: {repo or '(nao configurado)'}")
    print(f"  PlatformIO: {'pronto' if pio_cmd() else 'nao instalado (opcao 1)'}")
    bins = sorted(DIST.glob("*.bin"))
    print(f"  Compilados em dist/: {len(bins)}")


def main():
    p = argparse.ArgumentParser(description="DMK SMART")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("preparar").set_defaults(fn=cmd_preparar)
    sub.add_parser("portas").set_defaults(fn=cmd_portas)
    s = sub.add_parser("compilar"); s.add_argument("envs", nargs="*"); s.set_defaults(fn=cmd_compilar)
    s = sub.add_parser("gravar"); s.add_argument("env"); s.add_argument("porta"); s.add_argument("--apagar", action="store_true"); s.set_defaults(fn=cmd_gravar)
    s = sub.add_parser("monitor"); s.add_argument("porta"); s.set_defaults(fn=cmd_monitor)
    s = sub.add_parser("versao"); s.add_argument("placa", choices=["SERVE", "CONTROLE", "AMBOS"]); s.add_argument("tipo", nargs="?", default="patch", choices=["patch", "minor", "major"]); s.set_defaults(fn=cmd_versao)
    s = sub.add_parser("github"); s.add_argument("repo"); s.set_defaults(fn=cmd_github)
    sub.add_parser("publicar").set_defaults(fn=cmd_publicar)
    sub.add_parser("publicar-local").set_defaults(fn=cmd_publicar_local)
    s = sub.add_parser("manifest"); s.add_argument("--repo", required=True); s.add_argument("--tag", required=True); s.set_defaults(fn=cmd_manifest)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
