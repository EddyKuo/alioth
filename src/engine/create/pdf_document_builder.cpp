#include "engine/create/pdf_document_builder.h"

#include <QByteArray>

#include <algorithm>
#include <cstdint>
#include <map>

namespace alioth::engine::create {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfStream;

namespace {

std::string padded(std::size_t value, int width) {
    std::string text = std::to_string(value);
    if (static_cast<int>(text.size()) >= width) return text;
    return std::string(static_cast<std::size_t>(width) - text.size(), '0') + text;
}

// 檔案識別碼。內容雜湊而不是亂數：同樣的輸入要產出同樣的位元組，
// 否則測試無法比對，回歸也無從判斷差異是不是預期內的。
std::string fileIdentifier(const std::string& body) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : body) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    // 回傳的是原始位元組而不是十六進位文字：PdfString 的 hex 旗標會自己做
    // 十六進位編碼，先編一次再交給它等於編了兩次，長度會變成 32 位元組。
    std::string raw;
    for (int i = 7; i >= 0; --i) raw.push_back(static_cast<char>((hash >> (i * 8)) & 0xFF));
    return raw + raw;  // 16 位元組
}

}  // namespace

std::string deflateBytes(const std::string& raw) {
    if (raw.empty()) return {};
    // qCompress 在前面加了四個位元組的原始長度，那不是 zlib 串流的一部分；
    // 連著寫進 PDF 的話 FlateDecode 會在第一個位元組就失敗。
    const QByteArray compressed =
        qCompress(QByteArray::fromRawData(raw.data(), static_cast<qsizetype>(raw.size())), 9);
    if (compressed.size() <= 4) return {};
    return std::string(compressed.constData() + 4,
                       static_cast<std::size_t>(compressed.size()) - 4);
}

PdfDocumentBuilder::PdfDocumentBuilder() = default;

int PdfDocumentBuilder::allocateObject() {
    return nextNumber_++;
}

void PdfDocumentBuilder::setObject(int number, PdfObject object) {
    for (PendingObject& pending : objects_) {
        if (pending.number == number) {
            pending.object = std::move(object);
            return;
        }
    }
    objects_.push_back(PendingObject{number, std::move(object)});
}

int PdfDocumentBuilder::addPage(double widthPt, double heightPt, const std::string& content,
                                PdfDictionary resources) {
    const int contentNumber = allocateObject();
    PdfStream stream;
    stream.data = content;
    if (compressContent_ && content.size() > 128) {
        std::string compressed = deflateBytes(content);
        if (!compressed.empty() && compressed.size() < content.size()) {
            stream.data = std::move(compressed);
            stream.dict.set("Filter", objects::makeName("FlateDecode"));
        }
    }
    setObject(contentNumber, PdfObject{std::move(stream)});

    const int pageNumber = allocateObject();
    PdfDictionary page;
    page.set("Type", objects::makeName("Page"));
    page.set("Parent", objects::makeRef(pagesNumber_));
    page.set("MediaBox", objects::makeNumberArray({0.0, 0.0, widthPt, heightPt}));
    page.set("Resources", PdfObject{std::move(resources)});
    page.set("Contents", objects::makeRef(contentNumber));
    setObject(pageNumber, PdfObject{std::move(page)});

    pages_.push_back(pageNumber);
    return pageNumber;
}

DocumentBuildResult PdfDocumentBuilder::build() const {
    DocumentBuildResult result;
    if (pages_.empty()) {
        result.diagnostic = "沒有任何頁面；不產生空文件，因為那對呼叫端沒有意義";
        return result;
    }

    std::map<int, PdfObject> all;

    PdfDictionary catalog;
    catalog.set("Type", objects::makeName("Catalog"));
    catalog.set("Pages", objects::makeRef(pagesNumber_));
    all[catalogNumber_] = PdfObject{std::move(catalog)};

    PdfArray kids;
    for (const int page : pages_) kids.push_back(objects::makeRef(page));
    PdfDictionary pagesRoot;
    pagesRoot.set("Type", objects::makeName("Pages"));
    pagesRoot.set("Kids", PdfObject{std::move(kids)});
    pagesRoot.set("Count", PdfObject{static_cast<std::int64_t>(pages_.size())});
    all[pagesNumber_] = PdfObject{std::move(pagesRoot)};

    for (const PendingObject& pending : objects_) {
        if (pending.number == catalogNumber_ || pending.number == pagesNumber_) {
            result.diagnostic = "物件編號與型錄或頁面樹相撞";
            return result;
        }
        all[pending.number] = pending.object;
    }

    int infoNumber = 0;
    if (!producer_.empty() || !title_.empty()) {
        infoNumber = nextNumber_;
        PdfDictionary info;
        if (!producer_.empty()) info.set("Producer", objects::makeTextString(producer_));
        if (!title_.empty()) info.set("Title", objects::makeTextString(title_));
        all[infoNumber] = PdfObject{std::move(info)};
    }

    // 檔頭第二行的二進位註解：讓把檔案當文字傳輸的工具（FTP ASCII 模式、
    // 部分郵件閘道）判定這是二進位檔而不去改動換行。規格 §7.5.2 建議。
    std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";

    std::map<int, std::size_t> offsets;
    const int maxNumber = all.empty() ? 0 : all.rbegin()->first;
    for (const auto& [number, object] : all) {
        offsets[number] = out.size();
        out += objects::serializeIndirect(number, 0, object);
    }

    const std::size_t xrefOffset = out.size();
    const auto size = static_cast<std::size_t>(maxNumber) + 1;

    out += "xref\n0 " + std::to_string(size) + "\n";
    // 第 0 筆是自由物件鏈的頭，代數固定 65535。少了它整張表往前錯一格，
    // 而多數檢視器會靜默容忍，直到某個照規格讀的工具讀出垃圾。
    out += "0000000000 65535 f \n";
    for (std::size_t number = 1; number < size; ++number) {
        const auto found = offsets.find(static_cast<int>(number));
        if (found == offsets.end()) {
            out += "0000000000 65535 f \n";
        } else {
            out += padded(found->second, 10) + " 00000 n \n";
        }
    }

    PdfDictionary trailer;
    trailer.set("Size", PdfObject{static_cast<std::int64_t>(size)});
    trailer.set("Root", objects::makeRef(catalogNumber_));
    if (infoNumber != 0) trailer.set("Info", objects::makeRef(infoNumber));
    const std::string identifier = fileIdentifier(out);
    PdfArray ids;
    ids.push_back(PdfObject{objects::PdfString{identifier, true}});
    ids.push_back(PdfObject{objects::PdfString{identifier, true}});
    trailer.set("ID", PdfObject{std::move(ids)});

    out += "trailer\n";
    out += objects::serialize(PdfObject{std::move(trailer)});
    out += "\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    result.ok = true;
    result.bytes = std::move(out);
    return result;
}

PdfObject makeStandardFontDictionary(const std::string& baseFont) {
    PdfDictionary font;
    font.set("Type", objects::makeName("Font"));
    font.set("Subtype", objects::makeName("Type1"));
    font.set("BaseFont", objects::makeName(baseFont));
    font.set("Encoding", objects::makeName("WinAnsiEncoding"));
    return PdfObject{std::move(font)};
}

}  // namespace alioth::engine::create
