#!/usr/bin/env python3
# Núcleo compartilhado dos geradores de tabela de tradução (ja / en).
#
# O texto original (chave) é sempre o literal C++ como o Qt o entrega em
# runtime — literais adjacentes concatenados viram UMA string. Por isso a
# extração (extract_strings.py) e a tabela precisam concordar.
import os
import re

BASE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(BASE, "..", ".."))

# ── Configuração por idioma ──────────────────────────────────────
LANGS = {
    "ja": {
        "label": "japonês",
        "label_en": "Japanese",
        "arrow": "pt-BR → 日本語",
        "part": os.path.join(BASE, "ja.part1.txt"),
        "target": os.path.join(REPO, "src", "ui", "core", "JaTranslator.cpp"),
        "gen": "tools/i18n/gen_ja.py",
    },
    "en": {
        "label": "inglês",
        "label_en": "English",
        "arrow": "pt-BR → English",
        "part": os.path.join(BASE, "en.part1.txt"),
        "target": os.path.join(REPO, "src", "ui", "core", "EnTranslator.cpp"),
        "gen": "tools/i18n/gen_en.py",
    },
}

STRINGS = os.path.join(BASE, "strings.txt")

# Separador dos arquivos de pares (origem<SEP>traducao).
# TAB: o literal C++ cru nunca contém um TAB real (quebraria a linha) e
# nunca é ambíguo — o antigo "|||" colidia com chaves que terminam em "|"
# (ex.: "▶|").
SEP = "\t"

PAIR_RE = re.compile(
    r'\{ (?:QStringLiteral|QString::fromUtf8)\("((?:[^"\\]|\\.)*)"\),\s*'
    r'(?:QStringLiteral|QString::fromUtf8)\("((?:[^"\\]|\\.)*)"\) \},'
)


def has_bare_quote(s):
    """True se a string tiver aspas " sem escape (quebraria o literal C++)."""
    i = 0
    while i < len(s):
        if s[i] == "\\":
            i += 2
            continue
        if s[i] == '"':
            return True
        i += 1
    return False


def read_part(path):
    """Lê o arquivo de pares 'origem<SEP>traducao'."""
    pairs = []
    seen = set()
    if not os.path.exists(path):
        return pairs
    for raw in open(path, encoding="utf-8").read().split("\n"):
        if not raw:
            continue
        if SEP not in raw:
            raise ValueError("LINHA SEM SEPARADOR: %r" % raw)
        src, dst = raw.split(SEP, 1)
        if src in seen:
            raise ValueError("DUPLICADO: %r" % src)
        seen.add(src)
        pairs.append((src, dst))
    return pairs


def read_cpp_table(path):
    """Extrai os pares (chave, valor) da tabela gerada dentro do .cpp."""
    if not os.path.exists(path):
        return []
    text = open(path, encoding="utf-8").read()
    return PAIR_RE.findall(text)


def write_part(path, pairs):
    """Grava o arquivo de pares SEMPRE terminando em newline.

    Sem o newline final, um append posterior cola a nova entrada na
    anterior (corrompendo a tabela) — daí a insistência.
    """
    with open(path, "w", encoding="utf-8") as f:
        for src, dst in pairs:
            if SEP in src or SEP in dst:
                raise ValueError("TAB dentro do par: %r" % src[:60])
            f.write(src + SEP + dst + "\n")


def append_part(path, pairs):
    """Acrescenta pares, garantindo que o arquivo termine em newline antes."""
    with open(path, "a", encoding="utf-8") as f:
        if f.tell() > 0:
            with open(path, "rb") as r:
                r.seek(-1, 2)
                if r.read(1) != b"\n":
                    f.write("\n")
        for src, dst in pairs:
            if SEP in src or SEP in dst:
                raise ValueError("TAB dentro do par: %r" % src[:60])
            f.write(src + SEP + dst + "\n")


def known_strings():
    if not os.path.exists(STRINGS):
        return []
    return [l for l in open(STRINGS, encoding="utf-8").read().split("\n") if l]


def render_block(pairs, cfg):
    """Monta o bloco C++ entre BEGIN-GENERATED / END-GENERATED."""
    block = []
    block.append("        // BEGIN-GENERATED")
    block.append("        // Tabela de tradução %s. Gerada por" % cfg["arrow"])
    block.append("        // %s — não editar manualmente." % cfg["gen"])
    block.append("        // Entradas: %d" % len(pairs))
    for src, dst in pairs:
        # Origem/tradução com escape de byte \xHH precisam de fromUtf8: em
        # QStringLiteral (u"") o escape vira code point, não byte UTF-8, e não
        # casaria com o texto que o Qt entrega ao tradutor.
        key = ('QString::fromUtf8("%s")' % src if "\\x" in src
               else 'QStringLiteral("%s")' % src)
        val = ('QString::fromUtf8("%s")' % dst if "\\x" in dst
               else 'QStringLiteral("%s")' % dst)
        block.append('        { %s, %s },' % (key, val))
    block.append("        // END-GENERATED")
    return "\n".join(block) + "\n"


def inject(target, block):
    text = open(target, encoding="utf-8").read()
    start = text.index("        // BEGIN-GENERATED")
    end = text.index("        // END-GENERATED")
    end = text.index("\n", end) + 1
    new = text[:start] + block + text[end:]
    open(target, "w", encoding="utf-8").write(new)


def generate(lang, verbose=True):
    """Gera a tabela do idioma. Retorna (ok, relatorio)."""
    cfg = LANGS[lang]
    pairs = read_part(cfg["part"])
    known = set(known_strings())
    known_list = known_strings()

    # Traduções sem string correspondente no código: neutro (a string pode
    # ter sido removida do UI mas a tradução continua válida).
    extra = [s for s, _ in pairs if s not in known]

    # Toda string do código PRECISA de tradução, senão fica em pt-BR.
    translated = set(s for s, _ in pairs)
    missing = [s for s in known_list if s not in translated]

    # Segurança de escaping.
    for src, dst in pairs:
        if has_bare_quote(src):
            return False, "ASPAS NA ORIGEM: %r" % src
        if has_bare_quote(dst):
            return False, "ASPAS NA TRADUÇÃO: %r" % dst

    pairs.sort(key=lambda p: p[0])
    block = render_block(pairs, cfg)
    inject(cfg["target"], block)

    if verbose:
        print("[%s] Total de entradas: %d" % (lang, len(pairs)))
        print("[%s] Traduzidas (com par no arquivo): %d/%d strings do código"
              % (lang, len(known) - len(missing), len(known)))
        if extra:
            print("[%s] Traduções sem string no código (inofensivas): %d"
                  % (lang, len(extra)))
        if missing:
            print("[%s] NÃO traduzidas (ficam em pt-BR): %d" % (lang, len(missing)))
            for s in missing[:40]:
                print("   skip:", repr(s))
            if len(missing) > 40:
                print("   ... e mais %d" % (len(missing) - 40))
    return True, missing
