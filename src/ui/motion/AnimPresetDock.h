// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UI_ANIMPRESETDOCK_H
#define UI_ANIMPRESETDOCK_H

#include <QWidget>

class QLabel;
class QTimer;
class QShowEvent;
class QHideEvent;
class TimelineWidget;

// Dock "Animação": presets de keyframes (entrada/saída/idle/efeito) aplicados
// aos clipes SELECIONADOS na timeline. Os tempos do preset são escalados pela
// duração real de cada clipe; as props tocadas pelo preset são substituídas.
// Cada preset tem miniatura animada (estilo Vegas) redesenhada em loop por um
// timer único que só roda enquanto o dock está visível.
class AnimPresetDock : public QWidget {
    Q_OBJECT
public:
    explicit AnimPresetDock(QWidget* parent = nullptr);
    ~AnimPresetDock() override;
    void setTimeline(TimelineWidget* tl);
    void applyPreset(const QString& name);
    void applyTransition(const QString& type);

    // Timer único que anima as miniaturas dos presets (33 ms); os cards se
    // conectam a ele e só redesenham quando o dock está visível.
    QTimer* thumbTimer() const;

signals:
    void presetsApplied(int count);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    TimelineWidget* m_timeline = nullptr;
    QLabel* m_helpLabel = nullptr;
    QTimer* m_timer = nullptr;
};

#endif // UI_ANIMPRESETDOCK_H
