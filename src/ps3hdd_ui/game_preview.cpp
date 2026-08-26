#include "game_preview.h"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QRect>

#include <algorithm>

namespace ps3hdd::ui {

game_preview::game_preview(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(240);
    setAutoFillBackground(true);
}

void game_preview::set_content(const QString& title, const QString& subtitle, const QByteArray& icon0_png, const QByteArray& pic1_png) {
    title_ = title;
    subtitle_ = subtitle;
    icon0_ = QPixmap();
    pic1_ = QPixmap();
    if (!icon0_png.isEmpty()) icon0_.loadFromData(icon0_png);
    if (!pic1_png.isEmpty()) pic1_.loadFromData(pic1_png);
    has_ = !icon0_.isNull() || !title_.isEmpty();
    update();
}

void game_preview::clear_content() {
    title_.clear();
    subtitle_.clear();
    icon0_ = QPixmap();
    pic1_ = QPixmap();
    has_ = false;
    update();
}

void game_preview::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRect r = rect();

    if (!pic1_.isNull()) {
        const QPixmap bg = pic1_.scaled(r.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const int x = (r.width() - bg.width()) / 2;
        const int y = (r.height() - bg.height()) / 2;
        p.drawPixmap(x, y, bg);
        p.fillRect(r, QColor(0, 0, 0, 140));
    } else {
        p.fillRect(r, palette().color(QPalette::Base));
    }

    int y = 18;
    const int margin = 16;
    const int avail = r.width() - 2 * margin;

    if (!icon0_.isNull() && avail > 0) {
        const int maxW = avail;
        const int maxH = std::min(r.height() / 2, 220);
        const QPixmap cover = icon0_.scaled(maxW, maxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawPixmap((r.width() - cover.width()) / 2, y, cover);
        y += cover.height() + 14;
    }

    const bool overArt = !pic1_.isNull();
    if (!title_.isEmpty()) {
        QFont f = font();
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() + 1.5);
        p.setFont(f);
        p.setPen(overArt ? QColor(Qt::white) : palette().color(QPalette::WindowText));
        QRect tr(margin, y, avail, r.height() - y);
        QRect used;
        p.drawText(tr, Qt::AlignHCenter | Qt::TextWordWrap, title_, &used);
        y += used.height() + 6;
    }
    if (!subtitle_.isEmpty()) {
        p.setFont(font());
        p.setPen(overArt ? QColor(220, 220, 220) : palette().color(QPalette::WindowText));
        QRect sr(margin, y, avail, r.height() - y);
        p.drawText(sr, Qt::AlignHCenter | Qt::TextWordWrap, subtitle_);
    }
}

} // namespace ps3hdd::ui