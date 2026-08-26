#include "drives_dialog.h"

#include "key_store.h"
#include "profile_dialog.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ps3hdd::ui {

drives_dialog::drives_dialog(key_store& store, QWidget* parent) : QDialog(parent), store_(store) {
    setWindowTitle(QStringLiteral("Manage drives"));
    resize(640, 340);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(QStringLiteral("Saved console/drive profiles (EID key, IDPS and account):")));

    table_ = new QTableWidget(0, 5, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("Nickname"), QStringLiteral("Serial"), QStringLiteral("Key"), QStringLiteral("IDPS"), QStringLiteral("Account")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(table_, 1);

    auto* row = new QHBoxLayout();
    auto* add = new QPushButton(QStringLiteral("Add..."));
    auto* edit = new QPushButton(QStringLiteral("Edit..."));
    auto* del = new QPushButton(QStringLiteral("Remove"));
    auto* close = new QPushButton(QStringLiteral("Close"));
    row->addWidget(add);
    row->addWidget(edit);
    row->addWidget(del);
    row->addStretch(1);
    row->addWidget(close);
    lay->addLayout(row);

    connect(add, &QPushButton::clicked, this, &drives_dialog::add_profile);
    connect(edit, &QPushButton::clicked, this, &drives_dialog::edit_profile);
    connect(del, &QPushButton::clicked, this, &drives_dialog::remove_profile);
    connect(table_, &QTableWidget::doubleClicked, this, &drives_dialog::edit_profile);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    reload();
}

void drives_dialog::reload() {
    const auto& entries = store_.entries();
    table_->setRowCount(entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        const key_entry& e = entries[i];
        table_->setItem(i, 0, new QTableWidgetItem(e.nickname.isEmpty() ? QStringLiteral("(unnamed)") : e.nickname));
        table_->setItem(i, 1, new QTableWidgetItem(e.serial));
        table_->setItem(i, 2, new QTableWidgetItem(e.hex_key.isEmpty() ? QStringLiteral("-") : QStringLiteral("set")));
        table_->setItem(i, 3, new QTableWidgetItem(e.idps.isEmpty() ? QStringLiteral("-") : QStringLiteral("set")));
        table_->setItem(i, 4, new QTableWidgetItem(e.account_id.isEmpty() ? QStringLiteral("-") : e.account_id));
    }
    table_->resizeColumnsToContents();
    table_->horizontalHeader()->setStretchLastSection(true);
}

int drives_dialog::selected_row() const {
    const auto rows = table_->selectionModel()->selectedRows();
    return rows.isEmpty() ? -1 : rows.first().row();
}

void drives_dialog::add_profile() {
    profile_dialog dlg({}, /*serial_locked=*/false, this);
    if (dlg.exec() != QDialog::Accepted) return;
    store_.add_or_update(dlg.result());
    changed_ = true;
    reload();
}

void drives_dialog::edit_profile() {
    const int r = selected_row();
    if (r < 0 || r >= store_.entries().size()) return;
    const key_entry original = store_.entries()[r];
    profile_dialog dlg(original, /*serial_locked=*/false, this);
    if (dlg.exec() != QDialog::Accepted) return;
    store_.remove(original.hex_key);
    store_.add_or_update(dlg.result());
    changed_ = true;
    reload();
}

void drives_dialog::remove_profile() {
    const int r = selected_row();
    if (r < 0 || r >= store_.entries().size()) return;
    const key_entry e = store_.entries()[r];
    const QString name = e.nickname.isEmpty() ? (e.serial.isEmpty() ? QStringLiteral("this profile") : e.serial) : e.nickname;
    if (QMessageBox::warning(this, QStringLiteral("Remove drive"), QStringLiteral("Remove the saved profile for %1?\n\nThis only deletes the stored key/IDPS/account; the drive itself is untouched.").arg(name), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    store_.remove(e.hex_key);
    changed_ = true;
    reload();
}

} // namespace ps3hdd::ui