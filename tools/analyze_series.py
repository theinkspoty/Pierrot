#!/usr/bin/env python3
"""Analisa a série por quadro de um relatório do PreviewProfiler.

Focado nas falhas que o usuário sente: quadro pulado, quadro perdido,
corte atravessado e travamento. Distingue "skip de 1" (replay sequencial
normal) de salto real, porque o índice de frame avança 1 por tick mesmo
quando nada deu errado.
"""
from __future__ import annotations

import json
import statistics as st
import sys
from pathlib import Path


def pct(values, p):
    v = sorted(values)
    return v[min(len(v) - 1, int(p * len(v)))]


def analyze(path: Path) -> dict:
    d = json.loads(path.read_text())
    if 'series' not in d:
        print(f'=== {path.name} === (sem série: rode com PIERROT_PERF_VERBOSE=1)')
        return
    s = d["series"]
    n = len(s)
    if not n:
        raise SystemExit(f"{path}: relatório sem série de quadros")

    print(f"=== {path.name} ===")
    print(f"ticks={n}  span={d.get('playbackSpanSec', 0):.2f}s  "
          f"taxa={d.get('tickRateHz', 0):.1f}/s  fonte={d.get('sourceUsed', '?')}")
    fr = [r["fr"] for r in s if r.get("fr", -1) >= 0]
    if fr:
        print(f"playhead {s[0]['ph']:.3f} -> {s[-1]['ph']:.3f}s   "
              f"quadro {min(fr)} -> {max(fr)}")
    else:
        print(f"playhead {s[0]['ph']:.3f} -> {s[-1]['ph']:.3f}s   "
              f"ticks {s[0]['seq']} -> {s[-1]['seq']}")
    print()

    print(f"{'métrica':<12}{'média':>9}{'p95':>9}{'p99':>9}{'máx':>10}   pior em")
    for k in ("tick", "seek", "wrk", "paint", "mix", "res", "stall"):
        v = [r[k] for r in s]
        i = max(range(n), key=lambda j: s[j][k])
        print(f"{k:<12}{st.mean(v):>9.3f}{pct(v, .95):>9.3f}{pct(v, .99):>9.3f}"
              f"{max(v):>10.3f}   t={s[i]['ph']:.1f}s")
    print()

    # Perda de quadro: usa `fr` (índice do QUADRO), nunca `seq` (contador de
    # ticks, que sempre avança de 1 em 1 e por isso dava falso negativo).
    if fr:
        gaps = [(a, b) for a, b in zip(fr, fr[1:]) if b - a > 1]
        back = [(a, b) for a, b in zip(fr, fr[1:]) if b < a]
        vistos = set(fr)
        faltando = [i for i in range(min(fr), max(fr) + 1) if i not in vistos]
        print(f"quadros exibidos...........: {len(vistos)} distintos na faixa "
              f"{min(fr)}-{max(fr)} | saltos>1: {len(gaps)} | nunca exibidos: {len(faltando)}")
        for a, b in gaps[:12]:
            print(f"    salto {a} -> {b} (pula {b-a-1} quadro(s)) em t={s[0]['ph']:.1f}s")
        if back:
            print(f"    retrocesso de índice: {len(back)}")
    else:
        print("perda de quadro: sem o campo 'fr' (rodar com o build novo)")
    gaps = []
    for ph, g in gaps[:12]:
        print(f"    t={ph:8.2f}s  pulou {g} frames")
    print()

    print(f"ticks com drop>0......: {sum(1 for r in s if r['drop'] > 0)}"
          f"   (soma={sum(r['drop'] for r in s)})")
    print(f"ticks com corte.........: {sum(1 for r in s if r['cut'])}")
    print(f"ticks sem paint.........: {sum(1 for r in s if r['paint'] == 0)}")
    # prefetchHit só pode ser 1 em transição: updatePrefetch() age na janela
    # final do clipe. Em clipe único contínuo, 0 é o comportamento correto.
    print(f"prefetch (transição)......: {sum(1 for r in s if r['pfhit'])} acertos"
          f"   (0 é correto em clipe único)")
    print(f"ticks com fila (q>0)....: {sum(1 for r in s if r['q'] > 0)}")
    print(f"stall do event loop.......: max={d.get('stallMaxMs', 0):.1f}ms  "
          f"total={d.get('stallTotalMs', 0):.0f}ms  "
          f"eventos>1ms={d.get('stallEvents', 0)}")
    print(f"ticks com layers>0......: {sum(1 for r in s if r['layers'] > 0)}")
    print()

    # Onde o playhead não avançou apesar de o tick ter rodado:engineemperrida.
    stalls = []
    for i in range(1, n):
        gap = s[i]["ph"] - s[i - 1]["ph"]
        if gap <= 0 and s[i]["wrk"] > 20.0:
            stalls.append((s[i]["ph"], s[i]["wrk"]))
    print(f"ticks com decode >20ms e playhead parado: {len(stalls)}")
    for ph, w in stalls[:12]:
        print(f"    t={ph:8.2f}s  wrk={w:9.2f}ms")
    print()

    print("piores latências de decode:")
    for i in sorted(range(n), key=lambda j: -s[j]["wrk"])[:12]:
        print(f"    t={s[i]['ph']:8.2f}s  wrk={s[i]['wrk']:9.2f}ms  "
              f"seek={s[i]['seek']:7.2f}ms  tick={s[i]['tick']:6.2f}ms")
    return d


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        analyze(Path(arg))
        print()