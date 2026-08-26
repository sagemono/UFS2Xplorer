#include "sfo_editor.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>

#include <vector>

namespace ps3hdd::ui {

namespace {
int fmt_to_idx(quint16 fmt) {
    if (fmt == 0x0404) return 1;
    if (fmt == 0x0004) return 2;
    return 0; // 0x0204
}
quint16 idx_to_fmt(int idx) {
    if (idx == 1) return 0x0404;
    if (idx == 2) return 0x0004;
    return 0x0204;
}

struct bitdef { quint32 bit; const char* label; };

const std::vector<bitdef> kResolution = {
    {0x01, "480 (4:3)"},  {0x02, "576 (4:3)"},   {0x04, "720 (16:9)"},
    {0x08, "1080 (16:9)"}, {0x10, "480 (16:9)"}, {0x20, "576 (16:9)"},
};
const std::vector<bitdef> kSoundFormat = {
    {0x001, "LPCM 2.0"}, {0x004, "LPCM 5.1"}, {0x010, "LPCM 7.1"},
    {0x100, "Dolby Digital 5.1"}, {0x200, "DTS 5.1"},
};
const std::vector<bitdef> kAttribute = {
    {0x00000001, "PSP Remote Play (v1)"},   {0x00000002, "PSP Export"},
    {0x00000004, "PSP Remote Play (v2)"},   {0x00000008, "In-game XMB: forced on"},
    {0x00000010, "In-game XMB: disabled"},  {0x00000020, "In-game XMB: background music"},
    {0x00000080, "PS Vita Remote Play"},    {0x00000100, "Move controller warning"},
    {0x00000200, "Navigation controller warning"}, {0x00000400, "PlayStation Eye warning"},
    {0x00000800, "Move calibration notice"}, {0x00001000, "Stereoscopic 3D warning"},
    {0x00010000, "Install disc (hide disc icon)"}, {0x00020000, "Install packages"},
    {0x00080000, "Game purchase enabled"},  {0x00100000, "In-game XMB (software, BOOTABLE=2)"},
    {0x00200000, "PCEngine"},               {0x00400000, "License logo disabled"},
    {0x00800000, "Move controller enabled"}, {0x04000000, "NeoGeo"},
};

const std::vector<bitdef>* bits_for(const QString& key) {
    const QString k = key.trimmed().toUpper();
    if (k == QStringLiteral("RESOLUTION")) return &kResolution;
    if (k == QStringLiteral("SOUND_FORMAT")) return &kSoundFormat;
    if (k == QStringLiteral("ATTRIBUTE")) return &kAttribute;
    return nullptr;
}

QString decode_summary(const QString& key, const QString& value) {
    const QString k = key.trimmed().toUpper();
    bool ok = false;
    const quint32 v = value.toUInt(&ok, 10);
    if (k == QStringLiteral("PARENTAL_LEVEL"))
        return !ok ? QString() : (v == 0 ? QStringLiteral("Off") : QStringLiteral("Level %1").arg(v));
    if (k == QStringLiteral("CATEGORY")) return category_name(value);
    if (k == QStringLiteral("BOOTABLE")) return !ok ? QString() : (v ? QStringLiteral("Yes") : QStringLiteral("No"));
    if (const auto* defs = bits_for(k); defs && ok) {
        QStringList on;
        quint32 used = 0;
        for (const auto& d : *defs)
            if (v & d.bit) { on << QLatin1String(d.label); used |= d.bit; }
        if (const quint32 rem = v & ~used) on << QStringLiteral("+0x%1").arg(rem, 0, 16);
        return on.isEmpty() ? QStringLiteral("(none)") : on.join(QStringLiteral(", "));
    }
    return {};
}

long long edit_bitfield(QWidget* parent, const QString& title, quint32 value, const std::vector<bitdef>& defs) {
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(QStringLiteral("Tick the features to enable. Undocumented bits are kept via the raw value below.")));

    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    auto* holder = new QWidget();
    auto* grid = new QGridLayout(holder);
    std::vector<QCheckBox*> boxes;
    for (std::size_t i = 0; i < defs.size(); ++i) {
        auto* cb = new QCheckBox(QStringLiteral("%1  (0x%2)").arg(QLatin1String(defs[i].label)).arg(defs[i].bit, 0, 16));
        boxes.push_back(cb);
        grid->addWidget(cb, static_cast<int>(i / 2), static_cast<int>(i % 2));
    }
    scroll->setWidget(holder);
    lay->addWidget(scroll, 1);

    auto* hexRow = new QHBoxLayout();
    hexRow->addWidget(new QLabel(QStringLiteral("Raw value (hex):")));
    auto* hex = new QLineEdit();
    hexRow->addWidget(hex, 1);
    lay->addLayout(hexRow);

    auto val = std::make_shared<quint32>(value);
    auto refresh_from_val = [&, val] {
        for (std::size_t i = 0; i < defs.size(); ++i) {
            QSignalBlocker b(boxes[i]);
            boxes[i]->setChecked((*val & defs[i].bit) != 0);
        }
        QSignalBlocker b(hex);
        hex->setText(QStringLiteral("%1").arg(*val, 8, 16, QLatin1Char('0')));
    };
    for (std::size_t i = 0; i < defs.size(); ++i)
        QObject::connect(boxes[i], &QCheckBox::toggled, &dlg, [&, val, i](bool on) {
            if (on) *val |= defs[i].bit; else *val &= ~defs[i].bit;
            refresh_from_val();
        });
    QObject::connect(hex, &QLineEdit::editingFinished, &dlg, [&, val, hex] {
        bool ok = false;
        const quint32 v = hex->text().toUInt(&ok, 16);
        if (ok) { *val = v; refresh_from_val(); }
    });
    refresh_from_val();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return -1;
    return static_cast<long long>(*val);
}
} // namespace

sfo_editor::sfo_editor(std::vector<sfo_field> fields, const QString& subtitle, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Edit PARAM.SFO"));
    resize(560, 460);

    auto* lay = new QVBoxLayout(this);
    auto* head = new QLabel(subtitle.isEmpty() ? QStringLiteral("Edit the content metadata below.") : subtitle);
    head->setWordWrap(true);
    lay->addWidget(head);

    table_ = new QTableWidget(0, 4, this);
    table_->setHorizontalHeaderLabels(
        {QStringLiteral("Key"), QStringLiteral("Type"), QStringLiteral("Value"), QStringLiteral("Decoded")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    lay->addWidget(table_, 1);

    for (const sfo_field& f : fields) append_row(f);

    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (!it || it->column() != 2) return;
        refresh_decoded(it->row());
    });

    auto* row = new QHBoxLayout();
    auto* add = new QPushButton(QStringLiteral("Add key"));
    auto* del = new QPushButton(QStringLiteral("Remove key"));
    auto* edit = new QPushButton(QStringLiteral("Edit field…"));
    edit->setToolTip(QStringLiteral("Friendly editor for the selected RESOLUTION / SOUND_FORMAT / ATTRIBUTE / PARENTAL_LEVEL row (checkboxes / slider)."));
    auto* save = new QPushButton(QStringLiteral("Save"));
    auto* cancel = new QPushButton(QStringLiteral("Cancel"));
    row->addWidget(add);
    row->addWidget(del);
    row->addWidget(edit);
    row->addStretch(1);
    row->addWidget(save);
    row->addWidget(cancel);
    lay->addLayout(row);

    connect(add, &QPushButton::clicked, this, &sfo_editor::add_row);
    connect(del, &QPushButton::clicked, this, &sfo_editor::remove_row);
    connect(edit, &QPushButton::clicked, this, &sfo_editor::edit_field);
    connect(save, &QPushButton::clicked, this, &sfo_editor::save);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}

void sfo_editor::append_row(const sfo_field& f) {
    const int r = table_->rowCount();
    table_->insertRow(r);

    auto* keyItem = new QTableWidgetItem(f.key);
    keyItem->setData(Qt::UserRole, f.max_len); // preserve the reserved slot size
    const QString help = sfo_key_help(f.key);
    if (!help.isEmpty()) keyItem->setToolTip(help);
    table_->setItem(r, 0, keyItem);

    auto* type = new QComboBox();
    type->addItem(QStringLiteral("Text"), 0x0204);
    type->addItem(QStringLiteral("Integer"), 0x0404);
    type->addItem(QStringLiteral("Text (raw)"), 0x0004);
    type->setCurrentIndex(fmt_to_idx(f.fmt));
    table_->setCellWidget(r, 1, type);

    table_->setItem(r, 2, new QTableWidgetItem(f.value));
    auto* dec = new QTableWidgetItem(decode_summary(f.key, f.value));
    dec->setFlags(dec->flags() & ~Qt::ItemIsEditable);
    dec->setForeground(QColor(0x9a, 0x9a, 0x9a));
    dec->setToolTip(dec->text());
    table_->setItem(r, 3, dec);
}

void sfo_editor::refresh_decoded(int r) {
    if (r < 0 || !table_->item(r, 0) || !table_->item(r, 2) || !table_->item(r, 3)) return;
    const QString text = decode_summary(table_->item(r, 0)->text(), table_->item(r, 2)->text());
    QSignalBlocker block(table_); // dont reenter itemChanged
    table_->item(r, 3)->setText(text);
    table_->item(r, 3)->setToolTip(text);
}

void sfo_editor::add_row() {
    append_row(sfo_field{});
    table_->editItem(table_->item(table_->rowCount() - 1, 0));
}

void sfo_editor::remove_row() {
    const int r = table_->currentRow();
    if (r >= 0) table_->removeRow(r);
}

void sfo_editor::edit_field() {
    const int r = table_->currentRow();
    if (r < 0 || !table_->item(r, 0) || !table_->item(r, 2)) {
        QMessageBox::information(this, QStringLiteral("Edit field"), QStringLiteral("Select a row first."));
        return;
    }
    const QString key = table_->item(r, 0)->text().trimmed().toUpper();
    QTableWidgetItem* valItem = table_->item(r, 2);
    bool ok = false;
    quint32 cur = valItem->text().toUInt(&ok, 10);
    if (!ok) cur = 0;

    auto set_int = [&](quint32 v) {
        valItem->setText(QString::number(v));
        if (auto* combo = qobject_cast<QComboBox*>(table_->cellWidget(r, 1)))
            combo->setCurrentIndex(1); // Integer
    };

    if (key == QStringLiteral("PARENTAL_LEVEL")) {
        bool okd = false;
        const int v = QInputDialog::getInt(
            this, QStringLiteral("Parental level"),
            QStringLiteral("Level (0 = disabled, 1 = lowest … 11 = highest):"),
            static_cast<int>(cur), 0, 11, 1, &okd);
        if (okd) set_int(static_cast<quint32>(v));
        return;
    }
    const auto* defs = bits_for(key);
    if (!defs) {
        QMessageBox::information(
            this, QStringLiteral("Edit field"),
            QStringLiteral("This row is not RESOLUTION, SOUND_FORMAT, ATTRIBUTE or PARENTAL_LEVEL.\nEdit its value directly in the table."));
        return;
    }
    const long long nv = edit_bitfield(this, QStringLiteral("Edit %1").arg(key), cur, *defs);
    if (nv >= 0) set_int(static_cast<quint32>(nv));
}

void sfo_editor::save() {
    std::vector<sfo_field> out;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString key = table_->item(r, 0) ? table_->item(r, 0)->text().trimmed() : QString();
        if (key.isEmpty()) continue; // skip blank rows
        auto* type = qobject_cast<QComboBox*>(table_->cellWidget(r, 1));
        const quint16 fmt = idx_to_fmt(type ? type->currentIndex() : 0);
        const QString value = table_->item(r, 2) ? table_->item(r, 2)->text() : QString();
        if (fmt == 0x0404) {
            bool ok = false;
            value.toUInt(&ok);
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("Invalid value"), QStringLiteral("Key \"%1\" is an Integer but its value \"%2\" is not a non-negative whole number.").arg(key, value));
                return;
            }
        }
        sfo_field f;
        f.key = key;
        f.fmt = fmt;
        f.value = value;
        f.max_len = table_->item(r, 0)->data(Qt::UserRole).toUInt();
        out.push_back(std::move(f));
    }
    if (out.empty()) {
        QMessageBox::warning(this, QStringLiteral("Nothing to save"), QStringLiteral("There are no keys to write."));
        return;
    }
    result_ = std::move(out);
    accept();
}

} // namespace ps3hdd::ui