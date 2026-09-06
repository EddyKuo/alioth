#include "ui/signature_panel.h"

#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/signature_controller.h"
#include "ui/a11y_audit.h"

namespace alioth::ui {
namespace {

using engine::signature::SignatureTrust;

// 燈號的文字標籤。顏色之外一定要有文字：色盲使用者看不出綠與紅的差別，
// 而簽章有效與否是這個產品最不能靠顏色單獨傳達的資訊。
QString trustLabel(SignatureTrust trust) {
    switch (trust) {
        case SignatureTrust::Trusted:        return QObject::tr("有效");
        case SignatureTrust::Untrusted:      return QObject::tr("無法確認");
        case SignatureTrust::Invalid:        return QObject::tr("無效");
    }
    return QObject::tr("無效");
}

// 三態燈號的顏色是刻意硬編的，理由與其他面板不同：綠／琥珀／紅是簽章
// 狀態的跨產品慣例，換成佈景色會讓使用者失去唯一的視覺速讀線索。
// 意義本身不靠顏色承載——trustLabel() 的文字才是權威來源，顏色只是加速。
//
// 但硬編顏色在 Windows 高對比模式下會出事：深綠 0x1B7F3B 疊在高對比的
// 黑底上只有 2.6:1，等於看不見。因此這裡不是「用不用顏色」的二選一，
// 而是先量對比再決定：不合格就退回佈景的文字色，寧可少一個線索，
// 也不要一列讀不到的字。
QColor trustColor(SignatureTrust trust) {
    switch (trust) {
        case SignatureTrust::Trusted:   return QColor(0x1B, 0x7F, 0x3B);
        case SignatureTrust::Untrusted: return QColor(0xB8, 0x86, 0x00);
        case SignatureTrust::Invalid:   return QColor(0xC0, 0x30, 0x2B);
    }
    return QColor(0xC0, 0x30, 0x2B);
}

// 對比不足時退回佈景文字色（PRD-A11Y-005，WCAG AA 4.5:1）。
QColor readableTrustColor(SignatureTrust trust, const QPalette& palette) {
    const QColor candidate = trustColor(trust);
    const QColor background = palette.color(QPalette::Base);
    if (a11y::contrastRatio(candidate, background) >= a11y::kContrastAaNormal) return candidate;
    return palette.color(QPalette::Text);
}

}  // namespace

SignaturePanel::SignaturePanel(app::SignatureController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName(QStringLiteral("signaturePanel"));
    setAccessibleName(tr("簽章"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    summary_ = new QLabel(tr("尚未驗證"), this);
    summary_->setObjectName(QStringLiteral("signatureSummary"));
    summary_->setAccessibleName(tr("簽章驗證摘要"));
    summary_->setWordWrap(true);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("signatureTree"));
    tree_->setAccessibleName(tr("簽章清單"));
    tree_->setAccessibleDescription(
        tr("每一列同時有狀態文字與顏色；狀態以文字為準，顏色只是輔助"));
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("簽章"), tr("狀態")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    layout->addWidget(summary_);
    layout->addWidget(tree_);

    // 面板本身不接受焦點，焦點應直接落在清單上——Tab 停在一個空容器上
    // 會讓螢幕閱讀器念出「群組」然後沒有下文。
    setFocusProxy(tree_);

    connect(controller_, &app::SignatureController::reportsReady, this, [this] { rebuild(); });
    connect(controller_, &app::SignatureController::verifyFailed, this,
            [this](const QString& message) { summary_->setText(tr("驗證失敗：%1").arg(message)); });
}

void SignaturePanel::refresh() {
    tree_->clear();
    if (controller_->signatureCount() == 0) {
        summary_->setText(tr("這份文件沒有數位簽章"));
        return;
    }
    summary_->setText(tr("驗證中…"));
    controller_->verify();
}

void SignaturePanel::rebuild() {
    tree_->clear();

    const auto& reports = controller_->reports();
    if (reports.empty()) {
        summary_->setText(tr("這份文件沒有數位簽章"));
        return;
    }

    int invalid = 0;
    int indeterminate = 0;
    for (const engine::signature::SignatureReport& report : reports) {
        if (report.trust == SignatureTrust::Invalid) ++invalid;
        if (report.trust == SignatureTrust::Untrusted) ++indeterminate;

        auto* item = new QTreeWidgetItem(tree_);
        const QString signer = report.signerName.empty()
                                   ? tr("（未載明簽署者）")
                                   : QString::fromStdString(report.signerName);
        item->setText(0, tr("簽章 %1 · %2").arg(report.index + 1).arg(signer));
        item->setText(1, trustLabel(report.trust));
        item->setForeground(1, readableTrustColor(report.trust, tree_->palette()));

        // findings 逐條列出，不合併也不摘要。使用者需要知道的是「為什麼是這個顏色」，
        // 而那個理由往往決定他要不要接受這份文件。
        for (const std::string& finding : report.findings) {
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, QString::fromStdString(finding));
        }

        if (report.coverage.partiallyCovered()) {
            // 這是真實世界的簽章偽造手法（增量儲存攻擊）：密碼學驗證仍然通過，
            // 但簽署之後被追加的內容根本不在保護範圍內。必須講明白。
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, tr("簽章只涵蓋部分檔案，%1 位元組未受保護")
                                  .arg(report.coverage.uncoveredBytes()));
            child->setForeground(0, readableTrustColor(SignatureTrust::Invalid, tree_->palette()));
        }

        if (!report.signingTimeRaw.empty()) {
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, tr("簽署時間：%1").arg(QString::fromStdString(report.signingTimeRaw)));
        }
    }

    tree_->expandAll();

    // 摘要一律以最壞的那一則為準：三個有效加一個無效，整份文件就是不可信的。
    if (invalid > 0) {
        summary_->setText(tr("%1 個簽章中有 %2 個無效").arg(reports.size()).arg(invalid));
    } else if (indeterminate > 0) {
        summary_->setText(tr("%1 個簽章中有 %2 個無法確認").arg(reports.size()).arg(indeterminate));
    } else {
        summary_->setText(tr("%1 個簽章全部有效").arg(reports.size()));
    }
}

}  // namespace alioth::ui
