#pragma once

// 頁面合成的意圖描述（WBS 12，PRD-PAGE-007 ~ 011）。
//
// 這一層只算「內容該被放到哪裡」，不碰 PDF 物件。分開的實際好處是版面數學
// （縮放、置中、格線、對齊、原點平移）可以在沒有任何 PDF 的情況下測到底；
// 引擎層剩下的風險就只有「有沒有把算好的矩陣正確寫進檔案」這一件事。
//
// 座標系一律是 PDF 使用者空間：單位點、原點左下、Y 軸向上。
// 矩陣的分量順序與 PDF 的 cm / Tm 運算元一致（a b c d e f），
// 換成別的順序寫進內容串流會得到轉置後的結果——那不會崩潰，只會整頁歪掉。

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "domain/geometry.h"

namespace alioth::domain::compose {

// [a b c d e f]，對應 PDF 的 [ a b c d e f cm ]。
// 點的映射：x' = a·x + c·y + e，y' = b·x + d·y + f。
struct Matrix {
    double a{1.0};
    double b{0.0};
    double c{0.0};
    double d{1.0};
    double e{0.0};
    double f{0.0};

    friend constexpr bool operator==(const Matrix&, const Matrix&) = default;
};

[[nodiscard]] constexpr Matrix identity() noexcept { return Matrix{}; }

[[nodiscard]] constexpr Matrix translation(double dx, double dy) noexcept {
    return Matrix{1.0, 0.0, 0.0, 1.0, dx, dy};
}

[[nodiscard]] constexpr Matrix scaling(double sx, double sy) noexcept {
    return Matrix{sx, 0.0, 0.0, sy, 0.0, 0.0};
}

// first 先套用、second 後套用。順序寫反的症狀是整體位移錯位而不是崩潰，
// 所以參數名稱刻意寫成時序而不是數學上的左右乘。
[[nodiscard]] constexpr Matrix concat(const Matrix& first, const Matrix& second) noexcept {
    return Matrix{
        first.a * second.a + first.b * second.c,
        first.a * second.b + first.b * second.d,
        first.c * second.a + first.d * second.c,
        first.c * second.b + first.d * second.d,
        first.e * second.a + first.f * second.c + second.e,
        first.e * second.b + first.f * second.d + second.f,
    };
}

[[nodiscard]] constexpr PointF apply(const Matrix& m, const PointF& p) noexcept {
    return PointF{m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f};
}

// 矩形經矩陣後的外接矩形。旋轉矩陣下四個角都要算，只算左下右上會在 90 度時
// 得到一個左右顛倒的矩形，而 PDF 的 /Rect 必須是正規化的。
[[nodiscard]] inline RectF apply(const Matrix& m, const RectF& r) noexcept {
    const PointF corners[4] = {
        apply(m, PointF{r.left, r.bottom}),
        apply(m, PointF{r.right, r.bottom}),
        apply(m, PointF{r.right, r.top}),
        apply(m, PointF{r.left, r.top}),
    };
    RectF out{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const PointF& p : corners) {
        out.left = std::min(out.left, p.x);
        out.bottom = std::min(out.bottom, p.y);
        out.right = std::max(out.right, p.x);
        out.top = std::max(out.top, p.y);
    }
    return out;
}

[[nodiscard]] constexpr RectF translated(const RectF& r, double dx, double dy) noexcept {
    return RectF{r.left + dx, r.bottom + dy, r.right + dx, r.top + dy};
}

enum class ComposeStatus : std::uint8_t {
    Ok,
    InvalidGrid,        // 列數或欄數 ≤ 0
    InvalidPageSize,    // 目標頁尺寸非正，或扣掉邊界與間隔後沒有空間
    EmptySource,        // 沒有來源頁
    TooManySources,     // 來源頁數超過格數（多出來的要放哪裡不該由這一層猜）
    InvalidSourceSize,  // 某個來源頁尺寸非正
    InvalidBox,         // 框的寬或高非正
    BoxOutsideMedia,    // 子框超出 MediaBox，且呼叫端要求嚴格檢查
    NothingToDo,        // 沒有任何框要設定／已經正規化過
};

[[nodiscard]] inline const char* describe(ComposeStatus status) noexcept {
    switch (status) {
        case ComposeStatus::Ok: return "ok";
        case ComposeStatus::InvalidGrid: return "版面的列數與欄數必須為正";
        case ComposeStatus::InvalidPageSize: return "目標頁尺寸不足以容納版面";
        case ComposeStatus::EmptySource: return "沒有來源頁";
        case ComposeStatus::TooManySources: return "來源頁數超過版面格數";
        case ComposeStatus::InvalidSourceSize: return "來源頁尺寸非正";
        case ComposeStatus::InvalidBox: return "框的寬或高非正";
        case ComposeStatus::BoxOutsideMedia: return "子框超出 MediaBox";
        case ComposeStatus::NothingToDo: return "沒有需要變更的項目";
    }
    return "unknown";
}

// ---- PRD-PAGE-007 合併頁面（多頁疊為一頁） --------------------------------

// 格子的填入順序。列優先是閱讀順序（左到右、上到下），欄優先用於裝訂前的拼版。
enum class CellOrder : std::uint8_t {
    RowMajor,
    ColumnMajor,
};

// 內容放進格子的方式。
enum class CellFit : std::uint8_t {
    Contain,  // 等比縮放至完全放進格子，留白置中
    Stretch,  // 分別縮放兩軸填滿格子，會變形
};

struct MergeLayout {
    int rows{1};
    int columns{2};
    CellOrder order{CellOrder::RowMajor};
    CellFit fit{CellFit::Contain};

    // 目標頁尺寸。留空（寬或高非正）時由 planMerge 以第一個來源頁的尺寸代入，
    // 讓「把 2 頁 A4 併成一張 A4」這個最常見的用法不需要呼叫端自己查尺寸。
    SizeF pageSize{0.0, 0.0};

    double marginPt{0.0};  // 頁面四周留白
    double gutterPt{0.0};  // 格子之間的間隔

    [[nodiscard]] constexpr int capacity() const noexcept { return rows * columns; }

    // N-up 的慣用格線。2-up 是一列兩欄（橫向並排），4-up 是 2×2。
    // 不在這裡自作主張旋轉頁面：直向頁併成 2-up 之後每格會很扁，
    // 那是使用者選 2-up 的必然結果，偷偷轉 90 度只會讓人以為程式壞了。
    [[nodiscard]] static MergeLayout nUp(int n) noexcept {
        MergeLayout layout;
        switch (n) {
            case 1: layout.rows = 1; layout.columns = 1; break;
            case 2: layout.rows = 1; layout.columns = 2; break;
            case 3: layout.rows = 1; layout.columns = 3; break;
            case 4: layout.rows = 2; layout.columns = 2; break;
            case 6: layout.rows = 2; layout.columns = 3; break;
            case 8: layout.rows = 2; layout.columns = 4; break;
            case 9: layout.rows = 3; layout.columns = 3; break;
            case 16: layout.rows = 4; layout.columns = 4; break;
            default:
                // 其餘取接近正方的格線：列 = ceil(sqrt(n))。
                layout.columns = n > 0 ? static_cast<int>(std::ceil(std::sqrt(
                                             static_cast<double>(n))))
                                       : 0;
                layout.rows = (layout.columns > 0) ? (n + layout.columns - 1) / layout.columns : 0;
                break;
        }
        return layout;
    }
};

// 一個來源頁的落點。
//
// matrix 的定義域是「來源頁的頁面座標」——含非零原點的 MediaBox 也適用，
// 因為 planMerge 已經把原點平移併進矩陣裡。因此註解的 /Rect 直接套用同一個
// matrix 就會落在正確的位置，不需要呼叫端再補一次平移；漏掉那次平移正是
// 「圖對了、螢光筆跑掉」的成因。
struct Placement {
    int sourceIndex{0};
    int row{0};
    int column{0};
    RectF cell{};        // 格子在目標頁的位置
    RectF placedBounds{};// 來源頁實際佔用的區域（Contain 時小於 cell）
    Matrix matrix{};
    double scaleX{1.0};
    double scaleY{1.0};
};

struct MergePlan {
    ComposeStatus status{ComposeStatus::Ok};
    SizeF pageSize{};
    std::vector<Placement> placements;

    [[nodiscard]] bool ok() const noexcept { return status == ComposeStatus::Ok; }
};

// 來源頁的可見範圍。用 RectF 而不是 SizeF，因為真實檔案的 MediaBox 原點
// 常常不是 (0,0)（PRD-PAGE-011），而版面必須以可見區域而非絕對座標對齊。
[[nodiscard]] inline MergePlan planMerge(const MergeLayout& layout,
                                         const std::vector<RectF>& sourceBoxes) {
    MergePlan plan;
    if (layout.rows <= 0 || layout.columns <= 0) {
        plan.status = ComposeStatus::InvalidGrid;
        return plan;
    }
    if (sourceBoxes.empty()) {
        plan.status = ComposeStatus::EmptySource;
        return plan;
    }
    if (static_cast<int>(sourceBoxes.size()) > layout.capacity()) {
        plan.status = ComposeStatus::TooManySources;
        return plan;
    }
    for (const RectF& box : sourceBoxes) {
        if (box.isEmpty()) {
            plan.status = ComposeStatus::InvalidSourceSize;
            return plan;
        }
    }

    SizeF target = layout.pageSize;
    if (target.isEmpty()) target = sourceBoxes.front().size();
    if (target.isEmpty()) {
        plan.status = ComposeStatus::InvalidPageSize;
        return plan;
    }

    const double contentWidth =
        target.width - 2.0 * layout.marginPt - layout.gutterPt * (layout.columns - 1);
    const double contentHeight =
        target.height - 2.0 * layout.marginPt - layout.gutterPt * (layout.rows - 1);
    if (contentWidth <= 0.0 || contentHeight <= 0.0) {
        plan.status = ComposeStatus::InvalidPageSize;
        return plan;
    }

    const double cellWidth = contentWidth / layout.columns;
    const double cellHeight = contentHeight / layout.rows;

    plan.pageSize = target;
    plan.placements.reserve(sourceBoxes.size());

    for (int i = 0; i < static_cast<int>(sourceBoxes.size()); ++i) {
        Placement placement;
        placement.sourceIndex = i;
        if (layout.order == CellOrder::RowMajor) {
            placement.row = i / layout.columns;
            placement.column = i % layout.columns;
        } else {
            placement.column = i / layout.rows;
            placement.row = i % layout.rows;
        }

        // 第 0 列在頁面**上方**：格子編號是閱讀順序，而 PDF 的 Y 軸向上。
        const double left = layout.marginPt + placement.column * (cellWidth + layout.gutterPt);
        const double top =
            target.height - layout.marginPt - placement.row * (cellHeight + layout.gutterPt);
        placement.cell = RectF{left, top - cellHeight, left + cellWidth, top};

        const RectF& box = sourceBoxes[static_cast<std::size_t>(i)];
        double sx = cellWidth / box.width();
        double sy = cellHeight / box.height();
        if (layout.fit == CellFit::Contain) {
            const double s = std::min(sx, sy);
            sx = s;
            sy = s;
        }
        placement.scaleX = sx;
        placement.scaleY = sy;

        const double placedWidth = box.width() * sx;
        const double placedHeight = box.height() * sy;
        const double offsetX = placement.cell.left + (cellWidth - placedWidth) / 2.0;
        const double offsetY = placement.cell.bottom + (cellHeight - placedHeight) / 2.0;
        placement.placedBounds =
            RectF{offsetX, offsetY, offsetX + placedWidth, offsetY + placedHeight};

        // 先把來源框的原點移到 (0,0) 再縮放、再平移到格子裡。
        placement.matrix = concat(concat(translation(-box.left, -box.bottom), scaling(sx, sy)),
                                  translation(offsetX, offsetY));
        plan.placements.push_back(placement);
    }

    return plan;
}

// ---- PRD-PAGE-008 覆蓋頁面（Overlay） --------------------------------------

enum class OverlayAnchor : std::uint8_t {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
    Stretch,  // 兩軸各自縮放填滿底頁，會變形
};

// 疊在上或下。浮水印必須在下（否則會蓋掉正文），印章與騎縫章必須在上。
// 預設 Above：印章是誤放時看得見的那一種，浮水印放錯只會整份看不出來。
enum class OverlayLayer : std::uint8_t {
    Above,
    Below,
};

struct OverlayOptions {
    OverlayAnchor anchor{OverlayAnchor::Center};
    OverlayLayer layer{OverlayLayer::Above};
    double scale{1.0};      // Stretch 以外的模式適用
    double offsetXPt{0.0};  // 對齊之後再微調
    double offsetYPt{0.0};
};

struct OverlayPlacement {
    ComposeStatus status{ComposeStatus::Ok};
    Matrix matrix{};
    RectF bounds{};  // 覆蓋內容在底頁上的實際範圍

    [[nodiscard]] bool ok() const noexcept { return status == ComposeStatus::Ok; }
};

// baseBox 與 overlayBox 都用 RectF：兩份文件的 MediaBox 原點都可能不是 (0,0)，
// 只傳尺寸會讓對齊在其中一邊偏掉整個原點的量。
[[nodiscard]] inline OverlayPlacement planOverlay(const OverlayOptions& options,
                                                  const RectF& baseBox, const RectF& overlayBox) {
    OverlayPlacement placement;
    if (baseBox.isEmpty() || overlayBox.isEmpty()) {
        placement.status = ComposeStatus::InvalidBox;
        return placement;
    }

    double sx = options.scale;
    double sy = options.scale;
    if (options.anchor == OverlayAnchor::Stretch) {
        sx = baseBox.width() / overlayBox.width();
        sy = baseBox.height() / overlayBox.height();
    } else if (!(options.scale > 0.0)) {
        placement.status = ComposeStatus::InvalidBox;
        return placement;
    }

    const double width = overlayBox.width() * sx;
    const double height = overlayBox.height() * sy;

    double x = baseBox.left;
    double y = baseBox.top - height;  // 預設貼齊上緣
    switch (options.anchor) {
        case OverlayAnchor::TopLeft:
        case OverlayAnchor::CenterLeft:
        case OverlayAnchor::BottomLeft:
        case OverlayAnchor::Stretch:
            x = baseBox.left;
            break;
        case OverlayAnchor::TopCenter:
        case OverlayAnchor::Center:
        case OverlayAnchor::BottomCenter:
            x = baseBox.left + (baseBox.width() - width) / 2.0;
            break;
        case OverlayAnchor::TopRight:
        case OverlayAnchor::CenterRight:
        case OverlayAnchor::BottomRight:
            x = baseBox.right - width;
            break;
    }
    switch (options.anchor) {
        case OverlayAnchor::TopLeft:
        case OverlayAnchor::TopCenter:
        case OverlayAnchor::TopRight:
            y = baseBox.top - height;
            break;
        case OverlayAnchor::CenterLeft:
        case OverlayAnchor::Center:
        case OverlayAnchor::CenterRight:
            y = baseBox.bottom + (baseBox.height() - height) / 2.0;
            break;
        case OverlayAnchor::BottomLeft:
        case OverlayAnchor::BottomCenter:
        case OverlayAnchor::BottomRight:
        case OverlayAnchor::Stretch:
            y = baseBox.bottom;
            break;
    }
    x += options.offsetXPt;
    y += options.offsetYPt;

    placement.matrix =
        concat(concat(translation(-overlayBox.left, -overlayBox.bottom), scaling(sx, sy)),
               translation(x, y));
    placement.bounds = RectF{x, y, x + width, y + height};
    return placement;
}

// ---- PRD-PAGE-010 設定文件邊界 ---------------------------------------------

enum class PageBoxKind : std::uint8_t {
    Media,
    Crop,
    Bleed,
    Trim,
    Art,
};

[[nodiscard]] inline const char* boxKey(PageBoxKind kind) noexcept {
    switch (kind) {
        case PageBoxKind::Media: return "MediaBox";
        case PageBoxKind::Crop: return "CropBox";
        case PageBoxKind::Bleed: return "BleedBox";
        case PageBoxKind::Trim: return "TrimBox";
        case PageBoxKind::Art: return "ArtBox";
    }
    return "";
}

inline constexpr PageBoxKind kAllBoxKinds[5] = {PageBoxKind::Media, PageBoxKind::Crop,
                                                PageBoxKind::Bleed, PageBoxKind::Trim,
                                                PageBoxKind::Art};

// 五種框的設定意圖。沒設的框保持原樣，而不是被清成預設值——
// 「只想改 CropBox」是最常見的用法，順手把 TrimBox 刪掉會毀掉印刷用的檔案。
struct PageBoxSettings {
    std::optional<RectF> media{};
    std::optional<RectF> crop{};
    std::optional<RectF> bleed{};
    std::optional<RectF> trim{};
    std::optional<RectF> art{};

    // 子框超出 MediaBox 時夾進去而不是拒絕。PDF 規格要求子框落在 MediaBox 之內，
    // 而各家檢視器對越界的處理並不一致；與其寫出一份「看情況」的檔案，
    // 不如先夾好。要嚴格把關的呼叫端把它設為 false，越界會得到 BoxOutsideMedia。
    bool clampToMedia{true};

    [[nodiscard]] const std::optional<RectF>& box(PageBoxKind kind) const noexcept {
        switch (kind) {
            case PageBoxKind::Media: return media;
            case PageBoxKind::Crop: return crop;
            case PageBoxKind::Bleed: return bleed;
            case PageBoxKind::Trim: return trim;
            case PageBoxKind::Art: return art;
        }
        return media;
    }

    [[nodiscard]] std::optional<RectF>& box(PageBoxKind kind) noexcept {
        return const_cast<std::optional<RectF>&>(
            static_cast<const PageBoxSettings*>(this)->box(kind));
    }

    [[nodiscard]] bool empty() const noexcept {
        return !media && !crop && !bleed && !trim && !art;
    }
};

struct ResolvedPageBoxes {
    ComposeStatus status{ComposeStatus::Ok};
    RectF media{};
    std::optional<RectF> crop{};
    std::optional<RectF> bleed{};
    std::optional<RectF> trim{};
    std::optional<RectF> art{};
    bool clamped{false};  // 有任何一個子框被夾過，UI 應該告知使用者

    [[nodiscard]] bool ok() const noexcept { return status == ComposeStatus::Ok; }

    // 回傳值而非參考：MediaBox 解析後一定有值，其餘可能沒有，
    // 統一成 optional 讓呼叫端可以一個迴圈掃完五種框。
    [[nodiscard]] std::optional<RectF> box(PageBoxKind kind) const noexcept {
        switch (kind) {
            case PageBoxKind::Media: return media;
            case PageBoxKind::Crop: return crop;
            case PageBoxKind::Bleed: return bleed;
            case PageBoxKind::Trim: return trim;
            case PageBoxKind::Art: return art;
        }
        return std::nullopt;
    }
};

// currentMedia 是頁面既有的 MediaBox；settings.media 有值時以它為準。
[[nodiscard]] inline ResolvedPageBoxes resolvePageBoxes(const PageBoxSettings& settings,
                                                        const RectF& currentMedia) {
    ResolvedPageBoxes out;
    if (settings.empty()) {
        out.status = ComposeStatus::NothingToDo;
        return out;
    }

    out.media = settings.media ? settings.media->normalized() : currentMedia.normalized();
    if (out.media.isEmpty()) {
        out.status = ComposeStatus::InvalidBox;
        return out;
    }

    const auto resolveChild = [&](const std::optional<RectF>& in,
                                  std::optional<RectF>& slot) -> bool {
        if (!in) return true;
        const RectF box = in->normalized();
        if (box.isEmpty()) {
            out.status = ComposeStatus::InvalidBox;
            return false;
        }
        const RectF clipped = box.intersected(out.media);
        if (clipped != box) {
            if (!settings.clampToMedia) {
                out.status = ComposeStatus::BoxOutsideMedia;
                return false;
            }
            if (clipped.isEmpty()) {
                out.status = ComposeStatus::BoxOutsideMedia;  // 夾完什麼都不剩
                return false;
            }
            out.clamped = true;
            slot = clipped;
            return true;
        }
        slot = box;
        return true;
    };

    if (!resolveChild(settings.crop, out.crop)) return out;
    if (!resolveChild(settings.bleed, out.bleed)) return out;
    if (!resolveChild(settings.trim, out.trim)) return out;
    if (!resolveChild(settings.art, out.art)) return out;
    return out;
}

// ---- PRD-PAGE-011 正規化頁面與 MediaBox 偏移 -------------------------------

// 把 MediaBox 的原點移到 (0,0)。
//
// 多數程式碼（含我們自己的檢視層與註解幾何）假設頁面原點就是 (0,0)，
// 而真實檔案並不保證這件事。正規化的定義是「平移座標系」而不是「改數字」：
// MediaBox、所有子框、頁面內容、所有註解幾何都要平移同樣的量，
// 少平移其中任何一項，結果就是「頁面看起來對、標記卻跑掉」。
struct NormalizationPlan {
    ComposeStatus status{ComposeStatus::Ok};
    bool needed{false};
    double dx{0.0};
    double dy{0.0};
    RectF media{};  // 平移後的 MediaBox，原點恆為 (0,0)

    [[nodiscard]] bool ok() const noexcept { return status == ComposeStatus::Ok; }
    [[nodiscard]] Matrix matrix() const noexcept { return translation(dx, dy); }
};

// epsilonPt：小於這個量的偏移視為已正規化。浮點誤差造成的 1e-12 偏移不值得
// 重寫整頁內容——那會讓「存檔後檔案又變大了」變成每次開檔都發生的事。
[[nodiscard]] inline NormalizationPlan planNormalization(const RectF& media,
                                                         double epsilonPt = 1e-6) {
    NormalizationPlan plan;
    const RectF box = media.normalized();
    if (box.isEmpty()) {
        plan.status = ComposeStatus::InvalidBox;
        return plan;
    }
    plan.dx = -box.left;
    plan.dy = -box.bottom;
    plan.needed = std::fabs(plan.dx) > epsilonPt || std::fabs(plan.dy) > epsilonPt;
    plan.media = RectF{0.0, 0.0, box.width(), box.height()};
    if (!plan.needed) {
        plan.dx = 0.0;
        plan.dy = 0.0;
        plan.media = box;
    }
    return plan;
}

// ---- 頁面旋轉（/Rotate）------------------------------------------------------

// /Rotate 是「顯示時再轉」，內容串流本身沒有轉。把頁面當成素材放進別的頁面時
// 必須自己把它烘進矩陣裡，否則 2-up 出來的頁面會有一格是倒的。
//
// box 是來源頁的框（原點可能非零）；回傳的矩陣定義域是來源頁座標，
// 值域是「已旋轉、原點在 (0,0)」的空間，其尺寸見 rotatedSize()。
[[nodiscard]] inline Matrix rotationMatrix(const RectF& box, int degrees) noexcept {
    const int normalized = ((degrees % 360) + 360) % 360;
    const Matrix toOrigin = translation(-box.left, -box.bottom);
    const double w = box.width();
    const double h = box.height();
    switch (normalized) {
        case 90: return concat(toOrigin, Matrix{0.0, -1.0, 1.0, 0.0, 0.0, w});
        case 180: return concat(toOrigin, Matrix{-1.0, 0.0, 0.0, -1.0, w, h});
        case 270: return concat(toOrigin, Matrix{0.0, 1.0, -1.0, 0.0, h, 0.0});
        default: return toOrigin;
    }
}

[[nodiscard]] inline SizeF rotatedSize(const RectF& box, int degrees) noexcept {
    const int normalized = ((degrees % 360) + 360) % 360;
    if (normalized == 90 || normalized == 270) return SizeF{box.height(), box.width()};
    return SizeF{box.width(), box.height()};
}

}  // namespace alioth::domain::compose
