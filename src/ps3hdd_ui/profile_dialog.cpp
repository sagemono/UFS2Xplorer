#include "profile_dialog.h"

#include "reveal_field.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

namespace ps3hdd::ui {

namespace {
int hex_digits(const QString& s) {
    int n = 0;
    for (QChar c : s)
        if (c.isLetterOrNumber() && QString(QStringLiteral("0123456789abcdefABCDEF")).contains(c)) ++n;
    return n;
}
} // namespace

profile_dialog::profile_dialog(const key_entry& initial, bool serial_locked, QWidget* parent)
    : QDialog(parent), initial_(initial) {
    setWindowTitle(initial.hex_key.isEmpty() ? QStringLiteral("Add drive profile") : QStringLiteral("Edit drive profile"));
    resize(560, 260);

    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    nickname_ = new QLineEdit(initial.nickname);
    nickname_->setPlaceholderText(QStringLiteral("a name for this console (optional)"));
    serial_ = new QLineEdit(initial.serial);
    serial_->setPlaceholderText(QStringLiteral("drive serial"));
    serial_->setReadOnly(serial_locked);
    eid_ = new QLineEdit(initial.hex_key);
    eid_->setPlaceholderText(QStringLiteral("EID Root Key - 96 hex characters (48 bytes)"));
    add_reveal(eid_);
    idps_ = new QLineEdit(initial.idps);
    idps_->setPlaceholderText(QStringLiteral("IDPS - 32 hex characters (optional)"));
    add_reveal(idps_);
    account_ = new QLineEdit(initial.account_id);
    account_->setPlaceholderText(QStringLiteral("account id (optional)"));

    form->addRow(QStringLiteral("Nickname:"), nickname_);
    form->addRow(QStringLiteral("Serial:"), serial_);
    form->addRow(QStringLiteral("EID Root Key:"), eid_);
    form->addRow(QStringLiteral("IDPS:"), idps_);
    form->addRow(QStringLiteral("Account ID:"), account_);
    lay->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &profile_dialog::accept_if_valid);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void profile_dialog::accept_if_valid() {
    if (hex_digits(eid_->text()) != 96) {
        QMessageBox::warning(this, QStringLiteral("Invalid key"), QStringLiteral("The EID Root Key must be 96 hex characters (48 bytes)."));
        return;
    }
    const int idps_len = hex_digits(idps_->text());
    if (idps_len != 0 && idps_len != 32) {
        QMessageBox::warning(this, QStringLiteral("Invalid IDPS"), QStringLiteral("The IDPS must be 32 hex characters (16 bytes), or left blank."));
        return;
    }
    result_ = initial_; // keep date_added / encryption_type
    result_.nickname = nickname_->text().trimmed();
    result_.serial = serial_->text().trimmed();
    result_.hex_key = eid_->text().trimmed();
    result_.idps = idps_->text().trimmed();
    result_.account_id = account_->text().trimmed();
    accept();
}

} // namespace ps3hdd::ui