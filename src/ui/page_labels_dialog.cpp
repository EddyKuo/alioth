#include "ui/page_labels_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace alioth::ui {
namespace {

enum Column { ColumnStartPage = 0, ColumnStyle, ColumnPrefix, ColumnStartNumber, ColumnCount };

struct StyleEntry {
    domain::PageLabelStyle style;
    const char* label;
};

// 五種樣式就是 PDF 規格的全部（/D /R /r /A /a），沒有第六種。
// 「無編號」是 style 缺席、只有前綴的情況，在規格裡是合法的，
// 因此必須是可以選的——不然使用者做不出「附錄」這種只有文字沒有號碼的標籤。
const StyleEntry kStyles[] = {
    {domain::PageLabelStyle::Decimal, QT_TRANSLATE_NOOP("alioth::ui::PageLabelsDialog", "1, 2, 3")},
    {domain::PageLabelStyle::RomanUpper,
     QT_TRANSLATE_NOOP("alioth::ui::PageLabelsDialog", "I, II, III")},
    {domain::PageLabelStyle::RomanLower,
     QT_TRANSLATE_NOOP("alioth::ui::PageLabelsDialog", "i, ii, iii")},
    {domain::PageLabelStyle::LettersUpper,
     QT_TRANSLATE_NOOP("alioth::ui::PageLabelsDialog", "A, B, C")},
    {domain::PageLabelStyle::LettersLower,
     QT_TRANSLATE_NOOP("alioth::ui::PageLabelsDialog", "a, b, c")},
    {domain::PageLabelStyle::None,
     QT_TRANSLATE_NOOP("alioth::ui::PageLabelsDialog", "無編號（只用前綴）")},
};

}  // namespace

PageLabelsDialog::PageLabelsDialog(domain::PageLabelMap labels, std::int32_t pageCount,
                                   QWidget* parent)
    : QDialog(parent), labels_(std::move(labels)), pageCount_(std::max(1, pageCount)) {
    setWindowTitle(tr("頁面標籤"));
    setObjectName(QStringLiteral("pageLabelsDialog"));
    resize(560, 380);

    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(
        tr("每一段從指定頁開始，直到下一段開始為止。刪掉所有段落代表移除頁面標籤，"
           "每一頁會回到用自己的頁碼顯示。"),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    table_ = new QTableWidget(this);
    table_->setObjectName(QStringLiteral("pageLabelRanges"));
    table_->setAccessibleName(tr("頁面標籤範圍"));
    table_->setColumnCount(ColumnCount);
    table_->setHorizontalHeaderLabels(
        {tr("起始頁"), tr("編號樣式"), tr("前綴"), tr("起始編號")});
    table_->horizontalHeader()->setSectionResizeMode(ColumnPrefix, QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(table_, 1);

    auto* buttons = new QDialogButtonBox(this);
    auto* addButton = buttons->addButton(tr("新增範圍"), QDialogButtonBox::ActionRole);
    removeButton_ = buttons->addButton(tr("刪除範圍"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Ok);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    connect(addButton, &QPushButton::clicked, this, &PageLabelsDialog::addRange);
    connect(removeButton_, &QPushButton::clicked, this, &PageLabelsDialog::removeSelectedRange);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &PageLabelsDialog::updateButtons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        std::vector<domain::PageLabelRange> ranges;
        QString diagnostic;
        if (!collectRanges(ranges, diagnostic)) {
            // 擋下來而不是自己修正：使用者輸入的範圍重疊時，我們猜不出他想要
            // 哪一段贏，而猜錯的後果是整份文件的頁碼都跑掉。
            QMessageBox::warning(this, tr("頁面標籤"), diagnostic);
            return;
        }
        labels_.setRanges(std::move(ranges));
        accept();
    });

    rebuildTable();
}

void PageLabelsDialog::rebuildTable() {
    table_->setRowCount(0);
    for (const domain::PageLabelRange& range : labels_.ranges()) {
        const int row = table_->rowCount();
        table_->insertRow(row);

        auto* startPage = new QSpinBox(table_);
        startPage->setRange(1, pageCount_);
        startPage->setValue(range.startPageIndex + 1);  // 使用者看到的頁碼從 1 起算
        table_->setCellWidget(row, ColumnStartPage, startPage);

        auto* style = new QComboBox(table_);
        for (const StyleEntry& entry : kStyles) {
            style->addItem(tr(entry.label), static_cast<int>(entry.style));
        }
        style->setCurrentIndex(static_cast<int>(
            std::distance(std::begin(kStyles),
                          std::find_if(std::begin(kStyles), std::end(kStyles),
                                       [&range](const StyleEntry& e) {
                                           return e.style == range.style;
                                       }))));
        table_->setCellWidget(row, ColumnStyle, style);

        auto* prefix = new QLineEdit(QString::fromStdString(range.prefix), table_);
        table_->setCellWidget(row, ColumnPrefix, prefix);

        auto* startNumber = new QSpinBox(table_);
        // /St 的下限是 1（PDF 規格），不是 0。允許 0 會產出結構上非法的檔案。
        startNumber->setRange(1, 1000000);
        startNumber->setValue(range.startNumber);
        table_->setCellWidget(row, ColumnStartNumber, startNumber);
    }
    updateButtons();
}

void PageLabelsDialog::addRange() {
    std::vector<domain::PageLabelRange> ranges;
    QString diagnostic;
    // 先收回目前表格上的內容，否則新增一列會把使用者剛改過還沒套用的值丟掉。
    if (!collectRanges(ranges, diagnostic)) {
        QMessageBox::warning(this, tr("頁面標籤"), diagnostic);
        return;
    }

    // 新範圍的起點放在最後一段之後一頁；已經到最後一頁時就疊在最後一頁上，
    // 由確定時的重疊檢查去擋——這裡不要自作主張把它塞到別的地方。
    std::int32_t start = 0;
    if (!ranges.empty()) {
        start = std::min(ranges.back().startPageIndex + 1, pageCount_ - 1);
    }
    ranges.push_back(domain::PageLabelRange{start, domain::PageLabelStyle::Decimal, "", 1});
    labels_.setRanges(std::move(ranges));
    rebuildTable();
    table_->selectRow(table_->rowCount() - 1);
}

void PageLabelsDialog::removeSelectedRange() {
    const int row = table_->currentRow();
    if (row < 0) return;
    std::vector<domain::PageLabelRange> ranges;
    QString diagnostic;
    if (!collectRanges(ranges, diagnostic)) {
        QMessageBox::warning(this, tr("頁面標籤"), diagnostic);
        return;
    }
    if (row >= static_cast<int>(ranges.size())) return;
    ranges.erase(ranges.begin() + row);
    labels_.setRanges(std::move(ranges));
    rebuildTable();
}

void PageLabelsDialog::updateButtons() {
    if (removeButton_ != nullptr) removeButton_->setEnabled(table_->currentRow() >= 0);
}

bool PageLabelsDialog::collectRanges(std::vector<domain::PageLabelRange>& out,
                                     QString& diagnostic) const {
    out.clear();
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto* startPage = qobject_cast<QSpinBox*>(table_->cellWidget(row, ColumnStartPage));
        const auto* style = qobject_cast<QComboBox*>(table_->cellWidget(row, ColumnStyle));
        const auto* prefix = qobject_cast<QLineEdit*>(table_->cellWidget(row, ColumnPrefix));
        const auto* startNumber =
            qobject_cast<QSpinBox*>(table_->cellWidget(row, ColumnStartNumber));
        if (startPage == nullptr || style == nullptr || prefix == nullptr ||
            startNumber == nullptr) {
            continue;
        }

        domain::PageLabelRange range;
        range.startPageIndex = startPage->value() - 1;
        range.style = static_cast<domain::PageLabelStyle>(style->currentData().toInt());
        range.prefix = prefix->text().toStdString();
        range.startNumber = startNumber->value();
        out.push_back(std::move(range));
    }

    std::sort(out.begin(), out.end(),
              [](const domain::PageLabelRange& a, const domain::PageLabelRange& b) {
                  return a.startPageIndex < b.startPageIndex;
              });

    for (std::size_t i = 1; i < out.size(); ++i) {
        if (out[i].startPageIndex == out[i - 1].startPageIndex) {
            diagnostic = tr("第 %1 頁有兩段以上的範圍從這裡開始。每一頁只能屬於一段。")
                             .arg(out[i].startPageIndex + 1);
            return false;
        }
    }
    return true;
}

domain::PageLabelMap PageLabelsDialog::result() const { return labels_; }

}  // namespace alioth::ui
