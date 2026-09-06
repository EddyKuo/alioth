#include "ui/document_properties_dialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QVBoxLayout>

#include "domain/document.h"

namespace alioth::ui {
namespace {

QString yesNo(bool value) {
    return value ? QObject::tr("是") : QObject::tr("否");
}

// 權限是「不能做什麼」的清單。只列被拒絕的項目——列出全部會讓使用者
// 在十行「允許」裡找那一行「拒絕」。
QString restrictionsOf(const domain::Permissions& permissions) {
    QStringList denied;
    if (!permissions.print) denied << QObject::tr("列印");
    if (!permissions.modify) denied << QObject::tr("修改");
    if (!permissions.copy) denied << QObject::tr("複製內容");
    if (!permissions.annotate) denied << QObject::tr("加註");
    if (!permissions.fillForms) denied << QObject::tr("填寫表單");
    if (!permissions.assemble) denied << QObject::tr("組合頁面");
    if (!permissions.printHighQuality) denied << QObject::tr("高品質列印");
    return denied.isEmpty() ? QObject::tr("無限制") : denied.join(QStringLiteral("、"));
}

}  // namespace

DocumentPropertiesDialog::DocumentPropertiesDialog(const QString& path,
                                                   const domain::DocumentInfo& info,
                                                   QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("文件屬性"));

    const QFileInfo file(path);
    auto* form = new QFormLayout;

    form->addRow(tr("檔案"), new QLabel(file.fileName(), this));
    form->addRow(tr("路徑"), new QLabel(file.absolutePath(), this));
    form->addRow(tr("大小"), new QLabel(QLocale().formattedDataSize(file.size()), this));
    form->addRow(tr("頁數"), new QLabel(QString::number(info.pageCount), this));
    form->addRow(tr("PDF 版本"),
                 new QLabel(info.pdfVersion.empty() ? tr("未知")
                                                    : QString::fromStdString(info.pdfVersion),
                            this));

    if (!info.title.empty()) {
        form->addRow(tr("標題"), new QLabel(QString::fromStdString(info.title), this));
    }
    if (!info.author.empty()) {
        form->addRow(tr("作者"), new QLabel(QString::fromStdString(info.author), this));
    }
    if (!info.producer.empty()) {
        form->addRow(tr("製作程式"), new QLabel(QString::fromStdString(info.producer), this));
    }

    form->addRow(tr("加密"), new QLabel(yesNo(info.encrypted), this));
    form->addRow(tr("限制"), new QLabel(restrictionsOf(info.permissions), this));
    form->addRow(tr("數位簽章"), new QLabel(yesNo(info.hasSignatures), this));

    // 不支援的項目要明確說出來，而且要說清楚我們對它做了什麼（PRD §13）。
    QStringList notes;
    if (info.hasXfa) notes << tr("含 XFA 表單，僅顯示後備內容");
    if (info.hasJavaScript) notes << tr("含 JavaScript，不執行（安全設計）");
    if (!notes.isEmpty()) {
        auto* label = new QLabel(notes.join(QStringLiteral("\n")), this);
        label->setWordWrap(true);
        form->addRow(tr("注意"), label);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

}  // namespace alioth::ui
