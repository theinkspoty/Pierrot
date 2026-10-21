# Relatório — Sessão de estresse: teste de potencia.Blanc (2026-10-21)

**Build:** main local (pós-`903e652`, sem commits novos) · **Máquina:** mesma da
sesão de aceitação da Fase 8 · **Comando:**

```
PIERROT_PERF_JSON=… PIERROT_PLAY_DEBUG=1 PIERROT_COMPOSE_DEBUG=1 \
  pierrot --autoplay "teste de potencia.Blanc" --seconds=158 --warmup=3
```

**Projeto:** 149,4 s · 6 faixas de vídeo + 3 de áudio · 111 clipes · 18 textos ·
38 mídias (wallpapers/JPG/PNG/WebP/GIF) · proxy ativo (`sourceUsed: proxy`).

---

## 1. Resultado principal — a sessão passou limpa

| Métrica | Esta sessão | Baseline Fase 8 | Veredicto |
|---|---|---|---|
| cobertura | 149,46 s (projeto inteiro) | 150 s | ✅ |
| `droppedTotal` | **0** | 0 | ✅ |
| `skippedTotal` | **0** | 0 | ✅ |
| `stallMaxMs` | **11,2 ms** | ~15 ms | ✅ melhor |
| stalls (total/eventos) | 905 ms / 46 | — | ~0,6% da sessão |
| tick rate | 62,4 Hz | 58,8 Hz | ✅ |
| open de mídia (49) | p50 **12 ms** · p95 21 ms · máx 71 ms | ~500–800 ms (0.7.x) | ✅ abertura em contexto único pagando |
| worker (decode) | p50 0 · p95 3,5 ms · p99 16 ms · máx 1.422 ms | — | p99 bom; 1 outlier de 1,4 s |
| paint | p99 1,8 ms (composto p99 0,9 ms) | — | ✅ leve |

**Conclusão da seção 1:** em reprodução linear completa, o editor está
**estável** — zero perda de quadro, stalls beirando o ruído, custo de abertura
de mídia reduzido a dezenas de ms.

---

## 2. Os "pretos" investigados — todos legítimos

Os 45 paints com `m_frameNull` / os bursts pretos foram localizados no
timeline e conferidos contra o `.Blanc`:

| Janela | Causa |
|---|---|
| 0,0–0,5 s e início da 2ª passada | warmup (decode do primeiro frame) |
| 15,60 s · 70,50–70,87 s · 144,77–144,93 s | **buracos reais da timeline** (0,17–0,37 s sem nenhuma mídia visível) — preto correto |
| 56,27–56,43 s | buraco com **TEXTO ativo por cima** — o composite mostra `[TEXT]` sobre preto (comportamento correto; a hierarquia nova de textos funciona: `layer TEXT` presente no log) |

Nenhum preto espurio em região com conteúdo. A carência de corte (700 ms) e a
posse de frame cobrem as viradas — nenhum evento de corte apareceu como preto.

---

## 3. Achados (priorizados)

### P1 — Encerramento automático não disparou após o loop (fim da sessão)
O `--seconds=158` deveria fechar o app; a sessão só saiu pelo `timeout` (280 s).
Runs curtos (≤8 s) encerravam sozinho — algo no wrap do loop/tardo fim segura o
quit. **Investigar:** o timer de encerramento do autoplay após `loopOut`.

### P2 — Métrica do FrameCache zerada (instrumentação inconsistente)
`cacheHits: 0, cacheMisses: 0` apesar de ~9.000 decodes passarem por
`decodeCached()` (que chama `getOrCompute` e conta hit/miss nos contadores
estáticos). Ou o profiler amostra de outra fonte, ou há um copy de contadores.
**Investigar:** de onde o JSON lê `cacheHits`/`cacheMisses` (o caminho em si
está em uso — `FrameWorker.h:124`).

### P3 — Taxa de acerto do prefetch de corte: 24/60 (40%)
`prefetchHits: 24` para 60 cortes. Metade dos cortes paga o frame0 no worker
crítico (comproxies o custo é baixo, mas em originais 4K seria perceptível).
Alavancas: janela de warm maior, `PIERROT_DECODER_POOL` maior, ou prefetch do
frame0 também quando o corte é intra-arquivo.

### P4 — Outlier de decode: 1.422 ms (worker max)
Um único decode de 1,4 s (p99 = 16 ms). Provável cold open de mídia sem proxy
ou GOP longo. Sem timestamp no JSON — adicionar o caminho do arquivo ao aviso
de worker lento ajudaria a caçar.

### P5 — Fontes com tempo negativo no frameReady (`t= -1,7`)
Alguns `frameReady` chegam com `t` negativo (clip com speed/inversion?).
Aceito pela tolerância larga, mas denuncia matemática de `clipSrcTime` com
speed < 0 — conferir o tratamento no export (paridade).

### P6 — Ruído: plugin OFX Lainka com ELF inválido (2 avisos)
Pré-existente; só polui o log.

---

## 4. O que está saudável (não mexer)

- **Abertura de mídia**: p50 12 ms — o open em contexto único (a190a91) e o
  memo de probe transformaram o corte frio em não-evento.
- **Pintura**: composição p99 < 1 ms mesmo com 3 camadas + texto.
- **Hierarquia de textos**: `[TEXT, vídeo…]` correto até em buracos da timeline.
- **Proxy**: 5.620 quadros de proxy usados — o caminho proxy→decode estável.
- **Estabilidade geral**: 0/0 em 9k quadros = a base Laartman + L4 está cumprindo
  o contrato de "fluidez vem do pipeline acima" (relatório de estabilidade).

---

## 5. Próximos passos sugeridos (para estabilizar)

1. Corrigir P1 (quit pós-loop) — pequeno, sensível no harness de teste.
2. Corrigir P2 (métrica de cache) — sem ela não dá para medir ganhos futuros.
3. Medir sessão INTERATIVA (scrub real) — esta sessão cobre playback linear;
   o scrub continua sem harness automatizado.
4. Adicionar ao log de worker lento o caminho da mídia (P4).

---

*Artefatos:* `/tmp/pierrot-stress/perf.json` (relatório completo do profiler) e
`/tmp/pierrot-stress/run.log` (61.964 linhas de debug).
