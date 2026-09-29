# Relatório: Performance de Reprodução de Vídeo

**Data:** 2026-09-28  
**Status:** Análise completa  
**Arquivos analisados:** PlaybackEngine.cpp/h, PreviewWidget.cpp/h, FFmpegDecoder.cpp, AudioMixer

---

## 1. Pipeline completo: tick() → paintEvent()

```
QTimer (8ms, Qt::PreciseTimer)
  └─ PlaybackEngine::tick()                          [UI thread]
       ├─ Relógio de áudio + drift correction
       ├─ Frame-skip detection (m_droppedFrames)
       ├─ Loop boundary check
       ├─ applySeekInternal(t) → onSeek(t)
       │    └─ PreviewWidget::applySeekVisual(t)     [UI thread]
       │         ├─ desengasga() a cada ~10s → worker thread
       │         ├─ updateFrame()                     [UI thread]
       │         │    ├─ clipAt(m_playhead)           [UI thread, O(tracks*clips)]
       │         │    ├─ tryRenderMesa() se Mesa track
       │         │    ├─ Check prefetch cache hit
       │         │    ├─ Check same-frame cache
       │         │    ├─ requestLowerLayers(decW)     [UI thread]
       │         │    └─ requestFrame() → kickFrameWorker()
       │         │              └─ QMetaObject::invokeMethod("decodeOne") → worker thread
       │         └─ update() → paintEvent()           [UI thread, deferred]
       ├─ onPrefetch() → updatePrefetch()             [UI thread]
       │    └─ QMetaObject::invokeMethod("decode") → BgPrefetchWorker thread
       └─ onMixAudio(t) → updateMixAudio()            [UI thread]
            ├─ buildMixSources()                       [UI thread, itera todos os clipes]
            ├─ buildWarmSources()                      [UI thread, itera todos os clipes]
            └─ AudioMixer::updateSources()             [UI thread, adquire m_mutex]

Worker thread (FrameWorker::decodeOne):
  ├─ Check m_ready (1-frame-ahead hit) → instantâneo
  ├─ Check m_pReady (prefetch-swap hit) → instantâneo
  └─ FFmpegDecoder::frameAt()
       ├─ LRU cache lookup (O(n) index rebuild)
       ├─ Seek se necessário (av_seek_frame + flush)
       ├─ Decode loop (av_read_frame + avcodec_send/receive)
       ├─ sws_scale (conversão de formato + resize)
       └─ Store in LRU cache
  └─ emit frameReady → UI thread (QueuedConnection)
  └─ Pre-decode next frame into m_ready

UI thread: onFrameReady()
  ├─ Adaptive quality monitor
  ├─ Store in m_frameFull
  ├─ applyCrop()                                      [UI thread]
  │    ├─ QImage::copy() (DEEP COPY de frame inteiro)
  │    ├─ LAINKA effects (per-pixel)
  │    ├─ Motion blur (QPainter compositing)
  │    ├─ applyBasicEffects()                          [UI thread]
  │    │    ├─ applyMasks() (float vector w*h, per-pixel)
  │    │    ├─ Chroma key (aloca QImage novo, per-pixel)
  │    │    ├─ Box blur (aloca QImage temporário, 2-pass)
  │    │    ├─ Grayscale (per-pixel)
  │    │    └─ Brightness/contrast/saturation (per-pixel)
  │    └─ OFX effects (OfxRenderer::applyOfxEffects)
  └─ update() → paintEvent()

paintEvent():
  ├─ Multi-layer compositing (se >= 2 layers)
  │    ├─ QImage acc(canvas.size(), ARGB32) [alocação grande]
  │    ├─ Para cada layer: drawLayer() com QPainter
  │    │    └─ Se alpha < 1.0: COW → deep copy forçado
  │    ├─ Text rendering (QPainterPath)
  │    └─ Cache em m_compositedCache
  └─ Single-layer: drawLayer() diretamente no QPainter
```

---

## 2. Fontes de latência

### CRÍTICO

| # | Problema | Local | Impacto |
|---|----------|-------|---------|
| C1 | `waitReadyBeforeSink()` bloqueia UI thread até 300ms | PreviewWidget.cpp:2674 | Freeze no play (após 1ª vez) |
| C2 | `applyCrop()` faz deep copy de frame inteiro (8MB 1080p / 33MB 4K) | PreviewWidget.cpp:3411 | 1-15ms/frame |
| C3 | Efeitos (masks+chroma+blur+color) itera pixels na UI thread | PreviewWidget.cpp:3497 | 5-80ms/frame |
| C4 | `QAudioSink` criado na UI thread a cada play | PreviewWidget.cpp:2680 | 10-50ms |

### ALTO

| # | Problema | Local | Impacto |
|---|----------|-------|---------|
| H1 | Compositing aloca QImage cheia a cada paint (8-33MB) | PreviewWidget.cpp:1890 | 2-5ms/frame |
| H2 | `drawLayer()` deep copy via COW para alpha < 1.0 | PreviewWidget.cpp:1727 | 1-3ms/layer |
| H3 | Camadas inferiores roubam slots do worker (1 thread) | PreviewWidget.cpp:3012 | Trava frame do topo |
| H4 | `buildMixSources()` + `buildWarmSources()` itera todos clipes a cada tick | PreviewWidget.cpp:2412 | 0.5-2ms/tick |
| H5 | `AudioMixer::readData()` segura mutex por 10ms (480 samples) | PreviewWidget.cpp:797 | Bloqueia UI |
| H6 | Efeitos de camadas inferiores aplicados na UI thread | PreviewWidget.cpp:3245 | Multiplica C2+C3 |

### MÉDIO

| # | Problema | Local | Impacto |
|---|----------|-------|---------|
| M1 | QTimer 8ms = 3-4 fires por frame a 30fps | PreviewWidget.cpp:1527 | 0.5-2ms overhead |
| M2 | LRU cache index rebuild O(120) a cada decode | FFmpegDecoder.cpp:718 | ~6us/decode |
| M3 | `releaseBuffers()` a cada 10s causa cold re-seek (50-200ms) | PreviewWidget.cpp:2235 | 1-2 frames stall |
| M4 | `drawClipText()` recria QPainterPath a cada paint | PreviewWidget.cpp:2072 | 0.5-2ms/text |
| M5 | `scopesFrame()` copia+escala frame a cada chamada | PreviewWidget.cpp:1620 | 1-3ms |

### BAIXO

| # | Problema | Local | Impacto |
|---|----------|-------|---------|
| L1 | Seek síncrono durante playback | PlaybackEngine.cpp:56 | 1-5ms/scrub |
| L2 | Coalescing de requests não substitui por tempo | PreviewWidget.cpp:2998 | 2-3 frames intermediários |
| L3 | COW cross-thread copy no frame delivery | FFmpegDecoder.cpp:1067 | 1-10ms |

---

## 3. Sintomas × Causas

| Sintoma | Causas primárias | Severidade |
|---------|-----------------|------------|
| Freeze no play (1ª vez) | C1 (waitReady 300ms) + C4 (sink creation) | CRÍTICO |
| Video trava mas áudio continua | H3 (lower layers roubam worker) + H6 (efeitos na UI) | ALTO |
| Stall nos pontos de corte | M3 (desengasga cold re-seek) + H3 (novo clipe compete) | MÉDIO |
| Degradação progressiva | M3 (flush do codec a cada 10s) | MÉDIO |
| Multi-track trava | H3 + H6 + H1 (alocação de compositing) | ALTO |
| Scrub com lag | H5 (mutex do AudioMixer) + M1 (timer overhead) | MÉDIO |
| 4K impossível | C2 (33MB copy) + C3 (efeitos em 4K) + L3 (COW cross-thread) | CRÍTICO |

---

## 4. Race conditions e deadlocks

### RC1: Sem deadlock em `m_frameMutex`
Todas as aquisições são na UI thread (via `QueuedConnection`). O worker thread nunca adquire `m_frameMutex` diretamente.

### RC2: Flags `m_workerBusy` / `m_bgPrefetchBusy`
Setadas na UI thread, resetadas quando o sinal do worker chega (também UI thread via `QueuedConnection`). Se o worker crashar, as flags ficam `true` permanentemente → stalling. O destructor limpa, mas falha mid-session não é tratada.

### RC3: Transferência de decoder BgPrefetchWorker → FrameWorker
Segura: `BgPrefetchWorker::decode()` termina antes de emitir `done()` → `done()` é `QueuedConnection` → `warmUpPrefetchDecoder()` também é `QueuedConnection`. Sem race.

### RC4: AudioMixer mutex contention
`readData()` (thread de áudio) e `updateSources()` (UI thread) compartilham `m_mutex`. Sem deadlock, mas `readData()` longo bloqueia `updateSources()`.

---

## 5. Uso de memória

| Buffer | Tamanho | Risco |
|--------|---------|-------|
| `m_frameCacheLru` (FFmpegDecoder) | Até 120 entradas × 8MB (1080p) = 960MB | Alto em 4K (33MB × 120 = 4GB) |
| `m_layerCache` (PreviewWidget) | 1 QImage por layer ativo × 8MB | Médio (10+ tracks = 80MB+) |
| `m_compositedCache` | 1 QImage × 8-33MB | Baixo |
| `AudioConformCache` | 512MB budget | Alto com muitos áudios |
| `drawLayer()` transient QImage | 8-33MB por layer por paint | GC pressure (180 allocs/s a 60fps) |

---

## 6. Fixes recomendados (por prioridade)

| Prioridade | Fix | Esforço | Impacto |
|------------|-----|---------|---------|
| 1 | Mover `applyCrop()` + `applyBasicEffects()` para o worker thread | Alto | CRÍTICO — elimina C2, C3, H6 |
| 2 | Limitar requests de camadas inferiores a 1 por tick | Baixo | ALTO — prioriza frame do topo |
| 3 | Pré-alocar buffer de compositing (reutilizar entre frames) | Baixo | ALTO — elimina H1 |
| 4 | Usar `QPainter::setOpacity()` em vez de COW manual | Baixo | ALTO — elimina H2 |
| 5 | Double-buffer no AudioMixer (staging + swap atômico) | Médio | ALTO — elimina H5 |
| 6 | Cache de mix sources (reconstruir só na troca de clipe) | Baixo | MÉDIO — elimina H4 |
| 7 | Tornar `desengasga` adaptativo (intervalo > 10s para clipes curtos) | Baixo | MÉDIO — reduz M3 |
| 8 | Pré-criar `QAudioSink` e reutilizar entre play/stop | Baixo | CRÍTICO — elimina C4 |
| 9 | Remover `waitReadyBeforeSink()` do path da UI thread | Baixo | CRÍTICO — elimina C1 |
| 10 | Throttle `scopesFrame()` a 10-15fps | Baixo | MÉDIO — reduz M5 |

---

## 7. Notas de implementação

### Fix 1: Mover efeitos para o worker (maior impacto)

A mudança principal é mover `applyCrop()` + `applyBasicEffects()` para dentro de `FrameWorker::decodeOne()`, antes de emitir `frameReady`. O worker já tem o frame decodificado; adicionar crop + efeitos ali é natural.

**Arquivos afetados:**
- `PreviewWidget.cpp`: `decodeOne()` recebe parâmetros de crop/effects do clipe ativo
- `PreviewWidget.cpp`: `onFrameReady()` recebe frame já processado (sem `applyCrop`)
- `PreviewWidget.h`: novos campos no `FrameReq` ou parâmetros de `decodeOne()`

**Risco:** O clipe ativo pode mudar enquanto o worker decoda. Mitigação: snapshot dos parâmetros no momento do request.

### Fix 9: Remover waitReadyBeforeSink

O `waitReadyBeforeSink()` existe para evitar áudio mudo nos primeiros 300ms. Alternativa: iniciar o sink imediatamente e aceitar silêncio momentâneo (1-2 frames). O áudio conform já existe no cache; a demora é só o primeiro chunk.

**Arquivo afetado:** `PreviewWidget.cpp:2669-2678` — remover o `if (m_audioConformWarmed)` e chamar `waitReadyBeforeSink()` de forma assíncrona ou não chamar.
