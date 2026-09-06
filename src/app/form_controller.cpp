#include "app/form_controller.h"

#include <QMetaObject>

#include <algorithm>

#include "engine/forms/form_document.h"

namespace alioth::app {
namespace {

using engine::formbuild::BuildFieldType;
using engine::forms::FormFieldInfo;
using engine::forms::FormFieldType;

BuildFieldType toBuildType(FormFieldType type) {
    switch (type) {
        case FormFieldType::PushButton:  return BuildFieldType::PushButton;
        case FormFieldType::CheckBox:    return BuildFieldType::CheckBox;
        case FormFieldType::RadioButton: return BuildFieldType::RadioGroup;
        case FormFieldType::ComboBox:    return BuildFieldType::ComboBox;
        case FormFieldType::ListBox:     return BuildFieldType::ListBox;
        case FormFieldType::Signature:   return BuildFieldType::Signature;
        case FormFieldType::TextField:   return BuildFieldType::Text;
        case FormFieldType::Unknown:
        case FormFieldType::Xfa:         break;
    }
    // 認不得的型別當成文字欄位顯示。這裡刻意不隱藏它——面板漏列一個欄位，
    // 使用者會以為文件裡沒有那一格；顯示成文字至少讓他看得到它存在。
    return BuildFieldType::Text;
}

}  // namespace

std::vector<engine::formbuild::FieldSummary> summarizeFormFields(
    const std::vector<FormFieldInfo>& fields) {
    std::vector<engine::formbuild::FieldSummary> result;

    for (const FormFieldInfo& info : fields) {
        // 同名欄位合併。單選群組的每個按鈕都是獨立的 widget 但共用欄位名，
        // 逐個列出會變成三個一模一樣的「性別」。
        const auto existing =
            std::find_if(result.begin(), result.end(),
                         [&info](const engine::formbuild::FieldSummary& summary) {
                             return summary.name == info.name;
                         });
        if (existing != result.end()) {
            ++existing->widgetCount;
            // 合併後的頁碼取最小的那一頁：面板的「頁」欄位是用來跳過去的，
            // 跳到群組的第一個按鈕才是使用者要的。
            existing->pageIndex = std::min(existing->pageIndex, info.pageIndex);
            continue;
        }

        engine::formbuild::FieldSummary summary;
        summary.name = info.name;
        summary.type = toBuildType(info.type);
        summary.pageIndex = info.pageIndex;
        summary.rectPt = info.rectPt;
        summary.widgetCount = 1;
        summary.readOnly = info.flags.readOnly;
        summary.required = info.flags.required;
        result.push_back(std::move(summary));
    }
    return result;
}

FormController::FormController(QObject* parent)
    : QObject(parent), document_(std::make_unique<engine::forms::FormDocument>()) {}

FormController::~FormController() {
    // 表單執行緒的回呼會碰本物件的成員，先關掉它再讓其餘成員解構。
    document_.reset();
}

void FormController::openDocument(const QString& path, const QString& password) {
    fields_.clear();
    open_ = false;
    path_ = path;

    document_->open(path.toStdString(), password.toStdString(),
                    [this](domain::DocumentError error) {
                        const bool ok = error == domain::DocumentError::None;
                        QMetaObject::invokeMethod(
                            this,
                            [this, ok] {
                                open_ = ok;
                                if (ok) {
                                    refresh();
                                } else {
                                    // 表單讀不到不該讓整個開檔流程看起來失敗——
                                    // 多數文件根本沒有表單。訊息交給呼叫端決定要不要顯示。
                                    emit openFailed(tr("無法以表單模式開啟這份文件"));
                                }
                            },
                            Qt::QueuedConnection);
                    });
}

void FormController::closeDocument() {
    fields_.clear();
    open_ = false;
    document_->close();
}

void FormController::refresh() {
    if (!open_) return;
    document_->allFields([this](std::vector<FormFieldInfo> fields) {
        QMetaObject::invokeMethod(
            this,
            [this, fields = std::move(fields)] {
                fields_ = summarizeFormFields(fields);
                emit fieldsReady();
            },
            Qt::QueuedConnection);
    });
}

void FormController::exportData(engine::forms::FormDataFormat format,
                                std::function<void(QString)> callback) {
    if (!open_) {
        callback({});
        return;
    }
    document_->exportData(format, path_.toStdString(),
                          [callback](std::string text) {
                              callback(QString::fromStdString(text));
                          });
}

void FormController::importData(const QString& text, engine::forms::FormDataFormat format,
                                std::function<void(engine::forms::FormDataImport)> callback) {
    if (!open_) {
        callback({});
        return;
    }
    document_->importData(text.toStdString(), format, std::move(callback));
}

void FormController::resetForm(std::function<void(bool, QString)> callback) {
    if (!open_) {
        callback(false, tr("尚未開啟表單"));
        return;
    }
    document_->resetForm([callback](engine::forms::FormFillResult result) {
        callback(result.ok, QString::fromStdString(result.error));
    });
}

void FormController::flattenTo(const QString& targetPath,
                               std::function<void(bool, QString)> callback) {
    if (!open_) {
        callback(false, tr("尚未開啟表單"));
        return;
    }
    // 先攤平再另存：攤平改寫頁面內容串流，就地增量儲存做不到
    // （增量儲存的前提是純附加）。
    document_->flatten([this, targetPath, callback](engine::forms::FormFillResult flattened) {
        if (!flattened.ok) {
            callback(false, QString::fromStdString(flattened.error));
            return;
        }
        document_->saveCopy(targetPath.toStdString(),
                            [callback](engine::forms::FormFillResult saved) {
                                callback(saved.ok, QString::fromStdString(saved.error));
                            });
    });
}

}  // namespace alioth::app
