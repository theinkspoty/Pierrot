// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/dialogs/TextEditorDialog.h"

#include <QPlainTextEdit>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QSlider>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QFrame>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFont>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QIcon>
#include <QVariant>
#include <cmath>

namespace {

void setBtnColor(QPushButton* btn, const QColor& c) {
    QPixmap pm(18, 18);
    pm.fill(c);
    btn->setIcon(QIcon(pm));
    btn->setProperty("color", c);
    btn->setToolTip(c.name(QColor::HexArgb));
}

QColor btnColor(const QPushButton* btn) {
    const QVariant v = btn->property("color");
    if (v.isValid()) return v.value<QColor>();
    return btn->icon().pixmap(18, 18).toImage().pixelColor(9, 9);
}

} // namespace

// ── Prévia do texto sobre o quadro ─────────────────────────────────────────

class TextPreview : public QWidget {
public:
    explicit TextPreview(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setStyle(const TextStyle& s) { m_style = s; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        const QRect r = rect();
        // "Quadro": fundo escuro + terços de referência.
        p.fillRect(r, QColor(22, 22, 26));
        p.setPen(QColor(255, 255, 255, 16));
        p.drawLine(r.width() / 3, 0, r.width() / 3, r.height());
        p.drawLine(2 * r.width() / 3, 0, 2 * r.width() / 3, r.height());
        p.drawLine(0, r.height() / 3, r.width(), r.height() / 3);
        p.drawLine(0, 2 * r.height() / 3, r.width(), 2 * r.height() / 3);
        p.setPen(QColor(255, 255, 255, 26));
        p.drawRect(r.adjusted(0, 0, -1, -1));

        const QString txt = m_style.text.trimmed().isEmpty()
                                ? QStringLiteral("Prévia do texto")
                                : m_style.text;
        QFont f(m_style.fontFamily.isEmpty() ? QStringLiteral("DejaVu Sans")
                                             : m_style.fontFamily);
        f.setBold(m_style.textBold);
        const double frac = m_style.textSize > 0.0 ? m_style.textSize : (1.0 / 18.0);
        f.setPixelSize(std::max(6, (int)std::lround(frac * r.height())));
        p.setFont(f);

        Qt::Alignment hal = Qt::AlignHCenter;
        if (m_style.textAlign == 1) hal = Qt::AlignLeft;
        else if (m_style.textAlign == 2) hal = Qt::AlignRight;
        const int flags = Qt::TextWordWrap | hal;

        QFontMetrics fm(f);
        const int wrapW = std::max(20, r.width() - 20);
        QRect tb = fm.boundingRect(QRect(0, 0, wrapW, 10000), flags, txt);
        if (tb.width() > wrapW) tb.setWidth(wrapW);
        const int x = (int)std::lround(m_style.textX * r.width());
        const int y = (int)std::lround(m_style.textY * r.height());
        int left;
        if (m_style.textAlign == 1) left = x;
        else if (m_style.textAlign == 2) left = x - tb.width();
        else left = x - tb.width() / 2;
        const QRect drawRect(left, y - tb.height() / 2, tb.width(), tb.height());

        // Caixa de fundo.
        if (m_style.textBackground) {
            const int pad = std::max(3, (int)std::lround(0.012 * r.height()));
            p.setPen(Qt::NoPen);
            p.setBrush(m_style.textBackgroundColor);
            p.drawRoundedRect(drawRect.adjusted(-pad, -pad, pad, pad), 3, 3);
        }

        // Contorno: desenha o texto em 8 direções antes do preenchimento.
        if (m_style.textOutline > 0.0) {
            const int w = std::max(1, (int)std::lround(m_style.textOutline * r.height()));
            p.setPen(m_style.textOutlineColor);
            for (int dx = -w; dx <= w; ++dx)
                for (int dy = -w; dy <= w; ++dy) {
                    if (dx == 0 && dy == 0) continue;
                    if (dx * dx + dy * dy > w * w + 1) continue;
                    p.drawText(drawRect.translated(dx, dy), flags, txt);
                }
        }
        // Preenchimento.
        p.setPen(m_style.textColor);
        p.drawText(drawRect, flags, txt);
    }

private:
    TextStyle m_style;
};

// ── Formulário de edição ───────────────────────────────────────────────────

TextEditorWidget::TextEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    m_preview = new TextPreview(this);
    root->addWidget(m_preview);

    auto* form = new QWidget(this);
    auto* formLay = new QVBoxLayout(form);
    formLay->setContentsMargins(0, 0, 0, 0);
    formLay->setSpacing(8);

    // ---- Texto ----
    auto* textGroup = new QGroupBox(tr("Texto"), form);
    auto* textLay = new QVBoxLayout(textGroup);
    m_text = new QPlainTextEdit(form);
    m_text->setPlaceholderText(tr("Digite o texto/título…"));
    m_text->setMinimumHeight(84);
    textLay->addWidget(m_text);
    formLay->addWidget(textGroup);

    // ---- Fonte ----
    auto* fontGroup = new QGroupBox(tr("Fonte"), form);
    auto* fontLay = new QHBoxLayout(fontGroup);
    m_font = new QComboBox(form);
    const QStringList fams = QFontDatabase().families();
    m_font->addItems(fams);
    m_font->setEditable(true);
    m_size = new QDoubleSpinBox(form);
    m_size->setRange(1.0, 40.0);
    m_size->setSuffix(QStringLiteral(" %"));
    m_bold = new QCheckBox(tr("Negrito"), form);
    fontLay->addWidget(m_font, 1);
    fontLay->addWidget(m_size);
    fontLay->addWidget(m_bold);
    formLay->addWidget(fontGroup);

    // ---- Cor e contorno ----
    auto* colorGroup = new QGroupBox(tr("Cor e contorno"), form);
    auto* colorLay = new QVBoxLayout(colorGroup);
    m_fillBtn = makeColorButton(QColor(255, 255, 255));
    auto* fillRow = new QHBoxLayout;
    fillRow->addWidget(new QLabel(tr("Cor do texto:"), colorGroup));
    fillRow->addWidget(m_fillBtn, 1);
    colorLay->addLayout(fillRow);

    m_outlineOn = new QCheckBox(tr("Contorno"), colorGroup);
    m_outlineW = new QDoubleSpinBox(colorGroup);
    m_outlineW->setRange(0.1, 10.0);
    m_outlineW->setSuffix(QStringLiteral(" %"));
    m_outlineBtn = makeColorButton(QColor(0, 0, 0));
    auto* outRow = new QHBoxLayout;
    outRow->addWidget(m_outlineOn);
    outRow->addWidget(m_outlineW);
    outRow->addWidget(m_outlineBtn);
    outRow->addStretch(1);
    colorLay->addLayout(outRow);
    formLay->addWidget(colorGroup);

    // ---- Fundo ----
    auto* bgGroup = new QGroupBox(tr("Fundo"), form);
    auto* bgLay = new QHBoxLayout(bgGroup);
    m_bgOn = new QCheckBox(tr("Caixa"), bgGroup);
    m_bgBtn = makeColorButton(QColor(0, 0, 0, 150));
    m_bgAlpha = new QSlider(Qt::Horizontal, bgGroup);
    m_bgAlpha->setRange(0, 100);
    bgLay->addWidget(m_bgOn);
    bgLay->addWidget(m_bgBtn);
    bgLay->addWidget(new QLabel(tr("Opacidade:"), bgGroup));
    bgLay->addWidget(m_bgAlpha, 1);
    formLay->addWidget(bgGroup);

    // ---- Posição e alinhamento ----
    auto* posGroup = new QGroupBox(tr("Posição"), form);
    auto* posLay = new QVBoxLayout(posGroup);
    m_align = new QComboBox(posGroup);
    m_align->addItem(tr("Centro"));
    m_align->addItem(tr("Esquerda"));
    m_align->addItem(tr("Direita"));
    auto* alignRow = new QHBoxLayout;
    alignRow->addWidget(new QLabel(tr("Alinhamento:"), posGroup));
    alignRow->addWidget(m_align, 1);
    posLay->addLayout(alignRow);

    m_posX = new QSlider(Qt::Horizontal, posGroup);
    m_posX->setRange(0, 100);
    m_posY = new QSlider(Qt::Horizontal, posGroup);
    m_posY->setRange(0, 100);
    auto* xRow = new QHBoxLayout;
    xRow->addWidget(new QLabel(tr("X:"), posGroup));
    xRow->addWidget(m_posX, 1);
    auto* yRow = new QHBoxLayout;
    yRow->addWidget(new QLabel(tr("Y:"), posGroup));
    yRow->addWidget(m_posY, 1);
    posLay->addLayout(xRow);
    posLay->addLayout(yRow);
    formLay->addWidget(posGroup);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(form);
    root->addWidget(scroll, 1);

    // Todos os controles reagem a edição (prévia / aplicação ao vivo).
    connect(m_text, &QPlainTextEdit::textChanged, this, [this]() { onEdited(); });
    connect(m_font, &QComboBox::currentTextChanged, this, [this]() { onEdited(); });
    connect(m_size, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double) { onEdited(); });
    connect(m_bold, &QCheckBox::toggled, this, [this](bool) { onEdited(); });
    connect(m_outlineOn, &QCheckBox::toggled, this, [this](bool on) {
        m_outlineW->setEnabled(on);
        m_outlineBtn->setEnabled(on);
        onEdited();
    });
    connect(m_outlineW, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double) { onEdited(); });
    connect(m_bgOn, &QCheckBox::toggled, this, [this](bool on) {
        m_bgBtn->setEnabled(on);
        m_bgAlpha->setEnabled(on);
        onEdited();
    });
    connect(m_bgAlpha, &QSlider::valueChanged, this, [this](int) { onEdited(); });
    connect(m_align, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { onEdited(); });
    connect(m_posX, &QSlider::valueChanged, this, [this](int) { onEdited(); });
    connect(m_posY, &QSlider::valueChanged, this, [this](int) { onEdited(); });

    setContext(nullptr, nullptr);
}

QPushButton* TextEditorWidget::makeColorButton(const QColor& color) {
    auto* b = new QPushButton(this);
    b->setFixedWidth(48);
    b->setCursor(Qt::PointingHandCursor);
    setBtnColor(b, color);
    connect(b, &QPushButton::clicked, this, [this, b]() { pickColor(b); });
    return b;
}

void TextEditorWidget::pickColor(QPushButton* btn) {
    const QColor c = QColorDialog::getColor(btnColor(btn), this,
                                            tr("Escolher cor"),
                                            QColorDialog::ShowAlphaChannel);
    if (!c.isValid()) return;
    setBtnColor(btn, c);
    onEdited();
}

TextStyle* TextEditorWidget::style() const {
    if (!m_project || !m_clip) return nullptr;
    return m_project->textStyleFor(*m_clip);
}

void TextEditorWidget::setContext(Project* project, Clip* clip) {
    m_project = project;
    m_clip = clip;
    const bool ok = project && clip;
    setEnabled(ok);
    if (!ok) {
        m_preview->setStyle(TextStyle());
        return;
    }
    syncFromStyle();
}

void TextEditorWidget::syncFromStyle() {
    TextStyle* s = style();
    if (!s) return;
    m_updating = true;

    m_text->setPlainText(s->text);

    const QStringList fams = QFontDatabase().families();
    int fi = fams.indexOf(s->fontFamily.isEmpty()
                              ? QFont().family() : s->fontFamily);
    if (fi < 0) fi = fams.indexOf(QStringLiteral("DejaVu Sans"));
    if (fi < 0) fi = 0;
    m_font->setCurrentIndex(fi);

    m_size->setValue(s->textSize > 0.0 ? s->textSize * 100.0 : 5.6);
    m_bold->setChecked(s->textBold);
    setBtnColor(m_fillBtn, s->textColor);

    m_outlineOn->setChecked(s->textOutline > 0.0);
    m_outlineW->setValue(s->textOutline > 0.0 ? s->textOutline * 100.0 : 2.0);
    m_outlineW->setEnabled(s->textOutline > 0.0);
    setBtnColor(m_outlineBtn, s->textOutlineColor);
    m_outlineBtn->setEnabled(s->textOutline > 0.0);

    m_bgOn->setChecked(s->textBackground);
    setBtnColor(m_bgBtn, s->textBackgroundColor);
    m_bgBtn->setEnabled(s->textBackground);
    m_bgAlpha->setValue(s->textBackgroundColor.alpha() * 100 / 255);
    m_bgAlpha->setEnabled(s->textBackground);

    m_align->setCurrentIndex(qBound(0, s->textAlign, 2));
    m_posX->setValue((int)std::lround(s->textX * 100.0));
    m_posY->setValue((int)std::lround(s->textY * 100.0));

    m_updating = false;
    refreshPreview();
}

void TextEditorWidget::onEdited() {
    if (m_updating || !m_clip) return;
    if (m_live) apply();
    else refreshPreview();
}

void TextEditorWidget::apply() {
    TextStyle* s = style();
    if (!s) return;
    s->text = m_text->toPlainText();
    s->fontFamily = m_font->currentText().trimmed();
    s->textSize = m_size->value() / 100.0;
    s->textBold = m_bold->isChecked();
    s->textColor = btnColor(m_fillBtn);
    s->textOutline = m_outlineOn->isChecked() ? m_outlineW->value() / 100.0 : 0.0;
    s->textOutlineColor = btnColor(m_outlineBtn);
    s->textBackground = m_bgOn->isChecked();
    QColor bg = btnColor(m_bgBtn);
    bg.setAlpha(m_bgAlpha->value() * 255 / 100);
    s->textBackgroundColor = bg;
    s->textAlign = m_align->currentIndex();
    s->textX = m_posX->value() / 100.0;
    s->textY = m_posY->value() / 100.0;
    refreshPreview();
    emit changed();
}

void TextEditorWidget::refreshPreview() {
    TextStyle tmp;
    tmp.text = m_text->toPlainText();
    tmp.fontFamily = m_font->currentText().trimmed();
    tmp.textSize = m_size->value() / 100.0;
    tmp.textBold = m_bold->isChecked();
    tmp.textColor = btnColor(m_fillBtn);
    tmp.textOutline = m_outlineOn->isChecked() ? m_outlineW->value() / 100.0 : 0.0;
    tmp.textOutlineColor = btnColor(m_outlineBtn);
    tmp.textBackground = m_bgOn->isChecked();
    QColor bg = btnColor(m_bgBtn);
    bg.setAlpha(m_bgAlpha->value() * 255 / 100);
    tmp.textBackgroundColor = bg;
    tmp.textAlign = m_align->currentIndex();
    tmp.textX = m_posX->value() / 100.0;
    tmp.textY = m_posY->value() / 100.0;
    m_preview->setStyle(tmp);
}

// ── Diálogo modal ──────────────────────────────────────────────────────────

TextEditorDialog::TextEditorDialog(Project* project, Clip* clip, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Texto do clipe"));
    resize(520, 680);

    auto* root = new QVBoxLayout(this);
    m_editor = new TextEditorWidget(this);
    m_editor->setLive(false);
    m_editor->setContext(project, clip);
    root->addWidget(m_editor, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                         | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void TextEditorDialog::accept() {
    if (m_editor) m_editor->apply();
    QDialog::accept();
}
