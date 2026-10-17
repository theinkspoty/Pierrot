// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LazziDock.h"
#include "TimelineWidget.h"
#include "../colombina/models/Project.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QToolButton>
#include <QMenu>
#include <QFileDialog>
#include <QMessageBox>
#include <QFont>
#include <QRegularExpression>
#include <QFile>
#include <QTextStream>
#include <QKeySequence>
#include <algorithm>

namespace {

// Nome de easing do script → modo de interpolação do keyframe.
int easeToInterp(const QString& ease) {
    if (ease == QLatin1String("elastic")) return KfElastic;
    if (ease == QLatin1String("bounce"))  return KfBounce;
    if (ease == QLatin1String("back"))    return KfBack;
    if (ease == QLatin1String("smooth"))  return KfSmooth;
    if (ease == QLatin1String("step"))    return KfStep;
    if (ease == QLatin1String("bezier"))  return KfBezier;
    return KfLinear;
}

// Lê comandos "prop: a->b [@t0-t1] [smooth|step|bezier|linear|elastic|bounce|back]"
// e devolve os keyframes correspondentes (tempos RELATIVOS ao início do clipe,
// em segundos). O trecho "@t0-t1" aceita unidade ms ou s (misturável: @500ms-1.2s).
// Várias linhas para a mesma propriedade acumulam keyframes.
// Sem o trecho "@t0-t1", assume de 0 até a duração total do clipe.
// Sem easing explícito, usa `defaultEase` ("ease:elastic" no script).
struct MotionKey {
    double t0, v0, t1, v1;
    int interp;
};
QVector<MotionKey> parseMotionLines(const QString& text, const QString& propPattern,
                                    double dur, int defaultEase = KfLinear) {
    QVector<MotionKey> out;
    const QString pattern =
        QStringLiteral("(?:^|[\\s;,])(?:%1)\\s*:\\s*([+-]?\\d*\\.?\\d+)\\s*->\\s*([+-]?\\d*\\.?\\d+)"
                       "(?:\\s*@\\s*(\\d+(?:\\.\\d+)?)\\s*(ms|s)\\s*-\\s*"
                       "(\\d+(?:\\.\\d+)?)\\s*(ms|s))?"
                       "(?:\\s*(smooth|step|bezier|linear|elastic|bounce|back))?")
            .arg(propPattern);
    QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        MotionKey k;
        k.v0 = m.captured(1).toDouble();
        k.v1 = m.captured(2).toDouble();
        // Unidade: "ms" (padrão) ou "s" — vazio conta como ms (compat).
        const double u0 = (m.captured(4).toLower() == QLatin1String("s")) ? 1.0 : 0.001;
        const double u1 = (m.captured(6).toLower() == QLatin1String("s")) ? 1.0 : 0.001;
        k.t0 = m.captured(3).isEmpty() ? 0.0 : m.captured(3).toDouble() * u0;
        k.t1 = m.captured(5).isEmpty() ? dur : m.captured(5).toDouble() * u1;
        const QString ease = m.captured(7).toLower();
        k.interp = ease.isEmpty() ? defaultEase : easeToInterp(ease);
        out.append(k);
    }
    return out;
}

// Primeiro valor escalar "chave: valor" (uma linha por chave) do script.
QString scalarValue(const QString& text, const QString& keyPattern) {
    const QRegularExpression re(
        QStringLiteral(R"((?:^|[\n\r])\s*(?:%1)\s*:\s*([^\n\r]+))").arg(keyPattern),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(text);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

} // namespace

LazziDock::LazziDock(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);

    m_helpLabel = new QLabel(tr("Lazzi — motion por script: easings elásticos, "
                                "estilo de texto, motion blur e formas/fundos "
                                "coloridos (shape:). Converta em clipe animado."), this);
    m_helpLabel->setWordWrap(true);
    lay->addWidget(m_helpLabel);

    auto* topBtn = new QHBoxLayout;
    auto* tplBtn = new QToolButton(this);
    tplBtn->setText(tr("Exemplos"));
    tplBtn->setPopupMode(QToolButton::InstantPopup);
    auto* tplMenu = new QMenu(tplBtn);
    tplMenu->addAction(tr("Motion básico"), this, &LazziDock::applyTemplateDefault);
    tplMenu->addAction(tr("Elástico (estilo AE)"), this, &LazziDock::applyTemplateAe);
    tplMenu->addAction(tr("Fundo colorido"), this, &LazziDock::applyTemplateShape);
    tplMenu->addAction(tr("Baseado em textos animados"), this,
                       &LazziDock::applyTemplateTextAnim);
    tplBtn->setMenu(tplMenu);
    topBtn->addWidget(tplBtn);
    auto* openBtn = new QPushButton(tr("Abrir…"), this);
    connect(openBtn, &QPushButton::clicked, this, &LazziDock::loadFile);
    topBtn->addWidget(openBtn);
    auto* saveBtn = new QPushButton(tr("Salvar…"), this);
    connect(saveBtn, &QPushButton::clicked, this, &LazziDock::saveFile);
    topBtn->addWidget(saveBtn);
    topBtn->addStretch(1);
    lay->addLayout(topBtn);

    m_editor = new QPlainTextEdit(this);
    m_editor->setPlaceholderText(tr("text:Bom dia\n"
                                    "font:Impact\n"
                                    "color:#FFDD00\n"
                                    "ease:elastic\n"
                                    "dur:2s\n"
                                    "scale:0->1 @0s-0.6s\n"
                                    "y:120->0 @0s-0.6s\n"
                                    "rot:-10->0 @0s-0.7s"));
    QFont f = m_editor->font();
    f.setFamily(QStringLiteral("Monospace"));
    m_editor->setFont(f);
    lay->addWidget(m_editor, 1);

    auto* bottom = new QHBoxLayout;
    bottom->addWidget(new QLabel(tr("Vídeo:"), this));
    m_trackCombo = new QComboBox(this);
    bottom->addWidget(m_trackCombo, 1);
    m_convertBtn = new QPushButton(tr("Converter para clipe"), this);
    m_convertBtn->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    connect(m_convertBtn, &QPushButton::clicked, this, &LazziDock::convertToClip);
    bottom->addWidget(m_convertBtn);
    lay->addLayout(bottom);

    setMinimumSize(300, 240);
}

void LazziDock::setTimeline(TimelineWidget* tl) {
    m_timeline = tl;
    if (!m_timeline) return;
    m_trackCombo->clear();
    Project* p = m_timeline->project();
    if (p) {
        for (int i = 0; i < p->videoTracks.size(); ++i)
            m_trackCombo->addItem(tr("Vídeo %1").arg(i + 1), i);
    }
    if (m_trackCombo->count() == 0)
        m_trackCombo->addItem(tr("Vídeo 1"), 0);
}

void LazziDock::applyTemplateDefault() {
    m_editor->setPlainText(QStringLiteral(
        "text:Bom dia\n"
        "dur:2000ms\n"
        "x:0->200 @0ms-1000ms smooth\n"
        "y:100->0\n"
        "scale:1->1.4\n"
        "rot:0->360\n"
        "opacity:1->0 @1500ms-2000ms"));
}

void LazziDock::applyTemplateShape() {
    m_editor->setPlainText(QStringLiteral(
        "# Fundo colorido animado — gerador de gradiente como clipe.\n"
        "# shape: solid | gradient | checkerboard | noise\n"
        "# color/color2 = cores; tudo anima (x/y/scale/rot/opacity).\n"
        "shape:gradient\n"
        "color:#1B0F3B\n"
        "color2:#E91E63\n"
        "ease:smooth\n"
        "dur:8s\n"
        "scale:1.2->1.5 @0s-4s\n"
        "scale:1.5->1.15 @4s-8s\n"
        "rot:-3->3 @0s-8s\n"
        "opacity:0->1 @0s-0.5s"));
}

void LazziDock::applyTemplateTextAnim() {
    m_editor->setPlainText(QStringLiteral(
        "# Baseado no formato de textos animados.\n"
        "# Aqui usamos comandos de motion; veja o exemplo \"Motion básico\".\n"
        "text:Olá, mundo\n"
        "dur:3000ms\n"
        "x:-300->300 @0ms-3000ms\n"
        "scale:0.5->1.0 @0ms-800ms smooth\n"
        "opacity:0->1 @0ms-500ms"));
}

void LazziDock::applyTemplateAe() {
    m_editor->setPlainText(QStringLiteral(
        "# Elástico estilo After Effects — ease:elastic vale pra todas as\n"
        "# propriedades; cada linha pode sobrescrever com bounce/back/etc.\n"
        "# Tempo em @s ou @ms (misturável).\n"
        "text:PIERROT\n"
        "font:Impact\n"
        "size:14\n"
        "color:#FFDD00\n"
        "outline:1.2\n"
        "outlinecolor:#000000\n"
        "ease:elastic\n"
        "dur:2.5s\n"
        "scale:0->1 @0s-0.7s\n"
        "y:-200->0 @0s-0.7s\n"
        "rot:-25->0 @0s-0.9s\n"
        "scale:1->1.08 @1.5s-1.8s bounce\n"
        "scale:1.08->1 @1.8s-2.1s\n"
        "opacity:1->0 @2.1s-2.5s smooth"));
}

void LazziDock::loadFile() {
    const QString fn = QFileDialog::getOpenFileName(
        this, tr("Abrir script"), QString(),
        tr("Arquivos de texto (*.txt);;Todos (*.*)"));
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Lazzi"), tr("Não foi possível abrir o arquivo."));
        return;
    }
    m_editor->setPlainText(QString::fromUtf8(f.readAll()));
}

void LazziDock::saveFile() {
    QString fn = QFileDialog::getSaveFileName(
        this, tr("Salvar script"), QString(),
        tr("Arquivos de texto (*.txt);;Todos (*.*)"));
    if (fn.isEmpty()) return;
    if (!fn.contains(QLatin1Char('.'))) fn += QStringLiteral(".txt");
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Lazzi"), tr("Não foi possível salvar o arquivo."));
        return;
    }
    QTextStream ts(&f);
    ts << m_editor->toPlainText();
}

void LazziDock::convertToClip() {
    if (!m_timeline) {
        QMessageBox::warning(this, tr("Lazzi"), tr("Timeline não vinculada."));
        return;
    }
    Project* p = m_timeline->project();
    if (!p) return;

    const QString script = m_editor->toPlainText().trimmed();
    if (script.isEmpty()) {
        QMessageBox::warning(this, tr("Lazzi"), tr("Digite pelo menos um comando."));
        return;
    }

    int row = m_trackCombo->currentData().toInt();
    if (row < 0 || row >= p->videoTracks.size()) {
        if (p->videoTracks.isEmpty()) p->addTrack(false);
        row = 0;
    }
    if (row >= p->videoTracks.size()) row = p->videoTracks.size() - 1;

    // Duração: "dur:NNNms" ou "dur:NNNs"; padrão 3s.
    double dur = 3.0;
    {
        QRegularExpression re(QStringLiteral(R"((?:dur|duration)\s*:\s*(\d+(?:\.\d+)?)\s*(ms|s)\b)"),
                              QRegularExpression::CaseInsensitiveOption);
        if (const auto m = re.match(script); m.hasMatch())
            dur = m.captured(1).toDouble()
                  * ((m.captured(2).toLower() == QLatin1String("s")) ? 1.0 : 0.001);
    }
    if (dur <= 0.0) dur = 3.0;

    // Texto visível: "text:..." ou "caption:..."; padrão "Motion".
    QString caption = scalarValue(script, QStringLiteral("text|caption"));
    if (caption.isEmpty()) caption = tr("Motion");

    // Easing padrão: "ease:elastic" — vale pra toda propriedade sem easing
    // explícito na linha.
    int defaultEase = KfLinear;
    {
        const QString e = scalarValue(script, QStringLiteral("ease")).toLower();
        if (!e.isEmpty()) defaultEase = easeToInterp(e);
    }

    Clip c;
    c.id = newId();
    c.pos = m_timeline->playhead();
    c.in = 0.0;
    c.dur = dur;

    // Modo forma/fundo: "shape:solid|gradient|checkerboard|noise" gera um
    // clipe de mídia virtual (gerador de cor) animável — fundos coloridos e
    // formas sem depender de fonte. color/color2 definem as cores.
    const QString shapeVal =
        scalarValue(script, QStringLiteral("shape|forma")).toLower();
    MediaItem gen;
    if (!shapeVal.isEmpty()) {
        gen.id = newId();
        gen.isSolid = true;
        gen.hasVideo = true;
        gen.width = p->width;
        gen.height = p->height;
        gen.duration = qMax(p->duration(), dur);

        QColor col1(Qt::white);
        QColor col2(Qt::black);
        const QString c1 = scalarValue(script, QStringLiteral("color|cor"));
        if (!c1.isEmpty() && QColor::isValidColorName(c1)) col1 = QColor(c1);
        const QString c2 = scalarValue(script, QStringLiteral("color2|cor2"));
        if (!c2.isEmpty() && QColor::isValidColorName(c2)) col2 = QColor(c2);
        col1.setAlpha(255);
        col2.setAlpha(255);

        if (shapeVal.startsWith(QLatin1String("grad"))) {
            gen.generator = QStringLiteral("gradient");
            gen.name = tr("Lazzi Gradiente");
        } else if (shapeVal.startsWith(QLatin1String("check"))) {
            gen.generator = QStringLiteral("checkerboard");
            gen.name = tr("Lazzi Checkboard");
            const QString cellsVal = scalarValue(script, QStringLiteral("cells|celulas"));
            gen.genCells = cellsVal.isEmpty() ? 8 : qBound(2, cellsVal.toInt(), 64);
        } else if (shapeVal.startsWith(QLatin1String("noise"))
                   || shapeVal.startsWith(QLatin1String("ruido"))) {
            gen.generator = QStringLiteral("noise");
            gen.name = tr("Lazzi Ruído");
        } else {
            gen.generator = QString();
            gen.name = tr("Lazzi Cor");
        }
        gen.solidColor = col1;
        gen.solidColor2 = col2;
        c.mediaId = gen.id;
        c.name = gen.name;
    } else {
        c.isText = true;
        c.name = tr("Lazzi");
        c.text.text = caption;
        c.text.textX = 0.5;
        c.text.textY = 0.5;
        c.text.textAlign = 0;

        // Estilo do texto (estático — animação de cor ainda não é suportada).
        const QString fontVal = scalarValue(script, QStringLiteral("font|fonte"));
        if (!fontVal.isEmpty()) c.text.fontFamily = fontVal;

        // size/outline em % da altura do quadro (ex.: size:12 = 12%).
        const QString sizeVal = scalarValue(script, QStringLiteral("size|tamanho"));
        if (!sizeVal.isEmpty())
            c.text.textSize = qBound(0.5, sizeVal.toDouble() / 100.0, 60.0);

        const QString colorVal = scalarValue(script, QStringLiteral("color|cor"));
        if (!colorVal.isEmpty() && QColor::isValidColorName(colorVal))
            c.text.textColor = QColor(colorVal);

        const QString outlineVal = scalarValue(script, QStringLiteral("outline|contorno"));
        if (!outlineVal.isEmpty())
            c.text.textOutline = qBound(0.0, outlineVal.toDouble() / 100.0, 30.0);

        const QString outlineColVal =
            scalarValue(script, QStringLiteral("outlinecolor|contornocor"));
        if (!outlineColVal.isEmpty() && QColor::isValidColorName(outlineColVal))
            c.text.textOutlineColor = QColor(outlineColVal);

        const QString alignVal = scalarValue(script, QStringLiteral("align|alinhamento")).toLower();
        if (alignVal.startsWith(QLatin1String("left")) || alignVal.startsWith(QLatin1String("esq")))
            c.text.textAlign = 1;
        else if (alignVal.startsWith(QLatin1String("right")) || alignVal.startsWith(QLatin1String("dir")))
            c.text.textAlign = 2;

        const QString boldVal = scalarValue(script, QStringLiteral("bold|negrito")).toLower();
        if (boldVal == QLatin1String("off") || boldVal == QLatin1String("0")
            || boldVal == QLatin1String("nao") || boldVal == QLatin1String("não"))
            c.text.textBold = false;

        // Posição do texto no quadro: posx/posy em % (50 = centro).
        const QString posXVal = scalarValue(script, QStringLiteral("posx|px"));
        if (!posXVal.isEmpty())
            c.text.textX = qBound(0.0, posXVal.toDouble() / 100.0, 1.0);
        const QString posYVal = scalarValue(script, QStringLiteral("posy|py"));
        if (!posYVal.isEmpty())
            c.text.textY = qBound(0.0, posYVal.toDouble() / 100.0, 1.0);
    }

    // Motion blur do clipe: "mblur:40" (0-100) — vale pra texto e formas.
    const QString mblurVal = scalarValue(script, QStringLiteral("mblur|motionblur"));
    if (!mblurVal.isEmpty()) {
        const double amt = qBound(0.0, mblurVal.toDouble(), 100.0);
        if (amt > 0.0) {
            c.motionEnabled = true;
            c.motionAmount = amt;
        }
    }

    auto applyProp = [&](const QString& prop, QVector<Keyframe>& keys, double& base) {
        const QVector<MotionKey> list = parseMotionLines(script, prop, dur, defaultEase);
        if (list.isEmpty()) return;
        base = list.first().v0;
        for (const MotionKey& k : list) {
            upsertKeyframe(keys, std::clamp(k.t0, 0.0, dur), k.v0, k.interp);
            upsertKeyframe(keys, std::clamp(k.t1, 0.0, dur), k.v1, k.interp);
        }
    };

    applyProp(QStringLiteral("x|tx"), c.kfTx, c.tx);
    applyProp(QStringLiteral("y|ty"), c.kfTy, c.ty);
    applyProp(QStringLiteral("scale"), c.kfScale, c.scale);
    applyProp(QStringLiteral("scaleX|sx"), c.kfScaleX, c.scaleX);
    applyProp(QStringLiteral("scaleY|sy"), c.kfScaleY, c.scaleY);
    applyProp(QStringLiteral("rot|rotation"), c.kfRotation, c.rotation);
    applyProp(QStringLiteral("opacity"), c.kfOpacity, c.opacity);
    applyProp(QStringLiteral("anchorx"), c.kfAnchorX, c.anchorX);
    applyProp(QStringLiteral("anchory"), c.kfAnchorY, c.anchorY);

    emit m_timeline->editStart();

    if (!gen.id.isEmpty()) p->media.append(gen);

    auto& clips = p->videoTracks[row].clips;
    auto insertIt = clips.begin();
    while (insertIt != clips.end() && insertIt->pos <= c.pos) ++insertIt;
    clips.insert(insertIt, c);

    m_timeline->selectClip(c.id);
    m_timeline->invalidateScene();
    m_timeline->update();
    emit m_timeline->modified();

    emit clipConverted();
}
