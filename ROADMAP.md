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

- [x] **Suíte de testes** — 8 alvos em `CMakeLists.txt`, `ctest` verde (8/8).
  - Qt Test, sem dependência nova. Cobre os quatro pontos pedidos:
    round-trip `.Blanc` (`tst_serialization`), interpolação de keyframes
    `kfValue`/`upsertKeyframe` (`tst_keyframes`), geração do comando ffmpeg
    (`tst_exporter` p/ caminhos de erro, `tst_export_pipeline` p/ o caminho
    feliz rodando o ffmpeg CLI de verdade), frames de referência do
    `MesaRenderer` (`tst_export_pipeline` compara com `generatorFrame()` —
    mesma fonte de verdade do preview).
  - Extras já lá: `tst_edl`, `tst_audio_conform`, `tst_audio_conform_intervals`,
    `tst_crashreporter`.
- [ ] **CI** — build Qt6 + `ctest` já rodam (`.github/workflows/ci.yml`).
  - ⚠️ **O job Qt5 está quebrado e não é regressão recente**: o código usa APIs
    só de Qt6 sem guarda de versão e **não compila com Qt5** (~76 erros num
    checkout limpo do HEAD em 2026-09-29). Culpados: `QColor::isValidColorName`
    e `QVariant::metaType` (`Project.cpp`), `QVector::sort` e
    `QVector<QList>`↔`QVector<QList>` (`NleInterchange.cpp`),
    `QFuture::results` (`MesaRenderer.cpp`), `QVector::resize` de 2 args
    (`tests/tst_audio_conform.cpp`), e `#include <QDateTime>` faltando em
    `MediaCache.cpp`. Ou se adiciona as guardas de versão, ou se decide que Qt6
    é requisito e o job sai do CI — mas ele não pode continuar verde-por-mentira.
  - Falta: job `asan`/`ubsan`, AppImage como artefato, `-Wall -Wextra`.
- [~] **Endurecimento de crash** — diagnóstico e a lacuna de lock fechadas;
  os SIGSEGV em si ainda não foram reduzidos.
  - Auditoria do `FFmpegDecoder`: já está quase todo serializado
    (`open`/`close` com os dois mutexes; `isOpen`/`source`/`fps`/`usesHardware`
    em `m_mutex`; `hasAudio`/`seekAudio`/`decodeAudio` em `m_audioMutex`).
    Única lacuna real corrigida: `audioChannels()` lia `m_audioOutCh` sem
    `m_audioMutex` enquanto `open()` escreve sob ele.
  - O cache LRU de frames estava correto, mas por convenção e não por
    assert; as funções ganharam o sufixo `Locked` e a pré-condição foi
    documentada.
  - `CrashReporter` agora grava o inventário das threads: as registradas com o
    que estavam fazendo, e todas as de `/proc/self/task` (pool do Qt, FFmpeg
    interno) marcadas como não registradas. O `CrashReporter` foi movido para
    `colombina` — era infra de kernel presa em `src/`, inalcançável dos workers
    de decode — e os workers (`preview-frame`, `preview-prefetch`, `cache-picos`,
    `cache-thumbs`, `export-build`) usam `CrashReporter::TrackedThread`.
  - `tst_crashreporter` trava os dois bugs que só apareceram com um crash real:
    o TID vinha de `pthread_self()` (que no glibc é ponteiro de descritor, não
    TID, então nunca casava com o `/proc`), e o registro estava no `run()` do
    QThread — mas o Qt emite `started` **antes** de `run()`, então um worker
    ligado a `started` que bloqueia (o padrão de exportação) nunca se
    registrava.
  - Falta: rodar `asan`/`ubsan` em CI e caçar os SIGSEGV de verdade.
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
- [x] **Multicâmera** — sincronizar N clipes e cortar entre ângulos com teclas 1..N.
  - Model: `Clip::isMulticam` + `multicamSources`/`multicamIns`/`kfAngle`/`defaultAngle`
    (`src/colombina/models/Project.h`); serialização no `.Blanc`.
  - UI: menu de contexto **Criar multicam** (≥2 clipes de vídeo selecionados);
    teclas `1..N` gravam keyframe de ângulo (KfStep) no playhead.
  - Preview/Mesa/export usam `mediaIdAt(rel)`/`multicamInAt(rel)`/`clipSrcTime`;
    a exportação expande o multicam em segmentos por ângulo
    (`expandMulticamClip` em `ProjectExporter.cpp`).
  - Pendências: sync por áudio/timecode, UI de grade de ângulos, áudio do
    ângulo “programa” automático.
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
- [ ] **Catálogo de efeitos open source (OFX/frei0r)** — ✅ **host frei0r
  implementado** em 2026-10-02 (`src/colombina/frei0r/Frei0rPluginManager.*`):
  scan de `/usr/lib/frei0r-1` + `PIERROT_FREI0R_PATH`, pilha `Clip::frei0rFx`,
  preview via `applyEffects`, export via ffmpeg `frei0r=`, painel Efeitos +
  aba Express. Host OFX já existia. Inventário de bibliotecas:
  [`Arquivos/Relatorios/Relatorio-Bibliotecas-Efeitos-Video.md`](Arquivos/Relatorios/Relatorio-Bibliotecas-Efeitos-Video.md).
  Pendências: empacotar `openfx-misc`/`frei0r-plugins` no AppImage, UI de
  caminhos em Configurações, teste de paridade preview↔export com ffmpeg real.

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

- [x] **CI GitHub Actions** — jobs `build` (Qt6 + `ctest`) e `sanitizers`
  (`asan`+`ubsan`). O `tst_crashreporter` fica de fora do job de sanitizer,
  porque ele morre de SIGSEGV de propósito.
  - O job Qt5 foi removido: o código não compila com Qt5 (ver Fase 0) e o job
    estava verde-por-mentira. Qt6 é requisito.
  - Falta: AppImage como artefato, `-Wall -Wextra`, medir cobertura no CI.
- [ ] **Modo de build `-Wall -Wextra`** no default e aspirar `-Werror` só no CI.
- [ ] **`.clang-format` + `.clang-tidy`** e rodar (só format-diff) no CI —
  sem religar o codebase inteiro de uma vez.
- [ ] **Cobertura do kernel `colombina`** (gcov/lcov) — meta inicial >50% do
  kernel, medido no CI (baixar na hora de aceitar novo teste).
  - Medido em 2026-09-29 (gcov sobre o `ctest` atual): **15,0%** do kernel
    (1668/11097 linhas). Longe da meta de 50%.
  - O que está coberto: `NleInterchange` 88%, `clipattrs.h` 77%,
    `AudioConformCache` 42/57%, `Project.h` 33%, `Project.cpp` 18%,
    `ProjectExporter` 10%, `generators.h` 8%.
  - O buraco é justamente onde dói: **`MesaRenderer.cpp` 0,0%** (566 linhas),
    **`ProxyManager.cpp` 0,0%** (394), **`FFmpegDecoder.cpp` 2,3%** (2583) —
    os três arquivos que carregam a concorrência. Por isso o job `asan` sozinho
    não acha SIGSEGV de decode: o ASAN só reporta o que ele executa, e esses
    caminhos não rodam em nenhum teste. Teste de concorrência que abra e
    decodifique de verdade é pré-requisito, não detalhe.
- [ ] **Fuzz do parser `.Blanc`** — corpus de JSONs malformados/abruptos:
  abrir não pode crashar nem travar (parse com limites de recursão/tamanho).
- [x] **Regressão de crash** — primeiro caso fechado.
  - `tst_crashreporter` (fork + SIGSEGV real, confere o relatório gerado).
    Preservou dois bugs que só se manifesto com crash de verdade — ver Fase 0.
  - Meta: 0 crash refixado. Falta cobrir os achados do `asan`/`ubsan`.
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
- [x] **Proxy workflow** — geração **sob demanda** no import (2K+ → H.264 leve
  em worker, vídeo-apenas; thumb/preview decodificam o proxy; exportação sempre
  usa o **original**) e **preferência do projeto** `useProxies` (serializada,
  via Configurações do projeto): OFF decodifica o original no preview e não gera;
  ao reativar, re-enfileira as fontes conhecidas sem proxy. (feito em 2026-09-22)
- [ ] **Toggle proxy/original por faixa** — `Track::proxyPreview` (forceOriginal
  no `MesaRenderer::decodeFrame`, cache invalidado por `revision`) + toggle no
  cabeçalho da faixa. Adiado (escolha do escopo).
- [x] **Decode multi-thread por faixa** — `MesaRenderer::warmTracks()`: antes
  de compor um quadro, decodifica em PARALELO (QtConcurrent) as faixas visíveis
  com mídia real no instante do playhead, aquecendo o cache de quadro de cada
  decoder; o paint sequencial (preparação de layers + motion blur, conteúdo
  FIXO em relTime) reaproveita os frames já decodificados. Menos queda de FPS
  com várias camadas HD/4K. (feito em 2026-09-22)
- [x] **Thumbnail cache em disco** — `CacheWorker`: primeiro checa
  `~/.cache/pierrot/thumbs/`, decodifica só o que falta e salva PNG. O hash do
  nome inclui mtime+size da fonte: mídia alterada invalida sozinha (sem
  sidecar); poda mantém no máximo ~4096 thumbs. Reabrir um projeto não
  regenera do zero os mesmos instantes. (feito em 2026-09-22)
- [ ] **Métrica de abertura** — template de teste que mede tempo de
  abrir/salvar um projeto 4K sintético (a meta define o "estável").

## 4.5. Backlog kernel — anotado para atualização futura (2026-09-22)

Sem prioridade imediata; registrar para a próxima passada no núcleo
(`src/colombina`). Ordem sugerida pela análise do KERNEL.md.

> **Conferido contra o código em 2026-09-29:** nenhum item desta fila foi
> implementado; P2 e P5 estão parciais. O detalhamento com `arquivo:linha` e o
> inventário de testes estão na **seção 4.6** — use esta lista como resumo e a
> 4.6 como detalhe.

- [ ] **Versionamento de schema `.Blanc`** — `format_version`; abrir versão
  futura avisa em vez de desserializar errado. (já listado em "2. Robustez")
- [ ] **Indexação O(1) por id** — `findMedia`/`findMesa`/`findMesaForTrack`
  (Project.h) são varreduras lineares chamadas por quadro no MesaRenderer;
  passar para `QHash<id, idx>`. Banal hoje, custo dominante em projeto grande.
- [ ] **Load `.Blanc` assíncrono e defensivo** — `fromJson` ainda roda na UI
  thread (o save já é async); juntar com fuzz do parser.
- [ ] **GPU na composição** — `MesaRenderer` 100% CPU `QPainter`; maior gap
  vs. Vegas (OpenCL/CUDA). Maior investimento, melhor recompensa em 4K multi.
- [ ] **Smart render** — `stream copy` em trechos intactos do export.
- [ ] **Sanitizers + `-Wall -Wextra` no CI** e **cobertura do kernel >50%**.

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

## 7. Interface 0.7 — painéis, workspaces e densidade (2026-09-28)

Escopo desta seção: **a camada de organização da UI**, não features de edição.
Registrado a pedido de "deixar mais parecido com After Effects". A conclusão da
análise (ver "Por que painéis, não paleta" abaixo) é que o que faz AE/Premiere
parecerem profissionais **não é a cor** — é o sistema de painéis + workspaces
salvos. O Pierrot já tem os dois tijolos; falta a casa.

### Estado atual (auditado em `src/MainWindow.cpp`)

O que já existe e é **bom** — não redesenhar:

- **12 docks** criados programaticamente (`:823`–`:969`), todos com o mesmo
  bloco de 5 linhas (criar → `setWidget` → `setFeatures` → `addDockWidget`).
- **Layout já persistido**: `saveState()`/`restoreState()` em
  `saveSettings` (`:598`) e `restoreSettings` (`:688`), com guarda de
  `layoutVersion` (`kLayoutVersion = 3`, `:101`) **e** validação
  `saneLayoutArray()` (`:173`, checa o magic `0xff` do `QDataStream`).
  Isso é mais maduro que muito editor em produção.
- **Agrupamento em abas** já em 4 pilhas:
  `Efeitos|Express` (`:880`), `Pancrop|Mesa` (`:920`),
  `Mixer|Histórico` (`:939`), `Mixer|Histórico|Analisadores` (`:969`).
- **Menu Exibir** lista os 12 toggles (`:1225`–`:1235`).
- **89 tokens de cor** nomeados em `src/ui/Theme.h`, já cobrindo monitor, canvas,
  timeline, régua, faixa, clipe, dock, input, botão e accent. A paleta **não é
  o gargalo**.
- `setDockLocked` (`:819`, `:1213`) + ação "travar layout" — impede arrastar
  painel acidentalmente.

### Itens

- [ ] **Desempilhar Mixer / Histórico / Analisadores** — bug real, não feature.
  Hoje `tabifyDockWidget(m_mixerDock, m_histDock)` (`:939`) seguido de
  `tabifyDockWidget(m_histDock, m_scopesDock)` (`:969`) **encadeia** os três
  numa pilha só de 3 abas em ~100 px de altura útil. Aceito: Mixer e
  Analisadores em pilhas separadas, Histórico escondido por padrão.
  Risco: baixo. Arquivos: `MainWindow.cpp`.
- [ ] **Registry de painéis** — os 12 blocos de 5 linhas viram uma tabela
  `struct PanelDef { id, title, factory, defaultArea, tabGroup }` + um laço.
  Aceito: adicionar um 13º painel custa 1 linha na tabela; `Exibir` gerado a
  partir do registry em vez de 12 `addAction` manuais.
  Risco: baixo-médio (mexe na construção da janela). Ganho: maintibilidade.
  Arquivos: `MainWindow.cpp`, `MainWindow.h`.
- [ ] **Workspaces** — a feature que mais entrega "igual ao Adobe", e sai
  **barato** porque `saveState()`/`restoreState()` já serializam tudo:
  um workspace é só o `QByteArray` guardado sob um nome + um `QComboBox` no
  menu Exibir. Presets iniciais: `Padrão`, `Edição` (só timeline+pool+monitor),
  `Áudio` (mixer grande), `Composição` (Mesa grande, timeline encolhido),
  `Efeitos`.
  Risco: médio. Cuidado com `kLayoutVersion`: trocar o layout padrão tem que
  bumpingar a versão, senão `restoreState()` (`:688`) vai reidratar um estado gravado
  com a pilha de 3 abas. Arquivos: `MainWindow.cpp`, `SettingsDialog.*`.
- [ ] **Agrupamento do menu Exibir** — `QActionGroup` por região, com
  separadores e checkboxes de lock/hide. O `#include <QActionGroup>` já existe
  (`:62`) mas não é usado em lugar nenhum. Risco: baixo (cosmético).
- [ ] **Larguras iniciais das colunas** — nenhum `resizeDocks()` no código
  inteiro; as colunas herdam proportions do Qt. Definir larguras por
  workspace, junto do item de Workspaces. Risco: baixo.
- [ ] **`setTabPosition` por área** — nada define; herda o default do Qt
  (North). Esquerda/direitaBottom é o padrão do AE/Premiere para pilhas
  verticais. Risco: baixo (cosmético).
- [ ] **Densidade** — fonte menor, padding menor, números tabulares alinhados
  à direita (duração, tempo, frequência, dB). Barato, muda a percepção
  inteira do app. Risco: baixo, mas **revisar todas as caixas**: o
  `GraphEditorWidget` sozinho tem 2601 linhas e é o mais sensível.
  Arquivos: `Theme.cpp` + widgets.
- [ ] **Escuro em 3 níveis de profundidade** — `monitorBg` quase preto,
  `canvasBg` um tom acima, `timelineBg` entre os dois. Os tokens já existem
  separados; falta fixar os *valores*. Risco: baixo (só `Theme.cpp`).
- [ ] **Documentar a `Mesa` no `FEATURES.md`** — a `Mesa` é ~1200 linhas de
  canvas de composição descrito no código como "estilo After Effects
  Composition Panel", e **não aparece no FEATURES.md**.<Funcionalidade órfã:
  existe, funciona, e ninguém descobre. Risco: zero (só doc).

### Por que painéis, não paleta

Comparação com AE e Premiere (ambos usam o mesmo modelo):

| | After Effects | Premiere | Pierrot hoje |
|---|---|---|---|
| Unidade de UI | painéis dockáveis | painéis dockáveis | ✅ docks |
| Agrupamento | abas | abas | ✅ 4 pilhas |
| Layouts salvos | workspaces (Standard, Animation, Paint, Motion Tracking, Text, Minimal) | 16 workspaces | ❌ nenhum |
| Nº de painéis | ~20 | 25 | 12 |

A entrada `workspace` aparece **0 vezes** em `src/`. É o gap real.

### Sobre "parecer com After Effects" — decisão REVERTIDA em 2026-09-28

Cuidado de identidade: o README declara o Pierrot como **Vegas + Final Cut
Express**, e a epistemologia dos dois é oposta — AE é composição + nós,
Vegas é timeline + ferramentas. A decisão original era 0.7 = "escuro, denso,
profissional" mantendo identidade Vegas, não "clone do AE".

**Revertido pelo usuário após teste visual.** A UI com cor do Premiere mas
topologia do Pierrot foi avaliada como "o Pierrot com cores novas" — o que é
exatamente o resultado previsível: trocar tokens não muda a leitura da
interface. Cor é a camada mais superficial; a sensação de " Premiere" vem da
**estrutura de painéis**, não da paleta. Reclassificado: o alvo é **fidelidade
estrutural ao Premiere**, mantendo apenas a identidade Vegas onde não houver
equivalente (Mesa, Express, gravação de faixa).

Ver seção 7.1.

## 7.1 Replica estrutural do Premiere (2026-09-28)

> **Situação (atualizada nesta sessão):** a premissa do item 1 abaixo é falsa —
> `QMainWindow` **aceita** docks nos 4 lados com um widget central; o bug de
> tamanho das docks veio de o preview **virar dock** (sem `setCentralWidget`), e
> foi revertido. **Decisão:** o preview volta a ser o `setCentralWidget` (central
> host + barra de transporte no `m_centralLay`), o que destrava os passos 2–5
> sem risco de escala. A Central de Mídias já é fiel ao painel Project do
> Premiere (lista com colunas + ícones + barra de busca) e o monitor teve o
> transporte/toolbar desenhados no padrão do Program Monitor (veja 7.2).

O que separa o Pierrot do Premiere não é tema — é topologia. Cinco diferenças,
em ordem de impacto:

1. **Preview depende de `setCentralWidget`** (histórico do bug de docks). No
   Premiere o Program Monitor é um dock comum, o que permite Source ao lado
   e Inspector encostando. Com o preview central, docks funcionam nos lados;
   a réplica total do fluxo Source/Program ainda depende do passo 2.
   *(Passo 2 já entregue: Source Monitor dockado; Source/Program ainda não
   dividem a mesma pilha de abas — Program continua central.)*
2. **Source monitor.** ✅ Implementado como dock `sourceMonitorDock`
   (`SourceMonitorWidget`): abre por duplo-clique na Central de Mídias, In/Out
   com I/O, transporte básico, botões Insert/Overwrite + atalhos `,`/`.`
   (`TimelineWidget::insertSourceAtPlayhead` / `overwriteSourceAtPlayhead`).
   Menu de contexto da pool também oferece "Inserir no playhead" (caminho
   antigo do duplo clique).
3. **Inspector é janela flutuante** (`ClipPropertiesWidget`), não painel ao
   lado do preview. → ✅ virou dock `propsDock`.
4. **Timeline sem header no padrão Premiere** — não há coluna fixa com nome da
   faixa + toggle de visibilidade + controles. → ✅ refetido em 7.3.
5. **Mixer à parte**, em vez de controles inline nas faixas de áudio.
   → ⚠️ parcial (header M/S/R + VU; mixer continua dock).

### Ordem de implementação

O passo 1 destrava todos os outros; nada mais funciona direito antes dele.

| # | Passo | Risco | Reaproveita | Status |
|---|-------|-------|-------------|--------|
| 1 | Preview sai do centro e vira dock | **Alto** | `PreviewWidget` | **Revertido** — preview continua `setCentralWidget` (decisão; destrava 2–5) |
| 2 | Source monitor ao lado do Program | Médio | `PreviewWidget` + seek/playback | ✅ **Feito** — dock `Source` (`SourceMonitorWidget`), In/Out I/O, Insert `,` / Overwrite `.` |
| 3 | Inspector vira dock (sai da janela) | Baixo | `ClipPropertiesWidget` | ✅ Feito (`propsDock`) |
| 4 | Header de faixa estilo Premiere | Baixo | `TimelinePaint` + `TimelineDrag` | ✅ Feito (7.3) |
| 5 | Controles inline de áudio na timeline | Médio | `MixerWidget` | ⚠️ Parcial (M/S/R + VU no header; mixer segue dock) |

> **Risco do passo 1, anotado:** hoje o preview escala junto com a janela
> (ancorado no layout central). Ao virar dock, o cálculo de escala/posição muda
> e pode deixar o vídeo cortado, centralizado errado ou semletterbox. O vídeo
> **não** é reprocessado — é apresentação. Se quebrar, quebra a tela, não o
> projeto do usuário. Ainda assim: se um passo quebrar playback, o passo para
> e a palheta volta atrás. Nada de seguir adiante com o preview quebrado.
>
> Passos 2, 3 e 5 reaproveitam widgets existentes — não é reescrita de engine.
> O passo 4 mexe em `kHeaderW`/`kRulerH`, que são **duplicados em 3 arquivos**
> (`TimelinePaint`, `TimelineDrag`, `TimelineWidget`) e servem tanto para
> **pintar** quanto para **detectar clique**. Mudar só umdess tres desalinha o
> clique do que se vê. Mudar sempre os três juntos.

### 7.2 Program Monitor réplica (2026-09-28)

Monitor (preview central) desenhado no padrão do Program Monitor do Premiere:

- **Barra de transporte no topo**, com botões planos desenhados do tema
  (`makeMonitorIcon`): quadro anterior, play/pausa (ícone alterna no
  `onStateChanged`; o rótulo do `PlaybackEngine` é limpo), quadro seguinte e
  loop `QToolButton` checkable. `stepFrame(dir)` pausa e busca ±1 frame.
- **Cluster direito**, na ordem do Premiere: margens de segurança/grade
  (ex-`=#=`, agora `MSafe`), resolução (`m_qualityBtn`), zoom (`m_zoomCombo`),
  fullscreen expandi (`MFullscreen`; Esc sai).
- **Rótulo `Program: <nome>`** no topo esquerdo, atualizado no `setProject`
  (nome do `Project`).
- **Timecode flutuando** no canto inferior-esquerdo da área de vídeo
  (posicionado no `resizeEvent`, `WA_TransparentForMouseEvents`); o pill
  continua os tokens do tema (`refreshTimeLabelStyle`).
- **Barra de transporte inferior do MainWindow removida** — o transporte agora
  vive no monitor. `Home`/`End` (início/fim) e `Espaço` (play) seguem como
  atalhos da janela (via `addAction`), sem barra visual.
- **Paleta de ferramentas vertical e DOCÁVEL** na timeline (Tools do Premiere):
  virou um `QDockWidget` próprio (`toolsDock`, "Ferramentas") com a `QToolBar`
  em `Qt::Vertical`, estilo plano com hover/checked do tema. Encaixada por
  padrão à esquerda da timeline — `splitDockWidget(m_timelineDock,
  m_toolsDock, Qt::Horizontal)` + `resizeDocks({…},{1920, 60})` — atrás dos
  cabeçalhos de faixa, mas o usuário pode arrastar/encaixar/flutuar como no
  Premiere (entra em `m_allDocks`/`saveState`). Botões de snap/loop/ripple/
  estilo/grid/régua continuam abaixo do separador. A timeline ocupa o dock
  inteiro de novo (`tlLay->addWidget(m_timeline)`).
- **Atalhos**: Ctrl+G (grade) mantido; Alt+←/→ continuam no `nudgeSelected`
  global do MainWindow — frame-step ficou só nos botões do monitor (o
  `eventFilter` global já faz ±1 frame com ←/→).

### 7.3 Header de faixa réplica do Premiere (2026-09-28)

Passo 4 da 7.1. A coluna de cabeçalho já existia (`kHeaderW=150`), mas com
layout estilo Vegas (M/S/L embaixo à esquerda + %/barra no meio); o cabeçalho
foi **refeito do zero** no layout do Premiere CC 2018+ (tema escuro "Main"):

- **Linha superior de controles** (`y+2`, 18px, slots de x fixo via
  `headerBtnRect(y,slot)`): seta de recolher (0) + navegação de keyframes
  `◀ ◆ ▶` (1/2/3, funcional só no áudio por `kfVolume`) + sync lock (4, chip
  visual **sem backend**) + toggle de saída olho/falante (5) + botões **M/S/R**
  (6/7/8, só áudio). R (gravação de voz) é visual, sem backend.
- **Linha do nome** (`y+21`): NOME à esquerda (`headerNameRect`) e **cadeado**
  à direita (`headerLockRect`), como no Premiere — nome *abaixo* dos controles.
- **Áudio**: **VU meter vertical** na borda direita (`headerMeterRect`, da linha
  do nome até a alça de resize), exibição estática do volume+envelope **sem
  arrasto** (o `TrackOp` do header foi removido — meter é display-only). Vídeo
  não tem meter, como no Premiere.
- **Cores fiéis ao Premiere** (tema escuro): fundo monolítico `#2B2B2B`
  (seleção `#343E4E`, azul sutil), separador inferior `#151515`, nome
  ~`#D6D6D6`, chips `#333/#4C` com M/S ativos em azul `#4A6FA5` + legenda
  branca, ícones/cadeado/toggles em cinza (cadeado acende quando travado, sem
  "vermelho de lock"), **sem tira lateral colorida** (a cor de faixa vive nos
  clipes).
- **Faixa recolhida / baixa** (`collapsed` ou `contentH<22`): tira única com
  seta + nome + olho/falante + cadeado (`headerMiniNameRect`/`Toggle`/`Lock`).
- **Keyframes do header**: `◆` adiciona/remove `kfVolume` no playhead (padrão
  `upsertKeyframe`/`trackEnvelopePress`), `◀`/`▶` pulam o playhead para o
  keyframe anterior/próximo (`setPlayhead`). Vídeo desenha o cluster **apagado**
  (sem envelope de faixa de vídeo); os chips FX saem do header (o FX continua
  no badge do clipe).
- **Interações do Premiere**: duplo clique no nome renomeia **inline** (editor
  `QLineEdit` sobre a faixa — `beginTrackRename`/`commitTrackRename`); clique no
  header seleciona a faixa inteira e arrasta para reordenar (já existia); cursor
  de resize na alça (já existia).
- **Geometria centralizada** (`headerBtnRect`, `headerNameRect`, `headerLockRect`,
  `headerMini*Rect`, `headerMeterRect`) usada em comum por desenho, hit-test
  (`headerBtnAt` com slots 0..8 + cadeado 9) e editor inline — desenho e clique
  nunca desalinham.
- **Linha de controles compacta**: os 9 slots tinham o R em x=148..164 e
  vazavam para a área de clipes (`kHeaderW=150`). Reorganizados em x=2..147,
  com 3px de folga à direita; o cluster ◀◆▶ e o M/S/R seguem colados, como no
  Premiere.
- **Não-tocado**: clipes, régua, ferramentas, mixer — passo 4 é visual de
  cabeçalho + interação, não refactor de engine.

### 7.4 Corpo da faixa de áudio réplica do Premiere (2026-09-28)

Complemento da 7.3: o **cabeçalho** já estava fiel ao Premiere, mas o **corpo**
da faixa de áudio ainda tinha marcas do Vegas. Ajustado para o Premiere CC:

- **Fundo da pista chapado**: saiu o zebrado (`trackBg`/`trackBgAlt` alternado
  por índice) — toda pista de áudio usa `trackBg` e a separação vem só da linha
  divisória no rodapé. Vídeo e gravação mantêm o zebrado.
- **Clipe de áudio**: faixa clara no topo (mesma matiz do corpo, `lighter(155)`)
  com o **nome em texto escuro** no mesmo matiz, no lugar da caixa preta
  semitransparente; o sufixo `v N%` saiu do rótulo (o Premiere não mostra) e o
  badge `FX` foi mantido. Onda em tom **claro** (`waveVal 0.78`, alpha ~0.86–1.0)
  sobre o corpo sólido.
- **Linha de volume da faixa oculta por padrão**: o envelope de volume da
  faixa (e do clipe) só aparece com **Shift+V**, como no Premiere — o usuário
  vê o vídeo limpo até pedir os envelopes. O envelope perdeu o
  **preenchimento dourado** (Vegas) — agora é só curva + diamantes, linha
  branca (dourada só enquanto arrasta).
- **Envelope por Shift+V**: `drawTrackVolEnvelope` e a linha de volume da
  faixa ficam atrás de `m_showVolLines` (Shift+V). O atalho anterior `V` foi
  liberado.
- **Interação acompanha**: clicar **na linha** da faixa arma o ajuste de volume
  só quando a linha está visível (Shift+V). O "segurar na faixa inteira =
  volume" (toda a altura, estilo Vegas) **continua atrás de Shift+V** para não
  sequestrar o clique/marquee normal. Cursor `SizeVer` e tooltip dB só
  aparecem sobre a linha visível.
- **Vídeo no mesmo padrão** (para combinar com o áudio):
  - fundo da pista **chapado** (saiu o zebrado) e **mesma cor de seleção**
    (`#2A303E`) em vídeo, áudio e gravação — a separação entre faixas vem só
    da linha divisória;
  - o clipe de vídeo ganhou a **mesma faixa de nome no topo** que o de áudio,
    agora na **cor da faixa** (`tint`), com o texto escolhido por contraste
    (`readableTextOn`: claro→letra escura, escuro→letra clara). Antes era uma
    caixa preta semitransparente só no áudio;
  - a faixa some quando o clipe é baixo demais (`height < nome+8`), caindo
    para a caixa preta-antiga;
  - o **corpo** do clipe de vídeo continua cinza-azulado (`clipBg`) e o de
    áudio na cor da faixa: no Premiere vídeo é neutro e áudio é colorido.
- **Não-tocado**: cabeçalho (7.3), régua, ferramentas, mixer, modelo de dados
  — só desenho/interação do corpo das faixas.

### Escopo fechado

**Refator de layout, não de engine.** Corte, keyframe, export e paridade
preview↔export **não** são tocados — decisão mantida da conversa de escopo.
Se algo quebrar, quebra a apresentação, não os dados.

### Sobre usar modelo de IA com screenshot

Um print do Premiere não produz réplica. A diferença aqui é estrutural
(pegadas em `setCentralWidget` e janelas flutuantes), e um modelo que não rodou
o Pierrot não tem como saber onde elas estão. O que um modelo **entregou** foi
uma paleta neutra medida + acento `#2680EB` + densidade de timeline — útil,
mas é a camada mais rasa. **Restante: código.**

> Conflito com a regra do roadmap: o bloco "núcleo de foco" diz para não
> implementar feature nova antes de fechar QA/robustez/performance. Este bloco
> é **UI, não feature** — não mexe no kernel nem em paridade preview↔export,
> então não colide com a regra de ouro. Mas se o núcleo de foco estiver
> atrasado, isto espera: é o item com menor risco de causar perda de trabalho.

## Tarefas pendentes (2026-09-28)

> **Auditoria de 2026-09-29:** todos os itens desta seção foram conferidos
> contra o código. **Oito já estavam implementados** e ainda marcados como
> pendentes, e **dois diagnósticos de bug estavam errados**. ✅ = concluído,
> ⚠️ = parcial. As evidências `arquivo:linha` valem para o commit auditado.
> A auditoria cobriu `src/**` C++; `src/rust/` foi deixado de fora por não ser
> C++ — e é lá que mora a segunda implementação do DSP de áudio, o que só ficou
> visível depois.

### Bugfixes (urgent)

- [ ] **Bug dock** ⚠️ *diagnóstico corrigido*: a janela docável se destaca ao
  passar o mouse e volta à posição ao soltar. A causa **não** é
  `setAllowedAreas` nem `restoreState` — `restoreState` só roda em
  `applyWorkspace()` (`:642`) e no boot (`:810`), nunca durante um arraste. A
  causa real é `QMainWindow::AnimatedDocks` (que por definição "highlights the
  dock areas during a drag") somado a `AllowNestedDocks` (que faz o Qt devolver
  o dock ao soltar fora de uma área válida), ambos deliberadamente ativados em
  `MainWindow.cpp:960-962` para permitir docks aninhados. **É decisão de
  produto, não bug:** remover `AnimatedDocks` e/ou o `QDockWidget::title:hover`
  de `Theme.cpp:285-287` resolve, mas colide com o layout Premiere-like
  documentado em `MainWindow.cpp:955-956`.
- [ ] **Fallback de áudio duplicado sem teste de paridade (risco de divergência)**
  ⚠️ *auditado em 2026-09-29*. O DSP de efeitos existe em duas implementações
  independentes: a porta Rust `src/rust/pierrot_audiofx/src/lib.rs` e o
  `AudioFxFallback` C++ em `src/ui/PreviewWidget.cpp:311-498`. `PreviewWidget`
  escolhe via `using AudioFx = AudioFxBridge/Fallback` sob `#ifdef
  PIERROT_ENABLE_RUST`, e `CMakeLists.txt:157` deixa a opção **OFF por padrão** —
  ou seja, **o build padrão executa o C++ e o Rust nem é compilado**. Hoje as
  duas são portas fiéis (mesmos delays `[1557,1617,1491,1422]`, mesmo
  `fb = 0.60+0.28*size`, mesmo `damp = 0.4-0.25*size`, mesmos pesos de mix,
  mesmas bandas de EQ por canal), mas **nada impede que um refactor mexa numa e
  não na outra**, e a divergência apareceria só no áudio de quem compilou com
  Rust. Agravante: o fallback está enterrado dentro de um `.cpp` de 4000 linhas,
  então nenhum teste consegue alcançá-lo. Mitigação proposta: extrair
  `AudioFxFallback` para `src/ui/AudioFxFallback.h` (a classe é autocontida —
  só `std` math, nenhum tipo Qt) e adicionar `tst_audiofx_parity.cpp` que passe
  o mesmo buffer pelas duas implementações e falhe no CI se divergirem.
- [ ] **Faixa de gravação não pode ser removida**: `Project::addRecordingTrack()`
  é chamado pelo menu de faixas (`TimelineWidget.cpp:1853`), mas o
  `Project::removeRecordingTrack()` correspondente (`Project.h:743`) não tem
  **nenhum** caller. O usuário cria faixa de gravação pelo menu e não tem como
  apagá-la. O método foi preservado na limpeza de código morto de 2026-09-29
  justamente por ser feature faltando, não lixo. (Achado de 2026-09-29.)
- [ ] **Ctrl+Z undo excessivo**: desfazer às vezes volta mais de 1 ação. O
  arraste já empilha um passo por gesto (`m_dragUndoPushed`,
  `TimelineDrag.cpp:666`), então o excedente vem de outro caminho:
  `pushUndo()` (`MainWindow.cpp:2054`) empilha um snapshot inteiro do projeto a
  cada chamada, sem coalescing geral — os spinboxes de `ClipPropertiesWidget`
  emitem `editStart`/`emitEdited` por valor. Falta um macro/timer que agregue
  entradas relacionadas.
- [ ] **Selecionador de área sobre o cabeçalho** ⚠️ *diagnóstico corrigido*: o
  sintoma é real, mas o gatilho é o scroll **horizontal**, não "scrollada para
  cima" — a vertical já está correta (`rowY()` usa `kRulerH - m_viewTop`, e
  `renderScene` recorta tudo a partir de `y = kRulerH`, então `zr` bater). A
  causa é `xToTime()` (`TimelinePaint.cpp:126-128`) sem clamp para
  `x >= kHeaderW`: arrastando para a esquerda de `kHeaderW` com
  `m_viewStart > 0`, `timeToX(m_zoomT1)` volta para dentro da coluna do
  cabeçalho, e o rect do `ZoomSelect` (`TimelinePaint.cpp:878`) não tem o
  `.intersected()` que o Marquee já tem em `:888`. **Correção de uma linha.**
- [x] **Save de janelas dockáveis** ✅ persiste em disco
  (`settings.setValue("layout", saveState())`, `MainWindow.cpp:709`), com
  workspaces nomeados em `:671` e autosave de 500 ms em `:601-604`;
  `restoreSettings()` valida o blob com `saneLayoutArray()` antes de aplicar
  (`:810`). Risco residual: `kLayoutVersion` (`:114`) descarta silenciosamente
  todos os layouts salvos quando é incrementado — já aconteceu 3x, ver o
  comentário em `:101-113`.

### Timeline (alta prioridade)

- [x] **Keyframes de áudio ocultos por padrão** ✅ `m_showVolLines` começa
  `false` (`TimelineWidget.h:373`) e `Shift+V` liga/desliga
  (`TimelineWidget.cpp:1386`).
- [x] **M = marcador na timeline** ✅ `Qt::Key_M` → `toggleMarker(m_playhead)`
  em `TimelineWidget.cpp:1438`, como no Premiere. **Decidido em 2026-09-29:**
  `M` permanece como marcador; comentário (que ainda não existe) será
  `Ctrl+Alt+M`, também como no Premiere.
- [ ] **Clip resize/move responsivo e "duro"**: o arraste e resize de clipes
  está impreciso com delay visível. Causa provável: redesenho a cada
  `mouseMoveEvent` sem invalidação incremental; o cache `m_clipPix` recria o
  pixmap do clipe a cada quadro em `TimelinePaint.cpp:1000-1004`.
- [x] **Mover keyframes horizontal no editor de curvas** ✅ quanto ao
  *funcionamento*: `GraphCanvas::moveSelected()` reescreve `k.time` livremente,
  com snap ao frame (`GraphEditorWidget.cpp:800`, `snapTime` em `:798`). ⚠️ O
  que falta é a **fluência** pedida no texto original — o keyframe já é
  editável no eixo X.

### UI/Layout (média prioridade)

- [ ] **Preview responsivo**: o `PreviewWidget` tem `resizeEvent`
  (`PreviewWidget.cpp:1645`) e recalcula `m_videoRect`, mas o `PancropWidget`
  **não tem nenhum** `resizeEvent` — é essa a origem do PanCrop não se ajustar.
- [ ] **Barra de ferramentas com scroll** ⚠️ parcial: já é um `QToolBar`
  vertical com `setFixedWidth(46)` (`MainWindow.cpp:1427-1429`) e o Qt exibe um
  chevron quando o conteúdo não cabe, mas não há código de overflow próprio.
  Como a orientação é vertical, o problema prático é truncamento na **altura**,
  não na largura.
- [ ] **Tamanho mínimo do Media Pool** ⚠️ não existe `setMinimumWidth` no
  `MediaPoolWidget` nem no `FileBrowserWidget` — o único constrain é
  `m_places->setMaximumWidth(140)` (`FileBrowserWidget.cpp:262`), que limita o
  máximo da lista "places", não o mínimo do painel. A largura inicial (420) vem
  de `resizeDocks` em `MainWindow.cpp:1105`.
- [x] **Detector de rolagem automática** ✅ já reduzido: `rightEdge = 16` contra
  `leftEdge = 32` (`TimelineDrag.cpp:62-63`). Ressquício: os dois são literais
  mágicos dentro de `startAutoScroll()`, não `constexpr` nomeadas.
- [x] **Mixer de áudio estilo Premiere** ✅ *(estava fora do roadmap)*
  fader vertical por faixa, medidor VU com LED e peak hold de 1500 ms, knob de
  pan rotativo, mute/solo, conversão dB↔linear com label em dB e strip master
  (`MixerWidget.cpp:79-88`, `:90-152`, `:155-234`, `:254`, `:397-414`).
  ⚠️ Sem bus send/auxiliar, sem pan law selecionável e sem leitura numérica de
  pan.
- [ ] **Sistema de volume** ⚠️ parcial: o modelo tem `volume`/`pan`/mute/solo,
  automação (`kfVolume`/`kfPan` com resolvedores) e EQ/reverb/denoise por faixa
  (`Project.h:510-532`), e o mixer tem fader, VU, pan e mute/solo. Faltam
  normalização por faixa/master (hoje só existe `Clip::normalize`, por clipe),
  limiter/compressor/gate, leitura em dBFS (o VU consome RMS linear 0..1) e
  pan law selecionável (equal-power fixo).

### Features (média prioridade)

- [ ] **Exportação de GIF transparente**: a cadeia atual força saída **opaca** —
  o comentário em `ProjectExporter.cpp:1697-1700` registra que o `[vout]` chega
  com fundo preto composto por baixo. Falta um caminho com `colorkey`/`rgba` +
  `palettegen` com alpha.
- [ ] **PNG transparente otimizado**: não há cache nem lazy decode de PNG com
  alpha em lugar nenhum (busca por `lazy`/`pngCache`/`decodeCache` não retorna
  nada). Falta implementar.

### Roadmap v0.8

- [x] **Auto Track** ✅ criação automática de faixas ao arrastar clipes para
  áreas vazias: `findFreeTrack` (`TimelineDrag.cpp:1807-1821`) com hierarquia
  de 3 níveis — faixa sob o mouse → primeira faixa livre → **cria faixa nova**
  (`:1819`) —, aplicado a vídeo (`:1836`) e a cada stream de áudio (`:1861`).
- [ ] **Clip composto / Mesa** ⚠️ parcial: a **Mesa** existe
  (`MesaComposition`, `Project.h:215-246`) como composição 2D de camadas no
  estilo After Effects, com câmera e keyframes. Mas **não** é um nested
  sequence do Premiere: as faixas continuam visíveis e editáveis na timeline
  principal, e não existe um "clipe Mesa" que possa ser movido, cortado ou
  colocado em outra faixa. O agrupamento em sub-timeline renderizável continua
  não implementado.
- [x] **Multicâmera** ✅ (migrou de Fase 2 para cá na entrega de 2026-10-02):
  modelo `Clip::isMulticam` + `kfAngle`, UI Criar multicam + teclas 1..N,
  preview/export com expansão por ângulo. Sync por áudio e grade de ângulos
  ficam para depois.

### v0.8.1 (áudio/efeitos)

- [x] **Reverb melhorado** ✅ `SimpleReverb` = 4 comb + 2 allpass (Freeverb
  simplificado), exposto por `reverb`/`reverbMix`/`reverbSize`
  (`Project.h:521-523`). ⚠️ Ainda **sem IR**: `reverbSize` só mexe em `fb` e
  `damp`, não nos tempos de atraso, e o wet é mono.
  ⚠️ **Existe em DOIS lugares e o build padrão usa o C++**: a porta Rust em
  `src/rust/pierrot_audiofx/src/lib.rs:111`, e o fallback C++
  `AudioFxFallback` em `src/ui/PreviewWidget.cpp:311`. `PIERROT_ENABLE_RUST`
  está `OFF` por padrão (`CMakeLists.txt:157`), então quem roda é o C++.
- [ ] **EQ com gráfico editável**: existem 3 bandas
  (`eqLow`/`eqMid`/`eqHigh`, `Project.h:515-517`), mas são peaking de Q fixo em
  120/1000/6000 Hz (Q fixo), ajustáveis **só por número**. Não há
  widget de curva de resposta de frequência — o `GraphEditorWidget` edita
  keyframes de propriedades, não a curva de EQ. ⚠️ Mesma duplicação do reverb:
  `lib.rs:204-206` (Rust) e `PreviewWidget.cpp` (C++, `AudioFxFallback::configure`),
  com o C++ como o caminho realmente compilado por padrão.
- [ ] **Motion blur profissional** ⚠️ parcial: existe amostragem temporal com
  shutter em fração de quadro e 2–32 amostras (`MesaRenderer.cpp:117-140` para
  câmera, `:396-430` para camadas), mas **só na Mesa** — clipes da timeline
  normal não têm motion blur de shutter, e no export viram `boxblur` gaussiano
  (`ProjectExporter.cpp:1375-1376`). Faltam shutter angle em graus, detecção de
  velocidade e compilação/otimização das amostras.

## 4.6. Consolidação da pasta `Arquivos/` (auditoria de 2026-09-29)

> **Para quem vai pegar o projeto:** a pasta `Arquivos/` tem 7 documentos de
> análise — alguns são **relatório de estado** (leitura, não tarefa) e outros são
> **fila de trabalho**. A tabela abaixo diz o que é cada um e se o conteúdo já
> está no roadmap. Tudo foi reconferido contra o código em 2026-09-29; as
> evidências `arquivo:linha` valem para o commit auditado.
> Legenda: ✅ feito · ⚠️ parcial · ❌ não feito.

### Índice dos documentos

| Documento | Data | Tipo | Situação |
|---|---|---|---|
| `Arquivos/Atualizações/Plano-Atualizacao-Kernel.md` | 22/09 | **Fila P0–P6** | 1 ✅ parcial, 5 ❌ — consolidado abaixo |
| `Arquivos/Relatorios/Relatorio-Performance-Playback.md` | 28/09 | **Diagnóstico + 10 fixes** | 1 ✅, 9 ❌ — consolidado abaixo |
| `Arquivos/Relatorios/Relatorio-Features-Vegas-FCE.md` | 22/09 | Inventário ✓/◐/✗ + 8 itens | 1 ⚠️ melhor do que o doc, 7 ❌ — consolidado abaixo |
| `Arquivos/Relatorios/Relatorio-Audio.md` | 22/09 | Relatório ("nos conformes") | 1 ⚠️ novo (ordem de FX), resto já registrado |
| `Arquivos/Arquitetura/KERNEL.md` | 22/09 | Arquitetura + 6 gaps vs Vegas | Referência; gaps já no backlog do kernel |
| `Arquivos/Arquitetura/README.md` | 29/09 | Convenção de docs `ARQUIVOS.md` | Regra editorial, não tarefa |
| `Arquivos/Relatorios/Relatorio-Possiveis-Bugs.md` | 29/09 | 17 achados estáticos | ✅ todos tratados (seção "Tarefas pendentes") |
| `Arquivos/melhorias/LEMBRETE.md` | — | 1 item: "melhorar os mixers" | ✅ **resolvido** |

**LEMBRETE.md — resolvido.** O lembrete "Melhorar os mixers de áudio" foi
atendido pelo mixer estilo Premiere (fader vertical, VU com LED e peak hold de
1500 ms, knob de pan, mute/solo, dB↔linear, strip master —
`MixerWidget.cpp:79-88`, `:90-152`, `:155-234`, `:397-414`), já marcado em
"UI/Layout". ⚠️ Restam bus send/auxiliar, pan law selecionável e leitura numérica
de pan — podem ser anexados a esse item.

### Fila do kernel — `Plano-Atualizacao-Kernel.md` (P0–P6)

Confirma a seção 4.5 acima; aqui com o estado real. **Nada da fila foi
implementado** — o documento é de 22/09 e continua inteiro.

- [ ] **P0 — Versionamento de schema `.Blanc`** ❌ `toJson()`/`fromJson()`
  (`Project.cpp:637-698` / `:700-793`) não gravam nenhum campo de versão;
  `openProjectFile()` (`MainWindow.cpp:2249-2283`) só checa `QJsonParseError` e
  `doc.isObject()`, sem aviso de versão futura. Existe **apenas** uma migração
  pontual ad-hoc: o marcador `mesaPosAbs` (`Project.cpp:695`, aplicada em
  `:773-792`) — ou seja, o padrão "campo ausente = versão antiga" já é usado,
  mas não há leitura de versão. O arquivo não é mutado no load
  (`m_modified = false` em `MainWindow.cpp:2277`) — isso hoje é por acaso, não
  por design.
- [ ] **P1 — Indexação O(1) por id** ❌ os quatro finders são varreduras
  lineares: `findGroup` (`Project.h:673-677`), `findMesa` (`:679-688`),
  `findMesaForTrack` (`:694-698`, **O(n·m)** — o pior) e `findMedia`
  (`:765-769`). Chamados por quadro em `MesaRenderer.cpp:278,357` e
  `PlaybackEngine.cpp:404-408`. Não existe `QHash` de índice no modelo. Há um
  cache adjacente, mas só na casca e só para dimensões: `m_mediaSizes` em
  `MesaWidget.h:118-121`, cujo próprio comentário admite "evita varredura linear
  de findMedia a cada hover/hit test" — a correção foi local, não no `Project`.
- [ ] **P2 — Load `.Blanc` assíncrono e defensivo** ⚠️ metade feito. **Async
  ❌**: `openProjectFile()` (`MainWindow.cpp:2249-2264`) faz `readAll` +
  `fromJson` + `snapshotState()` (`:2270`) na thread da UI, enquanto o **save já
  é async** (`MainWindow.cpp:2326-2400`, `QtConcurrent::run` em `:2387`) — a
  assimetria é real. **Defensivo ⚠️**: sem `try`/`catch`, mas o
  `QJsonParseError` é checado (`:2257-2263`) e toda leitura usa
  `.toInt(default)`/`.toBool(false)` com guardas de tamanho
  (`Project.cpp:493`, `:747`), o que evita crash sem tratamento de exceção.
  Falta o que o plano pede: limite de recursão/tamanho (risco de DoS por JSON
  gigante) e **teste de fuzz** — não existe. O mais próximo é
  `tst_serialization.cpp:273-296` e `:343-355`, que testam campo ausente com
  JSON **válido**, nunca JSON quebrado; o caminho `QJsonParseError` vive no
  `MainWindow` e não é testável.
- [ ] **P3 — GPU na composição** ❌ `MesaRenderer` é 100% CPU:
  `QImage` + `QPainter` em `MesaRenderer.cpp:125-127` e `:162-164`, blend modes
  mapeados para `QPainter::CompositionMode_*` (`:383-390`), cache de composto
  também em `QImage`. Zero OpenCL/CUDA/QOpenGL/shader/Vulkan — as únicas
  menções a OpenCL no `src/` são comentários do SDK OFX vendorizado, e o
  `CMakeLists.txt` não linka OpenGL/OpenCL. (`warmTracks()` paraleliza o
  *decode* com QtConcurrent, mas a composição em si é serial e na CPU.)
  ⚠️ **Fora de escopo mas já existente:** VAAPI no decode
  (`FFmpegDecoder.cpp:101-124`, `:491-529`) e nvenc/vaapi no encode
  (`ProjectExporter.cpp:248-260`, `:1715-1738`).
- [ ] **P4 — Smart render (stream copy)** ❌ a cadeia sempre recodifica: `-map
  "[vout]"` (`ProjectExporter.cpp:1710`), `-c:v` explícito (`:1721`), `-c:a`
  explícito (`:1743`). Busca por `copy`/`stream copy` no arquivo só acha
  `this->copy` e copyright. O único caminho rápido é o opt-in de encoder de
  hardware (`:1718-1720`), que acelera mas **não** evita re-codificar. A
  pré-renderização existente (`:921-962`) só se aplica a sólidos/texto, nunca a
  mídia real.
- [ ] **P5 — Sanitizers + `-Wall -Wextra` no CI** ⚠️ o CI existe e é real
  (`.github/workflows/ci.yml`: push/PR em `main`, matrix Qt6+Qt5 em
  `ubuntu-24.04`, `cmake --build`, `ctest --output-on-failure`) — mas **zero
  sanitizers** e **zero `-Wall -Wextra`** no `CMakeLists.txt`
  (`CMAKE_CXX_FLAGS` vazio no `CMakeCache.txt:45`). Sem `.clang-format` nem
  `.clang-tidy`. ⚠️ Como não há `-Wall`, o CI hoje **não veria warnings** se
  houvesse — é pré-requisito de quase todo o resto desta seção.
- [ ] **P6 — Cobertura do kernel >50%** ❌ zero infraestrutura: sem
  `-DPIERROT_ENABLE_COVERAGE`, sem `--coverage`/`gcov`, sem lcov/gcovr/upload no
  workflow, sem `*.gcda` no `build/`. O `ctest` do CI já daria a base, mas sem
  instrumentação a métrica não existe.

**Inventário de testes (útil para quem pegar o projeto).** `enable_testing()` em
`CMakeLists.txt:243`; 7 alvos com `add_test` em `:258`, `:270`, `:283`, `:309`,
`:335`, `:348`, `:375` — `tst_keyframes`, `tst_serialization`, `tst_edl`,
`tst_exporter`, `tst_audio_conform`, `tst_audio_conform_intervals`,
`tst_export_pipeline`. ⚠️ **`MesaRenderer` não tem teste direto** (é linkado por
dependência de símbolo, mas `render()` nunca é exercitado) — é a maior peça do
kernel sem cobertura, o que pesa contra o P6.

### Performance de playback — `Relatorio-Performance-Playback.md` (10 fixes)

Relatório de 28/09, **anterior** às últimas mudanças. Verificado hoje: **1 dos
10 já está feito, 9 pendentes.** Nenhuma das mudanças do chroma key robusto tocou
estes caminhos.

- [x] **Fix 10 — Throttle de `scopesFrame()`** ✅ já existia **antes** do
  relatório (commit `d950f2b`, 27/08): timer dedicado de 70 ms ≈ 14,3 fps em
  `MainWindow.cpp:1074-1080`, com guarda `if (!m_scopesDock->isVisible()) return`.
  `scopesFrame()` (`PreviewWidget.cpp:1589-1598`) não é chamado em nenhum paint
  path. ⚠️ Ganho residual: o `scaled(160,90)` roda a cada disparo mesmo com o
  frame parado; um cache por `m_currentFrameIndex` daria o mesmo sem mudar a
  cadência.
- [ ] **Fix 1 — Mover `applyCrop()` + `applyBasicEffects()` para o worker** ❌
  maior impacto e maior risco. `FrameWorker::decodeOne()`
  (`PreviewWidget.cpp:93`) não recebe nenhum parâmetro de crop/efeito e o
  `struct FrameReq` (`PreviewWidget.h:224-230`) não tem campos para eles. Tudo
  roda na UI thread: `applyCrop` em `:3211` (caminho crítico), `:2936`, `:2947`,
  `:2372`, `:2832`; `applyBasicEffects` em `:3826`, `:3242`, `:1774`. Os
  parâmetros (`m_clipBrightness`, `m_clipMasks`, `m_clipChromaKey*`,
  `m_clipOfxFx`) são preenchidos na UI thread em `updateFrame()` (`:2750-2784`).
  ⚠️ **O snapshot descrito no plano ainda não existe** — é pré-requisito, não
  detalhe. E o `m_lainkaPrevFrame` (onion skin) atravessa essa fronteira, o que
  precisa ser decidido antes.
- [ ] **Fix 2 — Limitar `requestLowerLayers()` a 1 por tick** ❌ o laço
  (`PreviewWidget.cpp:3006-3032`) emite **um `requestFrame()` por camada
  visível** sem contador nem `break`; `dbgLayerDecodes` (`:2989`) existe mas só
  alimenta `qDebug` (`:3065-3070`). ⚠️ Mitigações que já amortecem: coalesce por
  clipe em `requestFrame()` (`:2969-2976`), prioridade do clipe do topo em
  `kickFrameWorker()` (`:3101-3107`) e poda de órfãos (`:3081-3095`), cache de
  camada tolerando ±3 frames (`:3026`). Com ≥4 faixas de vídeo o sintoma H3
  permanece. **Correção barata: um `break` em `:3030`.**
- [ ] **Fix 3 — Pré-alocar o buffer de compositing** ❌ `QImage acc(canvas.size(),
  ARGB32)` é alocado a cada paint (`:1859`). `m_compositedCache`
  (`PreviewWidget.h:287`) é cache de **resultado**, não de scratch: só é
  reaproveitado se `m_compositedEpoch == m_currentFrameIndex` (`:1852-1854`), o
  que nunca ocorre durante playback. ✅ **A infraestrutura já existe no mesmo
  arquivo:** `ImgPool` (`src/colombina/export/LainkaFx.h:46-83`, pool de 4
  buffers) já é usado em chroma key (`:3482`), blur (`:3578`) e motion blur
  (`:3811`, `:3822`) — só não no compositing.
- [ ] **Fix 4 — `QPainter::setOpacity()` em vez do COW manual** ❌
  `drawLayer()` (`:1695-1704`) ainda faz `QImage img2 = img;` + `QPainter
  ip(&img2)` + `fillRect`, o que **deta** o buffer compartilhado → deep copy de
  8–33 MB por camada com alpha < 1. `setOpacity` existe no arquivo, mas só em
  `:2123` e `:3818`. ⚠️ **Atenção:** a troca não é mecânica — muda o resultado
  visual quando `L.mode != CompositionMode_SourceOver`, porque os blend modes de
  faixa (`:1867`) interagem com o alpha pré-multiplicado. Precisa ser avaliado
  contra o blend mode.
- [ ] **Fix 5 — Double-buffer no `AudioMixer`** ❌ `AudioMixer` é definido
  **dentro** de `PreviewWidget.cpp:510` (não existe `AudioMixer.h`;
  `PreviewWidget.h:29` só faz forward-declare) — o que também torna a classe
  intestável. `readData()` segura `m_mutex` (`:1121`) do início (`:766`) ao fim
  (`:1090`) do chunk ≈ 10 ms a 480 samples; `updateSources()` adquire
  `m_jobMutex` + `m_mutex` (`:576-578`). Busca por `staging`/`doubleBuffer` no
  arquivo: **0 ocorrências**. O que existe é otimização de scratch, não de lock
  (`m_srcBuf`/`m_winBuf` realocados só quando a capacidade cresce, `:789-793`).
- [ ] **Fix 6 — Cache de mix sources** ❌ `updateMixAudio()` reconstrói as duas
  listas a cada tick (`:2695-2702`), chamado 1× por frame em
  `PlaybackEngine.cpp:391`. `buildMixSources()` (`:2381-2515`) itera todas as
  faixas e todos os clipes, com `findMedia()` por clipe e busca de transição em
  laço **O(clipes²)** por clipe (`:2437-2443`); `buildWarmSources()` varre tudo
  de novo (`:2522-2599`). `m_trackFx.clear()` roda a cada `updateSources`
  (`:594`). ⚠️ **Cuidado ao cachear:** `si.mediaPos` e `si.vol` variam
  continuamente (volume por keyframe/fade), então o cache precisa separar a
  **topologia** (keys, paths, streams, clipPos/Dur — invariantes) dos **valores
  por tick**.
- [ ] **Fix 7 — `desengasga` adaptativo** ❌ o intervalo é um literal fixo de
  10 s: `else if (nowMs - m_desengasgaLastMs >= 10000)` em
  `PreviewWidget.cpp:2202`, sem nenhuma referência a duração de clipe,
  `clipAt()` ou `m_playhead` no bloco (`:2196-2206`). `releaseBuffers()` no
  `FFmpegDecoder` limpa o cache de frames (`FFmpegDecoder.cpp:749`) sem noção
  de clipe. M3 (stall/degradação progressiva) permanece integral.
- [ ] **Fix 8 — Pré-criar e reusar o `QAudioSink`** ❌ `new QAudioSink(def, fmt,
  this)` dentro de `startAudio()` (`:2655`), destruído em `stopAudio()`
  (`:2670-2674`) — ou seja, a cada play, vindo de `togglePlay()`, `shuttle()` e
  `playFrom()` (`PlaybackEngine.cpp:148`, `:184`, `:219-224`). O
  `QAudioFormat` é remontado do zero a cada play (`:2616-2626`). `m_audioSink`/
  `m_audioOut` (`PreviewWidget.h:209-210`) são só ponteiros. ⚠️ O que já
  amortece: delay de 100 ms antes de `startAudio()`
  (`PlaybackEngine.cpp:124-127`), que esconde o freeze no primeiro tick mas não
  elimina o custo.
- [ ] **Fix 9 — Remover `waitReadyBeforeSink()` do path da UI** ❌ **ainda
  bloqueia a UI thread**, e para **todas** as reproduções após a primeira da
  sessão: `waitReadyBeforeSink(..., 300)` em `:2643`; o `else` só pula na 1ª vez
  (`:2646`) e `m_audioConformWarmed` nunca é resetado. `AudioConformCache::waitReady()`
  faz **loop com `sleep_for(2ms)`** até 300 ms
  (`AudioConformCache.cpp:193-216`). O próprio código admite o problema em
  `PlaybackEngine.cpp:125` ("~300ms"). ⚠️ Contra-medida do relatório (iniciar o
  sink e aceitar 1–2 quadros de silêncio) ainda não foi avaliada.
- **Uso de memória — inalterado.** `kFrameCacheMax = 120`
  (`FFmpegDecoder.h:158`) e `kBudgetBytes = 512 MB`
  (`AudioConformCache.h:138`) continuam iguais, sem limite por bytes (pior caso
  em 4K). ⚠️ **Correção ao relatório:** a tabela dele superestima o cache de
  quadros em ~4× (diz "960 MB"); o comentário do código diz ~240 MB, que é o
  número certo. **M2 (rebuild O(n) do índice do LRU) persiste** e **não estava
  na lista de 10 fixes** — `FFmpegDecoder.cpp:719-720`, `:731-732`, `:744-745`;
  vale ~6 µs/decode.

⚠️ **Ordem de esforço sugerida** (barata → cara), diferente da prioridade de
impacto do relatório: **Fix 4 e Fix 3** são as duas correções mais baratas do
arquivo (uma troca por `setOpacity` e uma por `ImgPool::get/release`, com a
infraestrutura já presente em `LainkaFx.h:46`); depois **Fix 6** (cache de
topologia), **Fix 2** (um `break`), **Fix 8**, e por último **Fix 1**. **Fix 7 e
Fix 9** se resolvem junto do Fix 1 (o `desengasga` já roda no worker; falta só
torná-lo adaptativo).

### Gaps Vegas/FCE — `Relatorio-Features-Vegas-FCE.md`, seção 4

Inventário de 22/09 com marcações ✓/◐/✗. Re conferido hoje: **7 dos 8 "pontos
para roubar" continuam não feitos, e 1 está melhor do que o documento registra.**

- [x] **Efeitos em nível de projeto/mídia** ❌ confirmado como não feito: busca
  por `projectFx`/`globalFx`/`mediaFx` em `src/` retorna **zero**. Efeitos de
  áudio e vídeo existem só em nível de **clipe** e de **faixa**
  (`AudioEffectsDialog`, `TrackAudioFxDialog`, `PreviewWidget:835`).
- [ ] **"Open Format Timeline"** ❌ não conforma o projeto à mídia. `fps` e
  resolução só são definidos **manualmente** (`ProjectSettingsDialog.cpp:119`,
  `WelcomeWindow.cpp:706-707`); nada ajusta o projeto ao primeiro clipe, e nada
  marca um clipe como "mestre" de resolução/fps.
- [ ] **Gaps restantes do fluxo Premiere** (após Source + Insert/Overwrite):
  - Three-point editing completo (marcadores de in/out na timeline + atalhos).
  - Expanded Edit / Trim no Program.
  - Multicam.
  - Nesting de sequences (a Mesa não é nested sequence).
  - Adjustment layers.
  - Master clips / efeitos em nível de mídia/projeto.
  - Lumetri simplificado (wheels + curves) além do Lift/Gamma/Gain atual.
  - Essential Sound / ducking.
  - MOGRT / captions / speech-to-text.
  - Project Manager / auto-reframe / scene detection.
  - VR/360, color management ACES, GPU de efeitos (Mercury).
  Detalhamento e status: ver resposta da auditoria Premiere (2026-10-02) e
  `Arquivos/Relatorios/Relatorio-Features-Vegas-FCE.md`.
- [ ] **Expanded Edit Mode / Trim Start–End** ❌ busca por `ExpandedEdit`,
  `TrimStart`, `TrimEnd`, `L-cut`, `JCut`, `LCut` retorna **zero**. O único
  diálogo é `TrimmerDialog`, que é in/out de **mídia**, não emenda na timeline.
  (A indicatorização de borda estilo Premiere foi implementada em 2026-09-29,
  mas é feedback visual, não Expanded Edit.)
- [ ] **Effects packages / presets de cadeia** ⚠️ **melhor do que o doc diz.**
  `saveClipPreset()`/`applyClipPreset()` (`TimelineWidget.cpp:994-1043`) já
  gravam em `QSettings` o **JSON completo do clipe**, e esse JSON inclui a cadeia
  **OFX** (`pluginId`, `enabled`, todos os `params` — `clipattrs.h:120-138`) e o
  áudio do clipe (`eqLow`/`eqMid`/`eqHigh`/`denoise`/`normalize`/`invertPhase` —
  `:112-118`). ⚠️ Falta: preset em nível de faixa e de projeto, e um formato de
  pacote em arquivo compartilhável (hoje é local e volátil).
- [ ] **Audio scrub / J-cut-L-cut / Voice Over** ❌ os três continuam
  inexistentes. (a) Sem pitch-shift no scrub — o áudio segue o playhead sem
  variação. (b) Sem conceito de J/L-cut: o export só usa `adelay`
  (`ProjectExporter.cpp:1523`) para alinhar o início do clipe, o que é
  sincronização, não transição de áudio entre clipes. (c) Zero ocorrências de
  `VoiceOver`/narração.
- [ ] **Multicam por matching de áudio + scene detection** ❌ busca por
  `silencedetect`, `scene_change`, `sceneDetect` retorna **zero**. Sem detecção
  de corte de cena e sem análise de waveform para sync entre clipes. (A ausência
  de multicam em si já está registrada no relatório e no KERNEL.md.)
- [ ] **Speech-to-text / edição baseada em texto** ❌ busca por `transcri`,
  `whisper`, `caption`, `subtitle` retorna **zero** — e **nem legendas
  existem** (o único match de "legenda" no `src/` é a legenda branca de um botão
  M/S, `TimelinePaint.cpp:1634`).
- [ ] **Scrub no valor numérico (arrastar o número)** ❌ não existe. Busca por
  `ScrubSpinBox`/`SpinBoxDrag`/`dragToScrub`: zero. Nenhum spinbox é
  customizado — em `src/ui/` só há *forward-declares* de `QSpinBox`/
  `QDoubleSpinBox` em 9 headers. Todos os campos numéricos exigem digitar.

**Correções ao relatório de Vegas/FCE:** as marcações ✓/◐/✗ continuam válidas em
geral, com **duas ressalvas** — (1) o item 4 é melhor que o registrado (o preset
já serializa a cadeia OFX inteira, não só atributos de transformação), e (2) a
seção 1.1 marcava "crossfade automático ✗" **e isso segue certo**: crossfade só
existe com sobreposição manual. Os itens ✗ de herança do FCE (three-point
editing, storyboard, nesting de sequências, batch/AAF) **continuam fora de
escopo** — são itens de produto, não dívida técnica.

### Relatório de áudio — `Relatorio-Audio.md` (4 pontos de atenção)

O relatório conclui "está nos conformes" e está **correto**: as duas pontas
concordam em volume, pan, invert, EQ, automação e velocidade. Dos 4 pontos de
atenção, **3 já estavam registrados** e **1 é novo**:

- [ ] **Ordem de invert/denoise difere entre preview e export** ❌ *(item novo —
  não estava no roadmap)* ⚠️ baixa severidade. No preview o invert é aplicado
  **antes** do gate/denoise; no export a cadeia é `afftdn` (denoise) **antes**
  do `aeval` de invert. Efeito auditivo mínimo, mas é uma assimetria real
  documentada — e num projeto que tem "paridade preview↔export é lei" como
  princípio (`KERNEL.md`), ela é dívida. **Barato:** inverter a ordem em um dos
  dois lados.
- **Áudio × envelope de velocidade (`kfSpeed`)** — ✅ já registrado na Fase 2
  (`ROADMAP.md:57,63`): vídeo usa `clipSrcTime` (integra a curva) e o áudio
  segue o `speed` base nos dois lados. Paridade preservada; o comportamento
  ideal (pitch/time variável) segue pendente.
- **Reverb/denoise/normalize são aproximações diferentes dos dois lados** — ℹ️
  informativo, não é bug nem tarefa. Carátersimilar, não bit-exato. O que
  **precisa** ficar registrado é que quem depende do loudness final deve
  validar no export (`loudnorm` real, I=−14, TP −1,5).
- **DSP duplicado Rust/C++ sem teste de paridade** — ✅ já registrado em
  "Bugfixes" (2026-09-29), com a proposta de extrair `AudioFxFallback` e criar
  `tst_audiofx_parity.cpp`.

## Critério geral (como saber que estamos no caminho)

> Pior feature é a que **perde trabalho**; segunda pior é a que **trava a UI**.
> Toda decisão no Pierrot passa por "isso aumenta ou diminui a chance de perder
> o projeto do usuário?" — se aumenta, o item vai para o fim da fila.

---

## Texto e fontes — plano After Effects (2026-10-02)

Objetivo: elevar o sistema de **texto/título** ao nível do painel **Character**
do After Effects (fontes, tracking, leading, contorno, animação de texto).

### Estado atual (fato)

| Área | Hoje |
|---|---|
| `TextStyle` | text, fontFamily, textSize, bold, fill, outline, background, x/y, align |
| UI | `TextEditorDialog` estático; sem preview ao vivo; sem animação |
| Desenho | `QPainterPath` + fill/stroke; wrap 90%; multi-linha |
| Animação | só opacity/fades/transform do **clipe** (não do texto) |
| Fontes | families do sistema (`QFontDatabase`); export resolve TTF no disco |
| Export | PNG gerado (clipe-texto) ou `drawtext` (texto anexado) |
| TextResource | cópia unificada estilo Vegas ✅ |

### O que falta vs AE (Character + Text Animator)

| Recurso AE | Status |
|---|---|
| Tracking (letter-spacing) | ❌ |
| Leading / line-height | ❌ |
| Baseline shift | ❌ |
| Italic / weight (beyond bold) | ❌ |
| Fill vs stroke separados | parcial (fill + outline simples) |
| Drop shadow | ❌ |
| Preview ao vivo no dialog | ❌ |
| **Text Animator** (typewriter, slide, fade por word) | ❌ |
| Fontes do app (embutidas) | ❌ (só sistema) |
| Estilo por caractere/palavra | ❌ |

### Fases

#### Fase 0 — Character panel (0.8)

- [ ] **Novos campos em `TextStyle`** (defaults = visual atual):
  - `tracking` (letter-spacing, unidade AE ~ em 1/1000 em)
  - `leading` (multiplicador de line-height, default 1.0)
  - `baselineShift` (fração da altura)
  - `italic` (bool)
  - `fontWeight` (100–900, mapeia QFont::Weight)
  - `dropShadow` (bool) + `dropShadowColor` + `dropShadowBlur` + `dropShadowOffsetX/Y`
- [ ] Serialização `.Blanc` + presets (`clipattrs`)
- [ ] **Preview ao vivo** no `TextEditorDialog` (label QPainter atualizando)
- [ ] **UI Character**: abas Texto / Aparência / Sombra; spin de tracking/leading/baseline
- [ ] Draw no preview/export: tracking via `QPainterPath` letter-by-letter ou `QTextLayout`
- [ ] Export: PNG do texto já aplica os novos campos (paridade)

**Aceite:** tracking/leading/italic/sombra visíveis no preview e no export;
projetos antigos sem os campos = visual atual.

#### Fase 1 — Text Animator (0.8/0.9)

- [ ] **Modelo de animator** no clipe de texto:
  ```
  TextAnimator {
    QString type; // "typewriter" | "fadeWords" | "slideUp" | "slideLeft" | "fadeChars"
    double duration;     // segundos da animação
    double delayPerUnit; // s por caractere/palavra
    int unit;            // 0=char, 1=word
    bool reverse;
  }
  ```
- [ ] `QVector<TextAnimator> textAnimators` no `Clip` (serialização)
- [ ] Preview: avaliar animator no tempo relativo do clipe (paridade com export)
- [ ] Export: rasterizar quadro-a-quadro do texto com o mesmo avaliador
  (caminho PNG já existe — reaproveitar `renderTextImage` com tempo)
- [ ] UI: painel “Animação de texto” no dialog (tipo, duração, delay, unit)
- [ ] Presets: Typewriter, Fade Words, Slide Up, Fade Chars

**Aceite:** typewriter e slide funcionam no preview e no export;
keyframes de transform do clipe continuam valendo por cima.

#### Fase 2 — Fontes e polish (0.9)

- [ ] **Fontes embutidas** (opcional): 1–2 TTF no `resources.qrc` + fallback
- [ ] Lista de fontes com **estilos por família** (QFontDatabase::styles)
- [ ] **Justify** / word-wrap configurável (largura %)
- [ ] Estilo por **palavra/seleção** (mínimo: whole-word highlight)
- [ ] Export: sempre PNG do texto (unificar com drawtext para paridade)
- [ ] Docs: FEATURES “Texto e fontes”

**Aceite:** dialog com preview; export = preview; fontes do app funcionam
sem fontconfig.

### Versionamento

| Versão | Entrega |
|---|---|
| **0.8** | Fase 0 (Character) + Fase 1 (animators) |
| **0.9** | Fase 2 (fontes embutidas, justify, polish) |

### Fora de escopo (por enquanto)

- Editar por caractere dentro do preview (só dialog)
- MOGRT / Essential Graphics
- Speech-to-text / legendas automáticas
- Renda de texto 3D / extrusão

> Referência UX: painel **Character** + **Text Animator** do After Effects.
> Identidade Vegas mantida: TextResource (cópia unificada) continua.

> **Referência:** After Effects **Classic 3D (2010–2018)** — o renderer de
> camadas 3D com luzes e malhas simples, **sem** Cinema 4D renderer, **sem**
> ray-trace, **sem** simulação. Alvo realista e suficiente para motion
> graphics e composição com profundidade.
>
> **Não** perseguir AE de hoje (C4D, ray-trace, PBR node, simulação) — muito
> robusto / outro produto.

### O que era o AE Classic 3D (2010–2018) — nosso alvo

| Recurso AE Classic | No Pierrot v1 3D |
|---|---|
| 3D layer: posição XYZ, rotação XYZ, orientation | `mesaZ` + rot XYZ + keyframes |
| 3D camera: posição, POI, zoom/FOV, orientation | `camZ/fov/pitch/yaw` + KFs |
| Lights: parallel, spot, point (lambert) | Luz simples parallel/point |
| Camada “Accepts Lights” | flag `acceptsLights` |
| Depth sort / empilhamento por Z | ordenar por Z no paint |
| Malha simples (OBJ/3D via plugin) | **glTF/OBJ estático** (sem skin) |
| Renderer software CPU | QPainter + projeção (GPU opcional depois) |
| Sem ray-trace / C4D / simulação | **fora de escopo** |

### Escopo fechado (v1 3D)

| Entra | Não entra |
|---|---|
| Z + rotação XYZ por camada | Cinema 4D renderer |
| Câmera FOV + Z + pitch/yaw + POI | Ray-tracing |
| Depth sort (Z) | Simulação (cloth/fluid) |
| Luz parallel/point (lambert) | PBR / node materials |
| `acceptsLights` por camada | Skinning / mocap |
| glTF/OBJ estático (sem anim) | GPU obrigatória na v1 |
| Export PNG sequence (atual) | Competir com AE em VFX |

### Fases (detalhadas)

#### Fase 0 — Fundação 3D (0.8)

**Objetivo:** preparar o modelo e o renderer sem mudar o visual 2D.

| Entrega | Arquivos |
|---|---|
| `Math3D.h` header-only: `Vec3`, `Mat4`, perspective, lookAt, rotateXYZ | `src/colombina/render/Math3D.h` |
| `tst_math3d` (projeção, lookAt, multiply) | `tests/tst_math3d.cpp` + CMake |
| Campos 3D no modelo (defaults = 2D atual) | `Project.h` |
| Serialização `.Blanc` + presets | `Project.cpp`, `clipattrs.h` |
| Flag `mesa3d` (default **false**) | `MesaComposition` |
| `MesaRenderer` lê flag; se false → caminho 2D idêntico | `MesaRenderer.cpp` |

**Campos a adicionar:**

```
MesaComposition:
  bool mesa3d = false;
  double camZ = 0, camFov = 50, camPitch = 0, camYaw = 0, camRoll = 0;
  double camPoiX, camPoiY, camPoiZ;   // point of interest
  // + kfCamZ, kfCamFov, kfCamPitch, kfCamYaw, kfCamPoi*

Track (quando em Mesa):
  double mesaZ = 0;
  double mesaRotX = 0, mesaRotY = 0, mesaRotZ = 0; // mesaRotZ = rotation atual
  bool mesaAcceptsLights = false;
```

**Aceite:**
- Projeto 2D existente renderiza **pixel-idêntico** (ou com tolerância 0).
- `tst_serialization` + `tst_math3d` verdes.
- Abrir projeto antigo sem campos 3D = defaults 2D.

---

#### Fase 1 — Câmera e layers Classic 3D (0.8)

**Objetivo:** parecer AE Classic — profundidade de verdade no preview.

| Entrega | Detalhe |
|---|---|
| Projeção perspectiva | quando `mesa3d=true`: view + projection (FOV, camZ) |
| Câmera POI | lookAt (poi) em vez de só pan/zoom |
| Layer transform 3D | posição Z, rotX/Y; painter aplica matriz 3D→2D |
| Depth sort | empilhar layers por Z (back-to-front) quando `mesa3d` |
| UI MesaWidget/Props | sliders Z, rotX/Y, FOV, pitch/yaw, POI |
| Graph Editor | `GPropMesaz`, `GPropCamZ`, `GPropCamFov`, `GPropCamPoi*` |
| Preview + export | mesmo `MesaRenderer` (paridade) |

**Aceite:**
- 2–3 camadas com Z diferente → parallax ao mover a câmera.
- FOV animado (zoom óptico ≠ só scale).
- Export idêntico ao preview.
- Projeto sem `mesa3d` continua 2D.

---

#### Fae 2 — Luzes Classic 3D (0.9)

**Objetivo:** luz como no AE Classic (parallel + point).

| Entrega | Detalhe |
|---|---|
| Modelo de luz | tipo (parallel/point), posição, cor, intensidade; keyframes |
| UI de luzes | painel na Mesa (add light layer) |
| `acceptsLights` | camada 2D só muda se flag true |
| Shading CPU | lambert N·L em quadros (normal ≈ 0,0,1 para 2D flat) |
| Export | mesma luz no `MesaRenderer` |

**Aceite:**
- Camada com luz: escurece/clareia conforme ângulo da luz.
- Luz animada (posição/intensidade com KFs).
- Sem luz ou flag false = look flat atual.

---

#### Fase 3 — Malhas estáticas (0.9)

**Objetivo:** importar um modelo simples na Mesa (AE Classic com OBJ/3D via plugin).

| Entrega | Detalhe |
|---|---|
| Loader glTF 2.0 estático | `colombina/mesh/GltfLoader.*` (sem skin/anim) |
| Loader OBJ estático | mesmo módulo |
| Camada Mesa `meshPath` | MediaItem tipo mesh ou campo na track |
| Raster software | triângulos + textura + Z (CPU) |
| Luz/câmera | já da fase 2 aplicam na malha |
| Export | malha no `MesaRenderer` → PNG sequence |

**Aceite:**
- glTF/OBJ simples (cubo, cadeira low-poly) aparece na Mesa.
- Câmera e luz afetam a malha.
- Export usa o mesmo raster.

**Fora:** skins, animações, materiais PBR, subdivisão.

---

#### Fase 4 — Polish 3D (1.0)

**Objetivo:** UX e docs — não nova capacidade de render.

| Entrega | Detalhe |
|---|---|
| Gizmos 3D | eixos XYZ na Mesa quando `mesa3d` |
| Pré-comp 3D | Mesa 3D como clipe na timeline |
| Docs | FEATURES “Mesa 3D” + guia rápido |
| Benchmark | comp 3D no `Bench` |
| Testes | `tst_math3d` + serialização 3D + paridade 2D |

**Aceite:**
- Usuário consegue montar cena 3D Classic sem ler código.
- Documentação e FEATURES atualizados.

---

### Resumo de esforço por fase

| Fase | Esforço | Risco | Depende de |
|---|---|---|---|
| 0 | baixo | baixo | — |
| 1 | médio | médio | Fase 0 |
| 2 | médio | médio | Fase 1 (Z) |
| 3 | médio-alto | médio | Fase 1 |
| 4 | baixo | baixo | Fases 1–3 |

### Ordem de ataque

```
0 → 1 → 2 → 3 → 4
```

Fase 0 e 1 podem entrar juntas na **0.8**. Luz + glTF na **0.9**. Polish na **1.0**.

### Por que parar por aqui

- AE 2010–2018 era **Classic 3D em CPU** — mesmo espírito do Pierrot (QPainter).
- Timeline Vegas + Mesa 2D já resolvem a maioria do público.
- Classic 3D dá **parallax, câmera cinematográfica e luz** — o essencial.
- C4D/ray-trace/PBR: **fora** — identidade e manutenção não pagam.
- Se um dia precisar de AE “de verdade”: **OFX** ou **Blender → clipe**.

### Versionamento

| Versão | Entrega |
|---|---|
| **0.8** | Fase 0 + 1 (Z/FOV/câmera Classic 3D) |
| **0.9** | Fase 2 + 3 (luz + glTF estático) |
| **1.0** | Polish + docs |

> 3D entra como **evolução da Mesa**, não como app separado. Timeline Vegas
> **não muda**.

---

## Cor — status vs Lumetri (Premiere) · 2026-10-02

- [x] **LGG clássico** ✅ lift/gamma/gain por canal (preview + export + preset).
- [x] **Lumetri Basic** ✅ exposure, contrast, highlights/shadows, whites/blacks,
  saturation, vibrance, temperature/tint, faded film, sharpen
  (`Clip::cg*` + `colorgrade::applyToImage` em `src/colombina/fx/ColorGrade.h`).
- [x] **Curvas RGB** ✅ master + R/G/B (pontos 0..1) no preview e `curves=` no export.
- [x] **Vinheta nativa** ✅ `cgVignette`/`cgVignetteFeather` (preview + `vignette=`).
- [x] **LUT 3D `.cube`** ✅ parser + interpolação trilinear no preview; `lut3d=` no export.
- [x] **Mix/blend do grade** ✅ `cgBlend` 0..1.
- [x] **UI Lumetri** ✅ abas Básico / Rodas / Curvas / Vinheta-LUT
  (`TimelineWidget::showGradingDialog`).
- [x] **Scopes** ✅ waveform (luma), histograma RGB, vectorscope (dock Analisadores).
- [ ] **RGB parade** no waveform (só luma hoje).
- [ ] **HSL Secondary** (key por hue/sat/lum + correção secundária).
- [ ] **Color match** entre clipes.
- [ ] **Keyframes de grade** (exposição/curvas animadas).
- [ ] **Curvas desenháveis** (canvas com arraste; hoje é lista de pontos X/Y).
- [ ] **Scopes embutidos no diálogo** de cor.

> “100% cor vs Premiere” no sentido de uso diário = Basic + Rodas + Curves +
> Vinheta + LUT. HSL Secondary, Color Match e keyframes são o restante
> profissional — registrados acima.

---

## UI — status vs Premiere (painéis/layout) · 2026-10-02

- [x] **Docks + workspaces nomeados** ✅ (saveState por workspace).
- [x] **Header de faixa estilo Premiere** ✅ (7.3).
- [x] **Program Monitor** ✅ transporte no monitor, rótulo Program, mini-waveform.
- [x] **Source Monitor dock** ✅ In/Out + Insert/Overwrite.
- [x] **Inspector dock** ✅ Propriedades ao lado do Program.
- [x] **Tools dockável** ✅ paleta vertical na timeline.
- [x] **Menu Exibir agrupado por região** ✅ esquerda/direita/embaixo.
- [x] **Presets de workspace** ✅ Edição / Áudio / Composição / Efeitos.
- [x] **Densidade** ✅ paddings mais justos no QSS (dock title, botões).
- [x] **Escuro em 3 níveis** ✅ `canvasBg` < `timelineBg` < painéis/monitor.
- [x] **Mesa documentada no FEATURES** ✅.
- [ ] **Source/Program no mesmo central** (Program segue central; Source é dock).
- [ ] **Registry `PanelDef`** (manutenção; menu já agrupado sem a tabela).
- [ ] **Curvas desenháveis / scopes no diálogo de cor** (janela de cor, não layout).

> “100% UI” no sentido de painéis/layout Premiere: workspaces, agrupamento,
> densidade, escuro, header, Source/Inspector/Tools. Resta polish de janela de
> cor e o dual-monitor hard (Program vira dock) — risco alto, já revertido.

---

## Efeitos nativos — inventário (2026-10-02)

Detalhamento completo:
[`Arquivos/Relatorios/Relatorio-Efeitos-Nativos.md`](Arquivos/Relatorios/Relatorio-Efeitos-Nativos.md).

- [x] **Nativos que já funcionam** ✅ brilho/contraste/sat, blur, grayscale,
  chroma key, masks, LGG + Lumetri (`ColorGrade.h`), LAINKA (`LainkaFx.h`),
  motion blur, EQ/Reverb/Denoise (C++/Rust).
- [x] **Plugins** ✅ host frei0r + host OFX.
- [ ] **Extrair `NativeFx.h`** — chroma/blur/eq/grayscale/masks no kernel
  (paridade preview↔export por construção; hoje inline em `PreviewWidget.cpp`).
- [ ] **`tst_audiofx_parity`** — Rust vs fallback C++ no mesmo buffer.
- [ ] **Efeitos de look** (Posterize, Find Edges, Vertical Hold, Tint, Replace
  Color) — via nativos novos ou catálogo frei0r curado.
- [ ] **Warp Stabilizer / compressor por clipe** — P2 (caros).

> Anotado para fazer depois — não entra no fechamento da 0.7.

---

## Editor de Velocidade (dock) · 2026-10-02

- [x] **UI de envelope de velocidade** ✅ dock `velocityDock` (aba do Editor de
  Curvas): canvas velocidade×tempo, keyframes arrastáveis, base 0,1–4×, add/del/reset.
  `src/ui/VelocityEditorWidget.*`. Menu do clipe ▸ “Editor de velocidade”.
- [x] **Banda de velocidade na timeline** ✅ `drawSpeedEnvelope` no clipe
  (curva azul, 1×, losangos, Rápido/Baixo); arraste no corpo = speed;
  duplo clique = keyframe (`TimelineDrag` `ClipSpeed`).
- [x] **Modelo/export já prontos** ✅ `kfSpeed` + `clipSrcTime` + `renderVelocitySequence`.
- [ ] **Diamantes de kfSpeed na lista de keyframes do GraphEditor** (GPropSpeed).
- [ ] **Áudio com envelope** (preview/export de áudio ainda usam `speed` base).
- [ ] **LAINKA/OFX com envelope** (usam `speed` base).
