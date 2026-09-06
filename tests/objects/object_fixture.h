#pragma once

// 物件層寫入的測試語料。
//
// 刻意自產而不依賴外部檔案：這些測試要能在乾淨的機器上跑，而且產生器自己算
// 偏移量與 xref，順便驗證我們對檔案結構的理解與寫入端是一致的。
//
// 這裡同時提供傳統 xref 表與 xref 串流兩種形態的原檔。兩者都必須測到：
// 附加段的形態一旦與原檔不符，產出的檔案在部分解析器上只會看到一半的更新，
// 而我們自己的檢視器（PDFium）容錯度高，不一定看得出來。

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <string>
#include <vector>

namespace alioth::test {

enum class XrefKind {
    Table,
    Stream,             // FlateDecode，無預測器
    StreamPredictor,    // FlateDecode + /Predictor 12，實務上最常見的形態
};

struct PdfFixtureOptions {
    int pageCount{1};
    XrefKind xref{XrefKind::Table};
    // 頁面不帶 /Resources，改由 /Pages 繼承。用來驗證附加字型資源時
    // 不會因為新建 /Resources 而遮蔽繼承來的資源。
    bool inheritResources{false};
    bool encrypted{false};
};

namespace detail {

// PDF 的 FlateDecode 就是 RFC 1950 的 zlib 串流；qCompress 會在前面多加
// 四個位元組的原始長度，那是 Qt 自己的格式，寫進 PDF 前必須去掉。
inline QByteArray zlibCompress(const QByteArray& raw) { return qCompress(raw, 9).mid(4); }

// PNG Up 預測器（/Predictor 12）。每列前面加上濾鏡型別位元組 2，
// 內容是與上一列的逐位元組差值。
inline QByteArray applyUpPredictor(const QByteArray& rows, int columns) {
    QByteArray out;
    QByteArray previous(columns, '\0');
    for (int offset = 0; offset + columns <= rows.size(); offset += columns) {
        out.append(static_cast<char>(2));
        for (int i = 0; i < columns; ++i) {
            const auto current = static_cast<unsigned char>(rows[offset + i]);
            const auto up = static_cast<unsigned char>(previous[i]);
            out.append(static_cast<char>(static_cast<unsigned char>(current - up)));
        }
        previous = rows.mid(offset, columns);
    }
    return out;
}

inline void appendBigEndian(QByteArray& out, quint64 value, int width) {
    for (int i = width - 1; i >= 0; --i) out.append(static_cast<char>((value >> (i * 8)) & 0xFF));
}

}  // namespace detail

// 產生一份最小但結構完整的 PDF。物件配置：
//   1 = Catalog、2 = Pages、之後每頁兩個物件（Page 與 Contents）。
//   encrypted 為真時再多一個 /Encrypt 字典。
inline QByteArray makeFixturePdf(const PdfFixtureOptions& options = {}) {
    const int pageCount = options.pageCount < 1 ? 1 : options.pageCount;

    std::vector<QByteArray> objects;
    QByteArray kids;
    for (int i = 0; i < pageCount; ++i) kids += QByteArray::number(3 + i * 2) + " 0 R ";

    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    QByteArray pages = "<< /Type /Pages /Kids [" + kids.trimmed() + "] /Count " +
                       QByteArray::number(pageCount) + " /MediaBox [0 0 200 400]";
    if (options.inheritResources) {
        // 繼承來的資源刻意放一個可辨識的項目，之後才驗得出它有沒有被遮蔽。
        pages += " /Resources << /ProcSet [/PDF /Text] >>";
    }
    pages += " >>";
    objects.push_back(pages);

    for (int i = 0; i < pageCount; ++i) {
        const QByteArray content = "0 0 0 rg\n0 0 100 200 re\nf\n";
        QByteArray page = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents " +
                          QByteArray::number(4 + i * 2) + " 0 R";
        if (!options.inheritResources) page += " /Resources << /ProcSet [/PDF] >>";
        page += " >>";
        objects.push_back(page);
        objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                          content + "endstream");
    }

    int encryptObject = 0;
    if (options.encrypted) {
        encryptObject = static_cast<int>(objects.size()) + 1;
        // 內容並未真的加密：這份語料只要能讓寫入通道認出 /Encrypt 並拒絕即可。
        objects.push_back("<< /Filter /Standard /V 1 /R 2 /O <00> /U <00> /P -1 >>");
    }

    QByteArray pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<int> offsets;
    offsets.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(static_cast<int>(i) + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const int objectCount = static_cast<int>(objects.size());
    QByteArray trailerExtras = " /Root 1 0 R";
    if (encryptObject != 0) trailerExtras += " /Encrypt " + QByteArray::number(encryptObject) + " 0 R";

    if (options.xref == XrefKind::Table) {
        const int xrefOffset = static_cast<int>(pdf.size());
        pdf += "xref\n0 " + QByteArray::number(objectCount + 1) + "\n";
        pdf += "0000000000 65535 f \n";
        for (const int offset : offsets) {
            pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        }
        pdf += "trailer\n<< /Size " + QByteArray::number(objectCount + 1) + trailerExtras +
               " >>\nstartxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";
        return pdf;
    }

    // xref 串流：它自己也是一個物件，因此要先知道自己的位移才寫得出來。
    const int xrefNumber = objectCount + 1;
    const int xrefOffset = static_cast<int>(pdf.size());
    constexpr int kColumns = 1 + 4 + 2;

    QByteArray rows;
    detail::appendBigEndian(rows, 0, 1);
    detail::appendBigEndian(rows, 0, 4);
    detail::appendBigEndian(rows, 65535, 2);
    for (const int offset : offsets) {
        detail::appendBigEndian(rows, 1, 1);
        detail::appendBigEndian(rows, static_cast<quint64>(offset), 4);
        detail::appendBigEndian(rows, 0, 2);
    }
    detail::appendBigEndian(rows, 1, 1);
    detail::appendBigEndian(rows, static_cast<quint64>(xrefOffset), 4);
    detail::appendBigEndian(rows, 0, 2);

    const bool predictor = options.xref == XrefKind::StreamPredictor;
    const QByteArray payload =
        detail::zlibCompress(predictor ? detail::applyUpPredictor(rows, kColumns) : rows);

    QByteArray dict = "<< /Type /XRef /Size " + QByteArray::number(xrefNumber + 1) + trailerExtras +
                      " /W [1 4 2] /Filter /FlateDecode";
    if (predictor) dict += " /DecodeParms << /Predictor 12 /Columns 7 >>";
    dict += " /Length " + QByteArray::number(payload.size()) + " >>";

    pdf += QByteArray::number(xrefNumber) + " 0 obj\n" + dict + "\nstream\n" + payload +
           "\nendstream\nendobj\n";
    pdf += "startxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

inline std::string toStdString(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

inline QByteArray toByteArray(const std::string& bytes) {
    return QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size()));
}

inline bool writeBytesTo(const QString& path, const std::string& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const qint64 written = file.write(bytes.data(), static_cast<qint64>(bytes.size()));
    file.close();
    return written == static_cast<qint64>(bytes.size());
}

}  // namespace alioth::test
