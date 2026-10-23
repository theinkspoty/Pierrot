// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/timeline/TimelineWidget.h"
#include "ui/timeline/TimelineCommands.h"
#include "colombina/util.h"
#include "colombina/models/ClipAttrs.h"

#include "colombina/ffmpeg/MediaCache.h"
#include "laartman/Decoder.h"
#include "colombina/mesh/BlenderBridge.h"
#include "colombina/render/MesaRenderer.h"
#include "ui/dialogs/TransformDialog.h"
#include "ui/audio/AudioEffectsDialog.h"
#include "ui/audio/TrackAudioFxDialog.h"
#include "ui/dialogs/TextEditorDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/timeline/TlLog.h"
#include "ui/core/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QCheckBox>
#include <QCursor>
#include <QList>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QSettings>
#include <QKeySequence>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QScrollBar>
#include <QMenu>
#include <QKeyEvent>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPolygon>
#include <QDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QColorDialog>
#include <QIcon>
#include <QPixmap>
#include <QTabWidget>
#include <QGroupBox>
#include <QListWidget>
#include <QFileDialog>
#include <QActionGroup>
#include <QPair>
#include <QVariantAnimation>
#include <QStyle>
#include <QStyleOptionRubberBand>
#include <QRubberBand>
#include <algorithm>
#include <cmath>
#include <utility>

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

void TimelineWidget::showGradingDialog(Clip* c) {
    if (!c) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Correção de cor (Lumetri)"));
    dlg.setMinimumSize(540, 580);

    Clip work = *c;
    auto* tabs = new QTabWidget(&dlg);

    auto makeSlider = [&](const QString& name, double* field, double lo, double hi,
                          const QString& tip = QString()) {
        auto* row = new QWidget(tabs);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        auto* lab = new QLabel(name, row);
        lab->setMinimumWidth(120);
        lay->addWidget(lab);
        auto* spin = new QDoubleSpinBox(row);
        spin->setRange(lo, hi);
        spin->setDecimals(2);
        spin->setSingleStep((hi - lo) / 100.0);
        spin->setValue(*field);
        if (!tip.isEmpty()) spin->setToolTip(tip);
        auto* slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(int(lo * 100), int(hi * 100));
        slider->setValue(int(std::lround(*field * 100.0)));
        connect(slider, &QSlider::valueChanged, &dlg, [field, spin](int v) {
            *field = v / 100.0;
            spin->blockSignals(true);
            spin->setValue(*field);
            spin->blockSignals(false);
        });
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dlg,
                [field, slider](double v) {
                    *field = v;
                    slider->blockSignals(true);
                    slider->setValue(int(std::lround(v * 100.0)));
                    slider->blockSignals(false);
                });
        lay->addWidget(spin);
        lay->addWidget(slider, 2);
        return row;
    };

    // Básico (Lumetri Basic Correction)
    {
        auto* w = new QWidget;
        auto* form = new QVBoxLayout(w);
        form->addWidget(makeSlider(tr("Exposição"), &work.cgExposure, -5, 5, tr("EV")));
        form->addWidget(makeSlider(tr("Contraste"), &work.cgContrast, -100, 100));
        form->addWidget(makeSlider(tr("Realces"), &work.cgHighlights, -100, 100));
        form->addWidget(makeSlider(tr("Sombras"), &work.cgShadows, -100, 100));
        form->addWidget(makeSlider(tr("Brancos"), &work.cgWhites, -100, 100));
        form->addWidget(makeSlider(tr("Pretos"), &work.cgBlacks, -100, 100));
        form->addWidget(makeSlider(tr("Saturação"), &work.cgSaturation, -100, 100));
        form->addWidget(makeSlider(tr("Vibrance"), &work.cgVibrance, -100, 100));
        form->addWidget(makeSlider(tr("Temperatura"), &work.cgTemperature, -100, 100,
                                    tr("Azul ↔ Laranja")));
        form->addWidget(makeSlider(tr("Matiz"), &work.cgTint, -100, 100,
                                    tr("Verde ↔ Magenta")));
        form->addWidget(makeSlider(tr("Filme desbotado"), &work.cgFadedFilm, 0, 100));
        form->addWidget(makeSlider(tr("Nitidez"), &work.cgSharpen, 0, 100));
        form->addStretch();
        tabs->addTab(w, tr("Básico"));
    }

    // Rodas Lift/Gamma/Gain
    {
        auto* w = new QWidget;
        auto* form = new QVBoxLayout(w);
        form->addWidget(new QLabel(
            tr("Lift = sombras · Gamma = meios · Gain = realces"), w));
        auto* grid = new QGridLayout;
        const QString names[3] = {tr("Lift"), tr("Gamma"), tr("Gain")};
        const int mins[3] = {-100, 10, -100};
        const int maxs[3] = {100, 400, 100};
        double* fields[3][3] = {
            {&work.liftR, &work.liftG, &work.liftB},
            {&work.gammaR, &work.gammaG, &work.gammaB},
            {&work.gainR, &work.gainG, &work.gainB},
        };
        for (int r = 0; r < 3; ++r) {
            grid->addWidget(new QLabel(names[r], w), r + 1, 0);
            for (int ch = 0; ch < 3; ++ch) {
                auto* s = new QSlider(Qt::Horizontal, w);
                s->setRange(mins[r], maxs[r]);
                s->setValue(int(std::lround(*fields[r][ch] * 100.0)));
                double* f = fields[r][ch];
                connect(s, &QSlider::valueChanged, &dlg, [f](int v) { *f = v / 100.0; });
                grid->addWidget(s, r + 1, ch + 1);
            }
        }
        grid->addWidget(new QLabel(tr("R"), w), 0, 1, Qt::AlignCenter);
        grid->addWidget(new QLabel(tr("G"), w), 0, 2, Qt::AlignCenter);
        grid->addWidget(new QLabel(tr("B"), w), 0, 3, Qt::AlignCenter);
        form->addLayout(grid);
        form->addStretch();
        tabs->addTab(w, tr("Rodas"));
    }

    // Curvas RGB
    {
        auto* w = new QWidget;
        auto* form = new QVBoxLayout(w);
        form->addWidget(new QLabel(
            tr("Curvas: adicione pontos X/Y em 0..1. Master afeta R, G e B."), w));
        auto addCurveBox = [&](const QString& title, QVector<QPointF>& pts) {
            auto* box = new QGroupBox(title, w);
            auto* bl = new QVBoxLayout(box);
            auto* list = new QListWidget(box);
            auto refresh = [&list, &pts]() {
                list->clear();
                for (const QPointF& p : pts)
                    list->addItem(QStringLiteral("%1 , %2")
                                      .arg(p.x(), 0, 'f', 3)
                                      .arg(p.y(), 0, 'f', 3));
            };
            refresh();
            auto* btns = new QHBoxLayout;
            auto* add = new QPushButton(tr("Adicionar"), box);
            auto* del = new QPushButton(tr("Remover último"), box);
            auto* lin = new QPushButton(tr("Reta"), box);
            QObject::connect(add, &QPushButton::clicked, &dlg, [&pts, &dlg, refresh]() {
                bool ok = false;
                const double x = QInputDialog::getDouble(&dlg, tr("Ponto"), tr("X:"),
                                                         0.5, 0.0, 1.0, 3, &ok);
                if (!ok) return;
                const double y = QInputDialog::getDouble(&dlg, tr("Ponto"), tr("Y:"),
                                                         x, 0.0, 1.0, 3, &ok);
                if (!ok) return;
                pts.append(QPointF(x, y));
                std::sort(pts.begin(), pts.end(),
                          [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
                refresh();
            });
            QObject::connect(del, &QPushButton::clicked, &dlg, [&pts, refresh]() {
                if (!pts.isEmpty()) pts.removeLast();
                refresh();
            });
            QObject::connect(lin, &QPushButton::clicked, &dlg, [&pts, refresh]() {
                pts.clear();
                refresh();
            });
            btns->addWidget(add);
            btns->addWidget(del);
            btns->addWidget(lin);
            bl->addWidget(list);
            bl->addLayout(btns);
            form->addWidget(box);
        };
        addCurveBox(tr("Master"), work.cgMasterCurve);
        addCurveBox(tr("Vermelho"), work.cgRCurve);
        addCurveBox(tr("Verde"), work.cgGCurve);
        addCurveBox(tr("Azul"), work.cgBCurve);
        form->addStretch();
        tabs->addTab(w, tr("Curvas"));
    }

    // Vinheta / LUT / Mix
    QLineEdit* lutEdit = nullptr;
    {
        auto* w = new QWidget;
        auto* form = new QVBoxLayout(w);
        form->addWidget(makeSlider(tr("Vinheta"), &work.cgVignette, -100, 100,
                                    tr("Negativo escurece as bordas")));
        form->addWidget(makeSlider(tr("Pena da vinheta"), &work.cgVignetteFeather, 0, 100));
        form->addWidget(makeSlider(tr("Intensidade do grade"), &work.cgBlend, 0, 1));
        form->addWidget(makeSlider(tr("Força da LUT"), &work.cgLutStrength, 0, 1));
        auto* lutRow = new QHBoxLayout;
        lutEdit = new QLineEdit(work.cgLutPath, w);
        lutEdit->setPlaceholderText(tr("caminho/do/arquivo.cube"));
        auto* lutBtn = new QPushButton(tr("Procurar .cube…"), w);
        QObject::connect(lutBtn, &QPushButton::clicked, &dlg, [lutEdit, &dlg]() {
            const QString path = QFileDialog::getOpenFileName(
                &dlg, tr("Abrir LUT 3D"), QString(), tr("LUTs (*.cube);;Todos (*)"));
            if (!path.isEmpty()) lutEdit->setText(path);
        });
        lutRow->addWidget(lutEdit, 1);
        lutRow->addWidget(lutBtn);
        form->addLayout(lutRow);
        auto* info = new QLabel(w);
        info->setWordWrap(true);
        info->setStyleSheet(QStringLiteral("color:#888;"));
        info->setText(work.cgLutPath.isEmpty()
                          ? tr("Sem LUT. Aceita .cube 3D (IRIDAS/Resolve).")
                          : tr("LUT: %1").arg(work.cgLutPath));
        form->addWidget(info);
        form->addStretch();
        tabs->addTab(w, tr("Vinheta / LUT"));
    }

    auto* reset = new QPushButton(tr("Resetar grade"), &dlg);
    QObject::connect(reset, &QPushButton::clicked, &dlg, [&work]() {
        const QString lut = work.cgLutPath;
        work = Clip{};
        work.cgLutPath = lut; // preserva o caminho da LUT
    });

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(tabs);
    lay->addWidget(reset);
    lay->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return;

    if (lutEdit) work.cgLutPath = lutEdit->text().trimmed();
    emit editStart();
    *c = work;
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

void TimelineWidget::showAudioEffectsDialog(Clip* c) {
    if (!c) return;
    AudioEffectsDialog dlg(c, this);
    if (dlg.exec() != QDialog::Accepted) return;
    emit editStart();
    update();
    emit modified();
}

void TimelineWidget::showTextEditorDialog(Clip* c) {
    if (!c) return;
    TextEditorDialog dlg(m_project, c, this);
    if (dlg.exec() != QDialog::Accepted) return;
    emit editStart();
    invalidateScene();
    update();
    emit modified();
}

