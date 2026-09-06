#include "ui/ribbon/icon_resolver.h"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QRectF>

#include <algorithm>

namespace alioth::ui::ribbon {
namespace {

// 圖示以向量畫，不放點陣資源。
//
// 三個理由，依重要性排序：
//   1. 高 DPI。點陣圖在 150% / 200% 縮放下要嘛糊、要嘛要備妥好幾份尺寸，
//      而按鈕尺寸本來就是從字型高度推導的（見 ribbon_button.cpp），不是固定像素。
//   2. 深色主題。線條顏色取自當下的 palette，主題一換就跟著換；
//      點陣圖得備淺色與深色兩套，然後總有幾張會忘了換。
//   3. 安裝包上限 130 MB（PRD §4.1），而且「不新增引擎級元件」的立場下
//      也不該為了圖示引入 SVG 資源管線。
//
// 畫法刻意保持極簡：單色線條、統一線寬、一律在單位方框內定義座標。
// 這不是要好看，是要**在一排按鈕裡一眼能分辨**——那才是圖示在 Ribbon 裡的作用。

// 單位座標 → 實際矩形。所有 glyph 都用 0..1 描述，縮放與置中在這裡一次處理。
class UnitCanvas {
public:
    UnitCanvas(QPainter& painter, const QRectF& bounds) : painter_(painter) {
        const double side = std::min(bounds.width(), bounds.height());
        // 留一圈邊距：貼著按鈕邊緣的圖示在密集排列時會糊成一片。
        const double inset = side * 0.12;
        box_ = QRectF(bounds.center().x() - side / 2.0 + inset,
                      bounds.center().y() - side / 2.0 + inset, side - inset * 2.0,
                      side - inset * 2.0);
    }

    [[nodiscard]] QPointF at(double x, double y) const {
        return QPointF(box_.left() + x * box_.width(), box_.top() + y * box_.height());
    }

    [[nodiscard]] QRectF rect(double x, double y, double w, double h) const {
        return QRectF(at(x, y), at(x + w, y + h));
    }

    [[nodiscard]] double unit() const { return box_.width(); }

    void line(double x1, double y1, double x2, double y2) const {
        painter_.drawLine(at(x1, y1), at(x2, y2));
    }

private:
    QPainter& painter_;
    QRectF box_;
};

// 一張紙。多個 glyph 共用，差別只在紙上加什麼記號——這樣「這是文件相關的動作」
// 在整排按鈕裡是一眼看得出來的族群，而不是二十個互不相干的形狀。
void drawPage(QPainter& painter, const UnitCanvas& canvas) {
    QPainterPath path;
    path.moveTo(canvas.at(0.22, 0.05));
    path.lineTo(canvas.at(0.62, 0.05));
    path.lineTo(canvas.at(0.78, 0.24));
    path.lineTo(canvas.at(0.78, 0.95));
    path.lineTo(canvas.at(0.22, 0.95));
    path.closeSubpath();
    painter.drawPath(path);
    // 折角。少了它就只是一個長方形，和「欄位」「頁面」那些 glyph 分不開。
    canvas.line(0.62, 0.05, 0.62, 0.24);
    canvas.line(0.62, 0.24, 0.78, 0.24);
}

void drawMagnifier(QPainter& painter, const UnitCanvas& canvas) {
    painter.drawEllipse(canvas.rect(0.08, 0.08, 0.55, 0.55));
    canvas.line(0.58, 0.58, 0.92, 0.92);
}

void drawPlus(const UnitCanvas& canvas, double cx, double cy, double size) {
    canvas.line(cx - size, cy, cx + size, cy);
    canvas.line(cx, cy - size, cx, cy + size);
}

void drawArrowHead(QPainter& painter, const UnitCanvas& canvas, double x, double y, double dx,
                   double dy) {
    QPainterPath head;
    head.moveTo(canvas.at(x, y));
    head.lineTo(canvas.at(x - dx - dy * 0.6, y - dy + dx * 0.6));
    head.lineTo(canvas.at(x - dx + dy * 0.6, y - dy - dx * 0.6));
    head.closeSubpath();
    painter.fillPath(head, painter.pen().color());
}

// 逆時針的圓弧箭頭，旋轉與重新整理共用。
void drawCircularArrow(QPainter& painter, const UnitCanvas& canvas) {
    const QRectF box = canvas.rect(0.12, 0.12, 0.76, 0.76);
    painter.drawArc(box, 30 * 16, 280 * 16);
    drawArrowHead(painter, canvas, 0.86, 0.36, 0.0, 0.18);
}

void drawGlyph(QPainter& painter, const QRectF& bounds, const QString& name) {
    const UnitCanvas canvas(painter, bounds);

    if (name == QLatin1String("file.open")) {
        // 資料夾：後片 + 掀開的前片。
        QPainterPath back;
        back.moveTo(canvas.at(0.06, 0.86));
        back.lineTo(canvas.at(0.06, 0.20));
        back.lineTo(canvas.at(0.40, 0.20));
        back.lineTo(canvas.at(0.48, 0.32));
        back.lineTo(canvas.at(0.88, 0.32));
        back.lineTo(canvas.at(0.88, 0.86));
        back.closeSubpath();
        painter.drawPath(back);
        canvas.line(0.06, 0.86, 0.94, 0.86);
        return;
    }
    if (name == QLatin1String("file.save")) {
        // 磁片。已經沒人看過實體磁片了，但它仍然是最無歧義的「儲存」。
        painter.drawRect(canvas.rect(0.10, 0.10, 0.80, 0.80));
        painter.drawRect(canvas.rect(0.28, 0.10, 0.44, 0.30));
        painter.drawRect(canvas.rect(0.24, 0.56, 0.52, 0.34));
        return;
    }
    if (name == QLatin1String("file.print")) {
        painter.drawRect(canvas.rect(0.10, 0.36, 0.80, 0.34));
        painter.drawRect(canvas.rect(0.26, 0.08, 0.48, 0.28));
        painter.drawRect(canvas.rect(0.26, 0.64, 0.48, 0.28));
        return;
    }
    if (name == QLatin1String("edit.copy")) {
        painter.drawRect(canvas.rect(0.08, 0.08, 0.56, 0.56));
        painter.drawRect(canvas.rect(0.36, 0.36, 0.56, 0.56));
        return;
    }
    if (name == QLatin1String("edit.undo") || name == QLatin1String("edit.redo")) {
        const bool undo = name == QLatin1String("edit.undo");
        const QRectF box = canvas.rect(0.14, 0.24, 0.72, 0.62);
        painter.drawArc(box, undo ? 40 * 16 : 100 * 16, undo ? 140 * 16 : -140 * 16);
        if (undo) {
            drawArrowHead(painter, canvas, 0.16, 0.34, -0.14, 0.10);
        } else {
            drawArrowHead(painter, canvas, 0.84, 0.34, 0.14, 0.10);
        }
        return;
    }
    if (name == QLatin1String("tool.select")) {
        // 文字游標（I 型）。
        canvas.line(0.50, 0.10, 0.50, 0.90);
        canvas.line(0.32, 0.10, 0.68, 0.10);
        canvas.line(0.32, 0.90, 0.68, 0.90);
        return;
    }
    if (name == QLatin1String("tool.hand")) {
        // 四方向箭頭。真正的手形在單色細線下辨識度很差，平移語意反而更清楚。
        canvas.line(0.50, 0.06, 0.50, 0.94);
        canvas.line(0.06, 0.50, 0.94, 0.50);
        drawArrowHead(painter, canvas, 0.50, 0.06, 0.0, -0.16);
        drawArrowHead(painter, canvas, 0.50, 0.94, 0.0, 0.16);
        drawArrowHead(painter, canvas, 0.06, 0.50, -0.16, 0.0);
        drawArrowHead(painter, canvas, 0.94, 0.50, 0.16, 0.0);
        return;
    }
    if (name == QLatin1String("annot.highlight")) {
        canvas.line(0.12, 0.28, 0.88, 0.28);
        canvas.line(0.12, 0.46, 0.72, 0.46);
        // 螢光筆的重點是那條粗色帶，不是文字。
        painter.fillRect(canvas.rect(0.12, 0.64, 0.76, 0.20), painter.pen().color());
        return;
    }
    if (name == QLatin1String("annot.stickyNote")) {
        QPainterPath note;
        note.moveTo(canvas.at(0.10, 0.10));
        note.lineTo(canvas.at(0.90, 0.10));
        note.lineTo(canvas.at(0.90, 0.62));
        note.lineTo(canvas.at(0.62, 0.90));
        note.lineTo(canvas.at(0.10, 0.90));
        note.closeSubpath();
        painter.drawPath(note);
        canvas.line(0.62, 0.90, 0.62, 0.62);
        canvas.line(0.62, 0.62, 0.90, 0.62);
        return;
    }
    if (name == QLatin1String("annot.ink")) {
        QPainterPath stroke;
        stroke.moveTo(canvas.at(0.10, 0.74));
        stroke.cubicTo(canvas.at(0.34, 0.18), canvas.at(0.52, 0.94), canvas.at(0.90, 0.30));
        painter.drawPath(stroke);
        return;
    }
    if (name == QLatin1String("annot.stamp")) {
        painter.drawEllipse(canvas.rect(0.10, 0.10, 0.80, 0.80));
        painter.drawEllipse(canvas.rect(0.26, 0.26, 0.48, 0.48));
        return;
    }
    if (name == QLatin1String("nav.back") || name == QLatin1String("nav.forward")) {
        // 瀏覽歷史。用單純的方向箭頭而不是 undo/redo 的弧線箭頭：
        // 「回到剛才看的位置」與「復原剛才做的修改」是兩件完全不同的事，
        // 圖示長得像會讓人按錯，而按錯 undo 會改到文件。
        const bool back = name == QLatin1String("nav.back");
        canvas.line(0.12, 0.50, 0.88, 0.50);
        if (back) {
            drawArrowHead(painter, canvas, 0.12, 0.50, -0.22, 0.0);
        } else {
            drawArrowHead(painter, canvas, 0.88, 0.50, 0.22, 0.0);
        }
        return;
    }
    if (name == QLatin1String("search.find")) {
        drawMagnifier(painter, canvas);
        return;
    }
    if (name == QLatin1String("view.zoomIn") || name == QLatin1String("view.zoomOut")) {
        drawMagnifier(painter, canvas);
        canvas.line(0.20, 0.355, 0.51, 0.355);
        if (name == QLatin1String("view.zoomIn")) canvas.line(0.355, 0.20, 0.355, 0.51);
        return;
    }
    if (name == QLatin1String("view.singlePage")) {
        painter.drawRect(canvas.rect(0.24, 0.08, 0.52, 0.84));
        return;
    }
    if (name == QLatin1String("view.fullScreen")) {
        // 四個角括號。整個方框會和「單頁」撞在一起。
        canvas.line(0.08, 0.08, 0.34, 0.08);
        canvas.line(0.08, 0.08, 0.08, 0.34);
        canvas.line(0.92, 0.08, 0.66, 0.08);
        canvas.line(0.92, 0.08, 0.92, 0.34);
        canvas.line(0.08, 0.92, 0.34, 0.92);
        canvas.line(0.08, 0.92, 0.08, 0.66);
        canvas.line(0.92, 0.92, 0.66, 0.92);
        canvas.line(0.92, 0.92, 0.92, 0.66);
        return;
    }
    if (name == QLatin1String("comment.list")) {
        QPainterPath bubble;
        bubble.addRoundedRect(canvas.rect(0.08, 0.14, 0.84, 0.56), canvas.unit() * 0.10,
                              canvas.unit() * 0.10);
        painter.drawPath(bubble);
        QPainterPath tail;
        tail.moveTo(canvas.at(0.26, 0.70));
        tail.lineTo(canvas.at(0.26, 0.92));
        tail.lineTo(canvas.at(0.48, 0.70));
        painter.drawPath(tail);
        return;
    }
    if (name == QLatin1String("protect.password")) {
        painter.drawRect(canvas.rect(0.16, 0.44, 0.68, 0.46));
        painter.drawArc(canvas.rect(0.28, 0.12, 0.44, 0.50), 0, 180 * 16);
        return;
    }
    if (name == QLatin1String("protect.redactMark")) {
        canvas.line(0.10, 0.20, 0.90, 0.20);
        // 塗黑就是塗黑：實心黑塊，不留任何「可能還看得到」的暗示。
        painter.fillRect(canvas.rect(0.10, 0.38, 0.80, 0.24), painter.pen().color());
        canvas.line(0.10, 0.80, 0.62, 0.80);
        return;
    }
    if (name == QLatin1String("sign.digitalSign") ||
        name == QLatin1String("form.signatureField")) {
        QPainterPath stroke;
        stroke.moveTo(canvas.at(0.08, 0.62));
        stroke.cubicTo(canvas.at(0.30, 0.10), canvas.at(0.44, 0.80), canvas.at(0.66, 0.34));
        painter.drawPath(stroke);
        canvas.line(0.08, 0.86, 0.92, 0.86);
        if (name == QLatin1String("form.signatureField")) {
            painter.drawRect(canvas.rect(0.04, 0.14, 0.92, 0.80));
        }
        return;
    }
    if (name == QLatin1String("form.textField")) {
        painter.drawRect(canvas.rect(0.06, 0.28, 0.88, 0.44));
        canvas.line(0.22, 0.36, 0.22, 0.64);
        return;
    }
    if (name == QLatin1String("form.highlightFields")) {
        painter.fillRect(canvas.rect(0.06, 0.14, 0.88, 0.30), painter.pen().color());
        painter.drawRect(canvas.rect(0.06, 0.56, 0.88, 0.30));
        return;
    }
    if (name == QLatin1String("page.insert")) {
        drawPage(painter, canvas);
        drawPlus(canvas, 0.50, 0.62, 0.16);
        return;
    }
    if (name == QLatin1String("page.rotate") || name == QLatin1String("help.checkUpdates")) {
        drawCircularArrow(painter, canvas);
        return;
    }
    if (name == QLatin1String("document.merge")) {
        painter.drawRect(canvas.rect(0.06, 0.08, 0.42, 0.52));
        painter.drawRect(canvas.rect(0.52, 0.08, 0.42, 0.52));
        canvas.line(0.50, 0.66, 0.50, 0.92);
        drawArrowHead(painter, canvas, 0.50, 0.94, 0.0, 0.16);
        return;
    }
    if (name == QLatin1String("page.watermark")) {
        drawPage(painter, canvas);
        canvas.line(0.28, 0.76, 0.72, 0.30);
        return;
    }
    if (name == QLatin1String("bookmark.add")) {
        QPainterPath mark;
        mark.moveTo(canvas.at(0.24, 0.06));
        mark.lineTo(canvas.at(0.76, 0.06));
        mark.lineTo(canvas.at(0.76, 0.94));
        mark.lineTo(canvas.at(0.50, 0.68));
        mark.lineTo(canvas.at(0.24, 0.94));
        mark.closeSubpath();
        painter.drawPath(mark);
        return;
    }
    if (name == QLatin1String("help.contents")) {
        painter.drawEllipse(canvas.rect(0.08, 0.08, 0.84, 0.84));
        QFont font = painter.font();
        font.setPixelSize(static_cast<int>(canvas.unit() * 0.62));
        font.setBold(true);
        painter.setFont(font);
        // 只用 ASCII：這個字形要在任何語系與任何字型後備下都畫得出來。
        painter.drawText(canvas.rect(0.08, 0.08, 0.84, 0.84), Qt::AlignCenter,
                         QStringLiteral("?"));
        return;
    }
    if (name == QLatin1String("file.properties") || name == QLatin1String("file.exportText")) {
        drawPage(painter, canvas);
        canvas.line(0.32, 0.52, 0.68, 0.52);
        canvas.line(0.32, 0.68, 0.68, 0.68);
        return;
    }
}

// 有沒有這個名字的向量圖示。resolveIconByName 用它決定要不要建立引擎——
// 認不得的名字必須回空圖示，按鈕才會退回純文字（見 ribbon_button.cpp）。
[[nodiscard]] bool hasGlyph(const QString& name) {
    static const QStringList kNames = {
        QStringLiteral("file.open"),          QStringLiteral("file.save"),
        QStringLiteral("file.print"),         QStringLiteral("file.properties"),
        QStringLiteral("file.exportText"),    QStringLiteral("edit.copy"),
        QStringLiteral("edit.undo"),          QStringLiteral("edit.redo"),
        QStringLiteral("tool.select"),        QStringLiteral("tool.hand"),
        QStringLiteral("annot.highlight"),    QStringLiteral("annot.stickyNote"),
        QStringLiteral("annot.ink"),          QStringLiteral("annot.stamp"),
        QStringLiteral("search.find"),        QStringLiteral("view.zoomIn"),
        QStringLiteral("nav.back"),           QStringLiteral("nav.forward"),
        QStringLiteral("view.zoomOut"),       QStringLiteral("view.singlePage"),
        QStringLiteral("view.fullScreen"),    QStringLiteral("comment.list"),
        QStringLiteral("protect.password"),   QStringLiteral("protect.redactMark"),
        QStringLiteral("sign.digitalSign"),   QStringLiteral("form.textField"),
        QStringLiteral("form.signatureField"), QStringLiteral("form.highlightFields"),
        QStringLiteral("page.insert"),        QStringLiteral("page.rotate"),
        QStringLiteral("document.merge"),     QStringLiteral("page.watermark"),
        QStringLiteral("bookmark.add"),       QStringLiteral("help.contents"),
        QStringLiteral("help.checkUpdates"),
    };
    return kNames.contains(name);
}

class GlyphIconEngine : public QIconEngine {
public:
    explicit GlyphIconEngine(QString name) : name_(std::move(name)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        if (painter == nullptr) return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        // 顏色取自當下的 palette，不寫死。停用狀態走 Disabled 群組，
        // 才會和旁邊的停用文字落在同一個對比上。
        const QPalette palette = QApplication::palette();
        const QPalette::ColorGroup group =
            mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
        QPen pen(palette.color(group, QPalette::ButtonText));
        // 線寬跟著圖示尺寸走：固定 1px 的線在 200% 縮放下會細到看不見。
        pen.setWidthF(std::max(1.0, std::min(rect.width(), rect.height()) / 14.0));
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);

        drawGlyph(*painter, QRectF(rect), name_);
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        // devicePixelRatio 交給呼叫端：這裡只保證在給定的像素尺寸下畫滿。
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }

    [[nodiscard]] QIconEngine* clone() const override { return new GlyphIconEngine(name_); }

private:
    QString name_;
};

}  // namespace

QIcon resolveIconByName(const QString& iconName) {
    if (iconName.isEmpty()) return QIcon{};
    // 內建向量圖示優先於系統主題：主題圖示在 Windows 上幾乎不存在，
    // 而且各平台長相不一致，Ribbon 需要三平台看起來一樣。
    if (hasGlyph(iconName)) return QIcon(new GlyphIconEngine(iconName));
    if (QIcon::hasThemeIcon(iconName)) return QIcon::fromTheme(iconName);
    QIcon icon(iconName);
    return icon.isNull() ? QIcon{} : icon;
}

}  // namespace alioth::ui::ribbon
