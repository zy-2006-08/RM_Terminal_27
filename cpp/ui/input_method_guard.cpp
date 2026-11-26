#include "input_method_guard.h"

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QEvent>
#include <QInputMethodQueryEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QQuickItem>
#include <QTextEdit>
#include <QWidget>

namespace rm_terminal
{

    namespace
    {

        // 这个对象自己会不会答「接输入法」:和 QWidget::event、QQuickItem 的默认处理一样算。
        bool claims_input_method(const QObject* object)
        {
            if (const auto* widget = qobject_cast<const QWidget*>(object))
            {
                const QVariant answer = widget->inputMethodQuery(Qt::ImEnabled);
                return answer.isValid() ? answer.toBool()
                                        : widget->isEnabled() && widget->testAttribute(Qt::WA_InputMethodEnabled);
            }
            if (const auto* item = qobject_cast<const QQuickItem*>(object))
                return item->flags().testFlag(QQuickItem::ItemAcceptsInputMethod);
            return false;
        }

        QString describe(const QObject* object)
        {
            QString text = QString::fromLatin1(object->metaObject()->className());
            if (!object->objectName().isEmpty())
                text += QLatin1Char('#') + object->objectName();
            if (const QObject* parent = object->parent())
            {
                text += QStringLiteral(" in ") + QString::fromLatin1(parent->metaObject()->className());
                if (!parent->objectName().isEmpty())
                    text += QLatin1Char('#') + parent->objectName();
            }
            return text;
        }

    } // namespace

    InputMethodGuard::InputMethodGuard(QObject* parent) : QObject(parent) {}

    bool InputMethodGuard::takesText(const QObject* object)
    {
        // 藏起来的框也不算:焦点留在一个看不见的输入框上时,操作手根本不知道字母去哪了。
        if (const auto* widget = qobject_cast<const QWidget*>(object))
        {
            if (!widget->isVisible())
                return false;
            if (qobject_cast<const QLineEdit*>(widget) || qobject_cast<const QTextEdit*>(widget) ||
                qobject_cast<const QPlainTextEdit*>(widget) || qobject_cast<const QAbstractSpinBox*>(widget))
                return true;
            if (const auto* combo = qobject_cast<const QComboBox*>(widget))
                return combo->isEditable();
            return false;
        }
        if (const auto* item = qobject_cast<const QQuickItem*>(object))
            return item->isVisible() && item->flags().testFlag(QQuickItem::ItemAcceptsInputMethod);
        return false;
    }

    bool InputMethodGuard::eventFilter(QObject* watched, QEvent* event)
    {
        if (event->type() != QEvent::InputMethodQuery || takesText(watched))
            return false;
        auto* query = static_cast<QInputMethodQueryEvent*>(event);
        if (!query->queries().testFlag(Qt::ImEnabled))
            return false;
        if (claims_input_method(watched))
        {
            const QString what = describe(watched);
            if (!reported_.contains(what))
            {
                reported_.insert(what);
                Q_EMIT blocked(what);
            }
        }
        query->setValue(Qt::ImEnabled, false);
        query->accept();
        return true;
    }

} // namespace rm_terminal
