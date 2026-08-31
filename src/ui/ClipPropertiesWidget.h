// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QVector>
#include <functional>
#include "models/Project.h"

class QLabel;
class QScrollArea;
class QDoubleSpinBox;
class QToolButton;
class QComboBox;
class QCheckBox;
class QPushButton;
class QTimer;

// Painel "Propriedades" (estilo Effect Controls do Premiere/After Effects):
// dock persistente que segue a seleção.
//
// Para CLIPES NORMAIS da timeline:
//   • Clipe: velocidade, volume, opacidade, fades.
//   • Motion (AE): posição, escala X/Y, rotação, âncora — com stopwatch de
//     keyframe (adiciona/remove keyframe no playhead, azul quando animado).
//   • Motion Blur (MotiOn): liga/desliga, intensidade, ângulo, amostras.
//   • Cor/Efeitos: brilho, contraste, saturação, desfoque, grayscale, chroma.
//   • Texto… (clipes de vídeo).
//
// Para camadas da MESA (camada selecionada no canvas, ou clipe cuja track
// pertence a uma composição):
//   • Motion (Mesa): posição, escala, rotação, âncora, opacidade — stopwatch.
//   • Camada: blend mode + "Motion blur (allow)" (Vegas).
//   • Câmera: X/Y/zoom/rotação — stopwatch.
//   • Composição: motion blur global + samples + shutter.
//
// Substitui o diálogo antigo "Propriedades do clipe".
class ClipPropertiesWidget : public QWidget {
    Q_OBJECT
public:
    explicit ClipPropertiesWidget(QWidget* parent = nullptr);

    void setProject(Project* p);
    void setPlayhead(double t);

    // Alvos (a timeline/canvas decide qual mostrar por seleção).
    void showClip(const QString& clipId);
    void showMesaLayer(const QString& trackId);
    void showMesaCamera(const QString& mesaId);

signals:
    void editStart();
    void modified();

public slots:
    // Re-resolve os alvos no projeto atual (usado após undo/redo/abertura).
    void refresh();

private:
    void rebuild();
    void clearBody();
    void addSectionTitle(const QString& title);
    QDoubleSpinBox* addSpinRow(const QString& label, double lo, double hi, double step,
                               int dec, int minW, double dim,
                               std::function<double()> read,
                               std::function<void(double)> write,
                               std::function<QVector<Keyframe>*()> keys = {});
    QCheckBox* addCheckRow(const QString& label, const QString& tip,
                           std::function<bool()> read,
                           std::function<void(bool)> write);
    QComboBox* addComboRow(const QString& label, const QStringList& items,
                           const QStringList& values,
                           std::function<QString()> read,
                           std::function<void(const QString&)> write);
    void addActionRow(const QString& label, const QString& text, const QString& tip,
                      std::function<void()> onClick);

    // Helpers de ferro (re-resolvem no projeto atual — seguros após undo).
    Clip* clipOf(const QString& id) const;
    Track* trackOf(const QString& id) const;
    Track* trackOfClip(Clip* c) const;
    Track* ctxMesaTrack() const;        // camada da Mesa em edição (ou do clipe)
    MesaComposition* ctxMesa() const;   // composição em contexto

    void beginEdit();
    void emitEdited();
    void refreshValues();
    void refreshStopwatches();

    Project* m_project = nullptr;
    QString m_clipId;
    QString m_trackId;
    QString m_mesaId;
    double m_playhead = 0.0;
    bool m_creating = false;   // durante rebuild (evita commits/resync)
    bool m_syncing = false;    // setValue programático (evita commits)
    bool m_undoPushed = false;

    QScrollArea* m_scroll = nullptr;
    QWidget* m_body = nullptr;
    QLabel* m_header = nullptr;

    struct SpinRow {
        QDoubleSpinBox* spin;
        QToolButton* sw;
        std::function<double()> read;      // leitura RAW (já interpolada no playhead)
        std::function<void(double)> write; // grava RAW (base ou keyframe no playhead)
        std::function<QVector<Keyframe>*()> keys;
        double dim = 1.0;                  // raw * dim = valor exibido no spin
    };
    QVector<SpinRow> m_spinRows;

    struct CheckRow {
        QCheckBox* box;
        std::function<bool()> read;
    };
    QVector<CheckRow> m_checkRows;

    struct ComboRow {
        QComboBox* box;
        std::function<QString()> read;
        QStringList values;
    };
    QVector<ComboRow> m_comboRows;

    QTimer* m_undoTimer = nullptr;
};