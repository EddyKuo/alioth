#pragma once

// 加入附件（PRD-ANN-015）。
//
// 兩種形態共用同一條路徑，差別只在 pageIndex：負值寫成文件層附件
// （/Names /EmbeddedFiles），非負值寫成該頁的檔案附件註解（/FileAttachment）。
// 引擎那一層本來就是這樣分的，這裡不再多一個旗標。
//
// 寫入走純附加，所以既有簽章仍然只會顯示「簽署後有變更」而不是「無效」，
// 復原也就是把檔案截回原長度——與註解同一條路。
//
// 大小上限來自引擎的 kMaxAttachmentBytes：附加式寫入本來就持有原檔的全部
// 位元組，一個 2 GB 的附件會讓整份文件在記憶體裡被複製好幾次。

#include <QObject>
#include <QString>

#include <cstdint>

#include "app/annotation_service.h"
#include "domain/geometry.h"

namespace alioth::app {

struct AttachmentRequest {
    QString path;          // 要寫入的 PDF
    QString sourceFile;    // 要附加的檔案
    QString description;
    QString author;
    // 負值代表文件層附件；非負值代表掛在該頁的檔案附件註解。
    std::int32_t pageIndex{-1};
    domain::RectF rectPt{};
};

class AttachmentService : public QObject {
    Q_OBJECT

public:
    explicit AttachmentService(QObject* parent = nullptr);

    [[nodiscard]] HighlightResult addAttachment(const AttachmentRequest& request);
};

}  // namespace alioth::app
