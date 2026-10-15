// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MotionScriptDock.h"
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

namespace {

// Lê comandos "prop: a->b [@t0-t1] [smooth|step|bezier]" e devolve os
// keyframes correspondentes (tempos RELATIVOS ao início do clipe, em segundos).
// Várias linhas para a mesma propriedade acumulam keyframes. Sem o trecho
// "@t0-t1", assume de 0 até a duração total do clipe.
struct MotionKey {
    double t0, v0, t1, v1;
    int interp;
};
QVector<MotionKey> parseMotionLines(const QString& text, const QString& propPattern, double dur) {
    QVector<MotionKey> out;
    const QString pattern =
        QStringLiteral("(?:^|[\\s;,])(?:%1)\\s*:\\s*([+-]?\\d*\\.?\\d+)\\s*->\\s*([+-]?\\d*\\.?\\d+)"
                       "(?:\\s*@\\s*(\\d+(?:\\.\\d+)?)\\s*ms\\s*-\\s*(\\d+(?:\\.\\d+)?)\\s*ms)?"
                       "(?:\\s*(smooth|step|bezier|linear))?")
            .arg(propPattern);
    QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        MotionKey k;
        k.v0 = m.captured(1).toDouble();
        k.v1 = m.captured(2).toDouble();
        k.t0 = m.captured(3).isEmpty() ? 0.0 : m.captured(3).toDouble() / 1000.0;
        k.t1 = m.captured(4).isEmpty() ? dur : m.captured(4).toDouble() / 1000.0;
        const QString ease = m.captured(5).toLower();
        k.interp = (ease == QLatin1String("smooth")) ? KfSmooth
                 : (ease == QLatin1String("step"))   ? KfStep
                 : (ease == QLatin1String("bezier")) ? KfBezier
                                                     : KfLinear;
        out.append(k);
    }
    return out;
}

} // namespace

MotionScriptDock::MotionScriptDock(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);

    m_helpLabel = new QLabel(tr("Editor de Script de Motion — escreva comandos e "
                                "converta em um clipe animado na timeline."), this);
    m_helpLabel->setWordWrap(true);
    lay->addWidget(m_helpLabel);

    auto* topBtn = new QHBoxLayout;
    auto* tplBtn = new QToolButton(this);
    tplBtn->setText(tr("Exemplos"));
    tplBtn->setPopupMode(QToolButton::InstantPopup);
    auto* tplMenu = new QMenu(tplBtn);
    tplMenu->addAction(tr("Motion básico"), this, &MotionScriptDock::applyTemplateDefault);
    tplMenu->addAction(tr("Baseado em textos animados"), this,
                       &MotionScriptDock::applyTemplateTextAnim);
    tplBtn->setMenu(tplMenu);
    topBtn->addWidget(tplBtn);
    auto* openBtn = new QPushButton(tr("Abrir…"), this);
    connect(openBtn, &QPushButton::clicked, this, &MotionScriptDock::loadFile);
    topBtn->addWidget(openBtn);
    auto* saveBtn = new QPushButton(tr("Salvar…"), this);
    connect(saveBtn, &QPushButton::clicked, this, &MotionScriptDock::saveFile);
    topBtn->addWidget(saveBtn);
    topBtn->addStretch(1);
    lay->addLayout(topBtn);

    m_editor = new QPlainTextEdit(this);
    m_editor->setPlaceholderText(tr("text:Bom dia\n"
                                    "dur:2000ms\n"
                                    "x:0->200 @0ms-1000ms smooth\n"
                                    "y:100->0\n"
                                    "scale:1->1.4\n"
                                    "rot:0->360\n"
                                    "opacity:1->0 @1500ms-2000ms"));
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
    connect(m_convertBtn, &QPushButton::clicked, this, &MotionScriptDock::convertToClip);
    bottom->addWidget(m_convertBtn);
    lay->addLayout(bottom);

    setMinimumSize(300, 240);
}

void MotionScriptDock::setTimeline(TimelineWidget* tl) {
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

void MotionScriptDock::applyTemplateDefault() {
    m_editor->setPlainText(QStringLiteral(
        "text:Bom dia\n"
        "dur:2000ms\n"
        "x:0->200 @0ms-1000ms smooth\n"
        "y:100->0\n"
        "scale:1->1.4\n"
        "rot:0->360\n"
        "opacity:1->0 @1500ms-2000ms"));
}

void MotionScriptDock::applyTemplateTextAnim() {
    m_editor->setPlainText(QStringLiteral(
        "# Baseado no formato de textos animados.\n"
        "# Aqui usamos comandos de motion; veja o exemplo \"Motion básico\".\n"
        "text:Olá, mundo\n"
        "dur:3000ms\n"
        "x:-300->300 @0ms-3000ms\n"
        "scale:0.5->1.0 @0ms-800ms smooth\n"
        "opacity:0->1 @0ms-500ms"));
}

void MotionScriptDock::loadFile() {
    const QString fn = QFileDialog::getOpenFileName(
        this, tr("Abrir script"), QString(),
        tr("Arquivos de texto (*.txt);;Todos (*.*)"));
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Motion Script"), tr("Não foi possível abrir o arquivo."));
        return;
    }
    m_editor->setPlainText(QString::fromUtf8(f.readAll()));
}

void MotionScriptDock::saveFile() {
    QString fn = QFileDialog::getSaveFileName(
        this, tr("Salvar script"), QString(),
        tr("Arquivos de texto (*.txt);;Todos (*.*)"));
    if (fn.isEmpty()) return;
    if (!fn.contains(QLatin1Char('.'))) fn += QStringLiteral(".txt");
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Motion Script"), tr("Não foi possível salvar o arquivo."));
        return;
    }
    QTextStream ts(&f);
    ts << m_editor->toPlainText();
}

void MotionScriptDock::convertToClip() {
    if (!m_timeline) {
        QMessageBox::warning(this, tr("Motion Script"), tr("Timeline não vinculada."));
        return;
    }
    Project* p = m_timeline->project();
    if (!p) return;

    const QString script = m_editor->toPlainText().trimmed();
    if (script.isEmpty()) {
        QMessageBox::warning(this, tr("Motion Script"), tr("Digite pelo menos um comando."));
        return;
    }

    int row = m_trackCombo->currentData().toInt();
    if (row < 0 || row >= p->videoTracks.size()) {
        if (p->videoTracks.isEmpty()) p->addTrack(false);
        row = 0;
    }
    if (row >= p->videoTracks.size()) row = p->videoTracks.size() - 1;

    // Duração: "dur:NNNms" ou "duration:NNNms"; padrão 3s.
    double dur = 3.0;
    {
        QRegularExpression re(QStringLiteral(R"((?:dur|duration)\s*:\s*(\d+(?:\.\d+)?)\s*ms)"),
                              QRegularExpression::CaseInsensitiveOption);
        if (const auto m = re.match(script); m.hasMatch())
            dur = m.captured(1).toDouble() / 1000.0;
    }
    if (dur <= 0.0) dur = 3.0;

    // Texto visível: "text:..." ou "caption:..."; padrão "Motion".
    QString caption = tr("Motion");
    {
        QRegularExpression re(QStringLiteral(R"((?:^|[\n\r])\s*(?:text|caption)\s*:\s*(.+))"),
                              QRegularExpression::CaseInsensitiveOption);
        if (const auto m = re.match(script); m.hasMatch())
            caption = m.captured(1).trimmed();
    }

    Clip c;
    c.id = newId();
    c.isText = true;
    c.pos = m_timeline->playhead();
    c.in = 0.0;
    c.dur = dur;
    c.name = tr("Motion Script");
    c.text.text = caption;
    c.text.textX = 0.5;
    c.text.textY = 0.5;
    c.text.textAlign = 0;

    auto applyProp = [&](const QString& prop, QVector<Keyframe>& keys, double& base) {
        const QVector<MotionKey> list = parseMotionLines(script, prop, dur);
        if (list.isEmpty()) return;
        base = list.first().v0;
        for (const MotionKey& k : list) {
            upsertKeyframe(keys, k.t0, k.v0, k.interp);
            upsertKeyframe(keys, k.t1, k.v1, k.interp);
        }
    };

    applyProp(QStringLiteral("x|tx"), c.kfTx, c.tx);
    applyProp(QStringLiteral("y|ty"), c.kfTy, c.ty);
    applyProp(QStringLiteral("scale"), c.kfScale, c.scale);
    applyProp(QStringLiteral("rot|rotation"), c.kfRotation, c.rotation);
    applyProp(QStringLiteral("opacity"), c.kfOpacity, c.opacity);

    emit m_timeline->editStart();

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
