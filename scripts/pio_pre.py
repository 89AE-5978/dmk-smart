# Executado pelo PlatformIO antes de compilar cada placa.
#  1) embute as paginas de data/<placa> no firmware (gzip)
#  2) grava o repositorio GitHub das atualizacoes no firmware
Import("env")
import os, sys
from pathlib import Path

root = Path(env.subst("$PROJECT_DIR"))
sys.path.insert(0, str(root / "scripts"))
import embed_web  # noqa: E402

role = "serve" if env.subst("$PIOENV").startswith("SERVE_") else "controle"
embed_web.build(root, role)

repo = os.environ.get("DMK_GITHUB_REPO", "").strip()
cfg = root / "dmk-github.txt"
if not repo and cfg.exists():
    repo = cfg.read_text(encoding="utf-8").strip()
if repo and "/" in repo:
    env.Append(CPPDEFINES=[("DMK_GITHUB_REPO", env.StringifyMacro(repo))])
    print("[DMK] Atualizacoes pelo GitHub:", repo)
else:
    print("[DMK] Repositorio GitHub ainda nao definido (DMK.bat > Configurar GitHub).")

# Velocidade de gravacao alternativa (DMK.bat tenta 115200 se 460800 falhar).
if os.environ.get("DMK_UPLOAD_SPEED"):
    env.Replace(UPLOAD_SPEED=os.environ["DMK_UPLOAD_SPEED"])

# "uploadfs" continua opcional (ex.: app-config.js personalizado).
env.Replace(PROJECT_DATA_DIR=str(root / "data" / role))
