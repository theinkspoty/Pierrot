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
(`src/colombina`). Ordem sugerida pela análise do KERNEL.md:

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
2. **Não há Source monitor.** O Premiere tem Source/Program lado a lado; o
   Pierrot só oferece preview em janela separada (2º monitor).
3. **Inspector é janela flutuante** (`ClipPropertiesWidget`), não painel ao
   lado do preview.
4. **Timeline sem header no padrão Premiere** — não há coluna fixa com nome da
   faixa + toggle de visibilidade + controles.
5. **Mixer à parte**, em vez de controles inline nas faixas de áudio.

### Ordem de implementação

O passo 1 destrava todos os outros; nada mais funciona direito antes dele.

| # | Passo | Risco | Reaproveita |
|---|-------|-------|-------------|
| 1 | Preview sai do centro e vira dock | **Alto** | `PreviewWidget` |
| 2 | Source monitor ao lado do Program | Médio | `PreviewWidget` + seek/playback |
| 3 | Inspector vira dock (sai da janela) | Baixo | `ClipPropertiesWidget` |
| 4 | Header de faixa estilo Premiere | Baixo | `TimelinePaint` + `TimelineDrag` |
| 5 | Controles inline de áudio na timeline | Médio | `MixerWidget` |

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

### v0.8.1 (áudio/efeitos)

- [x] **Reverb melhorado** ✅ `SimpleReverb`
  (`src/rust/pierrot_audiofx/src/lib.rs:111`) = 4 comb + 2 allpass (Freeverb
  simplificado), exposto por `reverb`/`reverbMix`/`reverbSize`
  (`Project.h:521-523`). ⚠️ Ainda **sem IR**: `reverbSize` só mexe em `fb` e
  `damp`, não nos tempos de atraso, e o wet é mono.
- [ ] **EQ com gráfico editável**: existem 3 bandas
  (`eqLow`/`eqMid`/`eqHigh`, `Project.h:515-517`), mas são peaking de Q fixo em
  120/1000/6000 Hz (`lib.rs:204-206`), ajustáveis **só por número**. Não há
  widget de curva de resposta de frequência — o `GraphEditorWidget` edita
  keyframes de propriedades, não a curva de EQ.
- [ ] **Motion blur profissional** ⚠️ parcial: existe amostragem temporal com
  shutter em fração de quadro e 2–32 amostras (`MesaRenderer.cpp:117-140` para
  câmera, `:396-430` para camadas), mas **só na Mesa** — clipes da timeline
  normal não têm motion blur de shutter, e no export viram `boxblur` gaussiano
  (`ProjectExporter.cpp:1375-1376`). Faltam shutter angle em graus, detecção de
  velocidade e compilação/otimização das amostras.

## Critério geral (como saber que estamos no caminho)

> Pior feature é a que **perde trabalho**; segunda pior é a que **trava a UI**.
> Toda decisão no Pierrot passa por "isso aumenta ou diminui a chance de perder
> o projeto do usuário?" — se aumenta, o item vai para o fim da fila.
