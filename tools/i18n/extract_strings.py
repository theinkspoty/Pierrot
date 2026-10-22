#!/usr/bin/env python3
# Extrai as strings tradutíveis (tr("...")) do código-fonte para strings.txt.
#
# Uso: python3 tools/i18n/extract_strings.py [--check]
#
#   sem --check: reescreve tools/i18n/strings.txt
#   com --check: só informa quantas strings faltam/faltaram (exit 1 se divergir)
#
# As strings são as MESMAS que o Qt entrega ao tradutor em runtime: o texto
# original em pt-BR. O tradutor (JaTranslator/EnTranslator) casa por esse
# texto, sem contexto.
import os
import re
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(BASE, "..", ".."))
STRINGS = os.path.join(BASE, "strings.txt")

# Arquivos que não contêm strings de UI tradutíveis.
# "packaging/arch/src/pierrot-0.7.1" é um snapshot tarball da release 0.7.1
# (código antigo, congelado) — não faz parte da UI viva.
SKIP_DIRS = {
    "build",
    "packaging/arch/src/build",
    "packaging/arch/src/pierrot-0.7.1",
    ".git",
    "tools",
}
# O próprio tradutor contém a tabela (sem chamada tr(), mas por segurança).
SKIP_FILES = {"JaTranslator.cpp", "EnTranslator.cpp"}

# tr("...") / QObject::tr("...") / someObj->tr("...") / ::tr("...")
# O lookbehind só impede identificador grudado (ex.: "mytr("); "::" é
# permitido para pegar QObject::tr(...) e afins.
TR_RE = re.compile(r"(?<![\w])tr\s*\(\s*")
# Chamadas com concatenação: tr("a" "b") ou tr("a" "b" "c").
STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def iter_source_files():
    for root, dirs, files in os.walk(REPO):
        rel_root = os.path.relpath(root, REPO)
        dirs[:] = [d for d in dirs
                   if os.path.normpath(os.path.join(rel_root, d)) not in SKIP_DIRS]
        for name in sorted(files):
            if not name.endswith((".cpp", ".h")):
                continue
            if name in SKIP_FILES:
                continue
            yield os.path.join(root, name)


def extract(text):
    """Retorna as strings tradutíveis encontradas em um arquivo .cpp/.h."""
    out = []
    for m in TR_RE.finditer(text):
        i = m.end()
        # Aceita concatenação de literais adjacentes: "a" "b" "c"
        lit = []
        while True:
            sm = STR_RE.match(text, i)
            if not sm:
                break
            lit.append(sm.group(1))
            i = sm.end()
            # pula whitespace entre literais
            j = i
            while j < len(text) and text[j] in " \t\n\r":
                j += 1
            if j < len(text) and text[j] == '"':
                i = j
                continue
            break
        if not lit:
            continue
        # Reconstrói o literal C++ como o Qt o entrega (concatenado).
        s = "".join(lit)
        if not s:
            continue
        out.append(s)
    return out


def unescape(s):
    """Resolve os escapes C++ mais comuns para o texto real.

    NÃO usado na geração: o arquivo strings.txt guarda o literal CRU (com
    "\\n" como dois caracteres), porque é linha-a-linha e porque é exatamente
    a forma da chave na tabela gerada. Fica aqui só para conferência.
    """
    res = []
    i = 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            res.append(c)
            i += 1
            continue
        i += 1
        if i >= len(s):
            res.append("\\")
            break
        e = s[i]
        i += 1
        if e == "n":
            res.append("\n")
        elif e == "t":
            res.append("\t")
        elif e == "r":
            res.append("\r")
        elif e == '"':
            res.append('"')
        elif e == "\\":
            res.append("\\")
        elif e == "'":
            res.append("'")
        elif e == "x" and i + 1 < len(s):
            hexs = s[i:i + 2]
            try:
                res.append(chr(int(hexs, 16)))
                i += 2
            except ValueError:
                res.append("\\x")
        else:
            res.append("\\" + e)
    return "".join(res)


def main():
    check = "--check" in sys.argv
    found = {}
    for path in iter_source_files():
        try:
            text = open(path, encoding="utf-8").read()
        except (UnicodeDecodeError, OSError):
            continue
        for raw in extract(text):
            # Guarda o literal CRU (escapes "\\n" preservados): é linha-a-linha
            # e é a forma exata da chave usada na tabela gerada.
            s = raw
            if not s.strip():
                continue
            found.setdefault(s, path)

    # Mantém a ordem estável (sorted) como no strings.txt atual.
    ordered = sorted(found.keys())
    old = []
    if os.path.exists(STRINGS):
        old = [l for l in open(STRINGS, encoding="utf-8").read().split("\n") if l]

    old_set = set(old)
    new_set = set(ordered)
    added = sorted(new_set - old_set)
    removed = sorted(old_set - new_set)

    print("Strings no código: %d" % len(ordered))
    print("Anotadas em strings.txt: %d" % len(old_set))
    print("Novas (no código, fora do strings.txt): %d" % len(added))
    for s in added:
        print("  + %r  [%s]" % (s, os.path.relpath(found[s], REPO)))
    print("Obsoletas (no strings.txt, fora do código): %d" % len(removed))
    for s in removed:
        print("  - %r" % s)

    if check:
        return 1 if (added or removed) else 0

    if not added and not removed:
        print("strings.txt já está atualizado.")
        return 0
    with open(STRINGS, "w", encoding="utf-8") as f:
        f.write("\n".join(ordered) + "\n")
    print("strings.txt reescrito (%d entradas)." % len(ordered))
    return 0


if __name__ == "__main__":
    sys.exit(main())
