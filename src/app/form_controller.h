#pragma once

// 表單控制器（PRD-FORM-001/005/021）。
//
// 存在的理由是 Fields 面板需要「這份文件有哪些欄位」，而那份資料只有
// engine::forms::FormDocument 拿得到——它有自己的 PDFium 文件把手與執行緒
// （CLAUDE.md 硬性限制 1）。這一層把非同步回呼編組回 GUI 執行緒，
// 並把引擎的 FormFieldInfo 轉成面板要的 FieldSummary。
//
// 為什麼要轉型別而不是讓面板直接吃 FormFieldInfo：面板同時要顯示
// 「原本就有的欄位」與「剛建立的欄位」，後者來自 formbuild::FieldDefinition。
// 綁死其中一種型別就等於要求呼叫端做轉換，而那個轉換遲早會有兩份。

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

#include "engine/formbuild/field_tree.h"
#include "engine/forms/form_data.h"
#include "engine/forms/form_types.h"

namespace alioth::engine::forms {
class FormDocument;
}

namespace alioth::app {

// FormFieldInfo -> FieldSummary。單選群組的多個 widget 共用同一個名字，
// 合併成一筆並記下 widget 數——面板列出三個同名的「性別」欄位只會讓人困惑。
[[nodiscard]] std::vector<engine::formbuild::FieldSummary> summarizeFormFields(
    const std::vector<engine::forms::FormFieldInfo>& fields);

class FormController : public QObject {
    Q_OBJECT

public:
    explicit FormController(QObject* parent = nullptr);
    ~FormController() override;

    void openDocument(const QString& path, const QString& password = {});
    [[nodiscard]] const QString& path() const noexcept { return path_; }
    void closeDocument();

    // 重新讀取欄位清單。結果以 fieldsReady 送出。
    void refresh();

    [[nodiscard]] const std::vector<engine::formbuild::FieldSummary>& fields() const noexcept {
        return fields_;
    }
    [[nodiscard]] bool hasForm() const noexcept { return !fields_.empty(); }

    // 表單資料匯出／匯入／重設（PRD-FORM-002）。
    //
    // 全部走 FormDocument 的非同步佇列——它持有自己的 PDFium 表單環境，
    // 而 PDFium 非執行緒安全。回呼在該佇列的執行緒上被呼叫，呼叫端要自己
    // 排回 GUI 執行緒（與這個類別其餘的訊號一致）。
    void exportData(engine::forms::FormDataFormat format,
                    std::function<void(QString)> callback);
    void importData(const QString& text, engine::forms::FormDataFormat format,
                    std::function<void(engine::forms::FormDataImport)> callback);
    void resetForm(std::function<void(bool, QString)> callback);

    // 另存一份攤平後的副本（PRD-FORM-002）。攤平是破壞性的，不能就地增量
    // 儲存——增量儲存的前提是純附加，而攤平改寫了頁面內容串流。
    void flattenTo(const QString& targetPath, std::function<void(bool, QString)> callback);

signals:
    void fieldsReady();
    void openFailed(const QString& message);

private:
    std::unique_ptr<engine::forms::FormDocument> document_;
    std::vector<engine::formbuild::FieldSummary> fields_;
    // exportData 需要原始路徑（它會重新讀一次檔案取欄位的完整資訊）。
    QString path_;
    bool open_{false};
};

}  // namespace alioth::app
