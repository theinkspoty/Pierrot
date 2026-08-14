# Bug: travamento da preview no corte de clipes

## Diagnóstico

Quando o playhead cruza um corte na timeline, o `clipAt()` retorna um clip diferente. O frame cacheado (`m_shownPath/m_shownT/m_shownW`) deixa de ser válido, e o `FrameWorker` precisa fazer **seek + decodificação** do zero para o novo clip. Se essa decodificação demorar mais que o intervalo do timer (≈33ms em 30fps), o preview fica com o frame anterior "travado" ou mostra branco até o frame chegar.

O mesmo acontece com o áudio: `updateMixAudio()` troca os decoders de áudio no momento do corte, causando pausas.

## Causa raiz

- `PreviewWidget::tick()` chama `seek(t)` a cada frame do timer.
- `seek()` → `updateFrame()` → `requestFrame()` → `decodeOne()` (no `FrameWorker`).
- No `FrameWorker::decodeOne()`, se o arquivo for diferente do que já está aberto, o decoder é **fechado e reaberto** (`m_decoder.open(path)`), o que implica:
  - `avformat_open_input`
  - `avformat_find_stream_info`
  - `avcodec_open2`
  - seek para o frame correto
  - decodificação frame-a-frame até o timestamp alvo

Isso tudo leva dezenas de milissegundos. Em cortes, o clip muda de arquivo, então o seek é inevitável — mas podemos **antecipar** o trabalho.

## Solução: pré-carregamento especulativo (prefetch)

### Conceito

Quando o clip atual está perto do fim (ex: < 0.5s restantes), decodificar **antecipadamente** o primeiro frame do próximo clip visível na track de vídeo. O frame fica pronto em cache e, no momento do corte, é exibido instantaneamente.

### Passos de implementação

#### 1. Adicionar membros de prefetch no `PreviewWidget` (`PreviewWidget.h`)

```cpp
struct PrefetchFrame {
    QString path;
    double t = 0.0;
    int maxW = 0;
    QImage img;
    bool valid = false;
};
PrefetchFrame m_prefetch;
```

#### 2. Adicionar slot de prefetch no `FrameWorker` (`PreviewWidget.cpp:57-74`)

```cpp
void decodePrefetch(const QString& path, double t, int maxW) {
    if (!m_decoder.isOpen() || m_decoder.source() != path)
        if (!m_decoder.open(path)) {
            emit prefetchReady(path, t, maxW, QImage());
            return;
        }
    emit prefetchReady(path, t, maxW, m_decoder.frameAt(t, maxW));
}
```

Conectar o signal `prefetchReady` no `PreviewWidget::onPrefetchReady()` (novo slot privado).

#### 3. Implementar `PrefetchFrame` no `PreviewWidget`

```cpp
void PreviewWidget::onPrefetchReady(const QString& path, double t, int maxW, const QImage& img) {
    QMutexLocker l(&m_frameMutex);
    if (m_prefetch.valid && m_prefetch.path == path
        && std::fabs(m_prefetch.t - t) < 1e-6 && m_prefetch.maxW == maxW) {
        m_prefetch.img = img;
        m_prefetch.valid = !img.isNull();
    }
}

void PreviewWidget::updatePrefetch() {
    if (!m_project || !m_frameWorker) return;
    const Clip* clip = clipAt(m_playhead);
    if (!clip) { m_prefetch.valid = false; return; }
    const double remain = (clip->pos + clip->dur) - m_playhead;
    if (remain > 0.5 || remain <= 0.0) { m_prefetch.valid = false; return; }
    // Procura próximo clip na mesma track de vídeo.
    for (const Track& tr : m_project->videoTracks) {
        for (const Clip& c : tr.clips) {
            if (c.pos > clip->pos + 1e-6 && c.mediaId != clip->mediaId) {
                const MediaItem* m = m_project->findMedia(c.mediaId);
                if (!m || !m->hasVideo) continue;
                const double srcT = c.in + 0.05; // 50ms dentro do clip
                const int decW = qMax(320, qMin(maxDecodeWidth(), m_videoRect.width() > 0
                                                ? m_videoRect.width() : 960));
                m_prefetch = {m->filePath, srcT, decW, QImage(), true};
                m_prefetch.valid = false; // ainda não decodificado
                QMetaObject::invokeMethod(m_frameWorker, "decodePrefetch",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, m->filePath),
                                          Q_ARG(double, srcT),
                                          Q_ARG(int, decW));
                return;
            }
        }
    }
    m_prefetch.valid = false;
}
```

#### 4. Chamar `updatePrefetch()` no `tick()` (`PreviewWidget.cpp:569`)

```cpp
void PreviewWidget::tick() {
    // ... código existente ...
    seek(t);
    updateMixAudio(t, false);
    updatePrefetch(); // <<< adicionar aqui
}
```

#### 5. Usar o prefetch no `updateFrame()` (`PreviewWidget.cpp:753`)

```cpp
void PreviewWidget::updateFrame() {
    // ... código existente ...
    {
        QMutexLocker l(&m_frameMutex);
        if (m_shownPath == m->filePath && std::fabs(m_shownT - srcT) < 1e-6
            && m_shownW == decW) {
            applyCrop(); update(); return;
        }
        // Se o prefetch bate com o frame que precisamos agora, usa direto.
        if (m_prefetch.valid && m_prefetch.path == m->filePath
            && std::fabs(m_prefetch.t - srcT) < 1e-6 && m_prefetch.maxW == decW) {
            m_shownPath = m->filePath;
            m_shownT = srcT;
            m_shownW = decW;
            m_frame = m_prefetch.img;
            applyCrop(); update();
            m_prefetch.valid = false;
            return;
        }
    }
    requestFrame(m->filePath, srcT, decW);
}
```

#### 6. (Opcional) Prefetch de áudio

No `AudioMixer::updateSources()`, se um source novo vai entrar em menos de 0.3s, abrir o decoder de áudio antecipadamente e fazer um `seekAudio()` prévio. Isso evita o "silêncio" no corte de áudio.

## Por que isso funciona

- O prefetch roda em paralelo (thread do `FrameWorker`) enquanto o clip atual ainda está tocando.
- No momento do corte, se o prefetch foi concluído a tempo, o frame é exibido **sem seek nem decodificação**.
- Se o prefetch não chegar a tempo (clip muito complexo), o comportamento cai para o atual (seek + decode), sem piora.

## Teste

1. Importar um vídeo longo + um clipe curto (ex: 2s) no início da timeline.
2. Play, esperar o playhead cruzar o corte para o clipe curto.
3. A preview deve continuar fluida, sem travar nem mostrar branco.
4. Repetir com 3+ cortes consecutivos.

## Arquivos modificados

- `src/ui/PreviewWidget.h` — adicionar `struct PrefetchFrame` e `void updatePrefetch()`
- `src/ui/PreviewWidget.cpp` — `FrameWorker::decodePrefetch()`, `onPrefetchReady()`, `updatePrefetch()`, usar prefetch em `updateFrame()`
