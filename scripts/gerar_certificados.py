"""Gera include/DmkRootCerts.h a partir do pacote certifi (Mozilla).
Uso: python scripts/gerar_certificados.py [caminho/cacert.pem]"""
import re, sys
from pathlib import Path
root = Path(__file__).resolve().parent.parent
if len(sys.argv) > 1:
    pem = Path(sys.argv[1]).read_text(encoding="utf-8")
else:
    import certifi
    pem = Path(certifi.where()).read_text(encoding="utf-8")
blocks = re.findall(r'# Label: "([^"]+)"\n(?:#[^\n]*\n)*(-----BEGIN CERTIFICATE-----\n.*?-----END CERTIFICATE-----)', pem, re.S)
d = dict(blocks)
full = ["USERTrust ECC Certification Authority", "USERTrust RSA Certification Authority",
        "Sectigo Public Server Authentication Root E46", "Sectigo Public Server Authentication Root R46",
        "DigiCert Global Root G2", "DigiCert Global Root G3", "ISRG Root X1", "ISRG Root X2",
        "GTS Root R1", "GTS Root R4"]
small = full[:5]
def emit(lst):
    s = "static const char DMK_ROOT_CA_BUNDLE[] PROGMEM =\n"
    for label in lst:
        s += f"    // {label}\n"
        for line in d[label].split("\n"):
            s += f'    "{line}\\n"\n'
    return s + "    ;\n"
out = ("#pragma once\n"
       "// Certificados raiz (Mozilla/certifi) para baixar firmware do GitHub e de\n"
       "// outros servidores HTTPS. Gerado por scripts/gerar_certificados.py.\n"
       "#include <Arduino.h>\n\n#if defined(DMK_BOARD_ESP8266)\n// ESP8266: conjunto menor (pouca RAM)\n"
       + emit(small) + "#else\n" + emit(full) + "#endif\n")
(root / "include" / "DmkRootCerts.h").write_text(out, encoding="utf-8")
print("DmkRootCerts.h gerado:", len(full), "raizes (ESP8266:", len(small), ")")
