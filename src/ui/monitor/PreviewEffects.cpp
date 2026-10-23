// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/monitor/PreviewWidget.h"
#include <cstdint>

#include "ui/dialogs/SettingsDialog.h"
#include "ui/timeline/TlLog.h"
#include "colombina/CrashReporter.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/ffmpeg/AudioConformCache.h"
#include "colombina/audio/EqBiquad.h"
#include "colombina/ofx/OfxRenderer.h"
#include "colombina/ofx/OfxPluginManager.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "colombina/fx/ColorGrade.h"
#include "ui/monitor/ScopeWidget.h"
#include "colombina/export/LainkaFx.h"
#include "ui/core/Theme.h"
#include "ui/monitor/MonitorUtils.h"
#include "colombina/generators.h"
#include "colombina/render/TextRender.h"
// DSP dos efeitos de áudio (AudioFx/GraphicEq/ParametricEqDsp) e os helpers
// de diagnóstico/config do preview (playDbg/audioDbg/composeDbg/
// previewAudioFormat/desengasgaIntervalMs) — vidrados em
// headers próprios, compartilhados com FrameWorker/FramePipeline/AudioMixer.
#include "ui/audio/AudioFxDsp.h"
#include "ui/audio/AudioMixer.h"
#include "ui/monitor/PreviewDebug.h"
#include "colombina/ffmpeg/SourcePool.h"
#include "colombina/ffmpeg/FramePipeline.h"
#include "laartman/core/DecoderMetrics.h"
#include "ui/monitor/FrameWorker.h"
#include "ui/monitor/MonitorScrubBar.h"

#include <QPainter>
#include <QPixmap>
#include <QPolygon>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QLabel>
#include <QComboBox>
#include <QSignalBlocker>
#include <QPointer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QShortcut>
#include <QIODevice>
#include <QAudioFormat>
#include <QPainterPath>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSettings>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif
#include <algorithm>
#include <atomic>
#include <QDebug>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <utility>
#include <QHash>
#include <QMutex>


#include "ui/monitor/PreviewPriv.h"
using namespace LainkaFx;

// Aplica pan/crop sobre um quadro e devolve o recorte.
QImage PreviewWidget::applyCropTo(const QImage& img, int cL, int cR, int cT, int cB) {
    const double w = img.width();
    const double h = img.height();
    if (w <= 1 || h <= 1 || (!cL && !cR && !cT && !cB)) return img;
    const int x = (int)std::lround(w * cL / 1000.0);
    const int y = (int)std::lround(h * cT / 1000.0);
    const int cw = (int)std::lround(w * (1.0 - (cL + cR) / 1000.0));
    const int ch = (int)std::lround(h * (1.0 - (cT + cB) / 1000.0));
    return img.copy(QRect(std::clamp(x, 0, (int)w - 1), std::clamp(y, 0, (int)h - 1),
                          qMin(cw, (int)w - std::clamp(x, 0, (int)w - 1)),
                          qMin(ch, (int)h - std::clamp(y, 0, (int)h - 1))));
}

// Aplica efeitos básicos (brilho, contraste, saturação, desfoque, preto e branco, chroma key)
// sobre o quadro do clipe ativo. Chamado após o crop em applyCrop().
void PreviewWidget::applyBasicEffects(QImage& img) {
    if (img.isNull()) return;
    Clip c;
    c.brightness = m_clipBrightness;
    c.contrast = m_clipContrast;
    c.saturation = m_clipSaturation;
    c.blur = m_clipBlur;
    c.grayscale = m_clipGrayscale;
    c.chromaKey = m_clipChromaKey;
    c.chromaKeyColor = m_clipChromaKeyColor;
    c.chromaKeySimilarity = m_clipChromaKeySimilarity;
    c.chromaKeySoftness = m_clipChromaKeySoftness;
    c.chromaKeySpillSuppress = m_clipChromaKeySpillSuppress;
    c.psxEnabled = m_clipPsxEnabled;
    c.psxDither = m_clipPsxDither;
    c.psxBits = m_clipPsxBits;
    c.filmGrain = m_clipFilmGrain;
    c.filmGrainAmount = m_clipFilmGrainAmount;
    c.filmGrainSize = m_clipFilmGrainSize;
    // Máscaras do clipe do topo: aplicadas na mesma ordem (início) e espaço
    // (quadro já cortado) que nas camadas inferiores — o topo também respeita
    // o recorte por forma. `m_lastSrcT` é o tempo relativo do clipe.
    c.masks = m_clipMasks;
    applyBasicEffectsOn(img, c, m_lastSrcT);
}

// Aplica as máscaras do clipe ao quadro: multiplica o alpha pela cobertura da
// forma (rect/ellipse), com feather (borda suave por distância assinada) e
// invert, combinadas em união. Avalia os keyframes em `rel` (tempo relativo).
// Warp de malha 3×3 (espremer/esticar a mídia). Os 9 pontos são as posições
// de DESTINO dos vértices da grade regular (2×2 células). Para cada pixel de
// destino inverte o mapeamento bilinear (Newton) e amostra o fonte.
static bool meshIsIdentity(const QVector<QPointF>& mesh, int cols, int rows) {
    const double eps = 1e-4;
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            const QPointF& p = mesh[cols * j + i];
            if (std::abs(p.x() - i / double(cols - 1)) > eps
                || std::abs(p.y() - j / double(rows - 1)) > eps)
                return false;
        }
    }
    return true;
}

static void warpByMesh(QImage& img, const QVector<QPointF>& mesh, int cols, int rows) {
    if (cols < 2 || rows < 2 || mesh.size() != cols * rows) return;
    const int w = img.width(), h = img.height();
    if (w < 2 || h < 2) return;
    // Grade regular (padrão) = identidade: pula o warp inteiro. Sem isso, todo
    // clipe com malha ativa rodava Newton por pixel sem alterar a imagem —
    // causava BLOQUEIO de segundos na UI e pressão de memória (4GB peak).
    if (meshIsIdentity(mesh, cols, rows)) return;
    const QImage src = img.convertToFormat(QImage::Format_ARGB32);
    if (src.isNull()) return;
    QImage dst(w, h, QImage::Format_ARGB32);
    if (dst.isNull()) return;
    dst.fill(Qt::transparent);

    auto S = [&](int i, int j) { // vértice regular (fonte) em px
        return QPointF(i * (w - 1) / (cols - 1.0), j * (h - 1) / (rows - 1.0));
    };
    auto D = [&](int i, int j) { // ponto de controle (destino) em px
        const QPointF& p = mesh[cols * j + i];
        return QPointF(std::clamp(p.x(), -0.5, 1.5) * (w - 1),
                       std::clamp(p.y(), -0.5, 1.5) * (h - 1));
    };
    auto bil = [](const QPointF& P00, const QPointF& P10, const QPointF& P11,
                  const QPointF& P01, double u, double v) {
        return (1 - v) * ((1 - u) * P00 + u * P10) + v * ((1 - u) * P01 + u * P11);
    };

    for (int cj = 0; cj < rows - 1; ++cj) {
        for (int ci = 0; ci < cols - 1; ++ci) {
            const QPointF P00 = D(ci, cj), P10 = D(ci + 1, cj);
            const QPointF P11 = D(ci + 1, cj + 1), P01 = D(ci, cj + 1);
            const double minX = std::min(std::min(P00.x(), P10.x()), std::min(P11.x(), P01.x()));
            const double maxX = std::max(std::max(P00.x(), P10.x()), std::max(P11.x(), P01.x()));
            const double minY = std::min(std::min(P00.y(), P10.y()), std::min(P11.y(), P01.y()));
            const double maxY = std::max(std::max(P00.y(), P10.y()), std::max(P11.y(), P01.y()));
            const int x0 = std::max(0, (int)std::floor(minX));
            const int x1 = std::min(w - 1, (int)std::ceil(maxX));
            const int y0 = std::max(0, (int)std::floor(minY));
            const int y1 = std::min(h - 1, (int)std::ceil(maxY));
            for (int y = y0; y <= y1; ++y) {
                QRgb* dline = reinterpret_cast<QRgb*>(dst.scanLine(y));
                for (int x = x0; x <= x1; ++x) {
                    const QPointF q(x, y);
                    double u = 0.5, v = 0.5;
                    for (int it = 0; it < 10; ++it) {
                        const QPointF cur = bil(P00, P10, P11, P01, u, v);
                        const QPointF e = cur - q;
                        if (std::abs(e.x()) < 0.3 && std::abs(e.y()) < 0.3) break;
                        const QPointF dPdu = (1 - v) * (P10 - P00) + v * (P11 - P01);
                        const QPointF dPdv = (1 - u) * (P01 - P00) + u * (P11 - P10);
                        const double det = dPdu.x() * dPdv.y() - dPdu.y() * dPdv.x();
                        if (std::abs(det) < 1e-9) break;
                        u += (-e.x() * dPdv.y() + e.y() * dPdv.x()) / det;
                        v += (-dPdu.x() * e.y() + dPdu.y() * e.x()) / det;
                        u = std::clamp(u, 0.0, 1.0);
                        v = std::clamp(v, 0.0, 1.0);
                    }
                    if (u < -0.001 || u > 1.001 || v < -0.001 || v > 1.001) continue;
                    const QPointF s = bil(S(ci, cj), S(ci + 1, cj),
                                          S(ci + 1, cj + 1), S(ci, cj + 1), u, v);
                    const int sx = std::clamp((int)std::lround(s.x()), 0, w - 1);
                    const int sy = std::clamp((int)std::lround(s.y()), 0, h - 1);
                    dline[x] = reinterpret_cast<const QRgb*>(src.constScanLine(sy))[sx];
                }
            }
        }
    }
    img = dst;
}

void PreviewWidget::applyMasks(QImage& img, const Clip& c, double rel) {
    const int w = img.width();
    const int h = img.height();
    if (w < 1 || h < 1) return;
    // A máscara modifica o canal alpha. Se a fonte não tem alpha por byte
    // (vídeo decodificado em RGB32), converte — antes retornava e a máscara
    // simplesmente não aparecia no preview.
    if (img.format() != QImage::Format_ARGB32
        && img.format() != QImage::Format_ARGB32_Premultiplied)
        img = img.convertToFormat(QImage::Format_ARGB32);

    std::vector<float> cov((size_t)w * (size_t)h, 0.0f);
    for (const Mask& m : c.masks) {
        if (!m.hasMask()) continue;
        if (m.type != QLatin1String("rect") && m.type != QLatin1String("ellipse"))
            continue; // poly: v1 não exporta ainda — ignorar p/ manter paridade
        const double cx = m.cxAt(rel) * w;
        const double cy = m.cyAt(rel) * h;
        const double rx = std::max(0.001, m.rxAt(rel) * w);
        const double ry = std::max(0.001, m.ryAt(rel) * h);
        const double rotRad = m.rotAt(rel) * M_PI / 180.0;
        const double cosR = std::cos(-rotRad);
        const double sinR = std::sin(-rotRad);
        const double fp = m.featherAt(rel) * (double)std::max(w, h);
        const bool invert = m.invert;
        const bool ellipse = (m.type == QLatin1String("ellipse"));

        for (int y = 0; y < h; ++y) {
            const double ly0 = y - cy;
            size_t idx = (size_t)y * w;
            for (int x = 0; x < w; ++x, ++idx) {
                const double lx0 = x - cx;
                const double lx = lx0 * cosR - ly0 * sinR;
                const double ly = lx0 * sinR + ly0 * cosR;
                double d;
                if (ellipse) {
                    const double n = (lx * lx) / (rx * rx) + (ly * ly) / (ry * ry);
                    d = (std::sqrt(std::max(0.0, n)) - 1.0) * std::min(rx, ry);
                } else {
                    d = std::max(std::abs(lx) - rx, std::abs(ly) - ry);
                }
                double v = (fp > 0.5) ? std::clamp(0.5 - d / fp, 0.0, 1.0)
                                      : ((d < 0.0) ? 1.0 : 0.0);
                if (invert) v = 1.0 - v;
                if (v > cov[idx]) cov[idx] = (float)v;
            }
        }
    }

    // Multiplica o alpha do quadro pela cobertura acumulada. (Em imagens
    // premultiplied, só o alpha muda; o RGB já guarda cor · alpha.)
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        const float* cv = &cov[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            uchar& a = line[x * 4 + 3];
            a = (uchar)std::lround(a * cv[x]);
        }
    }
}

// ── Grão de filme: máscaras pré-geradas ──────────────────────────────
// Em vez de calcular hash + lround + clamp por pixel a cada quadro (caro), o
// ruído é pré-renderizado em tiles pequenos (Grayscale8, valor = 128 + offset)
// guardados num cache. Aplicar o grão vira uma soma/subtração de bytes com wrap
// do tile — O(W·H) mas sem ramificações nem ponto flutuante por pixel. As
// variantes animam o grão (o índice muda por quadro); o amount só seleciona o
// nível pré-escalado (offsets multiplicados), então a intensidade não custa nada
// em runtime.
namespace {
constexpr int kGrainTile = 128;      // lado do tile em pixels
constexpr int kGrainVariants = 24;   // texturas distintas para a animação
constexpr int kGrainAmountLevels = 16; // quantização do amount (0..16)

struct GrainMaskKey {
    int variant = 0;
    int amountLevel = 0;
    bool operator==(const GrainMaskKey& o) const {
        return variant == o.variant && amountLevel == o.amountLevel;
    }
};

// Gera um tile de ruído monocromático (1 pixel de grão): offset em
// [-maxOff, +maxOff] escalado por amountLevel/16, codificado como 128+offset
// (centro neutro). O tamanho do grão é aplicado na leitura (subamostragem), então
// o mesmo tile serve para qualquer `size`.
inline QImage makeGrainTile(int variant, int amountLevel)
{
    QImage tile(kGrainTile, kGrainTile, QImage::Format_Grayscale8);
    const double maxOff = 255.0 * 0.5; // desvio máximo (amount = 1)
    const double scale = (double)amountLevel / (double)kGrainAmountLevels;
    uint32_t state = (uint32_t)(variant * 2654435761u + 0x9E3779B9u);
    auto next = [&state]() {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return state;
    };
    for (int y = 0; y < kGrainTile; ++y) {
        uchar* line = tile.scanLine(y);
        for (int x = 0; x < kGrainTile; ++x) {
            const double n = ((double)(next() & 0xFFFFu) / 32767.5) - 1.0;
            line[x] = (uchar)std::clamp(
                (int)std::lround(128 + n * maxOff * scale), 0, 255);
        }
    }
    return tile;
}

// Cache thread-safe das máscaras (o grão roda na thread do worker de vídeo e,
// com LAINKA, na UI thread). Retorna por valor: QImage é COW, então a cópia
// compartilha os bytes — e evita referência invalidada por realocação.
QImage grainMask(int variant, int amountLevel)
{
    static QMutex mtx;
    static std::vector<std::pair<GrainMaskKey, QImage>> cache;
    QMutexLocker lock(&mtx);
    for (auto& e : cache)
        if (e.first.variant == variant && e.first.amountLevel == amountLevel)
            return e.second;
    if (cache.size() >= 256) cache.erase(cache.begin());
    cache.emplace_back(GrainMaskKey{variant, amountLevel},
                       makeGrainTile(variant, amountLevel));
    return cache.back().second;
}
} // namespace

void PreviewWidget::applyBasicEffectsOn(QImage& img, const Clip& c, double rel) {
    if (img.isNull()) return;

    // Malha 3×3 (warp): espreime/estica a mídia antes das máscaras.
    if (c.hasMesh())
        warpByMesh(img, c.mesh, c.meshCols, c.meshRows);

    // Máscaras: recorta o quadro por forma (rect/ellipse), com feather e
    // invert, em união. Rodam antes do chroma/desfoque/cor para que esses
    // efeitos respeitem a área visível.
    if (c.hasMask())
        applyMasks(img, c, rel);

    // Chroma Key robusto: distância perceptual ponderada, bordas suaves,
    // supressão de spill e curva de transição suave (smoothstep).
    if (c.chromaKey) {
        QImage result = ImgPool::get(QImage::Format_ARGB32, img.width(), img.height());
        result.fill(Qt::transparent);
        const int w = img.width();
        const int h = img.height();
        const QColor key = c.chromaKeyColor;
        const double sim = std::clamp(c.chromaKeySimilarity, 0.0, 1.0);
        const double soft = std::clamp(c.chromaKeySoftness, 0.0, 1.0);
        const double spill = std::clamp(c.chromaKeySpillSuppress, 0.0, 1.0);
        const int kr = key.red(), kg = key.green(), kb = key.blue();

        // Pesos perceptuais (BT.601): olho humano mais sensível ao verde.
        constexpr double wR = 0.299, wG = 0.587, wB = 0.114;

        // Faixa total. A tolerância da borda é ABSOLUTA (distância perceptual),
        // não proporcional à similaridade: com o mapeamento relativo anterior
        // (core * (1 + soft*3)) a rampa default ficava com ~11 de 255 — corte
        // duro — e aumentar a similaridade estrangulava a suavidade.
        const double coreRange = sim * 255.0;
        const double fullRange = coreRange + soft * 255.0;
        const double fullSq = fullRange * fullRange;

        // Canal dominante da cor-chave: define qual componente o spill remove.
        // Antes isto era fixo em verde, então o controle não fazia nada em
        // fundo azul/vermelho. Ties (ciano, magenta, amarelo, cinza) ficam de
        // fora — sem canal dominante definido não há respingo a atribuir.
        const int keyCh = (kg >= kr && kg >= kb) ? 1 : (kb >= kr ? 2 : 0);
        const int keyVals[3] = { kr, kg, kb };
        const bool keyDominates = keyVals[keyCh] > keyVals[(keyCh + 1) % 3]
                               && keyVals[keyCh] > keyVals[(keyCh + 2) % 3];

        for (int y = 0; y < h; ++y) {
            const uchar* src = img.constScanLine(y);
            uchar* dst = result.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                const int pr = src[si + 0], pg = src[si + 1], pb = src[si + 2];
                const int dr = pr - kr, dg = pg - kg, db = pb - kb;

                // Distância perceptual ponderada (evita sqrt no loop principal).
                const double distSq = wR * dr * dr + wG * dg * dg + wB * db * db;

                if (distSq >= fullSq) {
                    // Fora da faixa: pixel opaco.
                    dst[si + 0] = pr;
                    dst[si + 1] = pg;
                    dst[si + 2] = pb;
                    dst[si + 3] = 255;
                } else {
                    // Dentro da faixa: alpha com transição suave.
                    const double dist = std::sqrt(distSq);
                    double alpha;
                    if (dist <= coreRange) {
                        alpha = 0.0; // match total → totalmente transparente
                    } else {
                        // Curva smoothstep na borda (mais suave que linear).
                        const double t = (dist - coreRange) / (fullRange - coreRange);
                        const double s = t * t * (3.0 - 2.0 * t); // smoothstep
                        alpha = s * 255.0;
                    }

                    dst[si + 0] = pr;
                    dst[si + 1] = pg;
                    dst[si + 2] = pb;

                    // Supressão de spill: remove o respingo da cor-chave nos
                    // pixels da borda (foreground contaminado pela luz do fundo).
                    if (spill > 0.0 && keyDominates) {
                        const int pv[3] = { pr, pg, pb };
                        const int otherA = pv[(keyCh + 1) % 3];
                        const int otherB = pv[(keyCh + 2) % 3];
                        // Só há respingo onde a cor-chave ainda domina o pixel.
                        // O teste antigo era `pixLum > keyLum`, que descartava
                        // justamente os pixels escuros contaminados (cabelo
                        // preto, sombra) — os casos mais visíveis.
                        if (pv[keyCh] > otherA && pv[keyCh] > otherB) {
                            // Peso em sino: o respingo é visível nos pixels de
                            // borda, meio transparentes. `1 - alpha/255` era
                            // máximo onde o pixel é invisível e zero onde é
                            // opaco — invertido em relação à visibilidade.
                            const double a = alpha / 255.0;
                            const double bell = 4.0 * a * (1.0 - a);
                            const double spillAmt = spill * (0.35 + 0.65 * bell);
                            // Despill clássico: puxa o canal da chave em direção
                            // à média dos outros dois. Com spillAmt = 1 o canal
                            // vira essa média; valores menores corrigem em parte.
                            const int replacement = (otherA + otherB) / 2;
                            const int corrected = (int)std::lround(
                                pv[keyCh] - spillAmt * (pv[keyCh] - replacement));
                            dst[si + keyCh] = (uchar)std::clamp(corrected, 0, 255);
                        }
                    }
                    dst[si + 3] = (uchar)std::clamp((int)std::lround(alpha), 0, 255);
                }
            }
        }
        ImgPool::release(img);
        img = result;
    }

    // Desfoque: box blur separável com sliding window — O(W·H) independente do raio.
    if (c.blur > 0.5) {
        const int radius = std::clamp((int)std::lround(c.blur * 0.5), 1, 30);
        const int w = img.width();
        const int h = img.height();

        // Buffers estáticos por thread (reutilizados entre frames).
        static thread_local std::vector<int> s_rowR, s_rowG, s_rowB, s_rowA;
        if ((int)s_rowR.size() < w) {
            s_rowR.resize(w);
            s_rowG.resize(w);
            s_rowB.resize(w);
            s_rowA.resize(w);
        }

        // Passada horizontal com sliding window: resultado em temp (buffer do
        // pool, evitando alocar uma imagem cheia por quadro).
        QImage temp = ImgPool::get(img.format(), w, h);
        for (int y = 0; y < h; ++y) {
            const uchar* src = img.constScanLine(y);
            uchar* dst = temp.scanLine(y);

            // Inicializa a soma da janela [0, radius].
            int sr = 0, sg = 0, sb = 0, sa = 0;
            const int initEnd = std::min(radius, w - 1);
            for (int sx = 0; sx <= initEnd; ++sx) {
                const int si = sx * 4;
                sr += src[si]; sg += src[si + 1]; sb += src[si + 2]; sa += src[si + 3];
            }
            int cnt = initEnd + 1;

            for (int x = 0; x < w; ++x) {
                // Escreve o pixel médio.
                const int di = x * 4;
                dst[di]     = (uchar)(sr / cnt);
                dst[di + 1] = (uchar)(sg / cnt);
                dst[di + 2] = (uchar)(sb / cnt);
                dst[di + 3] = (uchar)(sa / cnt);

                // Sliding window: adiciona o pixel que entra, remove o que sai.
                const int addX = x + radius + 1;
                const int remX = x - radius;
                if (addX < w) {
                    const int ai = addX * 4;
                    sr += src[ai]; sg += src[ai + 1]; sb += src[ai + 2]; sa += src[ai + 3];
                    ++cnt;
                }
                if (remX >= 0) {
                    const int ri = remX * 4;
                    sr -= src[ri]; sg -= src[ri + 1]; sb -= src[ri + 2]; sa -= src[ri + 3];
                    --cnt;
                }
            }
        }

        // Passada vertical com sliding window: resultado de volta em img.
        int* rowR = s_rowR.data();
        int* rowG = s_rowG.data();
        int* rowB = s_rowB.data();
        int* rowA = s_rowA.data();

        // Inicializa as somas com a banda [0, radius] para cada coluna.
        {
            const uchar* firstRow = temp.constScanLine(0);
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                rowR[x] = firstRow[si];
                rowG[x] = firstRow[si + 1];
                rowB[x] = firstRow[si + 2];
                rowA[x] = firstRow[si + 3];
            }
        }
        const int initEndV = std::min(radius, h - 1);
        for (int sy = 1; sy <= initEndV; ++sy) {
            const uchar* src = temp.constScanLine(sy);
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                rowR[x] += src[si];
                rowG[x] += src[si + 1];
                rowB[x] += src[si + 2];
                rowA[x] += src[si + 3];
            }
        }
        int cntV = initEndV + 1;

        for (int y = 0; y < h; ++y) {
            uchar* dst = img.scanLine(y);

            // Escreve pixel médio para esta linha.
            for (int x = 0; x < w; ++x) {
                const int di = x * 4;
                dst[di]     = (uchar)(rowR[x] / cntV);
                dst[di + 1] = (uchar)(rowG[x] / cntV);
                dst[di + 2] = (uchar)(rowB[x] / cntV);
                dst[di + 3] = (uchar)(rowA[x] / cntV);
            }

            // Sliding window: adiciona a linha que entra, remove a que sai.
            const int addY = y + radius + 1;
            const int remY = y - radius;
            if (addY < h) {
                const uchar* addRow = temp.constScanLine(addY);
                for (int x = 0; x < w; ++x) {
                    const int si = x * 4;
                    rowR[x] += addRow[si];
                    rowG[x] += addRow[si + 1];
                    rowB[x] += addRow[si + 2];
                    rowA[x] += addRow[si + 3];
                }
                ++cntV;
            }
            if (remY >= 0) {
                const uchar* remRow = temp.constScanLine(remY);
                for (int x = 0; x < w; ++x) {
                    const int si = x * 4;
                    rowR[x] -= remRow[si];
                    rowG[x] -= remRow[si + 1];
                    rowB[x] -= remRow[si + 2];
                    rowA[x] -= remRow[si + 3];
                }
                --cntV;
            }
        }
        ImgPool::release(temp);
    }

    // Preto e branco.
    if (c.grayscale) {
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                const int si = x * 4;
                const int gray = (int)std::lround(0.299 * line[si + 2] + 0.587 * line[si + 1] + 0.114 * line[si]);
                line[si + 0] = line[si + 1] = line[si + 2] = (uchar)std::clamp(gray, 0, 255);
            }
        }
    }

    // Brilho, contraste, saturação.
    const bool needEq = (c.brightness != 0.0 || c.contrast != 1.0 || c.saturation != 1.0);
    if (needEq) {
        const double br = std::clamp(c.brightness, -1.0, 1.0) * 255.0;
        const double ct = std::clamp(c.contrast, 0.0, 2.0);
        const double factor = (ct == 1.0) ? 1.0 : (259.0 * (ct * 255.0 + 255.0)) / (255.0 * (259.0 - ct * 255.0));
        const double sa = std::clamp(c.saturation, 0.0, 2.0);
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                const int si = x * 4;
                double r = line[si + 2], g = line[si + 1], b = line[si + 0];
                // Brilho
                r += br; g += br; b += br;
                // Contraste
                r = factor * (r - 128.0) + 128.0;
                g = factor * (g - 128.0) + 128.0;
                b = factor * (b - 128.0) + 128.0;
                // Saturação
                if (sa != 1.0) {
                    const double gray = 0.299 * r + 0.587 * g + 0.114 * b;
                    r = gray + sa * (r - gray);
                    g = gray + sa * (g - gray);
                    b = gray + sa * (b - gray);
                }
                line[si + 2] = (uchar)std::clamp((int)std::lround(r), 0, 255);
                line[si + 1] = (uchar)std::clamp((int)std::lround(g), 0, 255);
                line[si + 0] = (uchar)std::clamp((int)std::lround(b), 0, 255);
            }
        }
    }
// Color grade estilo Lumetri (LGG + exposure/curves/LUT/vignette…).
    // Ordem alinhada ao ProjectExporter (colorgrade::ffmpegFilters).
    if (c.hasColorGrade()) {
        colorgrade::CubeLut lut;
        const bool hasLut = !c.cgLutPath.isEmpty()
            && colorgrade::loadCubeFile(c.cgLutPath, lut);
        colorgrade::applyToImage(img, c, hasLut ? &lut : nullptr);
    }

    // PSX: quantização de cor para profundidade baixa + dithering ordenado
    // (padrão Bayer 4x4) — o "granulado" característico do PlayStation 1.
    // O desempenho é importante (loop por pixel na thread da UI), então só o
    // passo de quantização + o deslocamento determinístico do padrão.
    if (c.psxEnabled) {
        const int bits = std::clamp(c.psxBits, 3, 8);
        const double step = 255.0 / (1 << bits);
        const double dith = std::clamp(c.psxDither, 0.0, 1.0) * step;
        // Bayer 4x4 clássico (0..15) → offset -0.5..0.5 do passo.
        static const int bayer[16] = {
            0, 8, 2, 10,
            12, 4, 14, 6,
            3, 11, 1, 9,
            15, 7, 13, 5
        };
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            const int by = (y & 3) * 4;
            for (int x = 0; x < img.width(); ++x) {
                const int si = x * 4;
                const double off = (bayer[by + (x & 3)] - 7.5) / 15.0 * dith;
                for (int ch = 0; ch < 3; ++ch) {
                    double v = line[si + ch] + off;
                    v = std::lround(std::clamp(v, 0.0, 255.0) / step) * step;
                    line[si + ch] = (uchar)std::clamp((int)std::lround(v), 0, 255);
                }
            }
        }
    }

    // Grão de filme: ruído monocromático animado por quadro. A textura vem de um
    // tile pré-gerado (grainMask) cuja variante muda a cada quadro; aplicar é só
    // somar (offset mantido em 128 → subtrai-se 128) nos três canais, com wrap.
    if (c.filmGrain && c.filmGrainAmount > 0.0) {
        const int w = img.width();
        const int h = img.height();
        const int size = std::clamp(c.filmGrainSize, 1, 4);
        const double amount = std::clamp(c.filmGrainAmount, 0.0, 1.0);
        const int amountLevel = std::clamp((int)std::lround(amount * kGrainAmountLevels),
                                           1, kGrainAmountLevels);
        // Variante por quadro: muda rápido o bastante para "ferver" (usa o tempo
        // relativo em ~fps), mas estável em replays (determinístico por tempo).
        const int variant = (int)(((int64_t)std::llround(rel * 48.0)) % kGrainVariants
                                  + kGrainVariants) % kGrainVariants;
        const QImage mask = grainMask(variant, amountLevel);
        const int mw = mask.width();
        const int mh = mask.height();
        // Caminha por blocos de `size` px: o índice na máscara só avança quando
        // cruza um bloco, então não há divisão/módulo por pixel.
        for (int y = 0; y < h; ++y) {
            uchar* line = img.scanLine(y);
            const uchar* mline = mask.constScanLine((y / size) % mh);
            int mx = 0;
            int col = 0;
            for (int x = 0; x < w; ++x) {
                if (col == size) { col = 0; if (++mx == mw) mx = 0; }
                ++col;
                const int off = (int)mline[mx] - 128;
                if (off == 0) continue;
                const int si = x * 4;
                line[si + 0] = (uchar)std::clamp(line[si + 0] + off, 0, 255);
                line[si + 1] = (uchar)std::clamp(line[si + 1] + off, 0, 255);
                line[si + 2] = (uchar)std::clamp(line[si + 2] + off, 0, 255);
            }
        }
    }
}

// Aplica pan/crop sobre o quadro cheio (m_frameFull) e guarda em m_frame.
void PreviewWidget::applyMotionOn(QImage& img, const Clip& c) {
    if (img.isNull() || !c.motionEnabled || c.motionAmount <= 0.0) return;
    const double rad = c.motionAngle * M_PI / 180.0;
    const double dist = c.motionAmount * 0.4;
    const int sx = (int)std::lround(std::cos(rad) * dist);
    const int sy = (int)std::lround(std::sin(rad) * dist);
    const int ns = std::clamp(c.motionSamples, 1, 32);
    QImage result = ImgPool::get(img.format(), img.width(), img.height());
    {
        QPainter p(&result);
        p.drawImage(0, 0, img);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        for (int i = 1; i < ns; ++i) {
            const double alpha = 1.0 - (double)i / ns;
            p.setOpacity(alpha / ns);
            p.drawImage(sx * i, sy * i, img);
        }
    }
    ImgPool::release(img);
    img = result;
}

void PreviewWidget::applyTemporalFxOn(QImage& img, const Clip& c, double absTime, double rel,
                                      OfxPluginManager* ofxMgr, Frei0rPluginManager* frei0rMgr) {
    if (img.isNull()) return;
    applyMotionOn(img, c);
    if (!c.ofxFx.isEmpty() && ofxMgr)
        img = OfxRenderer::applyOfxEffects(img, c.ofxFx, ofxMgr, absTime);
    if (!c.frei0rFx.isEmpty() && frei0rMgr)
        img = Frei0rPluginManager::applyEffects(img, c.frei0rFx, frei0rMgr, rel);
}

void PreviewWidget::applyCrop() {
    // Memo LAINKA: em stop motion o tempo quantizado é constante por N quadros,
    // mas esta cadeia inteira é cara e síncrona na thread da UI (o warp 8x8 são
    // 64 transforms de tile e o MotiOn desenha o quadro N vezes com
    // SmoothPixmapTransform). Como nada aqui depende do tempo NÃO quantizado —
    // o jitter/flicker/warp usam m_lainkaQuantizedTime e as máscaras usam
    // m_lastSrcT, que também é o tempo quantizado — o resultado de um quadro
    // vale para os N-1 seguintes.
    //
    // Duas exceções, ambas com saída dependente do histórico/relógio:
    //  - onion skin e o MotiOn do LAINKA consomem `prevFrame` (o quadro
    //    anterior), que deixa de ser atualizado enquanto o memo acerta;
    //  - OFX recebe m_playhead cru e pode variar a cada quadro.
    // O crop entra na chave porque os keyframes dele são avaliados no tempo
    // NÃO quantizado: com crop animado o recorte anda dentro da janela de stop
    // motion mesmo com o tempo quantizado parado.
    const bool memoOk = m_clipLainkaEnabled
                        && m_clipOfxFx.isEmpty()
                        && m_clipLainkaOnionSkin < 1e-6
                        && m_clipLainkaMotionBlur < 1e-6;
    if (memoOk && !m_cropMemo.isNull() && m_cropMemoClipId == m_clipLainkaId
        && m_cropMemoFile == m_lastFile
        && m_cropMemoTime == m_lainkaQuantizedTime
        && m_cropMemoSrcT == m_lastSrcT
        && m_cropMemoSrcW == m_lastDecodeW
        && m_cropMemoCropL == m_lastCropL && m_cropMemoCropR == m_lastCropR
        && m_cropMemoCropT == m_lastCropT && m_cropMemoCropB == m_lastCropB) {
        m_frame = m_cropMemo;
        return;
    }

    m_frame = applyCropTo(m_frameFull, m_lastCropL, m_lastCropR, m_lastCropT, m_lastCropB);
    // Aplica efeito LAINKA (stop motion) após o crop.
    if (m_clipLainkaEnabled) {
        QImage prevFrame = m_lainkaPrevFrame;
        m_lainkaPrevFrame = m_frame;
        // Usa tempo quantizado para que jitter/flicker/warp fiquem
        // congelados no mesmo intervalo de stop motion.
        const double fxT = (m_lainkaQuantizedTime >= 0.0)
            ? m_lainkaQuantizedTime : m_playhead;
        m_frame = lainkaApplyFx(m_frame, m_clipLainkaId, fxT,
                                m_clipLainkaSkip, m_clipLainkaJitterPos,
                                m_clipLainkaJitterRot, m_clipLainkaJitterScale,
                                m_clipLainkaFlicker, m_clipLainkaFlickerSpeed,
                                m_clipLainkaWarpAmount, m_clipLainkaWarpSpeed,
                                m_clipLainkaWarpGrid, m_clipLainkaOnionSkin,
                                m_clipLainkaDustAmount, m_clipLainkaScratchAmount,
                                m_clipLainkaMotionBlur, m_clipLainkaOpacity,
                                m_clipLainkaTargetFps, m_clipLainkaAntialias,
                                prevFrame);
    }
    if (m_clipMotionEnabled && m_clipMotionAmount > 0.0 && !m_frame.isNull()) {
        Clip motionSnap;
        motionSnap.motionEnabled = true;
        motionSnap.motionAmount = m_clipMotionAmount;
        motionSnap.motionAngle = m_clipMotionAngle;
        motionSnap.motionSamples = m_clipMotionSamples;
        applyMotionOn(m_frame, motionSnap);
    }
    // Aplica efeitos básicos (brilho, contraste, saturação, etc.)
    applyBasicEffects(m_frame);

    // MotiOn/OFX/frei0r — mesma pilha do worker (applyTemporalFxOn). O tempo
    // do frei0r é o relativo do clipe do topo (paridade com o fast path).
    {
        Clip temporal;
        temporal.motionEnabled = false; // MotiOn já aplicado acima (snapshot)
        temporal.ofxFx = m_clipOfxFx;
        temporal.frei0rFx = m_clipFrei0rFx;
        double f0rRel = m_lastSrcT;
        if (m_project && !m_clipLainkaId.isEmpty()) {
            for (const Track& tr : m_project->videoTracks) {
                for (const Clip& c : tr.clips) {
                    if (c.id == m_clipLainkaId) {
                        f0rRel = m_playhead - c.pos;
                        break;
                    }
                }
            }
        }
        if (!temporal.ofxFx.isEmpty() && m_ofxManager && !m_frame.isNull())
            m_frame = OfxRenderer::applyOfxEffects(m_frame, temporal.ofxFx, m_ofxManager,
                                                   m_playhead);
        // Efeitos frei0r (padrão Kdenlive/Shotcut) — depois dos OFX; export
        // usa o mesmo filtro ffmpeg `frei0r=` (paridade preview↔export).
        if (!temporal.frei0rFx.isEmpty() && m_frei0rManager && !m_frame.isNull())
            m_frame = Frei0rPluginManager::applyEffects(m_frame, temporal.frei0rFx,
                                                        m_frei0rManager, f0rRel);
    }

    if (memoOk) {
        m_cropMemo = m_frame;
        m_cropMemoClipId = m_clipLainkaId;
        m_cropMemoFile = m_lastFile;
        m_cropMemoTime = m_lainkaQuantizedTime;
        m_cropMemoSrcT = m_lastSrcT;
        m_cropMemoSrcW = m_lastDecodeW;
        m_cropMemoCropL = m_lastCropL;
        m_cropMemoCropR = m_lastCropR;
        m_cropMemoCropT = m_lastCropT;
        m_cropMemoCropB = m_lastCropB;
    } else if (!m_cropMemo.isNull()) {
        // Fora do LAINKA (ou com OFX/onion skin/MotiOn temporal) não há acerto
        // possível, então libera a imagem em vez de segurar ~8 MB à toa — o que
        // o comentário do membro promete.
        m_cropMemo = QImage();
    }
}

