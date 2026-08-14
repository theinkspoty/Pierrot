// Pierrot — editor de vídeo estilo Vegas Pro
//
// Copyright (C) 2026 Pierrot contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "ui/ClickLogger.h"

#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QWidget>
#include <QToolButton>
#include <QPushButton>
#include <QComboBox>
#include <QMenu>
#include <QMenuBar>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QModelIndex>

ClickLogger::ClickLogger(QObject* parent) : QObject(parent) {}

void ClickLogger::install() {
    static ClickLogger* logger = nullptr;
    if (logger) return;
    if (qgetenv("PIERROT_CLICK_LOG").isNull()) return;
    logger = new ClickLogger(qApp);
    qApp->installEventFilter(logger);
}

static QString itemTextAt(QWidget* w, const QPoint& localPos) {
    if (auto* view = qobject_cast<QAbstractItemView*>(w)) {
        const QModelIndex idx = view->indexAt(localPos);
        if (idx.isValid())
            return idx.data().toString();
    }
    return {};
}

static QString describe(QWidget* w, const QPoint& localPos) {
    if (!w) return QStringLiteral("<none>");
    QString text;
    if (auto* tb = qobject_cast<QToolButton*>(w)) {
        if (tb->defaultAction()) text = tb->defaultAction()->text();
        else text = tb->text();
    } else if (auto* pb = qobject_cast<QPushButton*>(w)) {
        text = pb->text();
    } else if (auto* cb = qobject_cast<QComboBox*>(w)) {
        text = QStringLiteral("combo[%1]").arg(cb->currentText());
    } else if (auto* menu = qobject_cast<QMenu*>(w)) {
        if (QAction* a = menu->actionAt(localPos))
            text = QStringLiteral("menu[%1]").arg(a->text());
    } else if (auto* menubar = qobject_cast<QMenuBar*>(w)) {
        if (QAction* a = menubar->actionAt(localPos))
            text = QStringLiteral("menubar[%1]").arg(a->text());
    } else {
        const QString it = itemTextAt(w, localPos);
        if (!it.isEmpty()) text = QStringLiteral("item[%1]").arg(it);
    }
    QStringList chain;
    for (QWidget* cur = w; cur; cur = cur->parentWidget()) {
        const QString cls = QString::fromUtf8(cur->metaObject()->className());
        const QString oname = cur->objectName();
        chain << (oname.isEmpty() || oname == cls ? cls : QStringLiteral("%1(%2)").arg(cls, oname));
    }
    const QRect g = w->geometry();
    QString out = QStringLiteral("%1 text=\"%2\" pos=%3,%4 rect=%5,%6+%7x%8 global=%9,%10 win=\"%11\" chain=%12")
        .arg(w->metaObject()->className(),
             text,
             QString::number(localPos.x()), QString::number(localPos.y()),
             QString::number(g.x()), QString::number(g.y()),
             QString::number(g.width()), QString::number(g.height()),
             QString::number(QCursor::pos().x()), QString::number(QCursor::pos().y()),
             w->window() && w->window()->windowTitle().isEmpty()
                 ? QStringLiteral("(sem título)") : w->window() ? w->window()->windowTitle() : QStringLiteral("(sem janela)"),
             chain.join(" < "));
    return out;
}

bool ClickLogger::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseButtonDblClick) {
        if (auto* me = static_cast<QMouseEvent*>(ev)) {
            if (me->button() == Qt::LeftButton || me->button() == Qt::RightButton) {
                if (auto* w = qobject_cast<QWidget*>(obj)) {
                    const QString line = QStringLiteral("%1 %2  %3")
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                             ev->type() == QEvent::MouseButtonDblClick ? QStringLiteral("DCLK") : QStringLiteral("CLK "),
                             describe(w, me->pos()));
                    QFile f(QString::fromLocal8Bit(qgetenv("PIERROT_CLICK_LOG")));
                    if (f.open(QIODevice::Append | QIODevice::Text)) {
                        QTextStream ts(&f);
                        ts << line << Qt::endl;
                    }
                }
            }
        }
    }
    return QObject::eventFilter(obj, ev);
}
