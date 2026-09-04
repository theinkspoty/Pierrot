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
