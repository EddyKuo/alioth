#pragma once

// 把產出的位元組交給**別人**讀回來的共用工具（WBS 12）。
//
// 這一組刻意分成三條互相獨立的讀取路徑：
//
//   1. PDFium 的文字層（Alioth::text）——「使用者看到的東西在哪裡」的唯一可信答案
//   2. 我們自己的物件層解析器（Alioth::objects）——註解字典的幾何，文字層看不到
//   3. qpdf --check——結構是否真的正確，PDFium 的容錯度掩蓋不了它
//
// 只用其中一條都會漏：文字層看不到註解，物件層看不到「PDFium 怎麼解讀我們的
// 矩陣」，而兩者都對的檔案仍然可能有壞掉的 xref。

#include <QString>
#include <QTemporaryDir>

#include <atomic>
#include <string>
#include <vector>

#include "domain/geometry.h"
#include "engine/objects/pdf_parser.h"
#include "engine/objects/pdf_source_document.h"
#include "engine/text/text_extractor.h"
#include "pageops_fixture.h"

namespace alioth::test::pageops {

// 開一份暫存檔並以 PDFium 的文字層讀取，回呼在文字執行緒上執行。
template <typename Fn>
bool withTextPage(const std::string& bytes, const QString& directory, int pageIndex, Fn&& work) {
    const QString path =
        directory + QStringLiteral("/readback-%1.pdf").arg(QString::number(pageIndex));
    if (!writeBytes(path, bytes)) return false;

    engine::text::TextExtractor extractor;
    std::atomic<bool> opened{false};
    extractor.open(path.toStdString(), "", [&opened](domain::DocumentError error) {
        opened = error == domain::DocumentError::None;
    });
    extractor.waitForIdle();
    if (!opened.load()) return false;

    bool visited = false;
    extractor.withTextPage(pageIndex, [&work, &visited](const engine::text::TextPage* page) {
        if (page == nullptr) return;
        visited = true;
        work(*page);
    });
    extractor.waitForIdle();
    return visited;
}

inline std::string pageText(const std::string& bytes, const QString& directory, int pageIndex) {
    std::string text;
    (void)withTextPage(bytes, directory, pageIndex, [&text](const engine::text::TextPage& page) {
        text = engine::text::textForRange(page, engine::text::pageRange(page));
    });
    return text;
}

// 區域內的文字。合併與覆蓋的「位置對不對」就是靠它驗的：
// 文字內容本身在哪一頁都一樣，唯一有意義的問題是它落在版面的哪一格。
inline std::string textInArea(const std::string& bytes, const QString& directory, int pageIndex,
                              const domain::RectF& area) {
    std::string text;
    (void)withTextPage(bytes, directory, pageIndex,
                       [&text, &area](const engine::text::TextPage& page) {
                           text = engine::text::boundedText(page, area);
                       });
    return text;
}

// 註解讀回來的樣子。文字層看不到註解，因此這一條走物件層。
struct AnnotationReadback {
    std::string subtype;
    domain::RectF rect{};
    std::vector<double> quadPoints;
};

inline std::vector<AnnotationReadback> readAnnotations(const std::string& bytes, int pageIndex) {
    std::vector<AnnotationReadback> out;
    engine::objects::PdfSourceDocument document;
    if (document.open(bytes) != engine::objects::SourceStatus::Ok) return out;
    const std::vector<engine::objects::PdfRef>& pages = document.pages();
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pages.size())) return out;

    const engine::objects::PdfObject page =
        document.object(pages[static_cast<std::size_t>(pageIndex)].number);
    const engine::objects::PdfDictionary* dict = page.asDictionary();
    if (dict == nullptr) return out;
    const engine::objects::PdfObject* annots = dict->find("Annots");
    if (annots == nullptr) return out;

    const engine::objects::PdfObject resolved = document.resolve(*annots);
    const engine::objects::PdfArray* array = resolved.asArray();
    if (array == nullptr) return out;

    for (const engine::objects::PdfObject& item : *array) {
        const engine::objects::PdfObject annotation = document.resolve(item);
        const engine::objects::PdfDictionary* annotationDict = annotation.asDictionary();
        if (annotationDict == nullptr) continue;

        AnnotationReadback readback;
        if (const engine::objects::PdfObject* subtype = annotationDict->find("Subtype")) {
            readback.subtype = subtype->asName();
        }
        if (const engine::objects::PdfObject* rectValue = annotationDict->find("Rect")) {
            const engine::objects::PdfObject rectResolved = document.resolve(*rectValue);
            if (const engine::objects::PdfArray* numbers = rectResolved.asArray()) {
                if (numbers->size() >= 4) {
                    readback.rect = domain::RectF{document.resolve((*numbers)[0]).asNumber(),
                                                  document.resolve((*numbers)[1]).asNumber(),
                                                  document.resolve((*numbers)[2]).asNumber(),
                                                  document.resolve((*numbers)[3]).asNumber()}
                                        .normalized();
                }
            }
        }
        if (const engine::objects::PdfObject* quads = annotationDict->find("QuadPoints")) {
            const engine::objects::PdfObject quadsResolved = document.resolve(*quads);
            if (const engine::objects::PdfArray* numbers = quadsResolved.asArray()) {
                for (const engine::objects::PdfObject& value : *numbers) {
                    readback.quadPoints.push_back(document.resolve(value).asNumber());
                }
            }
        }
        out.push_back(std::move(readback));
    }
    return out;
}

// 頁面的某個框。設定文件邊界與正規化都要靠它驗。
inline bool readPageBox(const std::string& bytes, int pageIndex, const std::string& key,
                        domain::RectF& out) {
    engine::objects::PdfSourceDocument document;
    if (document.open(bytes) != engine::objects::SourceStatus::Ok) return false;
    const std::vector<engine::objects::PdfRef>& pages = document.pages();
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pages.size())) return false;

    const engine::objects::PdfRef pageRef = pages[static_cast<std::size_t>(pageIndex)];
    const engine::objects::PdfObject value = document.inheritedPageAttribute(pageRef, key);
    const engine::objects::PdfObject resolved = document.resolve(value);
    const engine::objects::PdfArray* numbers = resolved.asArray();
    if (numbers == nullptr || numbers->size() < 4) return false;
    out = domain::RectF{document.resolve((*numbers)[0]).asNumber(),
                        document.resolve((*numbers)[1]).asNumber(),
                        document.resolve((*numbers)[2]).asNumber(),
                        document.resolve((*numbers)[3]).asNumber()}
              .normalized();
    return true;
}

inline int documentPageCount(const std::string& bytes) {
    engine::objects::PdfSourceDocument document;
    if (document.open(bytes) != engine::objects::SourceStatus::Ok) return -1;
    return static_cast<int>(document.pages().size());
}

// 頁面自己的內容串流（不含 Form XObject 裡面的東西）。
// 「來源頁的運算子有沒有被直接串接進合併頁」就是靠它驗的。
inline std::string readPageContent(const std::string& bytes, int pageIndex) {
    engine::objects::PdfSourceDocument document;
    if (document.open(bytes) != engine::objects::SourceStatus::Ok) return {};
    const std::vector<engine::objects::PdfRef>& pages = document.pages();
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pages.size())) return {};

    const engine::objects::PdfObject page =
        document.object(pages[static_cast<std::size_t>(pageIndex)].number);
    const engine::objects::PdfDictionary* dict = page.asDictionary();
    if (dict == nullptr) return {};
    const engine::objects::PdfObject* contents = dict->find("Contents");
    if (contents == nullptr) return {};

    std::vector<engine::objects::PdfObject> streams;
    const engine::objects::PdfObject resolved = document.resolve(*contents);
    if (const engine::objects::PdfArray* array = resolved.asArray()) {
        for (const engine::objects::PdfObject& item : *array) {
            streams.push_back(document.resolve(item));
        }
    } else {
        streams.push_back(resolved);
    }

    std::string out;
    for (const engine::objects::PdfObject& item : streams) {
        const engine::objects::PdfStream* stream = item.asStream();
        if (stream == nullptr) continue;
        const engine::objects::DecodeResult decoded = engine::objects::decodeStream(
            *stream, [&document](const engine::objects::PdfRef& ref) {
                return document.object(ref.number);
            });
        if (!decoded.ok) continue;
        out += decoded.data;
        out += '\n';
    }
    return out;
}

}  // namespace alioth::test::pageops
