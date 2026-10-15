// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UI_MOTIONSCRIPTDOCK_H
#define UI_MOTIONSCRIPTDOCK_H

#include <QWidget>

class QPlainTextEdit;
class QPushButton;
class QComboBox;
class QLabel;
class QToolButton;
class TimelineWidget;

class MotionScriptDock : public QWidget {
    Q_OBJECT
public:
    explicit MotionScriptDock(QWidget* parent = nullptr);
    void setTimeline(TimelineWidget* tl);

signals:
    void clipConverted();

private slots:
    void convertToClip();
    void applyTemplateDefault();
    void applyTemplateTextAnim();
    void loadFile();
    void saveFile();

private:
    TimelineWidget* m_timeline = nullptr;
    QPlainTextEdit* m_editor = nullptr;
    QPushButton* m_convertBtn = nullptr;
    QComboBox* m_trackCombo = nullptr;
    QLabel* m_helpLabel = nullptr;
};

#endif // UI_MOTIONSCRIPTDOCK_H
