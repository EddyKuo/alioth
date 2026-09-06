#include "app/print/stamp_painter.h"

#include <QFont>
#include <QFontMetricsF>

#include <algorithm>
#include <cmath>

namespace alioth::app::print {
namespace {

constexpr double kEpsilon = 1e-9;

QFont buildFont(const QString& family, bool bold, double pixelSize) {
    QFont font;
    if (!family.isEmpty()) font.setFamily(family);
    font.setBold(bold);
    // 低於 1 像素的字級會讓 Qt 退回預設值，畫出比預期大得多的字。
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(pixelSize))));
    return font;
}

}  // namespace

void paintStampText(QPainter& painter, const QRectF& contentBoxPt, const StampText& style,
                    const QString& resolvedText, double deviceScale) {
    if (resolvedText.isEmpty() || contentBoxPt.isEmpty() || deviceScale <= kEpsilon) return;

    const QFont font = buildFont(style.fontFamily, style.bold, style.fontPointSize * deviceScale);
    const QFontMetricsF metrics(font);
    const QSizeF sizeDevice{metrics.horizontalAdvance(resolvedText), metrics.height()};
    const QSizeF sizePt{sizeDevice.width() / deviceScale, sizeDevice.height() / deviceScale};

    const QRectF slotPt = placeStamp(contentBoxPt, sizePt, style.anchor);
    if (slotPt.isEmpty()) return;

    const QRectF slotDevice{slotPt.left() * deviceScale, slotPt.top() * deviceScale,
                            slotPt.width() * deviceScale, slotPt.height() * deviceScale};

    painter.save();
    painter.setOpacity(std::clamp(style.opacity, 0.0, 1.0));
    painter.setFont(font);
    painter.setPen(style.color);
    if (std::abs(style.rotationDegrees) > kEpsilon) {
        painter.translate(slotDevice.center());
        painter.rotate(style.rotationDegrees);
        painter.translate(-slotDevice.center());
    }
    painter.drawText(slotDevice, Qt::AlignCenter | Qt::TextDontClip, resolvedText);
    painter.restore();
}

void paintWatermark(QPainter& painter, const QRectF& printableRectPt,
                    const WatermarkOptions& options, const StampContext& context,
                    double deviceScale) {
    if (!options.enabled || options.text.isEmpty() || printableRectPt.isEmpty() ||
        deviceScale <= kEpsilon) {
        return;
    }

    const QString text = expandStampTokens(options.text, context);
    if (text.isEmpty()) return;

    // 字級以「文字寬度佔可列印區的比例」反推，而不是由使用者填點數：
    // 同一組浮水印設定要能同時套在 A4 與 A0 上而視覺比重一致。
    const double fraction = std::clamp(options.widthFraction, 0.05, 1.0);
    const double probePixelSize = 100.0;
    const QFont probe = buildFont(options.fontFamily, true, probePixelSize);
    const double probeWidth = QFontMetricsF(probe).horizontalAdvance(text);
    if (probeWidth <= kEpsilon) return;

    const double targetWidthDevice = printableRectPt.width() * deviceScale * fraction;
    const QFont font = buildFont(options.fontFamily, true, probePixelSize * targetWidthDevice / probeWidth);
    const QFontMetricsF metrics(font);
    const QRectF box{0.0, 0.0, metrics.horizontalAdvance(text), metrics.height()};

    const QPointF centerDevice{printableRectPt.center().x() * deviceScale,
                               printableRectPt.center().y() * deviceScale};

    painter.save();
    painter.setOpacity(std::clamp(options.opacity, 0.0, 1.0));
    painter.setFont(font);
    painter.setPen(options.color);
    painter.translate(centerDevice);
    painter.rotate(options.rotationDegrees);
    painter.drawText(QRectF{-box.width() / 2.0, -box.height() / 2.0, box.width(), box.height()},
                     Qt::AlignCenter | Qt::TextDontClip, text);
    painter.restore();
}

void paintStamps(QPainter& painter, const QRectF& printableRectPt, const StampOptions& options,
                 const StampContext& context, double deviceScale) {
    // 浮水印先畫、頁首頁尾後畫：兩者重疊時該讓得清楚的是頁碼與 Bates，
    // 那些是要被人抄下來的資訊，浮水印只是背景聲明。
    //
    // 注意這裡的「先畫」仍然在頁面點陣之上——列印路徑拿到的頁面是不透明的
    // 白底點陣，無法把浮水印壓到內容底下。要真正做到內容下方的浮水印
    // 必須寫進文件本身，見 stamp_content_stream.h。
    paintWatermark(painter, printableRectPt, options.watermark, context, deviceScale);

    const QRectF box = stampContentBox(printableRectPt, options.marginsPt);
    if (box.isEmpty()) return;

    for (const StampText& item : options.headerFooter) {
        paintStampText(painter, box, item, expandStampTokens(item.text, context), deviceScale);
    }

    if (options.bates.enabled && !context.batesText.isEmpty()) {
        const QString templ =
            options.batesStyle.text.isEmpty() ? QStringLiteral("<<Bates>>") : options.batesStyle.text;
        paintStampText(painter, box, options.batesStyle, expandStampTokens(templ, context),
                       deviceScale);
    }
}

}  // namespace alioth::app::print
