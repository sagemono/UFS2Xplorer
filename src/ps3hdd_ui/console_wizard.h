#pragma once

#include <QString>
#include <QVector>
#include <QWizard>

#include <cstdint>
#include <functional>

class QComboBox;
class QLineEdit;
class QLabel;

namespace ps3hdd::ui {

class console_wizard : public QWizard {
    Q_OBJECT
public:
    struct device {
        QString path;
        QString serial;
        std::uint64_t size = 0;
        bool raw = false;
    };
    using test_fn = std::function<QString(const QString& path, const QString& eid_hex)>;

    console_wizard(QVector<device> devices, test_fn test, QWidget* parent = nullptr);

    QString device_path() const;
    QString device_serial() const;
    QString eid_hex() const;
    QString idps_hex() const;
    QString account_id() const;
    QString nickname() const;

private:
    QVector<device> devices_;
    QComboBox* combo_ = nullptr;
    QLineEdit* eid_edit_ = nullptr;
    QLabel* test_label_ = nullptr;
    QLineEdit* idps_edit_ = nullptr;
    QLineEdit* account_edit_ = nullptr;
    QLineEdit* nick_edit_ = nullptr;
};

} // namespace ps3hdd::ui