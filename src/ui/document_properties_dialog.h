#pragma once

// 文件屬性（PRD-ENH-005）。
//
// 除了中繼資料，這個對話框還負責一件容易被忽略的事：把「這份文件有哪些我們不支援的
// 東西」明確講出來（XFA、JavaScript、加密、簽章）。PRD §13 要求降級提示不得誤導，
// 而使用者第一個會去找答案的地方就是文件屬性。

#include <QDialog>

namespace alioth::domain {
struct DocumentInfo;
}

namespace alioth::ui {

class DocumentPropertiesDialog : public QDialog {
    Q_OBJECT

public:
    DocumentPropertiesDialog(const QString& path, const domain::DocumentInfo& info,
                             QWidget* parent = nullptr);
};

}  // namespace alioth::ui
