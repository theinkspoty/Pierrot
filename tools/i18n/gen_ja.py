#!/usr/bin/env python3
# Gera o bloco C++ da tabela de tradução japonesa e injeta nos marcadores
# BEGIN-GENERATED/END-GENERATED em src/ui/core/JaTranslator.cpp.
#
# Uso: python3 tools/i18n/gen_ja.py
#
# Os pares vem de tools/i18n/ja.part1.txt ("origem|||traducao"), no formato
# do literal C++ cru — é assim que o Qt entrega a string em runtime.
# Para atualizar a lista de strings do código: python3 tools/i18n/extract_strings.py
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import i18n_common as ic


def main():
    ok, info = ic.generate("ja")
    if not ok:
        print("ERRO:", info)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
