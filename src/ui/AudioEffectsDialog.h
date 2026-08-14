#pragma once

#include <QDialog>
#include "models/Project.h"

class QSlider;
class QLabel;
class QCheckBox;

// Diálogo de efeitos de áudio de um clipe.
class AudioEffectsDialog : public QDialog {
    Q_OBJECT
public:
    explicit AudioEffectsDialog(Clip* clip, QWidget* parent = nullptr);
    void accept() override;

private:
    Clip* m_clip;
    QSlider* m_low = nullptr;
    QLabel* m_lowLabel = nullptr;
    QSlider* m_mid = nullptr;
    QLabel* m_midLabel = nullptr;
    QSlider* m_high = nullptr;
    QLabel* m_highLabel = nullptr;
    QCheckBox* m_denoise = nullptr;
    QSlider* m_denoiseAmt = nullptr;
    QLabel* m_denoiseAmtLabel = nullptr;
    QCheckBox* m_normalize = nullptr;
    QCheckBox* m_invert = nullptr;
};
