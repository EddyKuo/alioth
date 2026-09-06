#include "app/print/stamp_layout.h"

#include <QLocale>

namespace alioth::app::print {

QString expandStampTokens(const QString& templ, const StampContext& context) {
    if (templ.isEmpty()) return templ;

    const QDateTime stamp = context.timestamp.isValid() ? context.timestamp
                                                        : QDateTime::currentDateTime();
    QString out = templ;
    const auto replace = [&out](const char* token, const QString& value) {
        out.replace(QLatin1String(token), value);
    };

    replace("<<Page>>", QString::number(context.pageNumber));
    replace("<<Pages>>", QString::number(context.pageCount));
    replace("<<Sequence>>", QString::number(context.printSequence));
    replace("<<Sheet>>", QString::number(context.sheetNumber));
    replace("<<Bates>>", context.batesText);
    replace("<<FileName>>", context.fileName);
    // 用 ISO 日期而非地區格式：頁首頁尾常被拿去做檔案比對與稽核，
    // 同一份文件在不同地區設定的機器上印出不同字串會讓比對失效。
    replace("<<Date>>", stamp.date().toString(Qt::ISODate));
    replace("<<Time>>", stamp.time().toString(QStringLiteral("HH:mm:ss")));
    return out;
}

QRectF stampContentBox(const QRectF& printableRectPt, const StampMargins& margins) {
    const double left = printableRectPt.left() + margins.left;
    const double top = printableRectPt.top() + margins.top;
    const double right = printableRectPt.right() - margins.right;
    const double bottom = printableRectPt.bottom() - margins.bottom;
    if (right <= left || bottom <= top) return QRectF{};
    return QRectF{QPointF{left, top}, QPointF{right, bottom}};
}

QRectF placeStamp(const QRectF& box, const QSizeF& itemSize, StampAnchor anchor) {
    if (box.isEmpty()) return QRectF{};

    double x = box.left();
    double y = box.top();
    const double slackX = box.width() - itemSize.width();
    const double slackY = box.height() - itemSize.height();

    switch (anchor) {
        case StampAnchor::TopLeft:
        case StampAnchor::MiddleLeft:
        case StampAnchor::BottomLeft:
            break;
        case StampAnchor::TopCenter:
        case StampAnchor::Center:
        case StampAnchor::BottomCenter:
            x += slackX / 2.0;
            break;
        case StampAnchor::TopRight:
        case StampAnchor::MiddleRight:
        case StampAnchor::BottomRight:
            x += slackX;
            break;
    }

    switch (anchor) {
        case StampAnchor::TopLeft:
        case StampAnchor::TopCenter:
        case StampAnchor::TopRight:
            break;
        case StampAnchor::MiddleLeft:
        case StampAnchor::Center:
        case StampAnchor::MiddleRight:
            y += slackY / 2.0;
            break;
        case StampAnchor::BottomLeft:
        case StampAnchor::BottomCenter:
        case StampAnchor::BottomRight:
            y += slackY;
            break;
    }

    // 內容比格子大時不往外溢：戳記跑到可列印區之外等於整條資訊消失，
    // 而被裁掉一半至少還看得出來出了問題。
    return QRectF{QPointF{x, y}, itemSize};
}

}  // namespace alioth::app::print
