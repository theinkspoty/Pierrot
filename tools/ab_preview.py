#!/usr/bin/env python3
"""A/B de reprodução do preview: proxy vs original.

Roda o Pierrot em modo `--autoplay` N vezes em cada configuração e compara a
distribuição das etapas do pipeline. Cada run grava um JSON do
`PreviewProfiler`; este script só agrega e compara.

    ./ab_preview.py projeto.Blanc --runs 3 --seconds 20
    ./ab_preview.py projeto.Blanc --runs 5 --seconds 30 --from 120
    ./ab_preview.py projeto.Blanc --prepare          # só gera os proxies

Por que o warm-up importa: os primeiros segundos de reprodução abrem decoders
e aquecem caches. Medir junto com isso compara "proxy ainda sendo gerado" com
"proxy quente", que não é a pergunta. O proxy é gerado uma vez (`--prepare`)
e todas as runs medidas usam o mesmo cache — assim a diferença é do decode, não
daavailability do proxy.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Etapas comparadas, na ordem em que aparecem no tick.
STAGES = [
    ("tickNs", "tick total"),
    ("clockNs", "  relógio"),
    ("seekNs", "  seek/decode"),
    ("prefetchNs", "  prefetch"),
    ("mixNs", "  áudio"),
    ("workerNs", "latência decode"),
    ("prefetchLatNs", "latência prefetch"),
    ("paintNs", "paint"),
    ("compositeNs", "  composição"),
    ("resolveNs", "  resolver proxy"),
]

# Onde cada etapa aparece no relatório. As do tick vivem em "stages"; as que
# são medidas fora do tick (paint, worker, prefetch) têm vida própria e entram
# em "paintStages"/"asyncStages" — por isso o mapa em vez de um prefixo fixo.
STAGE_GROUPS = {
    "tickNs": "stages", "clockNs": "stages", "seekNs": "stages",
    "prefetchNs": "stages", "mixNs": "stages",
    "workerNs": "asyncStages", "prefetchLatNs": "asyncStages",
    "paintNs": "paintStages", "compositeNs": "paintStages",
    "resolveNs": "paintStages",
}


def run_once(binary: Path, project: Path, *, seconds: float, from_sec: float,
             warmup: float, proxy: bool, wait_proxy: bool,
             json_path: Path, timeout: float, min_width: int) -> dict:
    """Dispara uma run e devolve o JSON do relatório."""
    cmd = [
        str(binary), "--autoplay", str(project),
        f"--seconds={seconds:g}", f"--from={from_sec:g}", f"--warmup={warmup:g}",
        f"--json={json_path}",
    ]
    cmd.append("--proxy" if proxy else "--no-proxy")
    if wait_proxy:
        cmd.append("--wait-proxy")
    if json_path.exists():
        json_path.unlink()

    env = dict(os.environ)
    # Overlay não é desejado em batch: é só uma QString por paint.
    env.pop("PIERROT_PERF_DEBUG", None)
    env["QT_LOGGING_RULES"] = "qt.qpa.*=false"
    env["PIERROT_PROXY_MIN_WIDTH"] = str(min_width)

    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, env=env, timeout=timeout,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        raise SystemExit(f"timeout ({timeout:.0f}s) na run: {' '.join(cmd)}")
    wall = time.monotonic() - t0

    if not json_path.exists():
        out = proc.stdout.decode(errors="replace")[-3000:]
        raise SystemExit(f"a run não gerou relatório (exit {proc.returncode}).\n{out}")
    data = json.loads(json_path.read_text())
    data["_wallSec"] = wall
    return data


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    return s[min(len(s) - 1, int(p * len(s)))]


def aggregate(runs: list[dict]) -> dict:
    """Funde N relatórios num agregado (p50/p95 do p50, etc. por run)."""
    if not runs:
        return {}
    agg: dict = {"runs": len(runs)}
    for key in ("frames", "cuts", "droppedTotal", "skippedTotal", "proxyFrames",
                "prefetchHits", "compositeCacheHits", "compositeCacheMisses",
                "lowerLayerReqs", "tickRateHz"):
        agg[key] = statistics.mean(r.get(key, 0) for r in runs)
    agg["sourceUsed"] = runs[0].get("sourceUsed", "?")
    agg["playbackSpanSec"] = statistics.mean(r.get("playbackSpanSec", 0) for r in runs)
    agg["wallSec"] = statistics.mean(r.get("_wallSec", 0) for r in runs)
    for field, _ in STAGES:
        group = STAGE_GROUPS[field]
        per_run = []
        for r in runs:
            node = r.get(group, {}).get(field, {})
            if node.get("n"):
                per_run.append((node.get("meanMs", 0), node.get("p95Ms", 0),
                                node.get("maxMs", 0)))
        if per_run:
            agg[field] = {
                "mean": statistics.mean(p[0] for p in per_run),
                "p95": statistics.mean(p[1] for p in per_run),
                "max": max(p[2] for p in per_run),
            }
    return agg


def print_table(title: str, runs: list[dict]):
    agg = aggregate(runs)
    if not agg:
        print(f"{title}: sem dados")
        return None
    span = agg.get("playbackSpanSec", 0) or 1
    print(f"\n=== {title} — {agg['runs']} run(s), {span:.1f}s medidos por run ===")
    print(f"fonte usada......: {agg['sourceUsed']}")
    print(f"quadros..........: {agg['frames']:.0f}/run")
    print(f"cortes cruzados..: {agg['cuts']:.1f}/run")
    print(f"dropped..........: {agg['droppedTotal']:.1f}/run")
    print(f"tick/s (sonda)..: {agg['tickRateHz']:.1f}   (2x o fps do projeto)")
    print(f"prefetch HIT.....: {agg['prefetchHits']:.1f}/run")
    print(f"camadas inferiores: {agg['lowerLayerReqs']:.1f} re-decode/run")
    print(f"cache composição.: {agg['compositeCacheHits']:.0f} hit / "
          f"{agg['compositeCacheMisses']:.0f} miss por run")
    print()
    print(f"  {'etapa':<20} {'média':>9} {'p95':>9} {'pior':>9}")
    print("  " + "-" * 50)
    for field, label in STAGES:
        s = agg.get(field)
        if not s:
            continue
        print(f"  {label:<20} {s['mean']:>8.2f}ms {s['p95']:>8.2f}ms {s['max']:>8.2f}ms")
    return agg


def compare(a: dict, b: dict, name_a: str, name_b: str):
    """Compara os dois agregados e aponta onde a diferença está."""
    print(f"\n=== {name_b} vs {name_a} (negativo = {name_b} é mais rápido) ===")
    rows = []
    for field, label in STAGES:
        sa, sb = a.get(field), b.get(field)
        if not sa or not sb or sa.get("mean", 0) <= 0:
            continue
        delta = (sb["mean"] - sa["mean"]) / sa["mean"] * 100.0
        rows.append((abs(delta), label, sa["mean"], sb["mean"], delta))
    rows.sort(reverse=True)
    if not rows:
        print("  (sem estágios comparáveis — alguma run não produziu dados?)")
        return
    for _, label, va, vb, delta in rows[:6]:
        bar = "#" * min(40, int(abs(delta) / 3))
        sign = "-" if delta < 0 else "+"
        print(f"  {label:<20} {va:7.2f} -> {vb:7.2f}ms  {sign}{abs(delta):5.1f}% {bar}")

    # Veredito automático sobre o gargalo dominante.
    def dominant(agg):
        best, bestv = None, 0.0
        for field, label in STAGES:
            s = agg.get(field)
            if s and s["mean"] > bestv:
                best, bestv = label.strip(), s["mean"]
        return best, bestv
    da, va = dominant(a)
    db, vb = dominant(b)
    print(f"\n  gargalo dominante {name_a}: {da} ({va:.2f}ms)")
    print(f"  gargalo dominante {name_b}: {db} ({vb:.2f}ms)")
    print(f"  dropped  {name_a}={a.get('droppedTotal', 0):.0f}  "
          f"{name_b}={b.get('droppedTotal', 0):.0f}")
    if abs(a.get("droppedTotal", 0) - b.get("droppedTotal", 0)) > 1:
        better = name_a if a.get("droppedTotal", 0) < b.get("droppedTotal", 0) else name_b
        print(f"  -> menos frames perdidos: {better}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("project", type=Path, help="arquivo .Blanc")
    ap.add_argument("--binary", type=Path, default=None,
                    help="executável do Pierrot (padrão: ../build/pierrot)")
    ap.add_argument("--runs", type=int, default=3, help="runs por configuração (padrão 3)")
    ap.add_argument("--seconds", type=float, default=20.0, help="segundos medidos por run")
    ap.add_argument("--from", dest="from_sec", type=float, default=0.0,
                    help="posição inicial na timeline (s)")
    ap.add_argument("--warmup", type=float, default=4.0,
                    help="segundos reproduzidos fora da coleta (padrão 4)")
    ap.add_argument("--prepare", action="store_true",
                    help="gera os proxies e sai (use antes do A/B)")
    ap.add_argument("--proxy-min-width", type=int, default=1920,
                    help="PIERROT_PROXY_MIN_WIDTH para as runs (padrão 1920). "
                         "O app usa 2560, o que nunca gera proxy numa biblioteca "
                         "só 1080p; 1920 é a largura em que o próprio proxy é "
                         "gerado, então é onde ele ainda é uma reencode útil.")
    ap.add_argument("--keep-json", type=Path, default=None,
                    help="onde gravar os JSON brutos")
    ap.add_argument("--timeout", type=float, default=1800.0,
                    help="teto por run (s) — o --wait-proxy pode demorar")
    args = ap.parse_args()

    here = Path(__file__).resolve().parent
    binary = args.binary or (here.parent / "build" / "pierrot")
    if not binary.exists():
        print(f"ERRO: executável não encontrado: {binary}\n"
              f"      compile com: cmake --build build --target pierrot -j", file=sys.stderr)
        return 1
    if not args.project.exists():
        print(f"ERRO: projeto não encontrado: {args.project}", file=sys.stderr)
        return 1

    if args.keep_json:
        args.keep_json.mkdir(parents=True, exist_ok=True)
        tmpdir = Path(tempfile.mkdtemp(prefix="pierrot-ab-"))
    else:
        tmpdir = Path(tempfile.mkdtemp(prefix="pierrot-ab-"))

    common = dict(binary=binary, project=args.project, seconds=args.seconds,
                  from_sec=args.from_sec, warmup=args.warmup,
                  timeout=args.timeout, min_width=args.proxy_min_width)

    if args.prepare:
        print("Gerando proxies (uma vez) — isso pode demorar minutos…")
        out = run_once(**common, proxy=True, wait_proxy=True,
                       json_path=tmpdir / "prepare.json")
        print(f"pronto: {out['frames']} quadros, fonte={out['sourceUsed']}")
        return 0

    # As runs alternam proxy/original para que uma deriva térmica ou de page
    # cache não vire "vantagem" da ponta medida primeiro.
    schedule = [("proxy" if k % 2 == 0 else "original") for k in range(2 * args.runs)]
    results: dict[str, list[dict]] = {"proxy": [], "original": []}
    counts = {"proxy": 0, "original": 0}
    for cfg in schedule:
        counts[cfg] += 1
        i = counts[cfg] - 1
        jpath = args.keep_json / f"{cfg}-{i}.json" if args.keep_json \
            else tmpdir / f"{cfg}-{i}.json"
        wait = (i == 0)  # só a primeira run de cada lado espera o proxy
        print(f"[{cfg} {i + 1}/{args.runs}] "
              f"{args.seconds:g}s a partir de {args.from_sec:g}s…", flush=True)
        r = run_once(**common, proxy=(cfg == "proxy"), wait_proxy=wait,
                     json_path=jpath)
        results[cfg].append(r)
        print(f"    {r['frames']} quadros, fonte={r['sourceUsed']}, "
              f"dropped={r.get('droppedTotal', 0)}, "
              f"cortes={r.get('cuts', 0)}, {r['_wallSec']:.0f}s de wall", flush=True)

    a = print_table("SEM PROXY (original)", results["original"])
    b = print_table("COM PROXY", results["proxy"])
    if a and b:
        compare(a, b, "original", "proxy")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())