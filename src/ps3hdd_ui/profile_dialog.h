#pragma once

#include "key_store.h"

#include <QDialog>

class QLineEdit;

namespace ps3hdd::ui {

class profile_dialog : public QDialog {
    Q_OBJECT
public:
    explicit profile_dialog(const key_entry& initial = {}, bool serial_locked = false,  QWidget* parent = nullptr);

    key_entry result() const { return result_; }

private slots:
    void accept_if_valid();

private:
    QLineEdit* nickname_ = nullptr;
    QLineEdit* serial_ = nullptr;
    QLineEdit* eid_ = nullptr;
    QLineEdit* idps_ = nullptr;
    QLineEdit* account_ = nullptr;
    key_entry initial_;
    key_entry result_;
};

} // namespace ps3hdd::ui