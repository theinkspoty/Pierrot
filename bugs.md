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

---

# Bug: `pasteClips()` não mantém ordem dos clips por posição

**Arquivo:** `src/ui/TimelineWidget.cpp:1353`  
**Severidade:** Alta

## Descrição

Ao colar clips com `pasteClips()`, os novos `Clip` são inseridos com `tr->clips.push_back(nc)`, sem ordenação por `pos`. A renderização (`renderScene`) e a detecção de sobreposição (`clampPosToTrack`, `clipAt`) assumem que os clips estão ordenados por posição.

Clips não ordenados causam:
- Ordem de renderização errada (clipes posteriores podem cobrir os anteriores incorretamente)
- Detecção de sobreposição falha em `clampPosToTrack` (só verifica clips após o atual no vetor)
- `finishDrop` insere corretamente ordenado (linha 2601-2603), mas `pasteClips` não

## Fix

Substituir `push_back` por inserção ordenada:

```cpp
auto it = tr->clips.begin();
while (it != tr->clips.end() && it->pos <= nc.pos) ++it;
tr->clips.insert(it, nc);
```

---

# Bug: `Project::fromJson()` aceita `fps=0` causando divisão por zero em cascata

**Arquivo:** `src/models/Project.cpp:271`  
**Severidade:** Alta

## Descrição

`fromJson()` lê `fps = o["fps"].toInt(30)`. Se um JSON corrompido ou editado manualmente contiver `"fps": 0`, o valor propaga para:
- `TimelineWidget::snapTime()` → `std::round(t * fps) / fps` → divisão por zero → NaN ou crash
- `PreviewWidget::tick()` → `1000.0 / fps` → infinito ou crash
- `TimelineWidget::nudgeSelected()` → `dir * frames / m_project->fps` → divisão por zero

## Fix

Validar e clamp fps após deserialização:

```cpp
fps = qMax(1, o["fps"].toInt(30));
```

---

# Bug: TrimLeft pode produzir `in` negativo

**Arquivo:** `src/ui/PreviewWidget.cpp:1913`  
**Severidade:** Média

## Descrição

Durante o arraste de trim à esquerda, `sc->in = o.in + delta` é calculado sem clamp para `>= 0.0`. Se o usuário arrastar a alça de trim para a direita (delta grande positivo) ou houver edge case com modificador, `in` pode ficar negativo, referenciando mídia antes do início do arquivo.

## Fix

```cpp
sc->in = std::max(0.0, o.in + delta);
```

---

# Bug: `escText()` não escapa caracteres especiais para ffmpeg drawtext

**Arquivo:** `src/export/ProjectExporter.cpp:161-167`  
**Severidade:** Média

## Descrição

`escText()` escapa apenas `\`, `:`, e `'`. Faltam escapes para:
- `%` — ffmpeg interpreta `%{...}` como expressão em drawtext
- `[` e `]` — sintaxe de labels de filtros do ffmpeg
- `;` — separador de filtros no filter_complex

Textos com esses caracteres causam falhas na exportação ou crashes no ffmpeg.

## Fix

```cpp
QString escText(const QString& t) {
    QString s = t;
    s.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    s.replace(QLatin1Char(':'), QLatin1String("\\:"));
    s.replace(QLatin1Char('\''), QLatin1String("\\'"));
    s.replace(QLatin1Char('%'), QLatin1String("\\%"));
    s.replace(QLatin1Char('['), QLatin1String("\\["));
    s.replace(QLatin1Char(']'), QLatin1String("\\]"));
    s.replace(QLatin1Char(';'), QLatin1String("\\;"));
    return s;
}
```

---

# Bug: `kfExpr()` gera divisão por zero quando span de keyframe é zero

**Arquivo:** `src/export/ProjectExporter.cpp:65-99`  
**Severidade:** Média

## Descrição

No branch `KfSmooth` e `KfBezier`, cálculos como `m0 = (b.value - p0.value) / dt0` não verificam se `dt0` é zero. Se dois keyframes consecutivos têm o mesmo timestamp, `dt0 = 0` e a expressão gerada contém `inf` ou `nan`, fazendo o ffmpeg falhar.

## Fix

```cpp
const double dt0 = (i > 1) ? (b.time - p0.time) : span;
const double m0 = (dt0 > 1e-9) ? (b.value - p0.value) / dt0 : 0.0;
```

---

# Bug: `Project::fromJson()` não valida `duration` nem `audioRate`

**Arquivo:** `src/models/Project.cpp:30, 272`  
**Severidade:** Média

## Descrição

`mediaFromJson()` lê `m.duration = o["duration"].toDouble()` sem validação. Duração negativa faz `Project::duration()` retornar valor negativo, causando timeline vazia ou retorno antecipado no preview. `audioRate = o["audioRate"].toDouble(48000.0)` pode ser 0, causando divisão por zero no `ProjectExporter`.

## Fix

```cpp
m.duration = qMax(0.0, o["duration"].toDouble());
audioRate = qMax(1.0, o["audioRate"].toDouble(48000.0));
```
