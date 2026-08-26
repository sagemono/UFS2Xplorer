#include "license_dialog.h"

#include "reveal_field.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace ps3hdd::ui {

namespace {
std::vector<std::byte> parse_hex(const QString& s) {
    std::string h;
    for (QChar c : s)
        if (std::isxdigit(static_cast<unsigned char>(c.toLatin1()))) h.push_back(c.toLatin1());
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(static_cast<std::byte>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}
QString content_id_from(const QString& rap_path) {
    QString name = rap_path.section(QRegularExpression(QStringLiteral("[/\\\\]")), -1);
    if (name.endsWith(QStringLiteral(".rap"), Qt::CaseInsensitive)) name.chop(4);
    return name;
}
} // namespace

license_dialog::license_dialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Install license"));
    auto* form = new QFormLayout();

    auto* rap_row = new QHBoxLayout();
    rap_edit_ = new QLineEdit();
    auto* browse = new QPushButton(QStringLiteral("Browse..."));
    rap_row->addWidget(rap_edit_, 1);
    rap_row->addWidget(browse);
    form->addRow(QStringLiteral("RAP file:"), rap_row);

    idps_edit_ = new QLineEdit();
    idps_edit_->setPlaceholderText(QStringLiteral("console IDPS (16 bytes hex)"));
    add_reveal(idps_edit_);
    form->addRow(QStringLiteral("IDPS:"), idps_edit_);

    reuse_radio_ = new QRadioButton(QStringLiteral("Reuse existing act.dat (recommended)"));
    full_radio_ = new QRadioButton(QStringLiteral("Full activation (fresh console)"));
    reuse_radio_->setChecked(true);
    form->addRow(QStringLiteral("Mode:"), reuse_radio_);
    form->addRow(QString(), full_radio_);

    account_edit_ = new QLineEdit();
    account_edit_->setPlaceholderText(QStringLiteral("account id (e.g. 0200000000000000)"));
    account_edit_->setEnabled(false);
    form->addRow(QStringLiteral("Account ID:"), account_edit_);

    force_check_ = new QCheckBox(QStringLiteral("Overwrite an existing act.dat (breaks other licenses)"));
    force_check_->setEnabled(false);
    form->addRow(QString(), force_check_);

    remember_check_ = new QCheckBox(QStringLiteral("Remember the IDPS for this console"));
    remember_check_->setChecked(true);
    form->addRow(QString(), remember_check_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(buttons);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Select RAP"), QString(), QStringLiteral("RAP files (*.rap);;All files (*)"));
        if (!f.isEmpty()) rap_edit_->setText(f);
    });
    auto sync_mode = [this] {
        const bool full = full_radio_->isChecked();
        account_edit_->setEnabled(full);
        force_check_->setEnabled(full);
    };
    connect(reuse_radio_, &QRadioButton::toggled, this, sync_mode);
    connect(full_radio_, &QRadioButton::toggled, this, sync_mode);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void license_dialog::set_rap(const QString& path) { rap_edit_->setText(path); }
void license_dialog::set_idps(const QString& hex) { idps_edit_->setText(hex); }
void license_dialog::set_account(const QString& id) { account_edit_->setText(id); }
void license_dialog::prefer_full_activation() { full_radio_->setChecked(true); }
QString license_dialog::idps_text() const { return idps_edit_->text(); }
QString license_dialog::account_text() const { return account_edit_->text(); }
bool license_dialog::remember() const { return remember_check_->isChecked(); }

bool license_dialog::fill(job& j) const {
    if (rap_edit_->text().isEmpty()) return false;
    const auto idps = parse_hex(idps_edit_->text());
    if (idps.size() != 16) return false;

    j.rap_path = rap_edit_->text();
    j.content_id = content_id_from(j.rap_path);
    j.idps = idps;
    if (full_radio_->isChecked()) {
        j.type = job::full_activation;
        j.account_id = std::strtoull(account_edit_->text().toStdString().c_str(), nullptr, 16);
        j.force = force_check_->isChecked();
    } else {
        j.type = job::license_rif_only;
    }
    return true;
}

} // namespace ps3hdd::ui