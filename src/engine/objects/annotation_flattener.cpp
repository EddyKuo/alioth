#include "engine/objects/annotation_flattener.h"

#include <array>
#include <cmath>

#include "engine/annotations/appearance_stream.h"
#include "engine/objects/content_stream_appender.h"
#include "engine/objects/page_object_editor.h"

namespace alioth::engine::objects {

namespace {

using annotations::Appearance;
using annotations::AppearanceOptions;
using annotations::BlendMode;
using annotations::ExtGState;

[[nodiscard]] FlattenResult failure(std::string reason) {
    FlattenResult result{};
    result.diagnostic = std::move(reason);
    return result;
}

[[nodiscard]] bool isIdentity(const std::array<double, 6>& m) noexcept {
    constexpr std::array<double, 6> kIdentity = {1, 0, 0, 1, 0, 0};
    for (std::size_t i = 0; i < 6; ++i) {
        if (std::abs(m[i] - kIdentity[i]) > 1e-9) return false;
    }
    return true;
}

}  // namespace

FlattenResult flattenAnnotation(IncrementalAppender& appender, int pageIndex,
                                const domain::Annotation& annotation,
                                domain::IrreversibleConsent /*consent*/,
                                const FlattenOptions& options) {
    if (!appender.isOpen()) return failure("附加器尚未開啟原檔");

    PdfRef pageRef{};
    if (!pageRefAt(appender, pageIndex, pageRef)) {
        return failure("頁碼超出範圍：" + std::to_string(pageIndex));
    }

    AppearanceOptions appearanceOptions{};
    appearanceOptions.pageRotation = options.pageRotation;
    appearanceOptions.resourcesSupported = true;

    const Appearance appearance = annotations::generateAppearance(annotation, appearanceOptions);
    if (!appearance.valid) return failure("外觀串流產生失敗：" + appearance.diagnostic);

    // 直接內嵌到頁面內容,因此資源要登記在頁面的 /Resources,而不是(不存在的)
    // Form XObject 自己的 /Resources。
    for (const ExtGState& state : appearance.extGStates) {
        PdfDictionary dict;
        dict.set("Type", makeName("ExtGState"));
        dict.set("CA", PdfObject{state.strokeAlpha});
        dict.set("ca", PdfObject{state.fillAlpha});
        dict.set("BM", makeName(state.blend == BlendMode::Multiply ? "Multiply" : "Normal"));
        const PageEditStatus status =
            setPageResource(appender, pageRef, "ExtGState", state.name, PdfObject{std::move(dict)});
        if (!status.ok) return failure("登記 ExtGState 失敗：" + status.diagnostic);
    }

    std::string content;
    if (!isIdentity(appearance.matrix)) {
        // 便利貼圖示的反向旋轉矩陣(見 appearance_stream.cpp 的
        // counterRotationMatrix)。BBox 恆為正方形時,這個矩陣直接套用
        // 等同於 AP 對映演算法的結果(見該檔案的說明),因此這裡可以省略
        // 完整的 §12.5.5 對映計算。
        for (const double v : appearance.matrix) {
            content += annotations::formatNumber(v);
            content += ' ';
        }
        content += "cm\n";
    }
    content += appearance.content;

    ContentAppendOptions contentOptions;
    contentOptions.wrapInGraphicsState = true;
    if (appearance.needsFont) {
        ContentFontRequest font;
        font.resourceName = "Helv";
        font.baseFont = "Helvetica";
        contentOptions.fonts.push_back(font);
    }

    const ContentAppendResult appended =
        appendPageContent(appender, pageIndex, content, contentOptions);
    if (!appended.ok) return failure("寫入攤平內容失敗：" + appended.diagnostic);

    FlattenResult result{};
    result.ok = true;
    result.contentObject = appended.contentObject;
    return result;
}

FlattenResult flattenAndDetachAnnotation(IncrementalAppender& appender, int pageIndex,
                                         const domain::Annotation& annotation,
                                         int annotationObjectNumber,
                                         domain::IrreversibleConsent consent,
                                         const FlattenOptions& options) {
    // 先燒進內容再摘掉參照。順序反過來的話，外觀產生失敗時註解已經從
    // /Annots 消失，結果是「那則註解人間蒸發」——比完全沒攤平糟得多。
    FlattenResult result = flattenAnnotation(appender, pageIndex, annotation, consent, options);
    if (!result.ok) return result;

    PdfRef pageRef{};
    if (!pageRefAt(appender, pageIndex, pageRef)) {
        return failure("頁碼超出範圍：" + std::to_string(pageIndex));
    }

    const PageEditStatus removed =
        removeFromPageArray(appender, pageRef, "Annots", annotationObjectNumber);
    if (!removed.ok) {
        // 這裡失敗代表內容已經燒進去、參照還在，畫面上會看到兩份。必須明說，
        // 讓呼叫端把整個附加段丟掉重來，而不是當成成功繼續。
        return failure("內容已寫入但移除 /Annots 項目失敗：" + removed.diagnostic);
    }
    result.detached = true;
    return result;
}

}  // namespace alioth::engine::objects
