// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UI_ANIMPRESETDOCK_H
#define UI_ANIMPRESETDOCK_H

#include <QWidget>

class QLabel;
class TimelineWidget;

// Dock "Animação": presets de keyframes (entrada/saída/idle/efeito) aplicados
// aos clipes SELECIONADOS na timeline. Os tempos do preset são escalados pela
// duração real de cada clipe; as props tocadas pelo preset são substituídas.
class AnimPresetDock : public QWidget {
    Q_OBJECT
public:
    explicit AnimPresetDock(QWidget* parent = nullptr);
    void setTimeline(TimelineWidget* tl);
    void applyPreset(const QString& name);

signals:
    void presetsApplied(int count);

private:
    TimelineWidget* m_timeline = nullptr;
    QLabel* m_helpLabel = nullptr;
};

#endif // UI_ANIMPRESETDOCK_H
