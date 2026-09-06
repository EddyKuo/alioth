#include "ui/inspector_panel.h"

#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

#include <utility>

namespace alioth::ui {
namespace {

using engine::objects::A11yCheckId;
using engine::objects::A11yCoverageEntry;
using engine::objects::A11yFinding;
using engine::objects::A11yReport;

constexpr int kPageRole = Qt::UserRole;

QString pageLabel(std::int32_t pageIndex) {
    return pageIndex >= 0 ? QObject::tr("第 %1 頁").arg(pageIndex + 1) : QObject::tr("文件層級");
}

}  // namespace

InspectorPanel::InspectorPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("inspectorPanel"));
    setAccessibleName(tr("無障礙檢查器"));
    setAccessibleDescription(tr("列出這份文件的無障礙缺陷，以及本次檢查涵蓋與未涵蓋的項目"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("inspectorSummary"));
    summary_->setWordWrap(true);
    summary_->setAccessibleName(tr("檢查結果狀態"));

    tree_view_ = new QTreeWidget(this);
    tree_view_->setObjectName(QStringLiteral("inspectorTree"));
    tree_view_->setAccessibleName(tr("檢查結果清單"));
    tree_view_->setAccessibleDescription(
        tr("依檢查項目分組列出發現的缺陷，以及本次檢查的涵蓋範圍與已知限制"));
    tree_view_->setColumnCount(2);
    tree_view_->setHeaderLabels({tr("項目"), tr("頁面")});
    tree_view_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    layout->addWidget(summary_);
    layout->addWidget(tree_view_);
    setFocusProxy(tree_view_);

    const auto activate = [this](QTreeWidgetItem* item, int) {
        if (item == nullptr) return;
        const QVariant page = item->data(0, kPageRole);
        if (page.isValid() && page.toInt() >= 0) emit findingActivated(page.toInt());
    };
    connect(tree_view_, &QTreeWidget::itemActivated, this, activate);

    clearDocument();
}

void InspectorPanel::clearDocument() {
    documentOpen_ = false;
    report_ = {};
    tree_view_->clear();
    summary_->setText(tr("尚未開啟文件。"));
}

void InspectorPanel::setReport(engine::objects::A11yReport report) {
    report_ = std::move(report);
    documentOpen_ = true;
    rebuild();
}

void InspectorPanel::rebuild() {
    tree_view_->clear();

    if (!documentOpen_) {
        summary_->setText(tr("尚未開啟文件。"));
        return;
    }

    // 依檢查項目分組列出發現。
    QHash<int, QTreeWidgetItem*> groups;
    int totalFindings = 0;
    for (const A11yFinding& finding : report_.findings) {
        const int key = static_cast<int>(finding.check);
        QTreeWidgetItem* group = groups.value(key, nullptr);
        if (group == nullptr) {
            group = new QTreeWidgetItem(tree_view_);
            group->setText(0, QString::fromLatin1(describe(finding.check)));
            groups.insert(key, group);
        }
        auto* row = new QTreeWidgetItem(group);
        row->setText(0, tr("%1 — %2").arg(QString::fromStdString(finding.element),
                                          QString::fromStdString(finding.reason)));
        row->setText(1, pageLabel(finding.pageIndex));
        row->setData(0, kPageRole, finding.pageIndex);
        row->setToolTip(0, tr("怎麼修：%1").arg(QString::fromStdString(finding.remedy)));
        row->setData(0, Qt::AccessibleTextRole,
                     tr("%1，%2，%3，怎麼修：%4")
                         .arg(QString::fromStdString(finding.element), row->text(1),
                              QString::fromStdString(finding.reason),
                              QString::fromStdString(finding.remedy)));
        ++totalFindings;
    }

    // 涵蓋範圍與限制：永遠顯示，即使沒有任何發現。
    auto* coverageRoot = new QTreeWidgetItem(tree_view_);
    coverageRoot->setText(0, tr("本次檢查涵蓋範圍與限制"));
    for (const A11yCoverageEntry& entry : report_.coverage) {
        auto* row = new QTreeWidgetItem(coverageRoot);
        const QString status = entry.supported ? tr("已檢查") : tr("未檢查");
        row->setText(0, tr("%1（%2）：%3").arg(QString::fromLatin1(describe(entry.check)), status,
                                             QString::fromStdString(entry.note)));
    }
    for (const std::string& note : report_.notCoveredAtAll) {
        auto* row = new QTreeWidgetItem(coverageRoot);
        row->setText(0, tr("未涵蓋：%1").arg(QString::fromStdString(note)));
    }

    tree_view_->expandAll();

    summary_->setText(totalFindings == 0
                          ? tr("在本次涵蓋範圍內沒有發現問題；請展開下方「涵蓋範圍與限制」"
                               "了解哪些項目沒有檢查")
                          : tr("發現 %1 項問題，展開各分類查看細節，Enter 可跳到對應頁面")
                                .arg(totalFindings));
}

}  // namespace alioth::ui
