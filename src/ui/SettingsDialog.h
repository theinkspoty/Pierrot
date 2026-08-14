#pragma once

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QSpinBox;
class QComboBox;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    // Configurações compartilhadas (lidas de qualquer lugar do app).
    static bool mkvWarningEnabled();
    static int  maxDecodeWidth();
    // Modo de exibição das miniaturas nos clipes da timeline:
    // 0 = todas (fatias contínuas), 1 = início e fim, 2 = nenhuma.
    static int  thumbMode();

    // Valores escolhidos no diálogo (aplicar depois do OK).
    bool autoSaveEnabled() const;
    int  autoSaveMinutes() const;
    bool mkvWarning() const;
    int  decodeWidth() const;

    // Avisa (uma vez por sessão) quando importa arquivos MKV experimentais.
    static void warnMkvIfNeeded(QWidget* parent, const QStringList& files);
protected:
    void accept() override;
private:
    QCheckBox* m_mkvWarn = nullptr;
    QCheckBox* m_autoSave = nullptr;
    QSpinBox*  m_autoInterval = nullptr;
    QComboBox* m_decodeWidth = nullptr;
    QComboBox* m_thumbMode = nullptr;
};
