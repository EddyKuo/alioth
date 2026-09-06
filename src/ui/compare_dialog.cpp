#include "ui/compare_dialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace alioth::ui {
namespace {

constexpr int kSummaryIndexRole = Qt::UserRole;

[[nodiscard]] QString describeKind(domain::PageMatchKind kind) {
    switch (kind) {
        case domain::PageMatchKind::Inserted: return CompareDialog::tr("新增");
        case domain::PageMatchKind::Deleted: return CompareDialog::tr("刪除");
        case domain::PageMatchKind::Matched: break;
    }
    return CompareDialog::tr("修改");
}

// 頁碼顯示成 1 起算；不存在的那一邊顯示破折號而不是 0——
// 顯示 0 會讓使用者以為那是「第 0 頁」。
[[nodiscard]] QString pageLabel(std::int32_t page) {
    return page < 0 ? QStringLiteral("—") : QString::number(page + 1);
}

// 差異文字可能很長（整段被替換時就是整段）。列表裡截斷，完整內容留在 tooltip。
[[nodiscard]] QString elide(const std::string& text) {
    QString out = QString::fromStdString(text).simplified();
    constexpr int kMax = 120;
    if (out.size() > kMax) out = out.left(kMax) + QStringLiteral("…");
    return out;
}

}  // namespace

CompareDialog::CompareDialog(const domain::DocumentDiff& diff, const QString& oldName,
                             const QString& newName, QWidget* parent)
    : QDialog(parent), diff_(diff) {
    setWindowTitle(tr("比較文件"));
    setObjectName(QStringLiteral("compareDialog"));
    resize(880, 560);

    auto* outer = new QVBoxLayout(this);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("compareSummary"));
    summary_->setWordWrap(true);
    outer->addWidget(summary_);

    auto* splitter = new QSplitter(Qt::Vertical, this);

    pages_ = new QTreeWidget(splitter);
    pages_->setObjectName(QStringLiteral("comparePages"));
    pages_->setAccessibleName(tr("有差異的頁面"));
    pages_->setRootIsDecorated(false);
    pages_->setColumnCount(5);
    pages_->setHeaderLabels({tr("狀態"), tr("舊文件頁"), tr("新文件頁"), tr("相似度"),
                             tr("變更段落")});
    splitter->addWidget(pages_);

    regions_ = new QListWidget(splitter);
    regions_->setObjectName(QStringLiteral("compareRegions"));
    regions_->setAccessibleName(tr("該頁的差異段落"));
    splitter->addWidget(regions_);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    outer->addWidget(splitter, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->setObjectName(QStringLiteral("compareButtons"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    connect(pages_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                showRegionsFor(current == nullptr ? -1
                                                  : current->data(0, kSummaryIndexRole).toInt());
            });
    connect(pages_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr) return;
        const int index = item->data(0, kSummaryIndexRole).toInt();
        const auto& summaries = diff_.summaries();
        if (index < 0 || index >= static_cast<int>(summaries.size())) return;
        emit pageActivated(summaries[static_cast<std::size_t>(index)].newPage);
    });

    populate(diff);

    setWindowTitle(tr("比較文件：%1 → %2").arg(oldName, newName));
}

void CompareDialog::populate(const domain::DocumentDiff& diff) {
    QString text;
    if (diff.isIdentical()) {
        // 「沒有差異」必須說出來。留一張空清單，使用者無法分辨是真的一樣
        // 還是比對失敗了。
        text = tr("兩份文件的文字內容完全相同。");
    } else {
        text = tr("%1 頁有差異，共 %2 段變更。")
                   .arg(diff.changedPageCount())
                   .arg(diff.regions().size());
    }
    if (diff.degraded()) {
        // 降級的結果仍然正確，只是某些區段被整段報成替換而不是最小編輯。
        // 不說的話使用者會以為看到的是最精細的比對。
        text += QLatin1Char(' ');
        text += tr("（文件過大，部分段落以整段替換呈現，非最小差異）");
    }
    summary_->setText(text);

    const auto& summaries = diff.summaries();
    for (int i = 0; i < static_cast<int>(summaries.size()); ++i) {
        const domain::PageDiffSummary& summary = summaries[static_cast<std::size_t>(i)];
        // 沒變的頁面不列。整份文件多數頁面通常沒變，列出來只會把真正
        // 要看的那幾頁埋掉。
        if (!summary.changed()) continue;

        auto* item = new QTreeWidgetItem(pages_);
        item->setText(0, describeKind(summary.kind));
        item->setText(1, pageLabel(summary.oldPage));
        item->setText(2, pageLabel(summary.newPage));
        item->setText(3, summary.kind == domain::PageMatchKind::Matched
                             ? QStringLiteral("%1%").arg(summary.similarity * 100.0, 0, 'f', 1)
                             : QStringLiteral("—"));
        item->setText(4, QString::number(summary.totalChanges()));
        item->setData(0, kSummaryIndexRole, i);
    }
    pages_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    if (pages_->topLevelItemCount() > 0) pages_->setCurrentItem(pages_->topLevelItem(0));
}

void CompareDialog::showRegionsFor(int row) {
    regions_->clear();
    const auto& summaries = diff_.summaries();
    if (row < 0 || row >= static_cast<int>(summaries.size())) return;
    const domain::PageDiffSummary& summary = summaries[static_cast<std::size_t>(row)];

    for (const domain::TextDiffRegion& region : diff_.regions()) {
        // 以新文件的頁碼比對；只存在於舊文件的頁面則用舊頁碼。兩邊都要看，
        // 否則被刪掉那一頁的差異在清單裡永遠是空的。
        const bool matchesNew = summary.newPage >= 0 && region.newPage == summary.newPage;
        const bool matchesOld = summary.newPage < 0 && region.oldPage == summary.oldPage;
        if (!matchesNew && !matchesOld) continue;

        QString line;
        switch (region.kind) {
            case domain::DiffKind::Insert:
                line = tr("＋ %1").arg(elide(region.newText));
                break;
            case domain::DiffKind::Delete:
                line = tr("－ %1").arg(elide(region.oldText));
                break;
            default:
                line = tr("%1 → %2").arg(elide(region.oldText), elide(region.newText));
                break;
        }
        auto* item = new QListWidgetItem(line, regions_);
        // 完整內容放 tooltip：清單裡截斷是為了讀得完，但使用者總有需要
        // 看到整段的時候。
        item->setToolTip(QStringLiteral("%1\n\n%2")
                             .arg(QString::fromStdString(region.oldText),
                                  QString::fromStdString(region.newText)));
    }

    if (regions_->count() == 0) {
        auto* empty = new QListWidgetItem(tr("這一頁沒有文字差異（整頁新增或刪除）"), regions_);
        empty->setFlags(Qt::NoItemFlags);
    }
}

}  // namespace alioth::ui
