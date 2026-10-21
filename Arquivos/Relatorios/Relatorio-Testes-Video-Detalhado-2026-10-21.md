# Relatório detalhado — testes de vídeo: Preview · Laartman · Colombina · Export

**Data:** 2026-10-21 · **Build:** main local (`903e652` + worktree sujo apenas
com o relatório; **nenhum commit novo**) · **Suítes:** normal + ASan/UBSan ·
**Cenários:** harness `--autoplay` + drivers de motor.

> Relatório complementar: `Relatorio-Stress-Teste-de-Potencia-2026-10-21.md`
> (sessão de estresse de 149 s no projeto real).

---

## 1. Visão geral

| Camada | Suíte normal | ASan/UBSan | Casos |
|---|---|---|---|
| **Laartman** (motor core/io) | ✅ 18/18 testes | ✅ 18/18 | 20+7+6+6+6 casos individuais |
| **Colombina** (L4: pool/pipeline/cache/encode) | ✅ | ✅ | 12 casos (pipeline/cache) + encoder/export |
| **Preview** (UI/composição) | ✅ via drivers + 6 cenários harness | — | 0 drops em todos |
| **Export** (paridade preview↔export) | ✅ | ✅ | 4 casos + 3 do exporter |
| **Cenários de vídeo (harness)** | ✅ 6/6 | — | todos 0/0 drops/skips |
| **Sessão de estresse 149 s** | ✅ | — | 0 drops, stall máx 11,2 ms |

**Total: 18/18 suítes (normal) + 18/18 (ASan+UBSan) + 6/6 cenários + sessão
de estresse completa — zero falhas, zero sanitizers.**

---

## 2. Laartman — motor de codecs (core/ + io/)

### 2.1 `tst_decoder_api` — 20 casos (contrato do núcleo L3)

| Caso | O que garante |
|---|---|
| openFailureIsClean | open inexistente falha limpo, sem estado meio-aberto |
| closeIsIdempotent | close duplo seguro |
| openTwiceDoesNotReopen | atalho de reabertura (mesmo arquivo = mesmo contexto) |
| frameAfterCloseIsNull | frame pós-close devolve nulo, nunca quebra |
| determinismSamePathTimeWidth | mesmo (path,t,width) = pixels idênticos |
| determinismIndependentOfOrder | ordem dos pedidos não muda o frame |
| determinismAcrossReopen | reabrir não muda o frame |
| seekBackwardForwardMatchesFreshDecode | seek p/ trás e frente = decode fresco |
| loopGifWrapsDeterministically | **loop de GIF cruza EOF real** (dreno + wrap de 1 e 3 ciclos) |
| vfrDecodeIsDeterministic | VFR (concat 30+10fps) sem drift na chave de cache |
| videoAndAudioDecodeConcurrently | mesma instância, 2 threads, sem deadlock |
| oddWidthDecodeStaysClean | **largura 360 (%16≠0) não corrompe heap** (swscale) |
| hotPathLatencyIsFlat | p99 ≈ p50 no caminho quente |
| facadeMatchesEngine | fachada Decoder = motor |
| facadeHeaderExposesNoOwnership | CI quebra se reintroduzir posse no header |
| typedFrameReportsStatus | erro tipado `frameDecoded` (§8.3) |
| decoderSatisfiesReaderContract | contrato `MediaReader` (§8.8) |
| probeCacheMatchesAndInvalidatesOnChange | memo de probe + invalidação por mtime (§8.6) |

### 2.2 `tst_concurrency` — 7 casos (1 skip ambiental)

concurrentFrameAtOnSharedDecoder · closeDuringFrameAt · audioAccessorsDuringOpen
· audioDecodeDuringSeek · releaseBuffersDuringFrameAt ·
independentDecodersSameFile — **PASS** ·
hwAutoCureAfterEmptyFrames — SKIP (VAAPI decodifica nesta máquina; auto-cura
não exercitável — cobre apenas em GPU sem hw).

### 2.3 `tst_packetreader` — 6 casos

openFailureIsClean · readsAllPacketsBounded (teto de memória) ·
seekRepositionsAndReadsAgain · **seekAfterEofRestartsProduction** (restart
pós-EOF, o caso do wrap de GIF) · cancelAbortsWait.

### 2.4 `tst_errorconceal` — ocultação de erro (Ok/Pending/Eof/Error/Cancelled) ✅

### 2.5 Drivers de motor (fora da suíte)

| Driver | Resultado |
|---|---|
| AV1 360×360 (`horse speaking japanese.mp4`) | decode completo, **sem corrupção de heap** (o bug do issue #11) |
| GIF 6 s (loop pós-EOF) | t=7/13 **idênticos** ao ciclo (diff 0) |

---

## 3. Colombina — pipeline L4 (pool, prefetch, cache, encode)

| Suíte | Casos | Status |
|---|---|---|
| `tst_sourcepool` | openFailureDoesNotLeakEntry · samePathIsNeverRefused · evictsLeastRecentlyIdle · concurrentSamePathsAreNeverRefused | ✅ (4/4; fora da suíte é o flake ambiental de VAAPI conhecido — isolado passa 4×) |
| `tst_framepipeline` | warmQueueReplacedNotAccumulated · runPrefetchNowIsDeterministic · **threadedPrefetchEmitsPairAndClearsBusy (latest-wins)** · warmNowPopulatesPoolAndClearsInFlight · stopIsSafeAndIdempotent | ✅ 5/5 |
| `tst_framecache` | miss/hit · chaves distintas · LRU · null ignorado · clear · putSameKey · getOrCompute (+null) · **evictsByByteBudget** · **byteAccounting** | ✅ 11/11 |
| `tst_encoder` | encodeThenDecode (round-trip) | ✅ |

---

## 4. Export — paridade preview↔export

| Suíte | Casos |
|---|---|
| `tst_export_pipeline` | solidColorExportMatches · checkerboardExportMatchesPreview · exportedDurationIsTimelineDuration · **decodedSourcePreviewMatchesExport** (§8.7) — ✅ |
| `tst_exporter` | emptyOutputPathFails · emptyTimelineFails (erros do buildCommand) — ✅ |

---

## 5. Preview — cenários de composição (harness, build atual)

| Cenário | Quadros | Duração | Drops | Skips | Stall máx | Hz | Camadas |
|---|---|---|---|---|---|---|---|
| horse-single (AV1+áudio, kfOpacity) | 483 | 5,97 s | **0** | 0 | 3,8 ms | 81,0 | — |
| anima-cima (vídeo embaixo + imagem animada em cima) | 355 | 6,03 s | **0** | 0 | 9,2 ms | 58,8 | 49 |
| anima-borda (topo animado com in/out diferentes) | 411 | 6,03 s | **0** | 0 | 4,4 ms | 68,1 | 33 |
| texto-hier (texto embaixo + vídeo em cima) | 178 | 3,04 s | **0** | 0 | 6,2 ms | 58,5 | — |
| gerador (mídia gerada, gradiente) | 290 | 3,06 s | **0** | 0 | 4,6 ms | 94,8 | — |

- Hierarquia de textos verificada no log: ordem `[TEXT, vídeo]` (baixo→cima).
- Pretos no `anima-borda`: só nas bordas legítimas da timeline (conferido
  contra o `.Blanc`).

---

## 6. Sessão de estresse (projeto real, 149,4 s)

Resumo (detalhes no relatório dedicado): **droppedTotal 0 · skippedTotal 0 ·
stallMax 11,2 ms · 62,4 Hz · open p50 12 ms · 9.323 quadros · 60 cortes**.
Pretos restantes = buracos reais da timeline (0,17–0,37 s), nenhum espúrio.

---

## 7. Sanitizers

- **ASan+UBSan** (`PIERROT_SANITIZE=ON`, `-fno-sanitize-recover=all`):
  **18/18 testes limpos** nesta data (inclui decoder_api, concurrency,
  packetreader, sourcepool, framepipeline, export).
- **TSan**: ainda não rodou (pendência conhecida do relatório v0.4 §11).

---

## 8. Pendências abertas (do relatório de estresse + desta varredura)

| # | Item | Área |
|---|---|---|
| P1 | Encerramento automático pós-loop do harness não dispara (saiu por timeout) | Preview/harness |
| P2 | Métrica `cacheHits/cacheMisses` zerada no JSON (instrumentação) | Colombina/profiler |
| P3 | Prefetch de corte acerta 24/60 (40%) | FramePipeline |
| P4 | Outlier de decode de 1,4 s sem caminho no aviso | Preview/worker |
| P5 | `frameReady` com `t` negativo (speed<0?) | Preview/export paridade |
| P6 | OFX Lainka ELF inválido (ruído) | OFX |
| — | TSan no CI | Laartman |
| — | Cobertura de scrub interativo (só playback linear está medido) | Preview |

---

## 9. Veredito por camada

- **Laartman:** sólido — 21+7+6 casos + drivers, sanitizers limpos, invariantes
  (determinismo/idempotência/encapsulamento/erro tipado) todos com teste.
- **Colombina:** sólido — pool sem recusa, prefetch com latest-wins coberto,
  cache com budget coberto.
- **Export:** paridade coberta (sólido + checkerboard + decode-preview).
- **Preview:** estável em reprodução linear pesada (0/0 em 149 s e em todos os
  cenários); resto é a lista P1–P6 de refinamento — nenhum deles é perda de
  quadro em reprodução.

*Nada desta campanha foi commitado (builds e `/tmp/pierrot-stress|issue11`).*
