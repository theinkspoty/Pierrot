# Relatório — campanha "corrija tudo" (2026-10-21)

**Build:** main local · **NADA foi commitado** (aguardando ordem) ·
**Validação final:** 19/19 testes normal + 19/19 ASan/UBSan + estresse
autoplay (0 drops) + scrub (0 pretos) — tudo saiu sozinho (EXIT=0).

---

## O que foi corrigido

### P1 — App não encerrava após sessões longas (EXIT=124)
**Causa:** `QCoreApplication::quit()` chamado uma vez encerrava só um loop
aninhado do Qt (pause de áudio/PipeWire); o loop principal seguia vivo
(comprovado: "quit executando" + watchdog vivo 3 s depois).
**Fix:** thread destacada martelando `quit()` a cada 200 ms até o processo
morrer; pós-teardown vira no-op. Sessões curtas E longas agora saem sozinhas.

### P2 — Métrica cacheHits/cacheMisses zerada
**Causa:** o escritor do JSON lia `m_frames.back()` — o registro do último
tick, que não recebia carimbo de worker (contadores são cumulativos,
monotônicos).
**Fix:** agregação por **máximo** das amostras (também aplicado a
`droppedTotal`, mesma classe). Sessão de estresse agora reporta
`cacheHits: 1734 / misses: 4877` (antes 0/0).

### P3 — Prefetch de corte "40%" → resolvido como não-bug
Com P2 vivo e P4 abaixo: apenas **1 decode >500 ms em 149 s** (60 cortes).
A causa é o arquivo `147396-…mp4` (**2048×1152, abaixo do limiar de proxy de
2560**) — sem proxy por design, paga open frio no frame0. **Ação: Configurações
▸ Desempenho ▸ limiar de proxy → 1920** (recomendação de config, não código).

### P4 — Outlier de decode invisível
**Fix:** aviso `qWarning "[worker] decode LENTO Nms → <caminho>"` sempre que
latência > 500 ms (antes só métrica anônima) + caminho no log de atraso.
Já capturou o culpado do P3 na primeira sessão.

### P5 — `frameReady` com `t` negativo
**Causa:** clipe com `in: -1,73` no projeto (trim além da head do arquivo);
`frameAt` derrubava p/ 0 mas o pedido viajava negativo e o export mandava
`-ss -1.7` pro ffmpeg.
**Fix em 5 camadas:** carga com clamp (`clipFromJson`), consumidor
(`clipSrcTime`), produtores (trimmer dialog, insert/overwrite), export
(`-ss ≥ 0`). Verificado: **0 `t` negativo** na região do projeto afetado.

### Flake do tst_sourcepool (abort intermitente)
Não reproduzido em 13 rodadas (inclusive pós-sessão de GPU). **Mitigação por
isolamento:** `PIERROT_GPU=0` nos testes de LÓGICA (sourcepool, framepipeline,
encoder) — os testes que cobrem hw (decoder_api, concurrency) seguem com GPU.
O abort só aparecia sob carga de GPU mista; a raiz exata não capturou.

### P6 — Aviso OFX repetido
Dedução por (caminho, erro): uma vez por sessão.

### TSan (nunca tinha rodado) — **código do projeto LIMPO**
- `tests/tsan.supp` + receita (build `-fsanitize=thread`, `PIERROT_GPU=0`
  no concurrency — evita um CHECK interno do TSan com threads de driver).
- Resultado: **62 relatórios — 0 com código do projeto nos blocos de
  acesso** (todos Qt/libav não instrumentados; as ocorrências `src/laartman`
  eram contexto de criação de mutex/thread).

### Scrub interativo — finalmente medível
- **Novo harness `pierrot --scrub <projeto> [--seconds] [--json]`**: saltos
  determinísticos do playhead (250 ms, degraus + voltas + saltos longos)
  espelhando a fiação real (`setPlayhead` + `PreviewWidget::seek`), com
  `profileStep()` para o profiler gravar sem ticks de playback.
- Resultado no estresse: 177 saltos / 161 viradas de clipe / **0 pretos**,
  0 drops, 1× o LENTO do P3.

### Fixture de mídia real — `tst_realmedia`
- Varre `tests/media/` (gitignored): probe plausível, frames não-nulos em
  0/middle/fim, determinismo pós-reabertura para CADA arquivo.
- Vazio → todos os casos pulam (CI inofensivo). Validado localmente com 3
  mídias reais (AV1 360, GIF 6 s, MP4 2048) = 10 casos PASS.
- Preencha `tests/media/` com VFR de câmera, TS, vertical 1080 etc. para
  cobrir as lacunas do relatório de estresse §6.

### Migração dos call sites para a fachada (§8 pendência)
- **22 → 9 arquivos** incluem o header do motor. 14 consumidores agora usam
  `laartman/Decoder.h`.
- **Fachada estendida** dentro do que o teste de invariantes permite:
  `frameCacheIndex`, `usesHardware`, `audioPeaks` + tipo `FFmpegAudioPeaks`
  (movido para a fachada), `MediaInfo.audioChannels`. Nenhum token proibido.
- **Migrados:** TimelineDrag, TimelineWidget, MediaPool, ProxyManager, Bench,
  SourceMonitor, Trimmer (hw via `DecodeOptions.hardwareAllowed` — a opção
  injetada já existia), AudioConformCache, Lainka/Ofx/ProjectExporter,
  PreviewWidget.h.
- **Stayers (por design):** SourcePool/FramePipeline/MesaRenderer (infra do
  pool devolve o motor), MediaCache/FrameWorker/UiDecode/Pantalone/Pivot
  (usam gestão de memória que o invariant test proíbe na fachada).

---

## Estado final validado

| Verificação | Resultado |
|---|---|
| Suíte normal | **19/19** |
| ASan+UBSan | **19/19** |
| Autoplay estresse (30 s / 149 s) | EXIT=0, **0 drops**, stall ≤11,4 ms |
| Scrub estresse | EXIT=0, **0 pretos**, 66 cortes/20 s |
| TSan (project code) | **0 corridas nossas** / 62 relatórios só de libs |

## Ainda em aberto (fora do alcance do código)

1. **Config do usuário:** limiar de proxy 2560 → 1920 (P3, 1 passo na UI).
2. **`tests/media/`**: popular com mídia real de câmera/telefone/TS/vertical.
3. **Hardware alheio** (Intel/AMD, AV1 sem VAAPI) — só testável em outra máquina.
4. §8.1 (índice de busca persistido) — feature, não correção.

*Artefatos em `/tmp/pierrot-stress/` (perf JSONs, logs, scrub). Nenhum commit
foi feito nesta campanha — o diff total está na árvore de trabalho.*
