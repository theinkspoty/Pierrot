# Relatório — Estado de estabilidade do motor Laartman (v0.4)

**Data:** 2026-10-20 · **Build avaliado:** `cc3ff0b` (main)
**Método:** inventário de código, mapeamento teste×invariante, histórico de
correções com repro da sessão de teste, medições de aceitação (Fase 8).

---

## 1. Resumo executivo

**O núcleo do Laartman está sólido e coberto por teste.** A sensação de
instabilidade vem de três fontes distintas:

1. **Bugs reais corrigidos nesta semana** (sentidos na testagem: crash/
   travamento, GIF sem loop, lentidão, preto nos cortes, tremida de animação)
   — todos com repro e fix commitado.
2. **Parte dos sintomas morava no PREVIEW (UI/Colombina), não no motor** —
   posse de frame, carência de corte, cache do composto por playhead. O motor
   entregava certo; a apresentação errava.
3. **Superfícies ainda finas** (seção 5) — hardware alheio, mídias reais fora
   da cobertura de CI, e o trabalho §8.3–§8.8 que entrou em 2026-10-20 e ainda
   não é consumido pela UI.

Recomendação central: **antes de mais mudanças, uma rodada de teste limpa no
build atual** — o volume de alterações de 2026-10-20 (25+ commits) explica
muito do "instável" sentido.

---

## 2. Estado atual do motor

| Item | Estado |
|---|---|
| Tamanho | 3.217 LOC em `src/laartman/` (raiz fachada + `core/` + `io/`) |
| API pública | `Decoder` (fachada congelada) + `MediaReader` (contrato substituível, §8.8) + `frameDecoded()` com status tipado (§8.3) |
| Métricas | `DecoderMetrics` (§8.5) — fora da superfície pública |
| Arquitetura | v0.4: núcleo **só decode**; pool/prefetch/cache/qualidade são política L4 do Pierrot |
| Testes do motor | `tst_decoder_api` (20 casos), `tst_concurrency`, `tst_packetreader`, `tst_sourcepool`, `tst_errorconceal`, `tst_framepipeline`, `tst_framecache`, `tst_encoder` |

---

## 3. O que está sólido (com evidência)

### Invariantes do relatório v0.4 §9 — cobertos por teste

| Invariante | Cobertura |
|---|---|
| Determinismo | `determinismSamePathTimeWidth`, `...IndependentOfOrder`, `...AcrossReopen`, `seekBackwardForwardMatchesFreshDecode`, `vfrDecodeIsDeterministic`, `loopGifWrapsDeterministically` |
| Encapsulamento (sem posse no header) | `facadeHeaderExposesNoOwnership` (CI quebra se reintroduzir) |
| Idempotência | `openFailureIsClean`, `closeIsIdempotent`, `openTwiceDoesNotReopen`, `frameAfterCloseIsNull` |
| Falha é tipo | `typedFrameReportsStatus` (§8.3) |
| Contrato substituível | `decoderSatisfiesReaderContract` (§8.8) |
| Larguras ímpares (swscale) | `oddWidthDecodeStaysClean` |
| Vídeo+áudio paralelo, sem deadlock | `videoAndAudioDecodeConcurrently` |
| Latência quente plana | `hotPathLatencyIsFlat` (p99 ≈ p50) |
| Cancelamento | `tst_packetreader::cancelAbortsWait` + token no `frameAt` |
| Concorrência do pool | `tst_sourcepool` (4 casos), `tst_concurrency` |
| Sanitizers | ASan+UBSan limpos nos testes-chave (rodado em 2026-10-20) |

### Medições de aceitação (Fase 8 — sessão de 150 s)

`droppedTotal: 0`, `skippedTotal: 0`, 0 falhas de pool (classe eliminada por
construção na Fase 1), stall máximo 2045 → 18 ms.

### Correções provadas com repro (não é achismo)

Crash do issue #11 (estouro do swscale, medido byte a byte com sentinel),
loop de GIF (frames nulos → idênticos ao ciclo, diff 0), seek sem restart
(60 seeks+decode em 1,55 s), open em contexto único (metade do
`find_stream_info` por abertura).

---

## 4. Histórico de estabilidade — 2026-10-20 (repro → fix → garantia)

| Incidente | Causa-raiz | Fix | Garantia |
|---|---|---|---|
| Crash/travamento (issue #11) | Kernel AVX2 do swscale escrevia 32 B além do QImage em larguras não múltiplas de 16 (360, 1080 vertical) | `swsScaleToRgba` — linhas alinhadas em larguras ímpares | `oddWidthDecodeStaysClean` |
| GIFs longos sem looping | Seek pós-EOF com codec drenado e SEM flush → `AVERROR_INVALIDDATA` em todo pacote | Flush em todo seek + supressão do wrap removida | teste de loop fortalecido (cruza o EOF real) |
| Lentidão geral (agulha/arrasto) | `PacketReader::seek` matava/recriava a thread a cada seek; open pagava 2× `find_stream_info` | Pedido de seek pendente (sem restart) + abertura em contexto único | 7 testes do reader + suíte completa |
| Preto na virada do corte | Correção de posse sem carência: frame novo atrasado → nada desenhado | Carência de 250 ms segurando o último quadro (padrão NLE) | manual (UI) |
| Tremida/congelamento de animação | Cache do composto chaveado por índice de frame; fast-path sem posse de `m_frame` | Chave = playhead exato; posse + guarda de dono | manual (UI) |

---

## 5. Onde a instabilidade ainda PODE morar (riscos restantes)

| # | Risco | Severidade | Nota |
|---|---|---|---|
| R1 | **Variabilidade de hardware** — VAAPI (auto-cura), AV1 sem VAAPI, GPUs distintas. Todo o teste de 2026-10-20 foi nesta máquina. | Média | O fallback de software existe e é testado; hardware alheio não. |
| R2 | **Trabalho §8.3–§8.8** — `frameDecoded`/`MediaReader`/`DecoderMetrics` recém-entrados (build quebrou; corrigido em `5f1a4d8`). Os 22 call sites da UI ainda incluem `laartman/core/FFmpegDecoder.h` direto. | Média | Duas superfícies convivem até a migração — divergência futura garantida. |
| R3 | **Larguras ímpares** pagam cópia (padded) por frame — ok em SD/720p; monitorar em 1080 vertical / 4K de largura ímpar. | Baixa | Cópia de ms; só incomoda em 4K. |
| R4 | **Formatos fora da cobertura**: MPEG-TS, VFR *real* (teste é sintético), WebP animado, codecs exóticos. dav1d/libraw (§10) não adotados. | Baixa | AV1 passou pelos fixes com dav1d do sistema. |
| R5 | **§8.1 (índice de busca persistido) pendente** — corte frio ainda paga a decodificação desde o keyframe (custo dominante dos stalls residuais ~1,3 s). | Baixa | Conhecido, medido, documentado. |
| R6 | **TSan nunca rodou** (mitigação escrita no relatório v0.4 §11). ASan/UBSan sim. | Baixa | `tst_concurrency` + `tst_packetreader` sob TSan fecharia o capítulo. |

---

## 6. Lacunas de teste (o que o CI não alcança)

1. **Mídia real de usuário**: VFR de câmera/telefone, GOP longo com B-frames,
   4K, verticais (1080 de largura — casam exatamente no bug do swscale).
2. **Scrub interativo** — os testes automatizam playback linear; o arrasto da
   agulha (caminho mais friccionado) só foi medido por driver sintético.
3. **Sessão longa com proxies + multi-faixa + áudio** — o acceptance da Fase 8
   foi feito antes das mudanças de 2026-10-20; merece repetição como baseline.
4. **GIF/WebP grandes** — o teste de loop usa GIF de 2 s; o repro de 6 s foi
   manual.

---

## 7. Recomendações (priorizadas)

1. **Baseline de estresse no build atual** — `--autoplay` no projeto de
   estresse (ou `teste de potencia.Blanc`, 150 s) com `PIERROT_PERF_JSON`;
   arquivar o JSON como marco para comparar as mudanças de 2026-10-20.
2. **Migrar os call sites para `MediaReader`/`Decoder`** — fecha o §8 de
   verdade: um único header de contrato, motor plugável, superfície
   congelada renderizando.
3. **Pacote de mídia real como fixture** — meia dúzia de arquivos (VFR de
   telefone, TS, vertical 1080, GIF longo, AV1) num diretório de testes com um
   `tst_realmedia` que roda só se o diretório existir.
4. **§8.1 índice de busca persistido** — mata o último custo dominante de
   corte frio.
5. **TSan no CI** sobre `tst_concurrency`/`tst_packetreader` — fecha a
   mitigação nunca feita do relatório v0.4.

---

## 8. Veredito

**O motor não é instável por desenho** — os oito invariantes de estabilidade
estão implementados e sete têm teste automatizado; as falhas da semana eram
bugs concretos (swscale, GIF, preview), todos com repro e garantia. O que
sobra é **cobertura de mundo real** (hardware alheio, mídia de verdade, scrub)
e **a migração dos consumidores** para a API nova. Enquanto isso não roda, o
"instável" é a memória dos bugs de 2026-10-20 — recomenda-se uma sessão de
teste limpa no build atual antes de qualquer conclusão em contrário.

---

*Fim do relatório.*
