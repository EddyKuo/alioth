#pragma once

// Fields 面板（PRD-UI-003 的第六個面板，資料模型見 PRD-FORM-021）。
//
// 樹狀結構來自欄位名的點分層，不是檔案結構。面板要能分辨「真的欄位」與
// 「只是分組用的中介節點」——把兩者畫成一樣，使用者會對著一個不存在的欄位
// 按屬性，然後得到一個他無法理解的錯誤。
//
// 唯讀與必填以文字標示而非只用顏色或圖示：這兩個屬性會決定使用者要不要
// 在這一欄花時間，靠圖示傳達等於要求他先學會圖例。

#include <QWidget>

#include <cstdint>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace alioth::engine::formbuild {
struct FieldSummary;
struct FieldTreeNode;
}

namespace alioth::ui {

class FieldsPanel : public QWidget {
    Q_OBJECT

public:
    explicit FieldsPanel(QWidget* parent = nullptr);
    ~FieldsPanel() override;

    // 換文件或欄位變動後由呼叫端餵入。面板不自己讀檔——
    // 讀檔要決定用哪個文件把手，那是 app 層的事。
    void setFields(std::vector<alioth::engine::formbuild::FieldSummary> fields);

    // 「只顯示本頁欄位」的檢視模式用。傳 -1 代表顯示全部。
    void setCurrentPage(std::int32_t pageIndex);

signals:
    // 使用者選了一個實體欄位（中介節點不會發出）。
    void fieldActivated(const QString& fullName, int pageIndex);

private:
    void rebuild();
    void addNode(const alioth::engine::formbuild::FieldTreeNode& node, QTreeWidgetItem* parent);

    std::vector<alioth::engine::formbuild::FieldSummary> fields_;
    std::int32_t currentPage_{-1};

    QLineEdit* search_{nullptr};
    QCheckBox* currentPageOnly_{nullptr};
    QLabel* summary_{nullptr};
    QTreeWidget* tree_{nullptr};
};

}  // namespace alioth::ui
