#!/usr/bin/env python3
"""Mede a margem de playback do preview sob carga de CPU.

Responde a pergunta "dá para testar isso mais pesado?": estrangula o app num
número limitado de núcleos e diz onde ele começa a perder quadros.

  python3 tools/stress_preview.py projeto.Blanc --fps 60
  python3 tools/stress_preview.py projeto.Blanc --cores 4,2,1
  python3 tools/stress_preview.py projeto.Blanc --cores 1 --hogs 8

O gargalo medido nesta máquina é CPU, não o preview: o decode de 1080p60 custa
0,33ms no p95 contra um orçamento de 16,7ms por quadro. O beco sem saída
aparece em 1 núcleo (p95 de 8,2ms). Por isso o utilário estrangula por CPU em
vez de mexer em cache — aumentar cache não aumenta margem (ver --help do
PreviewProfiler: o LRU do decoder acerta ~0,1% em playback linear).
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PIERROT = REPO / "build" / "pierrot"


def parse_cores(spec: str) -> list[str]:
    """'4' -> ['0-3'], '0,2,4' -> ['0,2,4']."""
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        out.append(part if "-" in part else ",".join(str(i) for i in range(int(part))))
    return out


def cpu_budget(core_list: str) -> int:
    """Quantos CPUs o taskset deixa disponíveis."""
    n = 0
    for grp in core_list.split(","):
        if "-" in grp:
            a, b = grp.split("-")
            n += int(b) - int(a) + 1
        else:
            n += 1
    return n


class Hogs:
    """Processos de fundo que競omem CPU, para simular máquina ocupada."""

    def __init__(self, count: int):
        self.count = count
        self.procs: list[subprocess.Popen] = []

    def __enter__(self):
        for _ in range(self.count):
            self.procs.append(
                subprocess.Popen(
                    ["python3", "-c", "\nwhile True: pass"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            )
        time.sleep(0.4)
        return self

    def __exit__(self, *exc):
        for p in self.procs:
            p.send_signal(signal.SIGKILL)
        for p in self.procs:
            p.wait()


def run_once(proj: Path, cores: str, seconds: float, fps: float, hogs: int,
             proxy: bool, json_path: Path) -> dict:
    cmd = []
    if cores and shutil.which("taskset"):
        cmd += ["taskset", "-c", cores]
    cmd += [
        str(PIERROT),
        "--autoplay", str(proj),
        f"--seconds={seconds:g}",
        "--from=0",
        "--warmup=3",
        "--proxy" if proxy else "--no-proxy",
        f"--json={json_path}",
    ]
    env = dict(os.environ, PIERROT_PERF_VERBOSE="1", PIERROT_DESENGASGA_MS="0")

    if hogs:
        with Hogs(hogs):
            subprocess.run(cmd, env=env, timeout=seconds * 4 + 180,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        subprocess.run(cmd, env=env, timeout=seconds * 4 + 180,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if not json_path.exists():
        raise SystemExit(f"sem relatório em {json_path} — o app não chegou a fechar a sessão")
    return json.loads(json_path.read_text())


def report(tag: str, d: dict, fps: float) -> dict:
    budget = 1000.0 / fps
    wk = d["asyncStages"]["workerNs"]
    hits, miss = d.get("cacheHits", 0), d.get("cacheMisses", 0)
    hit_pct = 100.0 * hits / max(hits + miss, 1)
    row = dict(
        tag=tag,
        drops=d["droppedTotal"],
        skips=d["skippedTotal"],
        dec_p95=wk["p95Ms"],
        dec_max=wk["maxMs"],
        tick_p95=d["stages"]["tickNs"]["p95Ms"],
        stall_max=d.get("stallMaxMs", 0.0),
        margin=budget / max(wk["p95Ms"], 1e-9),
        cache_hit=hit_pct,
    )
    print(f"{tag:<18}{row['drops']:>6}{row['skips']:>7}"
          f"{row['dec_p95']:>10.3f}{row['dec_max']:>10.1f}"
          f"{row['tick_p95']:>10.3f}{row['stall_max']:>10.1f}"
          f"{row['margin']:>10.1f}x{row['cache_hit']:>9.1f}%")
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("project", type=Path, help="projeto .Blanc")
    ap.add_argument("--fps", type=float, default=60.0, help="fps do projeto (orçamento por quadro)")
    ap.add_argument("--cores", default="full",
                    help="núcleos a testar: '4', '0,2,4', ou 'full' (todos). Ex.: --cores 4,2,1")
    ap.add_argument("--seconds", type=float, default=90.0, help="segundos por cenário")
    ap.add_argument("--hogs", type=int, default=0, help="processos de fundo que disputam CPU")
    ap.add_argument("--proxy", action="store_true", help="usar proxy em vez do original")
    args = ap.parse_args()

    if not PIERROT.exists():
        raise SystemExit(f"binário não encontrado: {PIERROT} (compile antes)")
    if not args.project.exists():
        raise SystemExit(f"projeto não encontrado: {args.project}")

    specs = [args.cores] if args.cores == "full" else parse_cores(args.cores)
    budget_ms = 1000.0 / args.fps
    print(f"projeto={args.project.name} fps={args.fps:g} "
          f"orçamento/quadro={budget_ms:.2f}ms proxy={args.proxy}")
    print(f"\n{'cenário':<18}{'drops':>6}{'skips':>7}{'dec p95':>10}{'dec máx':>10}"
          f"{'tick p95':>10}{'stall':>10}{'margem':>10}{'cache':>10}")
    print("-" * 81)

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for spec in specs:
            cpus = cpu_budget(spec) if spec != "full" else os.cpu_count()
            tag = f"{cpus}cpu" + ("+hogs" if args.hogs else "")
            out = Path(tmp) / f"s_{spec.replace(',', '_')}.json"
            t0 = time.time()
            try:
                d = run_once(args.project, "" if spec == "full" else spec,
                             args.seconds, args.fps, args.hogs, args.proxy, out)
            except subprocess.TimeoutExpired:
                print(f"{tag:<18}TIMEOUT após {time.time()-t0:.0f}s")
                continue
            rows.append(report(tag, d, args.fps))

    if not rows:
        raise SystemExit("nenhum cenário rodou")
    bad = [r for r in rows if r["drops"] or r["skips"]]
    worst = min(rows, key=lambda r: r["margin"])
    print("-" * 81)
    if bad:
        print(f"PERDE QUADRO em: {', '.join(r['tag'] for r in bad)}")
    else:
        print("zero drop e zero skip em todos os cenários")
    print(f"menor margem: {worst['tag']} = {worst['margin']:.1f}x o orçamento por quadro")
    return 0


if __name__ == "__main__":
    sys.exit(main())