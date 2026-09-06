#include "ui/sticky_note_popup.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace alioth::ui {

StickyNotePopup::StickyNotePopup(QWidget* parent) : QWidget(parent, Qt::Tool) {
    setObjectName(QStringLiteral("stickyNotePopup"));
    setWindowTitle(tr("註釋"));
    setAccessibleName(tr("註釋視窗"));
    resize(280, 200);

    auto* outer = new QVBoxLayout(this);

    header_ = new QLabel(this);
    header_->setObjectName(QStringLiteral("stickyNotePopupHeader"));
    header_->setWordWrap(true);
    outer->addWidget(header_);

    editor_ = new QPlainTextEdit(this);
    editor_->setObjectName(QStringLiteral("stickyNotePopupEditor"));
    editor_->setAccessibleName(tr("註釋內容"));
    editor_->setPlaceholderText(tr("輸入註釋內容"));
    outer->addWidget(editor_, 1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    saveButton_ = new QPushButton(tr("儲存"), this);
    saveButton_->setObjectName(QStringLiteral("stickyNotePopupSave"));
    auto* closeButton = new QPushButton(tr("關閉"), this);
    closeButton->setObjectName(QStringLiteral("stickyNotePopupClose"));
    buttons->addWidget(saveButton_);
    buttons->addWidget(closeButton);
    outer->addLayout(buttons);

    connect(saveButton_, &QPushButton::clicked, this, &StickyNotePopup::commit);
    connect(closeButton, &QPushButton::clicked, this, &QWidget::close);
    connect(editor_, &QPlainTextEdit::textChanged, this, &StickyNotePopup::updateSaveButton);

    updateSaveButton();
}

void StickyNotePopup::showAnnotation(std::int32_t pageIndex, std::int32_t indexOnPage,
                                     const QString& subtype, const QString& author,
                                     const QString& modified, const QString& contents) {
    pageIndex_ = pageIndex;
    indexOnPage_ = indexOnPage;
    originalContents_ = contents;

    QString heading = tr("第 %1 頁 · %2").arg(pageIndex + 1).arg(subtype);
    if (!author.isEmpty()) heading += tr(" · %1").arg(author);
    if (!modified.isEmpty()) heading += tr(" · %1").arg(modified);
    header_->setText(heading);
    // 視窗標題也帶上頁碼：多個註釋視窗同時開著時，工作列上只看得到標題。
    setWindowTitle(tr("註釋 — 第 %1 頁").arg(pageIndex + 1));

    // setPlainText 會觸發 textChanged，所以放在 originalContents_ 設好之後——
    // 順序反過來的話「儲存」會在剛開啟時就是啟用的。
    editor_->setPlainText(contents);
    updateSaveButton();

    show();
    raise();
    activateWindow();
    if (!readOnly_) editor_->setFocus();
}

QString StickyNotePopup::contents() const { return editor_->toPlainText(); }

void StickyNotePopup::setReadOnly(bool readOnly) {
    readOnly_ = readOnly;
    editor_->setReadOnly(readOnly);
    updateSaveButton();
}

void StickyNotePopup::commit() {
    if (readOnly_ || pageIndex_ < 0) return;
    const QString text = editor_->toPlainText();
    if (text == originalContents_) return;
    // 先更新基準再發訊號：呼叫端可能同步重新載入註解清單並回頭呼叫
    // showAnnotation()，基準沒先更新的話「儲存」會停在啟用狀態。
    originalContents_ = text;
    updateSaveButton();
    emit contentsCommitted(pageIndex_, indexOnPage_, text);
}

void StickyNotePopup::updateSaveButton() {
    saveButton_->setEnabled(!readOnly_ && pageIndex_ >= 0 &&
                            editor_->toPlainText() != originalContents_);
}

void StickyNotePopup::closeEvent(QCloseEvent* event) {
    // 未儲存的修改不靜靜丟掉，也不跳一個擋路的對話框：直接寫進去。
    // 這是便利貼，不是檔案；使用者對它的期待是「打字就會留著」。
    commit();
    QWidget::closeEvent(event);
    emit dismissed();
}

void StickyNotePopup::keyPressEvent(QKeyEvent* event) {
    // Esc 關閉；Ctrl+Enter 儲存但不關閉（連續改多則註釋時不必重開視窗）。
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        (event->modifiers() & Qt::ControlModifier) != 0) {
        commit();
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace alioth::ui
