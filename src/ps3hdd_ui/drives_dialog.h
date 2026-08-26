#pragma once

#include <QDialog>

class QTableWidget;

namespace ps3hdd::ui {

class key_store;

class drives_dialog : public QDialog {
    Q_OBJECT
public:
    explicit drives_dialog(key_store& store, QWidget* parent = nullptr);
    bool changed() const { return changed_; }

private slots:
    void add_profile();
    void edit_profile();
    void remove_profile();

private:
    void reload();
    int selected_row() const;

    key_store& store_;
    QTableWidget* table_ = nullptr;
    bool changed_ = false;
};

} // namespace ps3hdd::ui