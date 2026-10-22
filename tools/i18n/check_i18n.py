#!/usr/bin/env python3
# Verifica a integridade da i18n do Pierrot.
#
#   1. strings.txt está em dia com o código (roda extract_strings.py --check)
#   2. As tabelas geradas (.cpp) batem com os arquivos de pares
#   3. Cobertura: o inglês cobre TODAS as strings do código
#
# Uso: python3 tools/i18n/check_i18n.py
# Sai com 1 se algo estiver errado (útil em CI).
import os
import subprocess
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE)
import i18n_common as ic

REPO = ic.REPO


def main():
    errors = 0

    # 1) strings.txt em dia
    r = subprocess.run([sys.executable, os.path.join(BASE, "extract_strings.py"), "--check"],
                       capture_output=True, text=True, cwd=REPO)
    if r.returncode != 0:
        print("[FALHA] strings.txt desatualizado:")
        print(r.stdout)
        errors += 1
    else:
        print("[ok] strings.txt em dia com o codigo")

    # 2) tabelas geradas == arquivos de pares
    for lang in ("ja", "en"):
        part = ic.LANGS[lang]["part"]
        raw = open(part, encoding="utf-8").read()
        if not raw.endswith("\n"):
            print("[FALHA] %s: o arquivo de pares precisa terminar em newline"
                  % os.path.basename(part))
            errors += 1
        for i, line in enumerate(raw.split("\n"), 1):
            if not line:
                continue
            if line.count(ic.SEP) != 1:
                print("[FALHA] %s:%d: esperado exatamente 1 TAB (%d)"
                      % (os.path.basename(part), i, line.count(ic.SEP)))
                errors += 1
                break
        pairs = ic.read_part(part)
        block = ic.render_block(sorted(pairs), ic.LANGS[lang])
        text = open(ic.LANGS[lang]["target"], encoding="utf-8").read()
        start = text.index("        // BEGIN-GENERATED")
        end = text.index("        // END-GENERATED")
        end = text.index("\n", end) + 1
        if text[start:end] != block:
            print("[FALHA] %s: %s esta desatualizado — rode %s"
                  % (lang, os.path.basename(ic.LANGS[lang]["target"]),
                     ic.LANGS[lang]["gen"]))
            errors += 1
        else:
            print("[ok] %s: tabela gerada em dia" % lang)

    # 3) cobertura
    known = set(ic.known_strings())
    for lang, minimo in (("ja", 0), ("en", 1.0)):
        done = set(s for s, _ in ic.read_part(ic.LANGS[lang]["part"]))
        pct = 100.0 * len(done & known) / max(1, len(known))
        status = "ok" if pct >= minimo * 100 else "FALHA"
        if status == "FALHA":
            errors += 1
        print("[%s] %s: %d/%d strings traduzidas (%.1f%%)"
              % (status, lang, len(done & known), len(known), pct))

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
