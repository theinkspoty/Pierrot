# Roadmap — rumo ao nível Vegas

Plano vivo para levar o Pierrot de editor alpha a um NLE profissional no Linux.
Ordenado por fases; cada tarefa indica os arquivos envolvidos e o critério de
pronto. Marcamos `[ ]` (não feito) / `[x]` (feito).

> Restrição central: **dois caminhos de render** devem andar juntos.
> - Preview: CPU, `QPainter`/`QImage` (`src/render/MesaRenderer.*`, `src/ui/PreviewWidget.*`).
> - Exportação: CLI `ffmpeg` (`src/export/ProjectExporter.*`, gera `filter_complex`).
> Toda feature nova exige implementação nos dois lados (ou o preview mostra algo
> que a exportação não reproduz).

---

## Fase 0 — Fundação (estabilidade e confiança)

Sem isso, nenhuma feature nova sobrevive.

- [ ] **Suíte de testes** — hoje não há teste nenhum (CMakeLists não tem alvo de teste).
  - Usar Qt Test ou doctest (sem dependência nova pesada).
  - Cobrir: serialização `.Blanc` round-trip (`src/models/Project.cpp`), interpolação
    de keyframes `kfValue`/`upsertKeyframe` (`src/models/Project.h`), geração do
    comando ffmpeg (`ProjectExporter`), frames de referência do `MesaRenderer`.
  - Aceite: `ctest` verde no CI.
- [ ] **CI** — build em Ubuntu (Qt6 + Qt5) e geração do AppImage (`packaging/build-appimage.sh`).
- [ ] **Endurecimento de crash** — reduzir `SIGSEGV` em decode concorrente
  (o `MesaRenderer` já documenta mutex nos decoders; auditar caminhos de
  `FFmpegDecoder` não serializados). Estender o `CrashReporter` para capturar
  stack de threads secundárias.
- [ ] **Projetos grandes** — carregar/salvar com progresso e sem travar a UI
  (mover serialização para thread; hoje `toJson`/`fromJson` é síncrono).

## Fase 1 — Performance (base do "não trava")

- [ ] **Proxy files** — gerar versões leves (resolução menor) em background e
  alternar automaticamente na edição. Reusa o `MediaCache` (`src/ffmpeg/MediaCache.*`).
- [x] **Cache de render** — quadro composto cacheado por timestamp no
  `MesaRenderer` (empilhamento + câmera + motion blur), com invalidação via
  `Project::revision` (bump em `MainWindow::setModified` e `fromJson`). LRU de
  8 quadros em resolução cheia.
- [ ] **Decode multi-thread** — decodificar faixas em paralelo no preview
  (hoje o mutex do `MesaRenderer` serializa tudo; quebrar por decoder, não global).
- [ ] **Smart render** — re-exportar sem re-codificar trechos intactos
  (importante: depende de `stream copy` no `ProjectExporter`, só para cortes simples).

## Fase 2 — Edição (o "corpo" do Vegas)

- [ ] **Máscaras animadas** — máscara por clipe (formas + bezier, com keyframes).
  - Model: `Clip::masks` (novo struct `Mask` em `src/models/Project.h`).
  - Preview: recorte por `QPainterPath` no `MesaRenderer`/pipeline de preview.
  - Export: filtro `mask`/`crop`/`geq` + `overlay` no `ProjectExporter`.
  - Aceite: máscara visível no preview E idêntica na exportação.
- [ ] **Track Motion** — transformar a faixa inteira (não só o clipe).
  - A infra da `Mesa` já faz transform por camada (`Track::mesaX/mesaScaleX/…`);
    reaproveitar trazendo isso para faixas normais da timeline.
- [x] **Velocity envelopes** — velocidade em curva, não fixa.
  - Model: `Clip::kfSpeed` + `clipSpeedAt`/`clipSrcTime` (integral) + `hasVelocityEnvelope`
    (`src/models/Project.h`), serialização em `.Blanc` e presets (`clipattrs.h`).
  - Preview: `PreviewWidget` + `MesaRenderer` usam `clipSrcTime` (paridade com o export).
  - Export: clipes com envelope são pré-renderizados numa sequência PNG
    (`renderVelocitySequence`) e entram como image2 (cadência correta, setpts 1×).
  - Pendências: UI para editar a curva (desenhar keyframes no clipe), áudio com
    velocidade variável (hoje o áudio segue o `speed` base), e LAINKA/OFX/Bench
    com envelope (usam o `speed` base).
- [ ] **Multicâmera** — sincronizar N clipes e cortar entre ângulos com teclas 1..N.
  - Model: `Clip::isMulticam` + `Clip::multicamSource` (qual ângulo ativo por tempo).
  - Preview: troca de fonte em tempo real; export: recortes de cada ângulo.
- [ ] **Timeline aninhada** — abrir um projeto como mídia dentro de outro
  (base para groups/sequências). Reusa a composição da `Mesa`.

## Fase 3 — Áudio (motor forte do Vegas)

- [x] **Automação gravável** — gravação em tempo real dos faders/pan do mixer
  nos envelopes por faixa (`Track::kfVolume/kfPan`).
  - `MixerWidget`: sincronizado com o playhead (`playheadMoved`/`stateChanged`),
    modos Touch/Write/Latch (botão por faixa), sinais de toque no fader e knob.
  - `Track::hasAutomation/automationVolume/automationPan` (`src/models/Project.h`).
  - Preview e exportação **já** consumiam os envelopes de faixa
    (`buildMixSources` usa `kfValue(tr.kfVolume…)`; `ProjectExporter` gera o
    `volume`/`aeval` com a mesma curva) — a gravação escreve nesses mesmos
    envelopes, então reproduz igual nos dois lados.
  - Pendências: edição da curva na timeline (arrastar keyframes no clipe/faixa)
    e exibir o envelope no clipe; escrever keyframe de "retorno" no fim do toque
    (modo Touch volta ao valor anterior ao soltar).
- [ ] **Medidores LUFS + normalização** — loudness meters (`ScopeWidget`) e
  normalize ao target no export (`loudnorm`).
- [ ] **Mais efeitos por faixa** — compressor, de-esser, limiter (ffmpeg já tem
  `acompressor`, `alimiter`; espelhar no preview via DSP simples).
- [ ] **VST/CLAP (adiado, alto custo)** — só depois da Fase 2/3 estáveis;
  caminho realista é host via JACK/PipeWire, não plugar direto.

## Fase 4 — Fluxo profissional e ecossistema

- [ ] **EDL/XML/AAF import/export** — intercâmbio com outros NLEs.
  - Começar por EDL e FCPXML (estrutura simples); AAF/OMF por último.
- [ ] **Saída para monitor externo** (Decklink) e **entrada via SDI/NDI**.
- [ ] **Scripting/automação** (o Vegas tem .NET; opção: bindings Lua/Python
  expondo o `Project`).
- [ ] **Docs + comunidade** — tutorial, guia de build de plugins, página de
  presets e recursos.

---

## Ordem de ataque sugerida (o que destrava mais valor)

1. **Fase 0** (testes + CI) — pré-requisito de tudo.
2. **Máscaras** + **velocity envelopes** (Fase 2) — maior impacto percebido.
3. **Proxy + cache** (Fase 1) — faz o editor "agüentar" projeto real.
4. **Automação gravável** (Fase 3) — usa modelo que já existe.
5. **Multicâmera** + **track motion** (Fase 2).
6. Resto conforme demanda.

> Regra de ouro: nenhuma feature entra sem teste no Fase 0 e sem paridade
> preview ↔ exportação.

---

## Contra-check Kdenlive (foco decidido em 2026-09-22)

Comparação Pierrot vs. Kdenlive: Kdenlive é estável porque tem **24 anos de
depuração, CI e núcleo MLT**. Pierrot (v0.6 alpha, ~6 semanas) não compete em
feature count: compete em **não perder trabalho**. Por isso o foco, nesta
ordem, está abaixo.

### Núcleo de foco — o mínimo que muda o jogo (antes de feature nova)

- [x] **Backup rotativo do `.Blanc`** — timer salvando em `~/Pierrot/backups/`
  com rotação de N cópias. Barato (~30 min) e evita 90% da perda de trabalho.
  (feito em 2026-09-22: `MainWindow::makeBackupCopy`, N=10, ativado em todo
  save manual/autosave via `writeProjectFile`)
- [ ] **Tela de "mídia faltando" + relink** — ao abrir projeto, listar arquivos
  sumidos e re-apontar em lote.
- [x] **Teste real de render/export** — clipe sintético via `ffmpeg -lavfi`,
  comparar quadro do preview vs. exportação. Maior furo de teste hoje (os 28
  testes só cobrem lógica pura; o caminho feliz do `ProjectExporter` não roda).
  (feito em 2026-09-22: `tests/tst_export_pipeline.cpp` — roda o ffmpeg de
  verdade, extrai quadro e compara com `generatorFrame()`, a mesma fonte de
  verdade do preview; + `exportedDurationIsTimelineDuration` via ffprobe)
- [ ] **CI GitHub Actions** — build Qt6+Qt5, `ctest`, AppImage em artefato.
- [ ] **Save/load assíncrono** — `toJson`/`fromJson` fora da thread da UI
  (trava em projeto grande é bug de utilização, não de feature).

### Gaps de feature vs. Kdenlive (registrado para não se perder)

Prioridade alta (faz o editor "sentir estável"/ de produção):
- **Proxy workflow** — classe `ProxyManager` já existe no kernel; falta ativar
  na UI (gerar sob demanda, toggle proxy/original). Destrava edição 4K.
- **Decode multi-thread por faixa** — hoje mutex global serializa tudo.
- **Smart render** — `stream copy` nos trechos não editados.
- **Track Motion** — reaproveitar transform por camada da `Mesa` em faixas.
- **Máscaras animadas** — `Clip::masks` + keyframes bezier (GraphEditor pronto).

Prioridade média:
- **LUTs (3D)** — carregar `.cube` no pipeline.
- **Transições por luma-mask** — multiplicar as 8 atuais → ~30.
- **Normalização + limiter/compressor** — DSP no `pierrot_audiofx` (Rust).
- **Freeze frame, color tags, lock/hide de faixa, duplicar clipe, áudio scrub.**

Prioridade baixa / cara (só depois):
- Multi-câmera (sincronização por áudio/timecode) — a mais cara da lista.
- Timeline aninhada, AAF/OMF, scripting Lua/Python, monitor SDI/NDI,
  titler avançado, versão macOS.

> Decisão em pauta: **só implementar feature nova depois de fechar o "núcleo de
> foco" acima.** O ROADMAP original lista Fase 0 → 4; este bloco é o atalho
> para chegar em "estável o bastante" antes de correr atrás de Kdenlive.

---

# Foco geral do Pierrot 2026 (plano vivo)

Visão de conjunto: **fazer o Pierrot "não perder trabalho" e "não travar"**
primeiro; features de editor em segundo; profissional/ecossistema em terceiro.
Cada item abaixo indica quem destrava o quê, pra não virar lista sem critério.

## 1. Confiança do código (QA, CI e ferramentas)

- [ ] **CI GitHub Actions** — jobs: Ubuntu Qt6 + Qt5 (build), `ctest`, e um job
  "asan" com `-fsanitize=address,undefined`. Publicar AppImage como artefato.
- [ ] **Modo de build `-Wall -Wextra`** no default e aspirar `-Werror` só no CI.
- [ ] **`.clang-format` + `.clang-tidy`** e rodar (só format-diff) no CI —
  sem religar o codebase inteiro de uma vez.
- [ ] **Cobertura do kernel `colombina`** (gcov/lcov) — meta inicial >50% do
  kernel, medido no CI (baixar na hora de aceitar novo teste).
- [ ] **Fuzz do parser `.Blanc`** — corpus de JSONs malformados/abruptos:
  abrir não pode crashar nem travar (parse com limites de recursão/tamanho).
- [ ] **Regressão de crash** — cada crash encontrado vira um teste Qt Test
  reproduzindo o cenário (nem que seja "não crasha"). Meta: 0 crash refixado.
- [ ] **Benchmark no CI** — reusar `Bench` (`--bench`/`--stress`) com limite de
  tempo por execução, pra pegar regressão de performance antes da release.

## 2. Robustez do projeto (`.Blanc` e mídia)

- [x] **Escrita atômica no save** — salvar em `.tmp` + `rename` (hoje só o export
  faz isso); save truncado/corrompido deixa de ser possível.
  (já existia: `writeProjectFile` usa `QSaveFile` — temp + fsync + rename)
- [ ] **Versionamento de schema `.Blanc`** — campo `format_version` no JSON;
  abrir versão mais nova avisa em vez de desserializar errado.
- [ ] **Migração `.ovp` → `.Blanc`** com backup do arquivo original antes de
  converter (o suporte legado já existe; falta a rede de segurança).
- [ ] **Recuperação por backup** — se o `.Blanc` não parsear, oferecer
  automaticamente abrir o backup rotativo mais recente (`1. Núcleo`).
- [ ] **Tela "mídia faltando" + relink** — listar sumidos, re-apontar em lote,
  continuar mesmo com link quebrado (com aviso).
- [ ] **Limite de RAM em 4K** — auditoria: liberar buffers do
  `FrameWorker`/`MediaCache` em projeto 4K (pendência do `OBSERVACOES.md`).

## 3. Crashes e recuperação (não perder trabalho)

- [ ] **Backup rotativo do `.Blanc`** — timer em `~/Pierrot/backups/`, rotação
  de N cópias; restauração por "Abrir recente" → backups.
- [ ] **Auto-recovery pós-crash** — CrashReporter grava o caminho do `.Blanc`
  ativo; na próxima abertura perguntar "restaurar sessão?".
- [ ] **Stack de threads secundárias** no relatório de crash (hoje só o SIGSEGV).
- [ ] **Save de emergência** no handler de crash — tentar `toJson` do estado
  atual antes de abortar (melhor esforço, sem bloquear).
- [ ] **Watchdog de export** — se o processo `ffmpeg` filho morrer/timeout,
  abortar limpo e avisar (nunca deixar `.tmp` como resultado).

## 4. Performance (o "não trava" de verdade)

- [x] **Save assíncrono** — serialização na UI thread (snapshot consistente;
  o modelo só é mutado nela), gravação em disco + backup rotativo num worker
  `QtConcurrent`. Sem bloqueio da UI em projetos grandes; save durante save em
  voo é enfileirado; edição durante a gravação mantém o projeto "sujo"
  (comparação de `revision()`); `closeEvent()` espera o worker terminar.
  (feito em 2026-09-22)
- [ ] **Proxy workflow** — `ProxyManager` já existe; falta gerar sob demanda,
  toggle proxy/original e preferência do projeto.
- [ ] **Decode multi-thread por faixa** — quebrar o mutex global do
  `MesaRenderer` em pool por faixa, priorizando a região do playhead.
- [ ] **Thumbnail cache em disco** — não regenerar thumbs a cada abertura de
  projeto (cache em `~/.cache/pierrot/`).
- [ ] **Métrica de abertura** — template de teste que mede tempo de
  abrir/salvar um projeto 4K sintético (a meta define o "estável").

## 5. Feature gaps do Kdenlive (complemento já priorizado na seção acima)

Manter a ordem alta → média → cara. **Nada disso entra antes do bloco 1–4.**

## 6. Documentação, lançamento e comunidade

- [ ] **CHANGELOG.md** + nota de versão em cada tag (as releases hoje não têm
  texto — só tag `alpha`).
- [ ] **Template de issue** (bug report) pedindo: distro, Qt, ffmpeg, steps,
  e colar `~/Pierrot-crash-*.txt` quando houver.
- [ ] **`CONTRIBUTING.md`** — como buildar, rodar testes, rodar `--bench`,
  e a regra de ouro preview↔export.
- [ ] **Guia de plugin OFX** — mínimo pra terceiros escreverem efeito sem
  conhecer o codebase.
- [ ] **AppImage na página de Release** — gerado pelo CI, com
  `UPDATE_INFORMATION` (AppImageUpdate funcionando).
- [ ] **Critérios explícitos de v1.0 "estável"** — checklist fechado:
  1–4 verdes no CI, 0 crash conhecido sem fix, abertura 4K < meta, save nunca
  trunca. Só então o naipe muda de alpha pra beta/stable.

## Critério geral (como saber que estamos no caminho)

> Pior feature é a que **perde trabalho**; segunda pior é a que **trava a UI**.
> Toda decisão no Pierrot passa por "isso aumenta ou diminui a chance de perder
> o projeto do usuário?" — se aumenta, o item vai para o fim da fila.
