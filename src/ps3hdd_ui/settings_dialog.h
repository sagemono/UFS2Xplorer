#pragma once

#include <QDialog>
#include <QSettings>
#include <QString>

class QCheckBox;

namespace ps3hdd::ui {

namespace settings_keys {
inline QString org() { return QStringLiteral("ps3hddtool"); }
inline QString app() { return QStringLiteral("ps3hddtool"); }
inline const QString show_art_panel = QStringLiteral("ui/showArtPanel");
inline const QString show_tooltips = QStringLiteral("ui/showFolderTips");
inline const QString show_covers = QStringLiteral("ui/showCoverIcons");
inline const QString verify_before_install = QStringLiteral("ui/verifyBeforeInstall");
} // namespace settings_keys

bool app_setting(const QString& key, bool def);

class settings_dialog : public QDialog {
    Q_OBJECT
public:
    explicit settings_dialog(QWidget* parent = nullptr);

private slots:
    void save();

private:
    QCheckBox* art_ = nullptr;
    QCheckBox* tips_ = nullptr;
    QCheckBox* covers_ = nullptr;
    QCheckBox* verify_ = nullptr;
};

} // namespace ps3hdd::ui