#pragma once

// 內容替代文字設定的應用層服務（PRD-A11Y-004）。
//
// 與 AnnotationService（app/annotation_service.h）走同一套增量附加 + 原子寫入 +
// 邊界守衛的流程，這裡另立一個類別而不是塞進 AnnotationService：替代文字改的是
// 結構樹的元素，不是 /Annots，語意上是兩種不同的文件修改，混在一起會讓
// AnnotationService 的職責說不清楚。復原邏輯刻意各自持有一份而不是共用同一個
// 靜態函式——兩者現在完全相同，但一旦其中一個未來需要额外的前置檢查
// （例如替代文字之後可能牽涉到 /RoleMap 的連動），硬拆會比硬併容易。

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>

namespace alioth::app {

struct SetAlternateTextRequest {
    QString path;
    int structElementObjectNumber{0};
    QString altText;  // 空字串代表移除既有的 /Alt
};

struct SetAlternateTextResult {
    bool ok{false};
    QString message;
    quint64 previousSize{0};
    QByteArray boundaryGuard;
};

class AccessibilityService : public QObject {
    Q_OBJECT

public:
    explicit AccessibilityService(QObject* parent = nullptr);

    [[nodiscard]] SetAlternateTextResult setAlternateText(const SetAlternateTextRequest& request);

    // 復原一次附加式寫入：把檔案截回 previousSize。語意與
    // AnnotationService::revertAppend 完全相同（見上方類別註解）。
    [[nodiscard]] bool revertAppend(const QString& path, quint64 previousSize,
                                    const QByteArray& boundaryGuard, QString* message = nullptr);
};

}  // namespace alioth::app
