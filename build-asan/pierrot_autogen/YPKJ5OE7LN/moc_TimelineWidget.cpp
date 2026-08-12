/****************************************************************************
** Meta object code from reading C++ file 'TimelineWidget.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/ui/TimelineWidget.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'TimelineWidget.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN14TimelineWidgetE_t {};
} // unnamed namespace

template <> constexpr inline auto TimelineWidget::qt_create_metaobjectdata<qt_meta_tag_ZN14TimelineWidgetE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "TimelineWidget",
        "playheadChanged",
        "",
        "t",
        "modified",
        "playPauseRequested",
        "editStart",
        "toolChanged",
        "tool",
        "loopChanged",
        "in",
        "out",
        "selectionChanged",
        "id",
        "pancropRequested",
        "mediaImported",
        "cutAtPlayhead",
        "deleteSelected",
        "deleteSelectedLeaveGap",
        "deleteClipBeforePlayhead",
        "deleteClipAfterPlayhead",
        "zoomBy",
        "factor",
        "centerT",
        "toggleMarker",
        "setTool",
        "setSnap",
        "on",
        "setLoopInAtPlayhead",
        "setLoopOutAtPlayhead",
        "clearLoop"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'playheadChanged'
        QtMocHelpers::SignalData<void(double)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 3 },
        }}),
        // Signal 'modified'
        QtMocHelpers::SignalData<void()>(4, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'playPauseRequested'
        QtMocHelpers::SignalData<void()>(5, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'editStart'
        QtMocHelpers::SignalData<void()>(6, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'toolChanged'
        QtMocHelpers::SignalData<void(int)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 8 },
        }}),
        // Signal 'loopChanged'
        QtMocHelpers::SignalData<void(double, double)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 10 }, { QMetaType::Double, 11 },
        }}),
        // Signal 'selectionChanged'
        QtMocHelpers::SignalData<void(const QString &)>(12, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 13 },
        }}),
        // Signal 'pancropRequested'
        QtMocHelpers::SignalData<void(const QString &)>(14, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 13 },
        }}),
        // Signal 'mediaImported'
        QtMocHelpers::SignalData<void()>(15, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'cutAtPlayhead'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'deleteSelected'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'deleteSelectedLeaveGap'
        QtMocHelpers::SlotData<void()>(18, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'deleteClipBeforePlayhead'
        QtMocHelpers::SlotData<void()>(19, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'deleteClipAfterPlayhead'
        QtMocHelpers::SlotData<void()>(20, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'zoomBy'
        QtMocHelpers::SlotData<void(double, double)>(21, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 22 }, { QMetaType::Double, 23 },
        }}),
        // Slot 'toggleMarker'
        QtMocHelpers::SlotData<void(double)>(24, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 3 },
        }}),
        // Slot 'setTool'
        QtMocHelpers::SlotData<void(int)>(25, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 8 },
        }}),
        // Slot 'setSnap'
        QtMocHelpers::SlotData<void(bool)>(26, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 27 },
        }}),
        // Slot 'setLoopInAtPlayhead'
        QtMocHelpers::SlotData<void()>(28, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'setLoopOutAtPlayhead'
        QtMocHelpers::SlotData<void()>(29, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'clearLoop'
        QtMocHelpers::SlotData<void()>(30, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<TimelineWidget, qt_meta_tag_ZN14TimelineWidgetE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject TimelineWidget::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14TimelineWidgetE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14TimelineWidgetE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN14TimelineWidgetE_t>.metaTypes,
    nullptr
} };

void TimelineWidget::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<TimelineWidget *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->playheadChanged((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 1: _t->modified(); break;
        case 2: _t->playPauseRequested(); break;
        case 3: _t->editStart(); break;
        case 4: _t->toolChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 5: _t->loopChanged((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2]))); break;
        case 6: _t->selectionChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 7: _t->pancropRequested((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 8: _t->mediaImported(); break;
        case 9: _t->cutAtPlayhead(); break;
        case 10: _t->deleteSelected(); break;
        case 11: _t->deleteSelectedLeaveGap(); break;
        case 12: _t->deleteClipBeforePlayhead(); break;
        case 13: _t->deleteClipAfterPlayhead(); break;
        case 14: _t->zoomBy((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2]))); break;
        case 15: _t->toggleMarker((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 16: _t->setTool((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 17: _t->setSnap((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 18: _t->setLoopInAtPlayhead(); break;
        case 19: _t->setLoopOutAtPlayhead(); break;
        case 20: _t->clearLoop(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (TimelineWidget::*)(double )>(_a, &TimelineWidget::playheadChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (TimelineWidget::*)()>(_a, &TimelineWidget::modified, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (TimelineWidget::*)()>(_a, &TimelineWidget::playPauseRequested, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (TimelineWidget::*)()>(_a, &TimelineWidget::editStart, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (TimelineWidget::*)(int )>(_a, &TimelineWidget::toolChanged, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (TimelineWidget::*)(double , double )>(_a, &TimelineWidget::loopChanged, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (TimelineWidget::*)(const QString & )>(_a, &TimelineWidget::selectionChanged, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (TimelineWidget::*)(const QString & )>(_a, &TimelineWidget::pancropRequested, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (TimelineWidget::*)()>(_a, &TimelineWidget::mediaImported, 8))
            return;
    }
}

const QMetaObject *TimelineWidget::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *TimelineWidget::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14TimelineWidgetE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int TimelineWidget::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 21)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 21;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 21)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 21;
    }
    return _id;
}

// SIGNAL 0
void TimelineWidget::playheadChanged(double _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void TimelineWidget::modified()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void TimelineWidget::playPauseRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void TimelineWidget::editStart()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void TimelineWidget::toolChanged(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void TimelineWidget::loopChanged(double _t1, double _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1, _t2);
}

// SIGNAL 6
void TimelineWidget::selectionChanged(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1);
}

// SIGNAL 7
void TimelineWidget::pancropRequested(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1);
}

// SIGNAL 8
void TimelineWidget::mediaImported()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}
QT_WARNING_POP
