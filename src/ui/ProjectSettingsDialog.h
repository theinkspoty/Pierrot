#pragma once

#include <QDialog>

class QSpinBox;
class QComboBox;

class ProjectSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProjectSettingsDialog(int width, int height, int fps,
                                   QWidget* parent = nullptr);
    int width() const;
    int height() const;
    int fps() const;
private:
    QSpinBox* m_w = nullptr;
    QSpinBox* m_h = nullptr;
    QComboBox* m_fps = nullptr;
};
