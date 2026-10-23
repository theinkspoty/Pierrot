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

// Ao sair do widget, limpa os destaques de alças.
void TimelineWidget::leaveEvent(QEvent*) {
    if (!m_hoverCornerClip.isEmpty() || m_hoverCornerSide != 0) {
        m_hoverCornerClip.clear();
        m_hoverCornerSide = 0;
        m_mousePos = QPoint(-1, -1);
        m_mouseOnRuler = false;
        update();
    }
}

// mousePressEvent — implementado em TimelineDrag.cpp
// mouseMoveEvent — implementado em TimelineDrag.cpp
// mouseReleaseEvent — implementado em TimelineDrag.cpp
// mouseDoubleClickEvent — implementado em TimelineDrag.cpp
void TimelineWidget::wheelEvent(QWheelEvent* e) {
    const QPoint delta = e->angleDelta();
    if (e->modifiers() & Qt::ControlModifier) {
        if (delta.y() == 0) return;
        const double factor = std::pow(1.1, delta.y() / 120.0);
        const double anchorT = m_viewStart + (e->position().x() - kHeaderW) / m_pps;
        TlLog::note(QStringLiteral("roda zoom x%1 ancora=%2 (vs=%3 pps=%4)")
                        .arg(factor, 0, 'f', 2).arg(anchorT, 0, 'f', 1)
                        .arg(m_viewStart, 0, 'f', 1).arg(m_pps, 0, 'f', 1));
        animateZoomTo(m_pps * factor, anchorT);
    } else if (delta.y() != 0) {
        m_vbar->setValue(m_vbar->value() - delta.y());
    } else if (delta.x() != 0) {
        m_hbar->setValue(m_hbar->value() - delta.x());
    }
    e->accept();
}

// Tecla configurada para a Tesoura (atalho "tool2", default R), usada no modo
// hold-to-use (segurar ativa a tesoura, soltar restaura a ferramenta).
static Qt::Key razorHoldKey() {
    const QString v = QSettings().value("shortcuts/tool2").toString();
    if (!v.isEmpty()) {
        const QKeySequence ks(v, QKeySequence::PortableText);
        if (ks.count() == 1) {
            const int k = ks[0].key();
            if (k > 0 && k < 0x1000000) return Qt::Key(k);
        }
    }
    return Qt::Key_R;
}

void TimelineWidget::keyPressEvent(QKeyEvent* e) {
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    // Multicam: teclas 1..N trocam o ângulo ativo no playhead (Premiere).
    if (!ctrl && !shift && e->key() >= Qt::Key_1 && e->key() <= Qt::Key_9) {
        setMulticamAngleAtPlayhead(e->key() - Qt::Key_1);
        e->accept();
        return;
    }
    switch (e->key()) {
    case Qt::Key_Space:
        emit playPauseRequested();
        e->accept();
        break;
    case Qt::Key_Enter:
    case Qt::Key_Return:
        // Enter: pausa e move a agulha vermelha para o ponteiro branco (cursor).
        {
            if (m_playing) {
                emit playPauseRequested();
            }
            const double t = std::clamp(
                m_cursorT >= 0.0 ? m_cursorT : m_playhead, 0.0,
                m_project ? m_project->duration() : 0.0);
            setPlayhead(t);
            emit playheadChanged(t);
        }
        e->accept();
        break;
    case Qt::Key_S:
        cutAtPlayhead();
        e->accept();
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        // Vegas: com uma região de loop (seleção de tempo) ativa, Delete remove
        // o tempo dentro da região (ripple por padrão; Shift+Delete — ou ímã
        // desligado — deixa gap).
        if (m_loopOut > m_loopIn) {
            if (shift || !rippleCloseEnabled())
                deleteLoopLeaveGap();
            else
                deleteLoopRipple();
        } else {
            // Mescla seleção secundária na primária para deletar tudo junto.
            if (!m_secondarySelected.isEmpty()) {
                for (const QString& id : m_secondarySelected)
                    if (!m_selected.contains(id)) m_selected.append(id);
                m_secondarySelected.clear();
            }
            if (shift) {
                deleteSelectedLeaveGap();
            } else {
                // Fecha o vão (ripple) apenas com o ímã ligado e a opção ativa:
                // deleteSelection já considera os dois.
                deleteSelection();
            }
        }
        e->accept();
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomBy(1.6, m_playhead);
        e->accept();
        break;
    case Qt::Key_Minus:
        zoomBy(1.0 / 1.6, m_playhead);
        e->accept();
        break;
    case Qt::Key_C:
        if (ctrl) { copySelected(); e->accept(); break; }
        QWidget::keyPressEvent(e);
        break;
    case Qt::Key_X:
        if (ctrl) { cutSelected(); e->accept(); break; }
        QWidget::keyPressEvent(e);
        break;
    case Qt::Key_V:
        if (ctrl && shift) { pasteAttributes(); e->accept(); break; }
        if (ctrl) { pasteClips(); e->accept(); break; }
        if (shift) {
            // Shift+V: linha de volume/envelope da faixa de áudio.
            m_showVolLines = !m_showVolLines;
            invalidateScene();
            e->accept();
            break;
        }
        QWidget::keyPressEvent(e);
        break;
    case Qt::Key_D:
        if (ctrl) { duplicateSelected(); e->accept(); break; }
        cutAndDelete();
        e->accept();
        break;
    case Qt::Key_A:
        if (ctrl) { selectAllClips(); e->accept(); break; }
        QWidget::keyPressEvent(e);
        break;
    case Qt::Key_G:
        // Vegas: agrupa os clipes selecionados num grupo único.
        groupSelected();
        e->accept();
        break;
    case Qt::Key_B:
        // Ripple Edit (Premiere/Kdenlive): trim com ripple
        setTool(ToolRipple);
        e->accept();
        break;
    case Qt::Key_N:
        // Rolamento: ajustar fronteira entre 2 clipes
        setTool(ToolRolling);
        e->accept();
        break;
    case Qt::Key_Y:
        // Deslizar: mudar in/out sem mudar posição
        setTool(ToolSlip);
        e->accept();
        break;
    case Qt::Key_U:
        // Ctrl+U: Escorregar: mover clipe e adjacentes ajustam
        // U: Vegas: desagrupa os clipes selecionados
        if (ctrl) {
            setTool(ToolSlide);
            e->accept();
        } else {
            ungroupSelected();
            e->accept();
        }
        break;
    case Qt::Key_W:
        // Esticar Velocidade: mudar velocidade para preencher espaço
        setTool(ToolRateStretch);
        e->accept();
        break;
    case Qt::Key_M:
        // Marcador (Premiere): adiciona/remove marcador na agulha.
        toggleMarker(m_playhead);
        e->accept();
        break;
    case Qt::Key_Home:
        setPlayhead(0.0);
        emit playheadChanged(0.0);
        e->accept();
        break;
    case Qt::Key_End:
        if (m_project) {
            setPlayhead(m_project->duration());
            emit playheadChanged(m_project->duration());
        }
        e->accept();
        break;
    case Qt::Key_Q:
        // Vegas: "Q" liga/desliga o loop de reprodução da região de loop.
        m_loopEnabled = !m_loopEnabled;
        emit loopEnabledChanged(m_loopEnabled);
        update();
        e->accept();
        break;
    default:
        QWidget::keyPressEvent(e);
    }
}

bool TimelineWidget::eventFilter(QObject* o, QEvent* ev) {
    // Esc no editor inline de nome de faixa: restaura o texto e fecha.
    if (o == m_trackRename && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape) {
            m_trackRename->setText(m_renameOriginal);
            m_trackRename->clearFocus(); // → editingFinished → commit sem mudança
            if (m_trackRename && m_trackRename->isVisible())
                hideTrackRename();
            return true;
        }
        return false;
    }
    // Captura a tecla da Tesoura (default R) globalmente, inclusive quando o
    // foco do teclado está fora da timeline (preview, toolbar etc.). Nunca
    // interfere na digitação: qualquer campo de texto (ou diálogo modal) fica
    // fora da captura.
    if (ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == razorHoldKey()) {
            auto isTextInput = [](QObject* obj) -> bool {
                if (!obj || !obj->isWidgetType()) return false;
                return qobject_cast<QLineEdit*>(obj) || qobject_cast<QTextEdit*>(obj)
                    || qobject_cast<QPlainTextEdit*>(obj)
                    || qobject_cast<QAbstractSpinBox*>(obj)
                    || qobject_cast<QComboBox*>(obj)
                    || qobject_cast<QKeySequenceEdit*>(obj);
            };
            // Checa o receiver do evento E o widget realmente focado (cobre
            // editores internos, ex. o QPlainTextEdit do editor de texto).
            if (isTextInput(o) || isTextInput(qApp->focusWidget()))
                return false;
            // Com diálogo modal aberto (novo projeto, ajustes, salvar como…)
            // a tesoura não pode roubar a tecla.
            if (QApplication::activeModalWidget())
                return false;
            if (ev->type() == QEvent::KeyPress) {
                if (m_tool != ToolScissors && m_tempToolStore < 0) {
                    m_tempToolStore = m_tool;
                    setTool(ToolScissors);
                }
            } else if (m_tempToolStore >= 0) {
                // Se o usuário trocou de ferramenta na toolbar durante o
                // tecla segurada, respeita a escolha; senão restaura.
                if (m_tool == ToolScissors) setTool(m_tempToolStore);
                m_tempToolStore = -1;
            }
            return true; // consome: não vaza p/ outros widgets/atalhos
        }
    }
    return QWidget::eventFilter(o, ev);
}

