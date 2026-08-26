#pragma once

#include <QAction>
#include <QIcon>
#include <QLineEdit>

namespace ps3hdd::ui {

inline void add_reveal(QLineEdit* le) {
    le->setEchoMode(QLineEdit::Password);
    QAction* eye = le->addAction(QIcon(QStringLiteral(":/icons/eye.png")), QLineEdit::TrailingPosition);
    eye->setToolTip(QStringLiteral("Show / hide"));
    QObject::connect(eye, &QAction::triggered, le, [le] {
        le->setEchoMode(le->echoMode() == QLineEdit::Password ? QLineEdit::Normal : QLineEdit::Password);
    });
}

} // namespace ps3hdd::ui