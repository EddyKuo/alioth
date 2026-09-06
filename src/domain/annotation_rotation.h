#pragma once

// 註解旋轉(PRD-ANN-023):以外接矩形中心為支點旋轉幾何,Shift 時以 15 度
// 為增量吸附。
//
// 只轉幾何點與 Rect,不改變 Annotation 的其他欄位,也不寫回 /Rotate——
// 註解沒有頁面那種 /Rotate 鍵,「旋轉」對註解而言就是把構成它的每個頂點
// 繞支點轉過去,寫入層看到的仍然是轉完後的座標,不需要另外處理。
//
// 純函數,只讀 domain::Annotation 既有的 variant 型別,不修改
// domain/annotation.h 的定義。

#include <cmath>
#include <variant>
#include <vector>

#include "domain/annotation.h"
#include "domain/geometry.h"

namespace alioth::domain {

// 把角度吸附到 15 度的整數倍(PRD-ANN-023「Shift 以 15 度為增量」)。
[[nodiscard]] inline double snapToFifteenDegrees(double degrees) noexcept {
    constexpr double kStep = 15.0;
    return std::round(degrees / kStep) * kStep;
}

namespace detail {

[[nodiscard]] inline PointF rotatePoint(const PointF& p, const PointF& pivot, double radians) noexcept {
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const double dx = p.x - pivot.x;
    const double dy = p.y - pivot.y;
    return PointF{pivot.x + dx * c - dy * s, pivot.y + dx * s + dy * c};
}

[[nodiscard]] inline PointF rectCenter(const RectF& r) noexcept {
    return PointF{(r.left + r.right) * 0.5, (r.bottom + r.top) * 0.5};
}

// 逐點旋轉一個外接矩形,回傳旋轉後所有點各自的外接框(給呼叫端重算 /Rect)。
[[nodiscard]] inline RectF rotatedBounds(const RectF& r, const PointF& pivot, double radians) noexcept {
    const PointF corners[4] = {
        rotatePoint({r.left, r.bottom}, pivot, radians),
        rotatePoint({r.right, r.bottom}, pivot, radians),
        rotatePoint({r.left, r.top}, pivot, radians),
        rotatePoint({r.right, r.top}, pivot, radians),
    };
    RectF bounds{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const PointF& p : corners) {
        bounds.left = std::min(bounds.left, p.x);
        bounds.right = std::max(bounds.right, p.x);
        bounds.bottom = std::min(bounds.bottom, p.y);
        bounds.top = std::max(bounds.top, p.y);
    }
    return bounds;
}

struct RotateGeometryVisitor {
    PointF pivot{};
    double radians{0.0};

    AnnotationGeometry operator()(TextMarkupGeometry g) const {
        for (QuadPoint& q : g.quads) {
            q.upperLeft = rotatePoint(q.upperLeft, pivot, radians);
            q.upperRight = rotatePoint(q.upperRight, pivot, radians);
            q.lowerLeft = rotatePoint(q.lowerLeft, pivot, radians);
            q.lowerRight = rotatePoint(q.lowerRight, pivot, radians);
        }
        return g;
    }
    AnnotationGeometry operator()(ShapeGeometry g) const { return g; }  // 由 Rect 決定,呼叫端另轉 Rect
    AnnotationGeometry operator()(LineGeometry g) const {
        g.start = rotatePoint(g.start, pivot, radians);
        g.end = rotatePoint(g.end, pivot, radians);
        return g;
    }
    AnnotationGeometry operator()(InkGeometry g) const {
        for (auto& stroke : g.strokes) {
            for (PointF& p : stroke) p = rotatePoint(p, pivot, radians);
        }
        return g;
    }
    AnnotationGeometry operator()(TextNoteGeometry g) const { return g; }
    AnnotationGeometry operator()(CaretGeometry g) const { return g; }
    AnnotationGeometry operator()(FreeTextGeometry g) const {
        if (g.callout.has_value()) {
            g.callout->start = rotatePoint(g.callout->start, pivot, radians);
            if (g.callout->knee.has_value()) *g.callout->knee = rotatePoint(*g.callout->knee, pivot, radians);
            g.callout->end = rotatePoint(g.callout->end, pivot, radians);
        }
        return g;
    }
    AnnotationGeometry operator()(PolygonGeometry g) const {
        for (PointF& p : g.vertices) p = rotatePoint(p, pivot, radians);
        return g;
    }
    AnnotationGeometry operator()(PolyLineGeometry g) const {
        for (PointF& p : g.vertices) p = rotatePoint(p, pivot, radians);
        return g;
    }
    // 圖章的內容完全由 Rect 決定（內建圖章畫在框裡、自訂圖片鋪滿框），
    // 幾何本身沒有需要轉的座標；呼叫端另外轉 Rect。
    //
    // 已知限制：旋轉後的 Rect 是外接矩形，因此非 90 度倍數的旋轉會讓圖章
    // 變大而不是真的斜著擺。要真的斜擺必須用 /AP 的 /Matrix，那是另一件事。
    AnnotationGeometry operator()(StampGeometry g) const { return g; }
};

}  // namespace detail

// 旋轉一則註解的幾何與 /Rect,回傳新的 Annotation(其餘欄位原樣拷貝)。
// degrees 為逆時針角度(與頁面座標系 Y 向上一致);若 snapToShift 為 true,
// 角度先吸附到 15 度增量再套用。
[[nodiscard]] inline Annotation rotateAnnotation(const Annotation& annotation, double degrees,
                                                 bool snapToShift) {
    const double effectiveDegrees = snapToShift ? snapToFifteenDegrees(degrees) : degrees;
    const double radians = effectiveDegrees * 3.14159265358979323846 / 180.0;
    const PointF pivot = detail::rectCenter(annotation.rect.normalized());

    Annotation out = annotation;
    out.geometry = std::visit(detail::RotateGeometryVisitor{pivot, radians}, annotation.geometry);
    out.rect = detail::rotatedBounds(annotation.rect.normalized(), pivot, radians);
    return out;
}

}  // namespace alioth::domain
