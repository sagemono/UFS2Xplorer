#include "settings_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace ps3hdd::ui {

bool app_setting(const QString& key, bool def) {
    QSettings s(settings_keys::org(), settings_keys::app());
    return s.value(key, def).toBool();
}

settings_dialog::settings_dialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Settings"));
    resize(440, 320);

    auto* lay = new QVBoxLayout(this);

    auto* browser = new QGroupBox(QStringLiteral("File browser"));
    auto* bl = new QVBoxLayout(browser);
    covers_ = new QCheckBox(QStringLiteral("Show cover art (ICON0) on game/save folders"));
    tips_ = new QCheckBox(QStringLiteral("Show PARAM.SFO details when hovering a folder"));
    art_ = new QCheckBox(QStringLiteral("Show the large XMB art panel (ICON0 + PIC1 backdrop)"));
    art_->setToolTip(QStringLiteral("Adds a side panel that paints the selected game's PIC1 background "
                                    "with its cover on top. Off by default; the hover tooltip covers the "
                                    "common case."));
    bl->addWidget(covers_);
    bl->addWidget(tips_);
    bl->addWidget(art_);
    lay->addWidget(browser);

    auto* install = new QGroupBox(QStringLiteral("Installing"));
    auto* il = new QVBoxLayout(install);
    verify_ = new QCheckBox(QStringLiteral("Verify each package before installing"));
    verify_->setToolTip(QStringLiteral("Checks every .pkg's NPDRM signature (genuine + untampered) and header "
                                       "before writing it. Fast - only the header is read, not the whole file - "
                                       "so it is safe to leave on even for big install queues."));
    il->addWidget(verify_);
    lay->addWidget(install);

    lay->addStretch(1);

    covers_->setChecked(app_setting(settings_keys::show_covers, true));
    tips_->setChecked(app_setting(settings_keys::show_tooltips, true));
    art_->setChecked(app_setting(settings_keys::show_art_panel, false));
    verify_->setChecked(app_setting(settings_keys::verify_before_install, true));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &settings_dialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void settings_dialog::save() {
    QSettings s(settings_keys::org(), settings_keys::app());
    s.setValue(settings_keys::show_covers, covers_->isChecked());
    s.setValue(settings_keys::show_tooltips, tips_->isChecked());
    s.setValue(settings_keys::show_art_panel, art_->isChecked());
    s.setValue(settings_keys::verify_before_install, verify_->isChecked());
    accept();
}

} // namespace ps3hdd::ui