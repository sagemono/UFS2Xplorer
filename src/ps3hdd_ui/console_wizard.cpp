#include "console_wizard.h"

#include "reveal_field.h"

#include <ps3hdd_disk/disk_source.h>

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWizardPage>

namespace ps3hdd::ui {

namespace {
QString clean_hex(const QString& s) {
    QString h;
    for (QChar c : s)
        if (c.isLetterOrNumber()) h += c;
    return h;
}

class fn_page : public QWizardPage {
public:
    std::function<bool()> complete;
    std::function<void()> on_enter;
    bool isComplete() const override { return complete ? complete() : true; }
    void initializePage() override { if (on_enter) on_enter(); }
    void bump() { emit completeChanged(); }
};
} // namespace

console_wizard::console_wizard(QVector<device> devices, test_fn test, QWidget* parent)
    : QWizard(parent), devices_(std::move(devices)) {
    setWindowTitle(QStringLiteral("Add a PS3 Console"));
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    resize(560, 360);

    {
        auto* p = new fn_page();
        p->setTitle(QStringLiteral("Select the PS3 hard drive"));
        p->setSubTitle(QStringLiteral("A PS3 disk has an encrypted partition table, so it shows up as \"unformatted\". Those are the likely candidates."));
        auto* lay = new QVBoxLayout(p);
        combo_ = new QComboBox();
        int first_raw = -1;
        for (int i = 0; i < devices_.size(); ++i) {
            const device& d = devices_[i];
            const QString tag = d.raw ? QStringLiteral("  - unformatted (maybe PS3)") : QStringLiteral("  - has partitions");
            combo_->addItem(QStringLiteral("%1  (%2)%3").arg(d.path, QString::fromStdString(disk::format_size(d.size)), tag), d.path);
            if (d.raw && first_raw < 0) first_raw = i;
        }
        if (first_raw >= 0) combo_->setCurrentIndex(first_raw);
        lay->addWidget(new QLabel(QStringLiteral("Disk:")));
        lay->addWidget(combo_);
        lay->addStretch(1);
        p->complete = [this] { return combo_->count() > 0; };
        addPage(p);
    }

    {
        auto* p = new fn_page();
        p->setTitle(QStringLiteral("Enter the EID Root Key"));
        p->setSubTitle(QStringLiteral("48 bytes of hex from your console. Use Test to confirm it unlocks this disk before continuing."));
        auto* lay = new QVBoxLayout(p);
        eid_edit_ = new QLineEdit();
        eid_edit_->setPlaceholderText(QStringLiteral("EID Root Key (48 bytes hex)"));
        add_reveal(eid_edit_);
        lay->addWidget(eid_edit_);
        auto* row = new QHBoxLayout();
        auto* test_btn = new QPushButton(QStringLiteral("Test"));
        test_label_ = new QLabel();
        row->addWidget(test_btn);
        row->addWidget(test_label_, 1);
        lay->addLayout(row);
        lay->addStretch(1);
        p->complete = [this] { return clean_hex(eid_edit_->text()).size() == 96; };
        QObject::connect(eid_edit_, &QLineEdit::textChanged, p, [p] { p->bump(); });
        QObject::connect(test_btn, &QPushButton::clicked, this, [this, test] {
            if (!test) return;
            test_label_->setText(QStringLiteral("Testing..."));
            const QString err = test(device_path(), eid_hex());
            if (err.isEmpty()) {
                test_label_->setText(QStringLiteral("Mounted the GameOS partition."));
                test_label_->setStyleSheet(QStringLiteral("color: #2e7d32;"));
            } else {
                test_label_->setText(QStringLiteral("X%1").arg(err));
                test_label_->setStyleSheet(QStringLiteral("color: #c62828;"));
            }
        });
        addPage(p);
    }

    {
        auto* p = new fn_page();
        p->setTitle(QStringLiteral("Console identity (optional)"));
        p->setSubTitle(QStringLiteral("Needed only for installing licenses. You can leave these blank and add them later."));
        auto* form = new QFormLayout(p);
        idps_edit_ = new QLineEdit();
        idps_edit_->setPlaceholderText(QStringLiteral("IDPS (16 bytes hex)"));
        add_reveal(idps_edit_);
        account_edit_ = new QLineEdit();
        account_edit_->setPlaceholderText(QStringLiteral("Account ID (e.g. 0200000000000000)"));
        form->addRow(QStringLiteral("IDPS:"), idps_edit_);
        form->addRow(QStringLiteral("Account ID:"), account_edit_);
        addPage(p);
    }
    {
        auto* p = new fn_page();
        p->setTitle(QStringLiteral("Name this console"));
        p->setSubTitle(QStringLiteral("So it is recognised automatically next time you plug the drive in."));
        auto* form = new QFormLayout(p);
        nick_edit_ = new QLineEdit();
        form->addRow(QStringLiteral("Nickname:"), nick_edit_);
        p->complete = [this] { return !nick_edit_->text().trimmed().isEmpty(); };
        p->on_enter = [this] {
            if (nick_edit_->text().trimmed().isEmpty()) nick_edit_->setText(device_serial());
        };
        QObject::connect(nick_edit_, &QLineEdit::textChanged, p, [p] { p->bump(); });
        addPage(p);
    }
}

QString console_wizard::device_path() const {
    return combo_ ? combo_->currentData().toString() : QString();
}
QString console_wizard::device_serial() const {
    const QString path = device_path();
    for (const device& d : devices_)
        if (d.path == path) return d.serial;
    return QString();
}
QString console_wizard::eid_hex() const { return eid_edit_->text().trimmed(); }
QString console_wizard::idps_hex() const { return idps_edit_->text().trimmed(); }
QString console_wizard::account_id() const { return account_edit_->text().trimmed(); }
QString console_wizard::nickname() const { return nick_edit_->text().trimmed(); }

} // namespace ps3hdd::ui