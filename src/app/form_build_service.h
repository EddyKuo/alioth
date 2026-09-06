#pragma once

// 建立表單欄位（PRD-FORM-001~）。
//
// 引擎的 FormFieldWriter 需要一個開好的 IncrementalAppender、要處理欄位名
// 重複、還要在最後 finish() 產生位元組。這一層把那些收在後面，讓呈現層只需要
// 說「在這一頁的這塊矩形放一個文字欄位」。
//
// 寫入走純附加，所以既有簽章仍然只會顯示「簽署後有變更」而不是「無效」，
// 復原也就是把檔案截回原長度——與註解同一條路。
//
// 欄位名重複是這個功能最常見的錯誤，而且後果不是報錯而是**兩個欄位共用一個
// 值**（PDF 的規則：同名欄位是同一個欄位的多個 widget）。使用者會看到打字在
// 一格、另一格跟著變。因此這裡在寫入前就擋下重複名，並自動給一個不重複的
// 預設名，而不是讓使用者自己去猜。

#include <QObject>
#include <QString>

#include <cstdint>

#include "app/annotation_service.h"
#include "domain/geometry.h"
#include "engine/formbuild/field_definition.h"

namespace alioth::app {

struct FormFieldRequest {
    QString path;
    engine::formbuild::BuildFieldType type{engine::formbuild::BuildFieldType::Text};
    std::int32_t pageIndex{0};
    domain::RectF rectPt{};
    QString name;               // 留空時自動產生一個不重複的名稱
    QStringList options;        // ComboBox / ListBox 用
    bool required{false};
    bool readOnly{false};
    bool multiline{false};
};

class FormBuildService : public QObject {
    Q_OBJECT

public:
    explicit FormBuildService(QObject* parent = nullptr);

    // 建立一個欄位。回傳型別與註解服務相同，因此復原路徑也相同。
    [[nodiscard]] HighlightResult addField(const FormFieldRequest& request);

    // 這份文件已經有哪些欄位名。UI 用它在對話框裡先擋掉重複，
    // 而不是等寫入失敗才說。
    [[nodiscard]] QStringList existingFieldNames(const QString& path) const;
};

}  // namespace alioth::app
