#include "TimelineWidget.h"

#include "ffmpeg/MediaCache.h"
#include "ffmpeg/FFmpegDecoder.h"
#include "ui/TransformDialog.h"
#include "ui/AudioEffectsDialog.h"
#include "ui/SettingsDialog.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QScrollBar>
#include <QMenu>
#include <QKeyEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPolygon>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QPushButton>
#include <QCheckBox>
#include <QColorDialog>
#include <QActionGroup>
#include <QPair>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr int kHeaderW = 130;
constexpr int kRulerH = 26;
constexpr int kMarginR = 60;
constexpr int kZoomW = 64;
constexpr int kVideoRowH = 56;
constexpr int kAudioRowH = 44;
constexpr double kMinPps = 2.0;
constexpr double kMaxPps = 4000.0;
constexpr double kMinDur = 0.04;

// Modos de ferramenta (índices usados pela barra de ferramentas).
enum Tool {
    ToolSelect = 0, ToolMove = 1, ToolScissors = 2, ToolEnvelope = 3, ToolZoom = 4
};

QString fmtRuler(double t) {
    const int total = (int)std::floor(t);
    return QString("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Remove os clipes selecionados de uma faixa e desloca os que estão à
// direita da lacuna removida para a esquerda (edição ripple).
void rippleDeleteInTrack(Track& t, const QStringList& sel) {
    QVector<Clip> keep;
    QVector<QPair<double, double>> removed;
    for (Clip& c : t.clips) {
        if (sel.contains(c.id))
            removed.append(qMakePair(c.pos, c.pos + c.dur));
        else
            keep.append(c);
    }
    if (removed.isEmpty()) {
        t.clips = keep;
        return;
    }
    std::sort(removed.begin(), removed.end());
    QVector<QPair<double, double>> merged;
    for (const auto& iv : removed) {
        if (merged.isEmpty() || iv.first > merged.last().second + 1e-6)
            merged.append(iv);
        else
            merged.last().second = std::max(merged.last().second, iv.second);
    }
    for (Clip& c : keep) {
        double shift = 0.0;
        for (const auto& iv : merged)
            if (c.pos >= iv.second - 1e-6)
                shift += iv.second - iv.first;
        c.pos = std::max(0.0, c.pos - shift);
    }
    t.clips = keep;
}
}

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setMinimumHeight(180);

    m_hbar = new QScrollBar(Qt::Horizontal, this);
    m_vbar = new QScrollBar(Qt::Vertical, this);
    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) {
        m_viewStart = v / m_pps;
        invalidateScene();
    });
    connect(m_vbar, &QScrollBar::valueChanged, this, [this](int v) {
        m_viewTop = v;
        invalidateScene();
    });
    connect(&MediaCache::instance(), &MediaCache::waveformReady, this,
            [this](const QString&) { invalidateScene(); });
    connect(&MediaCache::instance(), &MediaCache::thumbnailReady, this,
            [this](const QString&, double) { invalidateScene(); });
    // Qualquer alteração estrutural do projeto invalida a cena estática
    // (clipes, marcadores, etc.), mas o playhead continua leve de repintar.
    connect(this, &TimelineWidget::modified, this, &TimelineWidget::invalidateScene);
}

void TimelineWidget::invalidateScene() {
    m_staticDirty = true;
    update();
}

void TimelineWidget::setProject(Project* p) {
    m_project = p;
    m_selected.clear();
    emit selectionChanged(QString());
    invalidateScene();
    updateScrollRanges();
}

void TimelineWidget::addTrack(bool audio) {
    if (!m_project) return;
    emit editStart();
    m_project->addTrack(audio);
    invalidateScene();
    updateScrollRanges();
    emit modified();
}

void TimelineWidget::resizeEvent(QResizeEvent*) {
    const int bar = m_vbar->sizeHint().width();
    const int hbarH = m_hbar->sizeHint().height();
    m_hbar->setGeometry(0, height() - hbarH, width() - bar, hbarH);
    m_vbar->setGeometry(width() - bar, 0, bar, height() - hbarH);
    invalidateScene();
    updateScrollRanges();
}

void TimelineWidget::updateScrollRanges() {
    if (!m_project) return;
    const int bar = m_vbar->sizeHint().width();
    const int hbarH = m_hbar->sizeHint().height();
    const int viewW = width() - bar;
    const int viewH = height() - hbarH - kRulerH;

    const double total = m_project->duration();
    const int rows = m_project->videoTracks.size() + m_project->audioTracks.size();
    const int totalH = kRulerH + rows * kVideoRowH + 20;
    const int totalW = kHeaderW + (int)(total * m_pps) + kMarginR;

    m_hbar->setRange(0, std::max(0, totalW - viewW));
    m_hbar->setPageStep(std::max(1, viewW));
    m_vbar->setRange(0, std::max(0, totalH - viewH));
    m_vbar->setPageStep(std::max(1, viewH));

    m_viewStart = m_hbar->value() / m_pps;
    m_viewTop = m_vbar->value();
}

double TimelineWidget::timeToX(double t) const {
    return kHeaderW + (t - m_viewStart) * m_pps;
}

double TimelineWidget::xToTime(int x) const {
    return m_viewStart + (x - kHeaderW) / m_pps;
}

int TimelineWidget::rowY(int videoIdx, int audioIdx) const {
    int y = kRulerH - m_viewTop;
    if (videoIdx >= 0)
        return y + videoIdx * kVideoRowH;
    y += (int)m_project->videoTracks.size() * kVideoRowH;
    return y + audioIdx * kAudioRowH;
}

bool TimelineWidget::rowFromY(int y, int& row, bool& audio) const {
    if (!m_project) return false;
    const int cy = y + m_viewTop;
    if (cy < kRulerH) return false;
    int rem = cy - kRulerH;
    const int vCount = (int)m_project->videoTracks.size();
    const int aCount = (int)m_project->audioTracks.size();
    int idx = rem / kVideoRowH;
    if (idx < vCount) { row = idx; audio = false; return true; }
    rem -= vCount * kVideoRowH;
    if (rem < 0) return false;
    idx = rem / kAudioRowH;
    if (idx < aCount) { row = idx; audio = true; return true; }
    return false;
}

Clip* TimelineWidget::clipAt(int row, bool audio, double t) {
    if (!m_project) return nullptr;
    if (audio) {
        if (row < 0 || row >= (int)m_project->audioTracks.size()) return nullptr;
        auto& clips = m_project->audioTracks[row].clips;
        for (auto& c : clips)
            if (t >= c.pos && t < c.pos + c.dur) return &c;
    } else {
        if (row < 0 || row >= (int)m_project->videoTracks.size()) return nullptr;
        auto& clips = m_project->videoTracks[row].clips;
        for (auto& c : clips)
            if (t >= c.pos && t < c.pos + c.dur) return &c;
    }
    return nullptr;
}

Clip* TimelineWidget::findClipById(const QString& id) {
    if (!m_project) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == id) return &c;
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == id) return &c;
    return nullptr;
}

Track* TimelineWidget::trackOf(Clip* c) {
    if (!m_project || !c) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& x : t.clips)
            if (&x == c) return &t;
    for (Track& t : m_project->audioTracks)
        for (Clip& x : t.clips)
            if (&x == c) return &t;
    return nullptr;
}

double TimelineWidget::snapTime(double t) const {
    if (!m_snap) return t;
    const double fps = m_project ? (double)m_project->fps : 30.0;
    return std::round(t * fps) / fps;
}

void TimelineWidget::setPlayhead(double t) {
    m_playhead = std::max(0.0, t);
    ensurePlayheadVisible();
    update();
}

void TimelineWidget::ensurePlayheadVisible() {
    if (!m_project) return;
    const double px = timeToX(m_playhead);
    const int viewW = width() - m_vbar->sizeHint().width();
    if (px < kHeaderW) {
        m_hbar->setValue((int)((m_playhead - (viewW - kHeaderW) * 0.2) * m_pps));
    } else if (px > viewW) {
        m_hbar->setValue((int)((m_playhead - (viewW - kHeaderW) * 0.8) * m_pps));
    }
    m_viewStart = m_hbar->value() / m_pps;
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 24, 26));
    if (!m_project) return;

    // A cena estática (grid, faixas, clipes, ondas, thumbs) é desenhada uma
    // vez e reutilizada: mover o playhead durante a reprodução/scrub custa só
    // as camadas finas (playhead, loop, indicações de arrasto), não a cena.
    if (m_staticDirty || m_staticCache.size() != size()) {
        m_staticCache = QPixmap(size());
        m_staticCache.fill(Qt::transparent);
        QPainter sp(&m_staticCache);
        renderScene(sp);
        m_staticDirty = false;
    }
    p.drawPixmap(0, 0, m_staticCache);
    renderOverlays(p);
}

// Parte fixa da timeline. Desenhada em QPixmap e reutilizada entre repaints.
void TimelineWidget::renderScene(QPainter& p) {
    const int H = kHeaderW;
    const int R = kRulerH;

    p.fillRect(0, 0, width(), R, QColor(40, 40, 44));
    p.setPen(QColor(70, 70, 76));
    p.drawLine(0, R - 1, width(), R - 1);
    p.drawLine(H - 1, R, H - 1, height());

    // O grid de tempo só vai até o fim das faixas, não pelo espaço vazio abaixo.
    const int gridBottom = std::min(height(),
        kRulerH - m_viewTop
            + (int)m_project->videoTracks.size() * kVideoRowH
            + (int)m_project->audioTracks.size() * kAudioRowH);
    double step = 1.0;
    while (step * m_pps < 70.0) step *= 2.0;
    const double last = m_viewStart + (width() - H) / m_pps;
    for (double t = std::ceil(m_viewStart / step) * step; t <= last; t += step) {
        const int x = (int)(H + (t - m_viewStart) * m_pps);
        p.setPen(QColor(52, 52, 58));
        p.drawLine(x, R, x, gridBottom);
        p.setPen(QColor(180, 180, 190));
        QFont f = p.font();
        f.setPointSizeF(8);
        p.setFont(f);
        p.drawText(x + 3, R - 7, fmtRuler(t));
    }

    for (const Marker& mk : m_project->markers) {
        const int mx = (int)(H + (mk.time - m_viewStart) * m_pps);
        if (mx < H || mx > width()) continue;
        p.setPen(QPen(mk.color, 1));
        p.drawLine(mx, 2, mx, R - 4);
        QPolygon flag;
        flag << QPoint(mx, 2) << QPoint(mx + 6, 2) << QPoint(mx + 6, 8) << QPoint(mx, 8);
        p.setPen(Qt::NoPen);
        p.setBrush(mk.color);
        p.drawPolygon(flag);
        if (!mk.name.isEmpty()) {
            p.setPen(mk.color);
            p.drawText(mx + 8, R - 8, mk.name);
        }
    }

    const int zx0 = 6;
    p.fillRect(zx0, 2, kZoomW, R - 4, QColor(60, 60, 66));
    p.setPen(QColor(200, 200, 205));
    QFont zf = p.font();
    zf.setPointSizeF(9);
    zf.setBold(true);
    p.setFont(zf);
    p.drawText(QRect(zx0, 2, kZoomW / 2, R - 4), Qt::AlignCenter, QStringLiteral("−"));
    p.drawText(QRect(zx0 + kZoomW / 2, 2, kZoomW / 2, R - 4), Qt::AlignCenter, QStringLiteral("+"));
    p.drawLine(zx0 + kZoomW / 2, 4, zx0 + kZoomW / 2, R - 6);

    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i) {
        const int y = rowY(i, -1);
        const int rowH = kVideoRowH;
        p.fillRect(0, y, width(), rowH, (i % 2) ? QColor(31, 31, 34) : QColor(28, 28, 31));
        p.setPen(QColor(48, 48, 54));
        p.drawLine(0, y + rowH, width(), y + rowH);
        p.fillRect(0, y, H, rowH, QColor(38, 38, 42));
        p.setPen(QColor(200, 200, 205));
        QFont f = p.font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(6, y + rowH / 2 + 4, m_project->videoTracks[i].name);
        f.setBold(false);
        p.setFont(f);

        const Track& tr = m_project->videoTracks[i];
        for (const Clip& c : tr.clips) {
            const int cx = (int)(H + (c.pos - m_viewStart) * m_pps);
            const int cw = std::max(2, (int)(c.dur * m_pps));
            QRect r(cx + 1, y + 4, cw - 2, rowH - 8);
            if (r.right() < H || r.left() > width()) continue;
            drawClip(p, r, c, tr, false);
        }
    }

    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
        const int y = rowY(-1, i);
        const int rowH = kAudioRowH;
        p.fillRect(0, y, width(), rowH, (i % 2) ? QColor(29, 29, 32) : QColor(26, 26, 29));
        p.setPen(QColor(46, 46, 52));
        p.drawLine(0, y + rowH, width(), y + rowH);
        p.fillRect(0, y, H, rowH, QColor(36, 36, 40));
        p.setPen(QColor(190, 190, 198));
        QFont f = p.font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(6, y + rowH / 2 + 4, m_project->audioTracks[i].name);
        f.setBold(false);
        p.setFont(f);

        const Track& tr = m_project->audioTracks[i];
        for (const Clip& c : tr.clips) {
            const int cx = (int)(H + (c.pos - m_viewStart) * m_pps);
            const int cw = std::max(2, (int)(c.dur * m_pps));
            QRect r(cx + 1, y + 4, cw - 2, rowH - 8);
            if (r.right() < H || r.left() > width()) continue;
            drawClip(p, r, c, tr, true);
        }
    }
}

// Camadas finas desenhadas por cima da cena estática em cada repaint.
void TimelineWidget::renderOverlays(QPainter& p) {
    const int H = kHeaderW;
    const int R = kRulerH;

    // Região de loop (in/out).
    if (m_loopOut > m_loopIn) {
        const int lx0 = (int)timeToX(m_loopIn);
        const int lx1 = (int)timeToX(m_loopOut);
        p.fillRect(QRect(lx0, 0, lx1 - lx0, R), QColor(255, 200, 40, 70));
        p.fillRect(QRect(lx0, R, lx1 - lx0, height() - R), QColor(255, 200, 40, 18));
        p.setPen(QColor(255, 200, 40, 170));
        p.drawLine(lx0, R, lx0, height());
        p.drawLine(lx1, R, lx1, height());
    }

    const double px = H + (m_playhead - m_viewStart) * m_pps;
    p.setPen(QColor(255, 70, 70));
    p.drawLine((int)px, R, (int)px, height());
    QPolygon tri;
    tri << QPoint((int)px - 6, 0) << QPoint((int)px + 6, 0) << QPoint((int)px, 9);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 70, 70));
    p.drawPolygon(tri);

    if (m_dragMode == Razor) {
        const int rx = (int)timeToX(m_razorT);
        p.setPen(QPen(QColor(255, 255, 255, 190), 1, Qt::DashLine));
        p.drawLine(rx, R, rx, height());
    } else if (m_dragMode == ZoomSelect) {
        const int zx1 = (int)timeToX(m_zoomT0);
        const int zx2 = (int)timeToX(m_zoomT1);
        const QRect zr(QPoint(std::min(zx1, zx2), R), QPoint(std::max(zx1, zx2), height()));
        p.fillRect(zr, QColor(120, 180, 255, 42));
        p.setPen(QColor(120, 180, 255, 210));
        p.drawRect(zr);
    } else if (m_dragMode == Marquee) {
        p.fillRect(m_marqueeRect, QColor(120, 180, 255, 40));
        p.setPen(QPen(QColor(150, 200, 255, 230), 1));
        p.drawRect(m_marqueeRect);
    }
}

void TimelineWidget::drawClip(QPainter& p, const QRect& r, const Clip& c,
                              const Track& tr, bool audio) {
    const bool sel = isSelected(c.id);
    QColor fill = audio ? QColor(26, 86, 66) : QColor(32, 66, 116);
    QColor border = audio ? QColor(70, 160, 120) : QColor(90, 140, 210);
    if (sel) {
        fill = audio ? QColor(40, 120, 92) : QColor(46, 96, 168);
        border = QColor(255, 170, 40);
    }
    p.setPen(QPen(border, sel ? 2 : 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, 3, 3);

    const MediaItem* mi = m_project ? m_project->findMedia(c.mediaId) : nullptr;
    const QString path = mi ? mi->filePath : QString();
    if (audio) {
        drawAudioWaveform(p, r, c, path);
    } else {
        drawVideoThumbs(p, r, c, path);
    }

    drawFadeCorners(p, r, c);

    if (m_tool == ToolEnvelope)
        drawEnvelope(p, r, c, audio);
    else
        drawKeyframeDiamonds(p, r, c, audio);

    QString label = c.name.isEmpty() ? tr.name : c.name;
    if (audio) {
        label += QString("  ·  v %1%").arg((int)llround(c.volume * 100.0));
        if (c.hasAudioFx())
            label += QString("  ·  FX");
    }
    if (std::fabs(c.speed - 1.0) > 1e-4)
        label += QString("  ·  %1×").arg(c.speed, 0, 'g', 3);
    QFont f = p.font();
    f.setPointSizeF(8.5);
    p.setFont(f);
    QFontMetrics fm(f);
    label = fm.elidedText(label, Qt::ElideRight, std::max(1, r.width() - 10));
    QRect labelRect(r.left() + 3, r.top() + 2, r.width() - 6, fm.height());
    p.fillRect(labelRect, QColor(0, 0, 0, 110));
    p.setPen(QColor(235, 235, 240));
    p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, label);

    if (!audio) {
        QRect obar(r.left() + 5, r.top() + 22, std::max(1, r.width() - 10), 3);
        const int w = (int)(obar.width() * c.opacity);
        p.fillRect(obar, QColor(0, 0, 0, 110));
        p.fillRect(QRect(obar.x(), obar.y(), w, obar.height()), QColor(255, 255, 255, 190));
    }

    const QString range = fmtRuler(c.in) + " – " + fmtRuler(c.in + c.dur);
    f.setPointSizeF(7.5);
    p.setFont(f);
    QFontMetrics fm2(f);
    const QString rng = fm2.elidedText(range, Qt::ElideRight, std::max(1, r.width() - 10));
    QRect rangeRect(r.left() + 3, r.bottom() - fm2.height() - 2, r.width() - 6, fm2.height());
    p.fillRect(rangeRect, QColor(0, 0, 0, 110));
    p.setPen(QColor(190, 195, 205));
    p.drawText(rangeRect, Qt::AlignLeft | Qt::AlignVCenter, rng);
}

void TimelineWidget::drawAudioWaveform(QPainter& p, const QRect& r, const Clip& c,
                                       const QString& path) {
    if (path.isEmpty() || r.width() < 2) return;
    MediaCache& cache = MediaCache::instance();
    if (!cache.hasPeaks(path)) {
        cache.requestPeaks(path);
        p.setPen(QColor(255, 255, 255, 45));
        p.drawLine(r.left(), r.center().y(), r.right(), r.center().y());
        return;
    }

    const FFmpegAudioPeaks& pk = cache.peaks(path);
    if (pk.min.isEmpty()) return;

    const int bps = pk.bucketsPerSecond > 0 ? pk.bucketsPerSecond : 1;
    const int x0 = r.left();
    const int x1 = r.right();
    const double dur = c.dur;
    const int midY = r.center().y();
    const double amp = r.height() / 2.0 - 2.0;

    p.setPen(QPen(QColor(110, 235, 185), 1));
    for (int x = x0; x <= x1; ++x) {
        const double t0 = c.in + (x - x0) * dur / (double)(x1 - x0);
        const double t1 = c.in + (x + 1 - x0) * dur / (double)(x1 - x0);
        int b0 = (int)std::floor(t0 * bps);
        int b1 = (int)std::floor(t1 * bps);
        if (b0 < 0) b0 = 0;
        if (b1 >= pk.min.size()) b1 = pk.min.size() - 1;
        if (b0 > b1) continue;

        float mn = 0.0f;
        float mx = 0.0f;
        for (int b = b0; b <= b1; ++b) {
            if (pk.min[b] < mn) mn = pk.min[b];
            if (pk.max[b] > mx) mx = pk.max[b];
        }
        const int y0 = (int)(midY - mx * amp);
        const int y1 = (int)(midY - mn * amp);
        if (y0 >= r.top() && y1 <= r.bottom())
            p.drawLine(x, y0, x, y1);
    }
}

void TimelineWidget::drawVideoThumbs(QPainter& p, const QRect& r, const Clip& c,
                                     const QString& path) {
    if (path.isEmpty()) return;
    MediaCache& cache = MediaCache::instance();

    const int sliceW = 96;
    const int n = std::max(1, r.width() / sliceW);
    QVector<QImage> thumbs;
    thumbs.reserve(n);
    bool allReady = true;

    for (int i = 0; i < n; ++i) {
        const double t = c.in + (i + 0.5) / n * c.dur;
        const double k = std::round(t * 10.0) / 10.0;
        QImage img = cache.thumb(path, k);
        if (img.isNull()) {
            allReady = false;
            cache.requestThumb(path, k);
        }
        thumbs.append(img);
    }

    if (!allReady) {
        p.fillRect(r.adjusted(1, 1, -1, -1), QColor(0, 0, 0, 70));
        return;
    }

    QPainterPath clipPath;
    clipPath.addRoundedRect(r, 3, 3);
    p.save();
    p.setClipPath(clipPath);
    for (int i = 0; i < n; ++i) {
        const QImage& img = thumbs[i];
        if (img.isNull()) continue;
        const int sliceLeft = r.left() + i * r.width() / n;
        const int sliceRight = r.left() + (i + 1) * r.width() / n;
        const int sw = sliceRight - sliceLeft;
        const double scale = std::max(sw / (double)img.width(),
                                      r.height() / (double)img.height());
        const int tw = (int)std::ceil(img.width() * scale);
        const int th = (int)std::ceil(img.height() * scale);
        p.drawImage(QRect(sliceLeft + (sw - tw) / 2, r.top() + (r.height() - th) / 2, tw, th),
                    img);
    }
    p.restore();
}

void TimelineWidget::drawEnvelope(QPainter& p, const QRect& r, const Clip& c, bool audio) {
    const double maxV = audio ? 3.0 : 1.0;
    const QVector<Keyframe>& keys = audio ? c.kfVolume : c.kfOpacity;

    QPainterPath path;
    const int steps = std::max(2, r.width() / 2);
    for (int i = 0; i <= steps; ++i) {
        // Keyframes são relativos ao início do clipe (padrão do exportador).
        const double t = (double)i / steps * c.dur;
        const double v = audio ? kfValue(c.kfVolume, c.volume, t)
                               : kfValue(c.kfOpacity, c.opacity, t);
        const double cl = std::clamp(v, 0.0, maxV) / maxV;
        const int x = r.left() + (int)std::lround((double)i / steps * r.width());
        const int y = r.bottom() - (int)std::lround(cl * (r.height() - 4.0)) - 2;
        if (i == 0) path.moveTo(x, y);
        else path.lineTo(x, y);
    }
    p.setPen(QPen(QColor(255, 220, 90), 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    for (const Keyframe& k : keys) {
        if (k.time < -1e-6 || k.time > c.dur + 1e-6) continue;
        const int x = r.left() + (int)std::lround(k.time / c.dur * r.width());
        const double cl = std::clamp(k.value, 0.0, maxV) / maxV;
        const int y = r.bottom() - (int)std::lround(cl * (r.height() - 4.0)) - 2;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 220, 90));
        p.drawEllipse(QPoint(x, y), 3, 3);
    }
}

void TimelineWidget::drawFadeCorners(QPainter& p, const QRect& r, const Clip& c) {
    if (c.fadeIn <= 0 && c.fadeOut <= 0) return;
    const double dur = std::max(c.dur, kMinDur);
    const int fi = (int)std::round(std::min(c.fadeIn, dur) / dur * r.width());
    const int fo = (int)std::round(std::min(c.fadeOut, dur) / dur * r.width());
    QPainterPath path;
    if (fi > 0) {
        path.moveTo(r.left(), r.bottom());
        path.lineTo(r.left() + fi, r.bottom());
        path.lineTo(r.left(), r.bottom() - fi);
        path.closeSubpath();
    }
    if (fo > 0) {
        path.moveTo(r.right(), r.bottom());
        path.lineTo(r.right() - fo, r.bottom());
        path.lineTo(r.right(), r.bottom() - fo);
        path.closeSubpath();
    }
    if (!path.isEmpty()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 130));
        p.drawPath(path);
    }
}

// Losangos de keyframe animados (estilo Vegas) marcando posição na linha do
// tempo. Uma cor por propriedade; lane central para não poluir o clipe.
void TimelineWidget::drawKeyframeDiamonds(QPainter& p, const QRect& r,
                                          const Clip& c, bool audio) {
    struct KfSet { const QVector<Keyframe>* keys; QColor color; };
    QVector<KfSet> sets;
    if (audio) {
        sets.append(KfSet{&c.kfVolume, QColor(110, 235, 185)});
    } else {
        sets.append(KfSet{&c.kfOpacity, QColor(255, 255, 255)});
        sets.append(KfSet{&c.kfScale, QColor(90, 200, 255)});
        sets.append(KfSet{&c.kfRotation, QColor(210, 150, 255)});
        sets.append(KfSet{&c.kfTx, QColor(255, 200, 90)});
        sets.append(KfSet{&c.kfTy, QColor(255, 160, 90)});
        sets.append(KfSet{&c.kfCropL, QColor(120, 255, 140)});
        sets.append(KfSet{&c.kfCropR, QColor(120, 255, 140)});
        sets.append(KfSet{&c.kfCropT, QColor(120, 255, 140)});
        sets.append(KfSet{&c.kfCropB, QColor(120, 255, 140)});
    }
    const double dur = std::max(c.dur, kMinDur);
    int lane = 0;
    for (const KfSet& s : sets) {
        if (!s.keys || s.keys->isEmpty()) continue;
        const int laneY = r.bottom() - (audio ? 20 : 22) - (lane % 2) * 10;
        for (const Keyframe& k : *s.keys) {
            const int x = r.left() + (int)std::lround(
                std::clamp(k.time, 0.0, dur) / dur * r.width());
            const QPolygonF dia = QPolygonF()
                << QPointF(x, laneY - 4) << QPointF(x + 4, laneY)
                << QPointF(x, laneY + 4) << QPointF(x - 4, laneY);
            p.setPen(QPen(QColor(20, 20, 24), 1));
            p.setBrush(s.color);
            p.drawPolygon(dia);
        }
        ++lane;
    }
}

void TimelineWidget::zoomBy(double factor, double centerT) {
    if (!m_project) return;
    const double anchorX = timeToX(centerT) - kHeaderW;
    const double oldPps = m_pps;
    m_pps = std::clamp(m_pps * factor, kMinPps, kMaxPps);
    const double newStart = centerT - anchorX / m_pps;
    m_hbar->setValue((int)(newStart * m_pps));
    m_viewStart = newStart;
    updateScrollRanges();
    update();
}

void TimelineWidget::toggleMarker(double t) {
    if (!m_project) return;
    emit editStart();
    const double tt = snapTime(std::max(0.0, t));
    for (int i = 0; i < m_project->markers.size(); ++i) {
        if (std::fabs(m_project->markers[i].time - tt) < 1e-6) {
            m_project->markers.removeAt(i);
            emit modified();
            update();
            return;
        }
    }
    Marker m;
    m.id = newId();
    m.time = tt;
    m_project->addMarker(m);
    emit modified();
    update();
}

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
    if (!m_project) return;
    const int x = e->pos().x();
    const int y = e->pos().y();
    setFocus();

    if (e->button() != Qt::LeftButton) return;

    if (y < kRulerH) {
        const int zx0 = 6;
        if (x >= zx0 && x < zx0 + kZoomW) {
            const bool zoomIn = x >= zx0 + kZoomW / 2;
            zoomBy(zoomIn ? 1.6 : 1.0 / 1.6, m_playhead);
            return;
        }
        if (e->modifiers() & Qt::ControlModifier) {
            toggleMarker(xToTime(x));
            return;
        }
        // Perto da agulha: arrasta o playhead. Fora dela: clique define o
        // playhead e arrasto define a região de loop.
        const double px = timeToX(m_playhead);
        if (std::fabs(x - px) <= 6) {
            m_dragMode = PlayheadDrag;
            const double t2 = std::max(0.0, snapTime(xToTime(x)));
            setPlayhead(t2);
            emit playheadChanged(t2);
            m_dragStart = e->pos();
            update();
            return;
        }
        // Arrastar na régua define a região de loop; clique define o playhead.
        m_dragMode = RulerLoop;
        m_loopPressT = std::max(0.0, snapTime(xToTime(x)));
        m_dragStart = e->pos();
        setPlayhead(m_loopPressT);
        emit playheadChanged(m_loopPressT);
        return;
    }

    int row;
    bool audio;
    if (rowFromY(y, row, audio)) {
        const double t = xToTime(x);
        Clip* clip = clipAt(row, audio, t);

        if (m_tool == ToolScissors) {
            m_dragMode = Razor;
            m_razorT = std::max(0.0, snapTime(t));
            m_dragStart = e->pos();
            update();
            return;
        }
        if (m_tool == ToolEnvelope) {
            if (clip) envelopePress(clip, std::max(0.0, snapTime(t)));
            return;
        }
        if (m_tool == ToolZoom) {
            m_dragMode = ZoomSelect;
            m_zoomT0 = std::max(0.0, t);
            m_zoomT1 = m_zoomT0;
            m_dragStart = e->pos();
            update();
            return;
        }

        if (clip) {
            if (e->modifiers() & Qt::ControlModifier)
                toggleSelection(clip->id);
            else if (!isSelected(clip->id))
                setSelection(clip->id);
            m_dragMode = None;
            if (m_tool == ToolMove) {
                m_dragMode = MoveClip;
            } else {
                const int cx = (int)timeToX(clip->pos);
                const int cw = (int)(clip->dur * m_pps);
                const int dx = x - cx;
                if (dx <= 8) m_dragMode = TrimLeft;
                else if (cw - dx <= 8) m_dragMode = TrimRight;
                else m_dragMode = MoveClip;
            }
            m_dragClip = clip->id;
            m_dragUndoPushed = false;
            m_dragStart = e->pos();
            m_dragOrigPos = clip->pos;
            m_dragOrigIn = clip->in;
            m_dragOrigDur = clip->dur;
            m_dragOrig.clear();
            // Alt = arrastar apenas o clipe, sem arrastar o grupo vinculado.
            const bool solo = (e->modifiers() & Qt::AltModifier) != 0;
            auto addOrig = [this, solo](Clip* c) {
                if (!c) return;
                if (!solo && !c->groupId.isEmpty()) {
                    for (Clip* m : groupMembers(c->groupId))
                        m_dragOrig.insert(m->id, {m->pos, m->in, m->dur});
                } else {
                    m_dragOrig.insert(c->id, {c->pos, c->in, c->dur});
                }
            };
            for (const QString& sid : m_selected) {
                Clip* sc = findClipById(sid);
                addOrig(sc);
            }
            addOrig(clip);
        } else if (m_tool == ToolSelect || m_tool == ToolMove) {
            // Espaço vazio com a ferramenta de seleção: caixa de seleção
            // (marquee). Com Ctrl, soma aos já selecionados.
            m_dragMode = Marquee;
            m_dragStart = e->pos();
            m_marqueeRect = QRect(e->pos(), QSize(0, 0));
            update();
        } else {
            m_selected.clear();
            invalidateScene();
            emit selectionChanged(QString());
            const double t2 = std::max(0.0, snapTime(t));
            setPlayhead(t2);
            emit playheadChanged(t2);
        }
        update();
    } else {
        m_selected.clear();
        invalidateScene();
        emit selectionChanged(QString());
        update();
    }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!m_project) return;

    if (e->buttons() & Qt::LeftButton) {
        if (m_dragMode == PlayheadDrag) {
            const double t2 = std::max(0.0, snapTime(xToTime(e->pos().x())));
            setPlayhead(t2);
            emit playheadChanged(t2);
            update();
            return;
        }
        if (m_dragMode == RulerLoop) {
            const double cur = std::max(0.0, snapTime(xToTime(e->pos().x())));
            const double a = std::min(m_loopPressT, cur);
            const double b = std::max(m_loopPressT, cur);
            if (b - a > 0.02) {
                m_loopIn = a;
                m_loopOut = b;
                emit loopChanged(m_loopIn, m_loopOut);
            }
            update();
            return;
        }
        if (m_dragMode == Razor) {
            m_razorT = std::max(0.0, snapTime(xToTime(e->pos().x())));
            update();
            return;
        }
        if (m_dragMode == ZoomSelect) {
            m_zoomT1 = std::max(0.0, xToTime(e->pos().x()));
            update();
            return;
        }
        if (m_dragMode == Marquee) {
            m_marqueeRect = QRect(m_dragStart, e->pos()).normalized();
            update();
            return;
        }
        if (m_dragMode != None && !m_dragClip.isEmpty()) {
            Clip* clip = findClipById(m_dragClip);
            if (clip) {
                const double dt = (e->pos().x() - m_dragStart.x()) / m_pps;
                if (!m_dragUndoPushed && std::fabs(dt) > 1e-9) {
                    emit editStart();
                    m_dragUndoPushed = true;
                }
                if (m_dragMode == MoveClip) {
                    const double raw = m_dragOrigPos + dt;
                    const double snapped = snapToEdges(snapTime(raw), m_dragClip);
                    const double delta = snapped - m_dragOrigPos;
                    QSet<QString> moving;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it)
                        moving.insert(it.key());
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (sc)
                            sc->pos = clampPosToTrack(sc,
                                                      std::max(0.0, it.value().pos + delta),
                                                      moving);
                    }
                } else if (m_dragMode == TrimLeft) {
                    const double minEnd = std::max(0.0, m_dragOrigPos + m_dragOrigDur - kMinDur);
                    const double np = std::clamp(snapToEdges(snapTime(m_dragOrigPos + dt)),
                                                 0.0, minEnd);
                    const double delta = np - m_dragOrigPos;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (!sc) continue;
                        const ClipOrig& o = it.value();
                        sc->pos = std::max(0.0, o.pos + delta);
                        sc->in = o.in + delta;
                        sc->dur = o.dur - delta;
                    }
                } else if (m_dragMode == TrimRight) {
                    const double end = snapToEdges(snapTime(m_dragOrigPos + m_dragOrigDur + dt));
                    const double newDur = std::max(kMinDur, end - clip->pos);
                    const double deltaDur = newDur - m_dragOrigDur;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (sc) sc->dur = std::max(kMinDur, it.value().dur + deltaDur);
                    }
                }
                updateScrollRanges();
                update();
                emit modified();
            }
        }
        return;
    }

    if (m_tool == ToolScissors) {
        setCursor(Qt::SplitVCursor);
        return;
    }
    if (m_tool == ToolEnvelope || m_tool == ToolZoom) {
        setCursor(Qt::CrossCursor);
        return;
    }

    int row;
    bool audio;
    if (rowFromY(e->pos().y(), row, audio)) {
        Clip* clip = clipAt(row, audio, xToTime(e->pos().x()));
        if (clip) {
            const int cx = (int)timeToX(clip->pos);
            const int cw = (int)(clip->dur * m_pps);
            const int dx = e->pos().x() - cx;
            if (m_tool == ToolMove) {
                setCursor(Qt::OpenHandCursor);
            } else if (dx <= 8 || cw - dx <= 8) {
                setCursor(Qt::SizeHorCursor);
            } else {
                setCursor(Qt::OpenHandCursor);
            }
        } else {
            setCursor(Qt::ArrowCursor);
        }
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e) {
    switch (m_dragMode) {
    case RulerLoop:
        break;
    case Razor:
        razorSplitAt(m_razorT);
        break;
    case Marquee:
        if (m_marqueeRect.width() < 4 && m_marqueeRect.height() < 4) {
            // Clique simples no vazio: desfaz a seleção (Ctrl mantém).
            if (!(e->modifiers() & Qt::ControlModifier)) {
                m_selected.clear();
                invalidateScene();
                emit selectionChanged(QString());
            }
        } else {
            selectInMarquee(e->modifiers() & Qt::ControlModifier);
        }
        m_marqueeRect = QRect();
        break;
    case ZoomSelect:
        if (std::fabs(m_zoomT1 - m_zoomT0) < 0.02)
            zoomBy(1.6, m_zoomT0);
        else
            applyZoomRect(m_zoomT0, m_zoomT1);
        break;
    default:
        break;
    }
    m_dragMode = None;
    m_dragClip.clear();
    m_dragUndoPushed = false;
    m_dragOrig.clear();
    update();
}

// Caixa de seleção: seleciona todos os clipes cujo retângulo cruza a caixa.
void TimelineWidget::selectInMarquee(bool add) {
    if (!m_project) return;
    const double t0 = xToTime(m_marqueeRect.left());
    const double t1 = xToTime(m_marqueeRect.right());
    if (t1 - t0 < 1e-9) return;
    QStringList found;
    auto collect = [&](const Track& tr, bool audio, int row) {
        const int y = audio ? rowY(-1, row) : rowY(row, -1);
        const int rowH = audio ? kAudioRowH : kVideoRowH;
        if (m_marqueeRect.bottom() < y || m_marqueeRect.top() > y + rowH) return;
        for (const Clip& c : tr.clips)
            if (c.pos + c.dur > t0 && c.pos < t1)
                found.append(c.id);
    };
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        collect(m_project->videoTracks[i], false, i);
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        collect(m_project->audioTracks[i], true, i);
    if (!add) m_selected.clear();
    for (const QString& id : found)
        if (!m_selected.contains(id)) m_selected.append(id);
    invalidateScene();
    emit selectionChanged(m_selected.isEmpty() ? QString() : m_selected.last());
    update();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_project) return;
    int row;
    bool audio;
    if (rowFromY(e->pos().y(), row, audio)) {
        Clip* clip = clipAt(row, audio, xToTime(e->pos().x()));
        if (clip) {
            toggleSelection(clip->id);
            update();
        }
    }
}

void TimelineWidget::wheelEvent(QWheelEvent* e) {
    const QPoint delta = e->angleDelta();
    if (e->modifiers() & Qt::ControlModifier) {
        if (delta.y() == 0) return;
        const double factor = std::pow(1.1, delta.y() / 120.0);
        const double oldPps = m_pps;
        const double anchorX = e->position().x() - kHeaderW;
        const double anchorT = m_viewStart + anchorX / oldPps;
        m_pps = std::clamp(m_pps * factor, kMinPps, kMaxPps);
        const double newStart = anchorT - anchorX / m_pps;
        m_hbar->setValue((int)(newStart * m_pps));
        m_viewStart = newStart;
        updateScrollRanges();
        update();
    } else if (delta.y() != 0) {
        m_vbar->setValue(m_vbar->value() - delta.y());
    } else if (delta.x() != 0) {
        m_hbar->setValue(m_hbar->value() - delta.x());
    }
    e->accept();
}

void TimelineWidget::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_Space:
        emit playPauseRequested();
        e->accept();
        break;
    case Qt::Key_S:
        cutAtPlayhead();
        e->accept();
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (e->modifiers() & Qt::ShiftModifier)
            deleteSelectedLeaveGap();
        else
            deleteSelected();
        e->accept();
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomBy(1.6, m_playhead);
        e->accept();
        break;
    case Qt::Key_Minus:
        zoomBy(1.0 / 1.6, m_playhead);
        e->accept();
        break;
    default:
        QWidget::keyPressEvent(e);
    }
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* e) {
    if (!m_project) return;
    QMenu menu(this);
    Clip* clip = nullptr;
    int row;
    bool audio;
    if (rowFromY(e->pos().y(), row, audio))
        clip = clipAt(row, audio, xToTime(e->pos().x()));

    if (clip) {
        if (!isSelected(clip->id))
            setSelection(clip->id);
        update();
        QAction* cut = menu.addAction(tr("Dividir no playhead"));
        QAction* del = menu.addAction(tr("Excluir (junta os espaços)"));
        QAction* gapDel = menu.addAction(tr("Excluir (deixa espaço)"));
        QAction* dup = menu.addAction(tr("Duplicar"));
        menu.addSeparator();
        QAction* props = menu.addAction(tr("Propriedades…"));
        QAction* unlink = nullptr;
        if (!clip->groupId.isEmpty())
            unlink = menu.addAction(tr("Desvincular grupo"));
        QAction* fx = nullptr;
        QAction* audioFx = nullptr;
        QAction* transform = nullptr;
        QAction* panCrop = nullptr;
        if (!audio) {
            fx = menu.addAction(tr("Efeitos de vídeo…"));
            transform = menu.addAction(tr("Transformar…"));
            panCrop = menu.addAction(tr("Pancrop…"));
        } else {
            audioFx = menu.addAction(tr("Efeitos de áudio…"));
        }
        QAction* act = menu.exec(e->globalPos());
        if (act == cut) cutAtPlayhead();
        else if (act == del) deleteSelected();
        else if (act == gapDel) deleteSelectedLeaveGap();
        else if (act == dup) duplicateClip(clip);
        else if (act == props) showProperties(clip);
        else if (act == unlink) {
            emit editStart();
            for (Clip* m : groupMembers(clip->groupId))
                m->groupId.clear();
            update();
            emit modified();
        }
        else if (act == fx) showEffectsDialog(clip);
        else if (act == audioFx) showAudioEffectsDialog(clip);
        else if (act == transform) showTransformDialog(clip);
        else if (act == panCrop) emit pancropRequested(clip->id);
    } else if (e->pos().y() < kRulerH) {
        const double t = std::max(0.0, snapTime(xToTime(e->pos().x())));
        QAction* addM = menu.addAction(tr("Adicionar marcador"));
        QAction* addMN = menu.addAction(tr("Adicionar marcador nomeado…"));
        QAction* clearM = menu.addAction(tr("Limpar todos os marcadores"));
        QAction* act = menu.exec(e->globalPos());
        if (act == addM) {
            toggleMarker(t);
        } else if (act == addMN) {
            bool ok = false;
            const QString name =
                QInputDialog::getText(this, tr("Marcador"), tr("Nome do marcador:"),
                                      QLineEdit::Normal, QString(), &ok);
            if (ok && !name.isEmpty()) {
                emit editStart();
                Marker m;
                m.id = newId();
                m.time = t;
                m.name = name;
                m_project->addMarker(m);
                emit modified();
                update();
            }
        } else if (act == clearM) {
            emit editStart();
            m_project->markers.clear();
            emit modified();
            update();
        }
    } else {
        QAction* addV = menu.addAction(tr("Adicionar faixa de vídeo"));
        QAction* addA = menu.addAction(tr("Adicionar faixa de áudio"));
        QAction* act = nullptr;
        Track* track = nullptr;
        QMenu* blendMenu = nullptr;
        int vrow = -1;
        if (rowFromY(e->pos().y(), vrow, audio) && !audio
            && vrow >= 0 && vrow < m_project->videoTracks.size()) {
            track = &m_project->videoTracks[vrow];
        }
        if (track) {
            menu.addSeparator();
            blendMenu = menu.addMenu(tr("Modo de composição"));
            const QStringList modes = {"normal", "screen", "multiply", "overlay",
                                       "darken", "lighten", "softlight", "hardlight",
                                       "difference", "addition", "subtract", "exclusion"};
            QActionGroup* grp = new QActionGroup(blendMenu);
            for (const QString& m : modes) {
                QAction* a = blendMenu->addAction(m);
                a->setCheckable(true);
                a->setChecked(track->blendMode == m);
                grp->addAction(a);
            }
        }
        act = menu.exec(e->globalPos());
        if (act == addV) addTrack(false);
        else if (act == addA) addTrack(true);
        else if (track && act && blendMenu && act->parent() == blendMenu) {
            emit editStart();
            track->blendMode = act->text();
            emit modified();
            update();
        }
    }
}
void TimelineWidget::dragEnterEvent(QDragEnterEvent* e) {
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QLatin1String(kMimeMedia)) || md->hasUrls())
        e->acceptProposedAction();
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* e) {
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QLatin1String(kMimeMedia)) || md->hasUrls())
        e->acceptProposedAction();
}

void TimelineWidget::dropEvent(QDropEvent* e) {
    if (!m_project) return;
    const QMimeData* md = e->mimeData();

    QStringList mediaIds;
    if (md->hasFormat(QLatin1String(kMimeMedia))) {
        // Arrasto vindo da lista de mídia do aplicativo.
        const QByteArray data = md->data(QLatin1String(kMimeMedia));
        for (QByteArray line : data.split('\n')) {
            line = line.trimmed();
            if (!line.isEmpty()) mediaIds.append(QString::fromUtf8(line));
        }
    } else if (md->hasUrls()) {
        // Arrasto de arquivos vindos do gerenciador de arquivos: importa para
        // a mídia do projeto e já coloca clipes no ponto solto.
        int added = 0;
        QStringList imported;
        for (const QUrl& u : md->urls()) {
            if (!u.isLocalFile()) continue;
            const QString path = u.toLocalFile();
            const FFmpegMediaInfo info = FFmpegDecoder::probe(path);
            if (!info.hasVideo && !info.hasAudio) continue;
            MediaItem m;
            m.id = newId();
            m.filePath = path;
            m.name = QFileInfo(path).completeBaseName();
            m.duration = info.duration;
            m.width = info.width;
            m.height = info.height;
            m.hasVideo = info.hasVideo;
            m.hasAudio = info.hasAudio;
            m_project->media.append(m);
            mediaIds.append(m.id);
            imported.append(path);
            ++added;
        }
        if (added == 0) return;
        SettingsDialog::warnMkvIfNeeded(this, imported);
        emit mediaImported(); // atualiza a lista de mídia e marca o projeto
    } else {
        return;
    }
    if (mediaIds.isEmpty()) return;

    const QPoint dropPos = e->position().toPoint();
    int row;
    bool audio;
    if (!rowFromY(dropPos.y(), row, audio)) return;

    emit editStart();
    double t = snapTime(std::max(0.0, xToTime(dropPos.x())));
    QString lastPlaced;
    for (const QString& mid : mediaIds) {
        const MediaItem* m = m_project->findMedia(mid);
        if (!m) continue;
        const bool fits = audio ? m->hasAudio : m->hasVideo;
        if (!fits) continue;

        const bool both = m->hasVideo && m->hasAudio;
        const QString gid = both ? newId() : QString();

        Clip c;
        c.id = newId();
        c.groupId = gid;
        c.mediaId = mid;
        c.pos = t;
        c.in = 0.0;
        c.dur = m->duration > 0 ? m->duration : 1.0;
        c.name = m->name;

        // Apara a duração para o clipe caber sem sobrepor os existentes na faixa.
        double dur = c.dur;
        if (audio) {
            dur = fitDurationInTrack(m_project->audioTracks[row], t, dur, QString());
            if (m->hasVideo && !m_project->videoTracks.isEmpty())
                dur = std::min(dur, fitDurationInTrack(m_project->videoTracks[0], t, dur, QString()));
        } else {
            dur = fitDurationInTrack(m_project->videoTracks[row], t, dur, QString());
            if (m->hasAudio) {
                if (m_project->audioTracks.isEmpty())
                    m_project->addTrack(true);
                dur = std::min(dur, fitDurationInTrack(m_project->audioTracks[0], t, dur, QString()));
            }
        }
        if (dur < kMinDur) { // não cabe: pula para depois deste item
            t += c.dur;
            continue;
        }
        c.dur = dur;

        if (audio) {
            if (m->hasVideo && !m_project->videoTracks.isEmpty()) {
                Clip vc = c;
                vc.id = newId();
                m_project->videoTracks[0].clips.push_back(vc);
            }
            m_project->audioTracks[row].clips.push_back(c);
            lastPlaced = c.id;
        } else {
            if (m->hasAudio) {
                Clip ac = c;
                ac.id = newId();
                m_project->audioTracks[0].clips.push_back(ac);
            }
            m_project->videoTracks[row].clips.push_back(c);
            lastPlaced = c.id;
        }
        t += dur;
    }
    if (!lastPlaced.isEmpty()) setSelection(lastPlaced);
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::cutAtPlayhead() {
    if (!m_project) return;
    emit editStart();
    // Comportamento clássico (Vegas): "S" divide TODOS os clipes (vídeo e
    // áudio) que cruzam o playhead — independentemente da seleção. Assim o
    // corte sempre funciona: basta posicionar o playhead sobre o clipe.
    // Coleta um representante por grupo ANTES de dividir: a divisão troca os
    // groupIds, então deduplicar durante o processo faria um mesmo grupo ser
    // reprocessado (e desagrupado).
    QHash<QString, QString> units; // unidade -> id de um representante
    auto consider = [&](const Clip& c) {
        if (m_playhead > c.pos + 1e-6 && m_playhead < c.pos + c.dur - 1e-6) {
            const QString key = c.groupId.isEmpty() ? c.id : c.groupId;
            if (!units.contains(key)) units.insert(key, c.id);
        }
    };
    for (const Track& t : m_project->videoTracks)
        for (const Clip& c : t.clips) consider(c);
    for (const Track& t : m_project->audioTracks)
        for (const Clip& c : t.clips) consider(c);
    for (const QString& rep : units.values()) {
        Clip* cc = findClipById(rep);
        if (cc) splitClipAt(cc, m_playhead);
    }
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::splitClipAt(Clip* c, double t) {
    if (!c || !m_project) return;
    // Usa ids (não ponteiros): inserir clipes pode realocar o vetor e
    // invalidar ponteiros coletados antes do split.
    QStringList ids;
    if (!c->groupId.isEmpty()) {
        for (Clip* m : groupMembers(c->groupId)) ids.append(m->id);
    } else {
        ids.append(c->id);
    }
    const bool grouped = ids.size() > 1;
    // Cada metade vira um grupo próprio (vídeo+áudio da frente ficam
    // juntos, e os de trás juntos), para que excluir um lado não apague o outro.
    const QString fg = grouped ? newId() : QString();
    const QString bg = grouped ? newId() : QString();
    for (const QString& id : ids) {
        Clip* cc = findClipById(id);
        if (!cc) continue;
        if (t <= cc->pos + 1e-6 || t >= cc->pos + cc->dur - 1e-6) {
            // Membro do grupo que não cruza o playhead: vira grupo próprio,
            // para que excluir uma das metades não arraste o clipe inteiro.
            if (grouped) cc->groupId = newId();
            continue;
        }
        Clip b = *cc;
        b.id = newId();
        b.pos = t;
        b.in = cc->in + (t - cc->pos);
        b.dur = cc->dur - (t - cc->pos);
        cc->dur = t - cc->pos;
        cc->groupId = fg;
        b.groupId = bg;
        Track* tr = trackOf(cc);
        if (tr) {
            int idx = -1;
            for (int i = 0; i < tr->clips.size(); ++i)
                if (tr->clips[i].id == cc->id) { idx = i; break; }
            if (idx >= 0) tr->clips.insert(idx + 1, b);
        }
    }
}

void TimelineWidget::duplicateClip(Clip* c) {
    if (!c) return;
    emit editStart();
    const double shift = c->dur;
    const QString gid = newId();
    // Usa ids (não ponteiros): push_back pode realocar o vetor e invalidar
    // os ponteiros ainda não processados.
    QStringList ids = c->groupId.isEmpty() ? QStringList{c->id} : QStringList();
    if (ids.isEmpty())
        for (Clip* m : groupMembers(c->groupId)) ids.append(m->id);
    QString firstDup;
    for (const QString& id : ids) {
        Clip* src = findClipById(id);
        if (!src) continue;
        Clip b = *src;
        b.id = newId();
        b.groupId = gid;
        b.pos = snapTime(src->pos + shift);
        Track* tr = trackOf(src);
        if (tr) {
            tr->clips.push_back(b);
            if (id == c->id) firstDup = b.id;
        }
    }
    if (!firstDup.isEmpty()) setSelection(firstDup);
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::showProperties(Clip* c) {
    if (!c) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Propriedades do clipe"));

    auto* vol = new QSlider(Qt::Horizontal, &dlg);
    vol->setRange(0, 200);
    vol->setValue((int)llround(c->volume * 100.0));
    auto* volLabel = new QLabel(QString("%1%").arg(vol->value()), &dlg);
    connect(vol, &QSlider::valueChanged, &dlg, [volLabel](int v) {
        volLabel->setText(QString("%1%").arg(v));
    });

    auto* op = new QSlider(Qt::Horizontal, &dlg);
    op->setRange(0, 100);
    op->setValue((int)llround(c->opacity * 100.0));
    auto* opLabel = new QLabel(QString("%1%").arg(op->value()), &dlg);
    connect(op, &QSlider::valueChanged, &dlg, [opLabel](int v) {
        opLabel->setText(QString("%1%").arg(v));
    });

    auto* volRow = new QHBoxLayout;
    volRow->addWidget(vol, 1);
    volRow->addWidget(volLabel);
    auto* opRow = new QHBoxLayout;
    opRow->addWidget(op, 1);
    opRow->addWidget(opLabel);

    auto* speed = new QSlider(Qt::Horizontal, &dlg);
    speed->setRange(10, 400);
    speed->setValue((int)llround(c->speed * 100.0));
    auto* speedLabel = new QLabel(QString("%1×").arg(speed->value() / 100.0), &dlg);
    connect(speed, &QSlider::valueChanged, &dlg, [speedLabel](int v) {
        speedLabel->setText(QString("%1×").arg(v / 100.0));
    });
    auto* speedRow = new QHBoxLayout;
    speedRow->addWidget(speed, 1);
    speedRow->addWidget(speedLabel);

    auto* fIn = new QSlider(Qt::Horizontal, &dlg);
    fIn->setRange(0, 5000);
    fIn->setValue((int)llround(c->fadeIn * 1000.0));
    auto* fInLabel = new QLabel(QString("%1 s").arg(fIn->value() / 1000.0), &dlg);
    connect(fIn, &QSlider::valueChanged, &dlg, [fInLabel](int v) {
        fInLabel->setText(QString("%1 s").arg(v / 1000.0));
    });
    auto* fInRow = new QHBoxLayout;
    fInRow->addWidget(fIn, 1);
    fInRow->addWidget(fInLabel);

    auto* fOut = new QSlider(Qt::Horizontal, &dlg);
    fOut->setRange(0, 5000);
    fOut->setValue((int)llround(c->fadeOut * 1000.0));
    auto* fOutLabel = new QLabel(QString("%1 s").arg(fOut->value() / 1000.0), &dlg);
    connect(fOut, &QSlider::valueChanged, &dlg, [fOutLabel](int v) {
        fOutLabel->setText(QString("%1 s").arg(v / 1000.0));
    });
    auto* fOutRow = new QHBoxLayout;
    fOutRow->addWidget(fOut, 1);
    fOutRow->addWidget(fOutLabel);

    auto* textEdit = new QLineEdit(c->text, &dlg);
    textEdit->setPlaceholderText(tr("Deixe vazio para sem texto"));

    const bool isAudio = trackOf(c)->audio;

    auto* form = new QFormLayout;
    form->addRow(tr("Volume:"), volRow);
    form->addRow(tr("Opacidade:"), opRow);
    form->addRow(tr("Velocidade:"), speedRow);
    form->addRow(tr("Fade in:"), fInRow);
    form->addRow(tr("Fade out:"), fOutRow);
    if (!isAudio) form->addRow(tr("Texto:"), textEdit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return;

    emit editStart();
    c->volume = vol->value() / 100.0;
    c->opacity = op->value() / 100.0;
    c->speed = speed->value() / 100.0;
    c->fadeIn = fIn->value() / 1000.0;
    c->fadeOut = fOut->value() / 1000.0;
    if (!isAudio) c->text = textEdit->text();
    update();
    emit modified();
}

void TimelineWidget::showEffectsDialog(Clip* c) {
    if (!c) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Efeitos de vídeo"));

    auto* bright = new QSlider(Qt::Horizontal, &dlg);
    bright->setRange(-100, 100);
    bright->setValue((int)llround(c->brightness * 100.0));
    auto* brightLabel = new QLabel(QString("%1").arg(bright->value()), &dlg);
    connect(bright, &QSlider::valueChanged, &dlg, [brightLabel](int v) {
        brightLabel->setText(QString("%1").arg(v));
    });
    auto* brightRow = new QHBoxLayout;
    brightRow->addWidget(bright, 1);
    brightRow->addWidget(brightLabel);

    auto* cont = new QSlider(Qt::Horizontal, &dlg);
    cont->setRange(0, 200);
    cont->setValue((int)llround(c->contrast * 100.0));
    auto* contLabel = new QLabel(QString("%1%").arg(cont->value()), &dlg);
    connect(cont, &QSlider::valueChanged, &dlg, [contLabel](int v) {
        contLabel->setText(QString("%1%").arg(v));
    });
    auto* contRow = new QHBoxLayout;
    contRow->addWidget(cont, 1);
    contRow->addWidget(contLabel);

    auto* sat = new QSlider(Qt::Horizontal, &dlg);
    sat->setRange(0, 200);
    sat->setValue((int)llround(c->saturation * 100.0));
    auto* satLabel = new QLabel(QString("%1%").arg(sat->value()), &dlg);
    connect(sat, &QSlider::valueChanged, &dlg, [satLabel](int v) {
        satLabel->setText(QString("%1%").arg(v));
    });
    auto* satRow = new QHBoxLayout;
    satRow->addWidget(sat, 1);
    satRow->addWidget(satLabel);

    auto* blur = new QSlider(Qt::Horizontal, &dlg);
    blur->setRange(0, 40);
    blur->setValue((int)llround(c->blur));
    auto* blurLabel = new QLabel(QString("%1").arg(blur->value()), &dlg);
    connect(blur, &QSlider::valueChanged, &dlg, [blurLabel](int v) {
        blurLabel->setText(QString("%1").arg(v));
    });
    auto* blurRow = new QHBoxLayout;
    blurRow->addWidget(blur, 1);
    blurRow->addWidget(blurLabel);

    auto* gray = new QCheckBox(tr("Preto e branco"), &dlg);
    gray->setChecked(c->grayscale);

    auto* ck = new QCheckBox(tr("Chroma key"), &dlg);
    ck->setChecked(c->chromaKey);
    auto* ckColor = new QPushButton(tr("Cor…"), &dlg);
    ckColor->setEnabled(c->chromaKey);
    connect(ck, &QCheckBox::toggled, ckColor, &QPushButton::setEnabled);
    QColor ckSelected = c->chromaKeyColor;
    connect(ckColor, &QPushButton::clicked, &dlg, [&dlg, &ckSelected]() {
        const QColor col = QColorDialog::getColor(ckSelected, &dlg, tr("Cor do chroma key"));
        if (col.isValid()) ckSelected = col;
    });
    auto* ckSim = new QSlider(Qt::Horizontal, &dlg);
    ckSim->setRange(0, 100);
    ckSim->setValue((int)llround(c->chromaKeySimilarity * 100.0));
    ckSim->setEnabled(c->chromaKey);
    connect(ck, &QCheckBox::toggled, ckSim, &QSlider::setEnabled);
    auto* ckSimLabel = new QLabel(QString("%1").arg(ckSim->value()), &dlg);
    connect(ckSim, &QSlider::valueChanged, &dlg, [ckSimLabel](int v) {
        ckSimLabel->setText(QString("%1").arg(v));
    });
    auto* ckSimRow = new QHBoxLayout;
    ckSimRow->addWidget(ckSim, 1);
    ckSimRow->addWidget(ckSimLabel);

    auto* form = new QFormLayout;
    form->addRow(tr("Brilho:"), brightRow);
    form->addRow(tr("Contraste:"), contRow);
    form->addRow(tr("Saturação:"), satRow);
    form->addRow(tr("Desfoque:"), blurRow);
    form->addRow(gray);
    form->addRow(ck, ckColor);
    form->addRow(tr("Similaridade:"), ckSimRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return;

    emit editStart();
    c->brightness = bright->value() / 100.0;
    c->contrast = cont->value() / 100.0;
    c->saturation = sat->value() / 100.0;
    c->blur = blur->value();
    c->grayscale = gray->isChecked();
    c->chromaKey = ck->isChecked();
    c->chromaKeyColor = ckSelected;
    c->chromaKeySimilarity = ckSim->value() / 100.0;
    update();
    emit modified();
}

void TimelineWidget::showTransformDialog(Clip* c) {
    if (!c) return;
    TransformDialog dlg(c, this);
    if (dlg.exec() != QDialog::Accepted) return;
    emit editStart();
    update();
    emit modified();
}

void TimelineWidget::removeClipsByIds(const QStringList& ids) {
    if (!m_project) return;
    auto eraseIn = [&ids](QVector<Clip>& clips) {
        clips.erase(std::remove_if(clips.begin(), clips.end(),
                                   [&ids](const Clip& c) { return ids.contains(c.id); }),
                    clips.end());
    };
    for (Track& t : m_project->videoTracks)
        eraseIn(t.clips);
    for (Track& t : m_project->audioTracks)
        eraseIn(t.clips);
}

void TimelineWidget::deleteSelected() {
    // Ripple: ao excluir, os clipes seguintes deslizam e fecham o espaço.
    if (!m_project || m_selected.isEmpty()) return;
    emit editStart();
    const QStringList sel = expandToGroups(m_selected);
    for (Track& t : m_project->videoTracks)
        rippleDeleteInTrack(t, sel);
    for (Track& t : m_project->audioTracks)
        rippleDeleteInTrack(t, sel);
    m_selected.clear();
    invalidateScene();
    updateScrollRanges();
    emit modified();
}

void TimelineWidget::deleteSelectedLeaveGap() {
    if (!m_project || m_selected.isEmpty()) return;
    emit editStart();
    removeClipsByIds(expandToGroups(m_selected));
    m_selected.clear();
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::deleteClipBeforePlayhead() {
    if (!m_project) return;
    const QStringList scope = m_selected.isEmpty() ? [this]() {
        QStringList all;
        for (const Track& t : m_project->videoTracks)
            for (const Clip& c : t.clips) all.append(c.id);
        for (const Track& t : m_project->audioTracks)
            for (const Clip& c : t.clips) all.append(c.id);
        return all;
    }() : m_selected;
    QStringList victims;
    double bestEnd = -1.0;
    for (const QString& id : scope) {
        Clip* c = findClipById(id);
        if (!c) continue;
        if (c->pos + c->dur <= m_playhead + 1e-6) {
            const double end = c->pos + c->dur;
            if (end > bestEnd + 1e-6) {
                bestEnd = end;
                victims = {id};
            } else if (std::fabs(end - bestEnd) < 1e-6) {
                victims.append(id);
            }
        }
    }
    if (victims.isEmpty()) return;
    emit editStart();
    removeClipsByIds(expandToGroups(victims));
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::deleteClipAfterPlayhead() {
    if (!m_project) return;
    const QStringList scope = m_selected.isEmpty() ? [this]() {
        QStringList all;
        for (const Track& t : m_project->videoTracks)
            for (const Clip& c : t.clips) all.append(c.id);
        for (const Track& t : m_project->audioTracks)
            for (const Clip& c : t.clips) all.append(c.id);
        return all;
    }() : m_selected;
    QStringList victims;
    double bestPos = 1e18;
    for (const QString& id : scope) {
        Clip* c = findClipById(id);
        if (!c) continue;
        if (c->pos >= m_playhead - 1e-6) {
            if (c->pos < bestPos - 1e-6) {
                bestPos = c->pos;
                victims = {id};
            } else if (std::fabs(c->pos - bestPos) < 1e-6) {
                victims.append(id);
            }
        }
    }
    if (victims.isEmpty()) return;
    emit editStart();
    removeClipsByIds(expandToGroups(victims));
    updateScrollRanges();
    update();
    emit modified();
}

bool TimelineWidget::isSelected(const QString& id) const {
    return m_selected.contains(id);
}

void TimelineWidget::setSelection(const QString& id) {
    m_selected.clear();
    m_selected.append(id);
    invalidateScene();
    emit selectionChanged(id);
}

void TimelineWidget::toggleSelection(const QString& id) {
    const int i = m_selected.indexOf(id);
    if (i >= 0) m_selected.removeAt(i);
    else m_selected.append(id);
    invalidateScene();
    emit selectionChanged(m_selected.isEmpty() ? QString() : m_selected.last());
}

double TimelineWidget::snapToEdges(double t, const QString& excludeId) const {
    if (!m_snap) return t;
    const double thresh = 8.0 / m_pps;
    double best = t;
    double bestD = thresh;
    auto consider = [&](double x) {
        const double d = std::fabs(x - t);
        if (d < bestD) { bestD = d; best = x; }
    };
    consider(0.0);
    consider(m_playhead);
    auto considerClip = [&](const Clip& c) {
        if (c.id == excludeId) return;
        consider(c.pos);
        consider(c.pos + c.dur);
    };
    if (m_project) {
        for (const Track& tr : m_project->videoTracks)
            for (const Clip& c : tr.clips)
                considerClip(c);
        for (const Track& tr : m_project->audioTracks)
            for (const Clip& c : tr.clips)
                considerClip(c);
    }
    return best;
}

double TimelineWidget::clampPosToTrack(Clip* c, double newPos,
                                       const QSet<QString>& moving) const {
    if (!m_project || !c) return newPos;
    newPos = std::max(0.0, newPos);
    const QVector<Clip>* clips = nullptr;
    auto findIn = [&](const QVector<Track>& tracks) {
        for (const Track& tr : tracks)
            for (const Clip& o : tr.clips)
                if (o.id == c->id) { clips = &tr.clips; return true; }
        return false;
    };
    if (findIn(m_project->videoTracks)) { /* ok */ }
    else findIn(m_project->audioTracks);
    if (!clips) return newPos;
    for (const Clip& o : *clips) {
        if (o.id == c->id || moving.contains(o.id)) continue;
        if (newPos < o.pos) {
            if (newPos + c->dur > o.pos)
                newPos = o.pos - c->dur;
        } else {
            if (newPos < o.pos + o.dur)
                newPos = o.pos + o.dur;
        }
    }
    return std::max(0.0, newPos);
}

double TimelineWidget::fitDurationInTrack(const Track& tr, double t, double dur,
                                          const QString& excludeId) const {
    double maxDur = dur;
    for (const Clip& o : tr.clips) {
        if (o.id == excludeId) continue;
        if (o.pos >= t) {
            if (o.pos < t + maxDur)
                maxDur = o.pos - t;
        } else if (t < o.pos + o.dur) {
            maxDur = std::min(maxDur, o.pos + o.dur - t);
        }
        if (maxDur <= 0.0) break;
    }
    return std::max(0.0, maxDur);
}

void TimelineWidget::showAudioEffectsDialog(Clip* c) {
    if (!c) return;
    AudioEffectsDialog dlg(c, this);
    if (dlg.exec() != QDialog::Accepted) return;
    emit editStart();
    update();
    emit modified();
}

void TimelineWidget::setTool(int tool) {
    if (tool == m_tool) return;
    m_tool = tool;
    invalidateScene();
    emit toolChanged(tool);
}

void TimelineWidget::setSnap(bool on) {
    m_snap = on;
    update();
}

void TimelineWidget::setLoopInAtPlayhead() {
    if (!m_project) return;
    m_loopIn = std::max(0.0, snapTime(m_playhead));
    if (m_loopOut > 0 && m_loopOut <= m_loopIn)
        std::swap(m_loopIn, m_loopOut);
    emit loopChanged(m_loopIn, m_loopOut);
    update();
}

void TimelineWidget::setLoopOutAtPlayhead() {
    if (!m_project) return;
    m_loopOut = std::max(0.0, snapTime(m_playhead));
    if (m_loopIn < 0.0) m_loopIn = 0.0;
    if (m_loopOut <= m_loopIn)
        std::swap(m_loopIn, m_loopOut);
    emit loopChanged(m_loopIn, m_loopOut);
    update();
}

void TimelineWidget::clearLoop() {
    m_loopIn = m_loopOut = -1.0;
    emit loopChanged(-1.0, -1.0);
    update();
}

QVector<Clip*> TimelineWidget::groupMembers(const QString& gid) {
    QVector<Clip*> out;
    if (gid.isEmpty() || !m_project) return out;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.groupId == gid) out.append(&c);
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.groupId == gid) out.append(&c);
    return out;
}

QStringList TimelineWidget::expandToGroups(const QStringList& ids) {
    QStringList out = ids;
    for (const QString& id : ids) {
        Clip* c = findClipById(id);
        if (!c || c->groupId.isEmpty()) continue;
        for (Clip* m : groupMembers(c->groupId))
            if (!out.contains(m->id)) out.append(m->id);
    }
    return out;
}

void TimelineWidget::razorSplitAt(double t) {
    if (!m_project) return;
    emit editStart();
    QStringList toSplit;
    auto consider = [&](const Clip& c) {
        if (t > c.pos + 1e-6 && t < c.pos + c.dur - 1e-6)
            toSplit.append(c.id);
    };
    for (Track& tr : m_project->videoTracks)
        for (const Clip& c : tr.clips) consider(c);
    for (Track& tr : m_project->audioTracks)
        for (const Clip& c : tr.clips) consider(c);
    // Coleta os alvos primeiro; depois divide. Não pode iterar o vetor de
    // clipes enquanto o split insere nele (iterator inválido).
    QStringList handled; // dedupe por grupo (vídeo+áudio vinculados)
    for (const QString& id : toSplit) {
        Clip* cc = findClipById(id);
        if (!cc) continue;
        const QString key = cc->groupId.isEmpty() ? cc->id : cc->groupId;
        if (handled.contains(key)) continue;
        handled.append(key);
        splitClipAt(cc, t);
    }
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::envelopePress(Clip* c, double t) {
    if (!c) return;
    emit editStart();
    const bool audio = trackOf(c) ? trackOf(c)->audio : false;
    QVector<Keyframe>& keys = audio ? c->kfVolume : c->kfOpacity;
    double& base = audio ? c->volume : c->opacity;
    // t é absoluto na timeline; keyframes são relativos ao clipe.
    const double rel = t - c->pos;
    for (int i = 0; i < keys.size(); ++i) {
        if (std::fabs(keys[i].time - rel) < 1e-3) {
            keys.removeAt(i);
            update();
            emit modified();
            return;
        }
    }
    Keyframe k;
    k.time = rel;
    k.value = kfValue(keys, base, rel);
    keys.append(k);
    std::sort(keys.begin(), keys.end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    update();
    emit modified();
}

void TimelineWidget::applyZoomRect(double t0, double t1) {
    if (t1 < t0) std::swap(t0, t1);
    const double span = std::max(0.1, t1 - t0);
    const int usable = width() - m_vbar->sizeHint().width() - kHeaderW;
    const double pps = usable > 0 ? (double)usable / span : m_pps;
    m_pps = std::clamp(pps, kMinPps, kMaxPps);
    m_viewStart = t0;
    m_hbar->setValue((int)(t0 * m_pps));
    updateScrollRanges();
    update();
}
