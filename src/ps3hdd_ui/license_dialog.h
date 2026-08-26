#pragma once

#include "worker.h"

#include <QDialog>

class QLineEdit;
class QRadioButton;
class QCheckBox;

namespace ps3hdd::ui {

class license_dialog : public QDialog {
    Q_OBJECT
public:
    explicit license_dialog(QWidget* parent = nullptr);

    bool fill(job& j) const;

    void set_rap(const QString& path);
    void set_idps(const QString& hex);
    void set_account(const QString& id);
    void prefer_full_activation();
    QString idps_text() const;
    QString account_text() const;
    bool remember() const;

private:
    QLineEdit* rap_edit_;
    QLineEdit* idps_edit_;
    QRadioButton* reuse_radio_;
    QRadioButton* full_radio_;
    QLineEdit* account_edit_;
    QCheckBox* force_check_;
    QCheckBox* remember_check_;
};

} // namespace ps3hdd::ui