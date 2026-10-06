# Executado pelo PlatformIO depois de compilar: copia o firmware para dist/
# com nome, tamanho e SHA-256, e atualiza dist/manifest.json.
Import("env")
import hashlib, json, os, re, shutil, tempfile, datetime
from pathlib import Path

root = Path(env.subst("$PROJECT_DIR"))

def save_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent))
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    os.replace(tmp, path)

def after_build(source, target, env):
    name = env.subst("$PIOENV")
    role = name.split("_")[0]
    versions = dict(re.findall(r'#define (DMK_VERSION_\w+) "([^"]+)"', (root / "include/DmkVersion.h").read_text()))
    version = versions["DMK_VERSION_" + role]
    src = Path(env.subst("$BUILD_DIR")) / "firmware.bin"
    dist = root / "dist"
    dist.mkdir(exist_ok=True)
    for old in dist.glob(f"DMK-{name}-*.bin"):
        old.unlink()
    file = f"DMK-{name}-{version}.bin"
    shutil.copy2(src, dist / file)
    data = (dist / file).read_bytes()
    mf = dist / "manifest.json"
    doc = json.loads(mf.read_text(encoding="utf-8")) if mf.exists() else {"system": "DMK SMART", "targets": {}}
    doc.setdefault("targets", {})[name] = {
        "role": role, "version": version, "file": file, "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "built": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
    }
    save_json(mf, doc)
    print(f"[DMK] {name} {version}: dist/{file} ({len(data)//1024} KB)")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)
