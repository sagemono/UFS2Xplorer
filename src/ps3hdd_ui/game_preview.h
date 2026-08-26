#pragma once

#include <QByteArray>
#include <QPixmap>
#include <QString>
#include <QWidget>

namespace ps3hdd::ui {

class game_preview : public QWidget {
    Q_OBJECT
public:
    explicit game_preview(QWidget* parent = nullptr);
    void set_content(const QString& title, const QString& subtitle, const QByteArray& icon0_png, const QByteArray& pic1_png);
    void clear_content();
    bool has_content() const { return has_; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QString title_;
    QString subtitle_;
    QPixmap icon0_;
    QPixmap pic1_;
    bool has_ = false;
};

} // namespace ps3hdd::ui