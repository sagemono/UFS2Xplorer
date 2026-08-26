#pragma once

#include "sfo_util.h"

#include <QDialog>
#include <QString>

#include <vector>

class QTableWidget;

namespace ps3hdd::ui {

class sfo_editor : public QDialog {
    Q_OBJECT
public:
    sfo_editor(std::vector<sfo_field> fields, const QString& subtitle, QWidget* parent = nullptr);

    std::vector<sfo_field> fields() const { return result_; }

private slots:
    void add_row();
    void remove_row();
    void edit_field();
    void save();

private:
    void append_row(const sfo_field& f);
    void refresh_decoded(int row);

    QTableWidget* table_ = nullptr;
    std::vector<sfo_field> result_;
};

} // namespace ps3hdd::ui