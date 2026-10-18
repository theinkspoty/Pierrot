#!/usr/bin/env python3
# Gera o bloco C++ da tabela de tradução japonesa e injeta nos marcadores
# BEGIN-GENERATED/END-GENERATED em src/ui/JaTranslator.cpp.
#
# Uso: python3 tools/i18n/gen_ja.py
import os
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(BASE, "..", ".."))
STRINGS = os.path.join(BASE, "strings.txt")
PART = os.path.join(BASE, "ja.part1.txt")
TARGET = os.path.join(REPO, "src", "ui", "JaTranslator.cpp")

# Strings novas que ainda não estavam em strings.txt (adicionadas depois da extração).
EXTRAS = {
    "O japonês já está disponível. O inglês chega na versão 0.7.5. "
    "Requer reinício do aplicativo para tomar efeito.":
        "日本語が利用可能になりました。英語はバージョン 0.7.5 で対応予定です。"
        "変更を反映するにはアプリの再起動が必要です。",
}

def has_bare_quote(s):
    # Verifica se há aspas não escapadas
    i = 0
    while i < len(s):
        if s[i] == '\\':
            i += 2
            continue
        if s[i] == '"':
            return True
        i += 1
    return False

def main():
    known = set(s for s in open(STRINGS, encoding="utf-8").read().split("\n") if s)
    pairs = []
    seen = set()
    for raw in open(PART, encoding="utf-8").read().split("\n"):
        if not raw:
            continue
        if "|||" not in raw:
            print("LINHA SEM SEPARADOR:", repr(raw)); return 2
        src, ja = raw.split("|||", 1)
        if src in seen:
            print("DUPLICADO:", repr(src)); return 2
        seen.add(src)
        pairs.append((src, ja))

    for src, ja in EXTRAS.items():
        if src not in seen:
            pairs.append((src, ja))
            seen.add(src)

    # Validação: toda origem (exceto extras) deve existir em strings.txt.
    extras_set = set(EXTRAS)
    unknown = [s for s, _ in pairs if s not in known and s not in extras_set]
    if unknown:
        print("ORIGENS DESCONHECIDAS (%d):" % len(unknown))
        for s in unknown:
            print("  ", repr(s))
        return 3

    # Segurança de escaping.
    for src, ja in pairs:
        if has_bare_quote(src):
            print("ASPAS NA ORIGEM:", repr(src)); return 4
        if has_bare_quote(ja):
            print("ASPAS NA TRADUÇÃO:", repr(ja)); return 4

    pairs.sort(key=lambda p: p[0])

    block = []
    block.append("        // BEGIN-GENERATED")
    block.append("        // Tabela de tradução pt-BR → 日本語. Gerada por")
    block.append("        // tools/i18n/gen_ja.py — não editar manualmente.")
    block.append("        // Entradas: %d" % len(pairs))
    for src, ja in pairs:
        # Origem com escape de byte \xHH precisa de fromUtf8: em QStringLiteral
        # (u"") o escape vira code point, não byte UTF-8, e não casaria com o
        # texto que o Qt entrega ao tradutor.
        if "\\x" in src:
            key = 'QString::fromUtf8("%s")' % src
        else:
            key = 'QStringLiteral("%s")' % src
        block.append('        { %s, QStringLiteral("%s") },' % (key, ja))
    block.append("        // END-GENERATED")

    text = open(TARGET, encoding="utf-8").read()
    start = text.index("        // BEGIN-GENERATED")
    end = text.index("        // END-GENERATED")
    end = text.index("\n", end) + 1
    new = text[:start] + "\n".join(block) + "\n" + text[end:]
    open(TARGET, "w", encoding="utf-8").write(new)

    missing = [s for s in known if s not in seen]
    print("Total de entradas: %d" % len(pairs))
    print("Origem conhecida não traduzida (fallback pt-BR): %d" % len(missing))
    for s in missing:
        print("  skip:", repr(s))
    return 0

if __name__ == "__main__":
    sys.exit(main())
