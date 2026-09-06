#pragma once

// 註解的彈出視窗（PRD-ANN-004）。
//
// 這是便利貼在畫面上唯一能看到自己文字的地方——/Text 註解的外觀串流畫的是
// 一個固定圖示，註釋內容不在頁面上。沒有這個視窗，使用者放了一則便利貼之後
// 只能到右側的註解清單去讀它，而那正是「便利貼」這個隱喻要避免的事。
//
// 刻意做成獨立的 QWidget（Qt::Tool）而不是畫在 PageView 上：
//   - 頁面捲動時視窗不必跟著重畫，也不會被圖磚蓋掉
//   - 內容是可編輯的多行文字，畫在畫布上就得自己實作游標、選取與 IME
//   - 螢幕閱讀器看得到它（畫在畫布上的文字對 UIA 而言不存在）
//
// 它不認識檔案也不認識引擎：文字改動以訊號送出，由持有者決定要不要寫進檔案。

#include <QWidget>

#include <cstdint>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace alioth::ui {

class StickyNotePopup : public QWidget {
    Q_OBJECT

public:
    explicit StickyNotePopup(QWidget* parent = nullptr);

    // 綁定到某一則註解。pageIndex／indexOnPage 與註解清單用的是同一組座標，
    // 這樣「清單選了哪一則」與「視窗顯示哪一則」不可能對不上。
    void showAnnotation(std::int32_t pageIndex, std::int32_t indexOnPage, const QString& subtype,
                        const QString& author, const QString& modified, const QString& contents);

    [[nodiscard]] std::int32_t pageIndex() const noexcept { return pageIndex_; }
    [[nodiscard]] std::int32_t indexOnPage() const noexcept { return indexOnPage_; }
    [[nodiscard]] QString contents() const;

    // 唯讀模式：文件權限不允許加註時仍然可以看，只是不能改（PRD-SEC-002）。
    void setReadOnly(bool readOnly);

signals:
    // 使用者按下「儲存」。呼叫端負責寫檔並回報結果——這個元件不碰檔案。
    void contentsCommitted(int pageIndex, int indexOnPage, const QString& contents);
    // 視窗被關閉。呼叫端據此取消清單上的關聯高亮。
    void dismissed();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void commit();
    void updateSaveButton();

    QLabel* header_{nullptr};
    QPlainTextEdit* editor_{nullptr};
    QPushButton* saveButton_{nullptr};

    std::int32_t pageIndex_{-1};
    std::int32_t indexOnPage_{-1};
    QString originalContents_;
    bool readOnly_{false};
};

}  // namespace alioth::ui
