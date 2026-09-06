#include "engine/objects/incremental_appender.h"

#include <algorithm>
#include <vector>

namespace alioth::engine::objects {

namespace {

// 把連續的物件編號切成 xref 的子段落。不合併不連續的編號是必要的：
// 一段涵蓋了實際上沒寫出來的編號時，那些編號會指到別的物件的位元組。
struct NumberRun {
    int first{0};
    std::vector<int> numbers;
};

[[nodiscard]] std::vector<NumberRun> groupContiguous(const std::vector<int>& sorted) {
    std::vector<NumberRun> runs;
    for (const int number : sorted) {
        if (!runs.empty() && runs.back().numbers.back() + 1 == number) {
            runs.back().numbers.push_back(number);
            continue;
        }
        NumberRun run{};
        run.first = number;
        run.numbers.push_back(number);
        runs.push_back(std::move(run));
    }
    return runs;
}

[[nodiscard]] std::string padded(std::size_t value, int width) {
    std::string text = std::to_string(value);
    while (static_cast<int>(text.size()) < width) text.insert(text.begin(), '0');
    return text;
}

void appendBigEndian(std::string& out, std::uint64_t value, int width) {
    for (int i = width - 1; i >= 0; --i) {
        out += static_cast<char>((value >> (i * 8)) & 0xFF);
    }
}

}  // namespace

SourceStatus IncrementalAppender::open(std::string bytes, std::string* diagnostic) {
    opened_ = false;
    pending_.clear();
    const SourceStatus status = source_.open(std::move(bytes), diagnostic);
    if (status != SourceStatus::Ok) return status;

    // 取號起點取「trailer /Size」與「實際看到的最大編號 + 1」的較大者。
    // 只信 /Size 的話，遇到 /Size 被工具寫小的檔案就會覆蓋既有物件。
    nextNumber_ = static_cast<int>(std::max<std::int64_t>(source_.trailerSize(), 1));
    while (source_.hasObject(nextNumber_)) ++nextNumber_;
    opened_ = true;
    return SourceStatus::Ok;
}

int IncrementalAppender::allocateObject() {
    while (source_.hasObject(nextNumber_) || pending_.count(nextNumber_) != 0) ++nextNumber_;
    const int number = nextNumber_++;
    pending_.emplace(number, Pending{PdfObject{}, 0, true});
    return number;
}

void IncrementalAppender::setObject(int number, PdfObject object) {
    auto it = pending_.find(number);
    if (it == pending_.end()) {
        pending_.emplace(number, Pending{std::move(object), 0, true});
        return;
    }
    it->second.object = std::move(object);
}

bool IncrementalAppender::updateObject(int number, PdfObject object) {
    if (!source_.hasObject(number)) return false;
    Pending pending{};
    pending.object = std::move(object);
    pending.generation = source_.generationOf(number);
    pending.isNew = false;
    pending_[number] = std::move(pending);
    return true;
}

PdfObject IncrementalAppender::currentObject(int number) const {
    const auto it = pending_.find(number);
    if (it != pending_.end()) return it->second.object;
    return source_.object(number);
}

PdfDictionary IncrementalAppender::buildTrailerDictionary(std::int64_t newSize) const {
    PdfDictionary trailer;
    // /Root 與 /Info 必須沿用原檔；/ID 保留讓增量段仍屬於同一份文件。
    for (const auto& entry : source_.trailer().entries()) {
        if (entry.first == "Size" || entry.first == "Prev" || entry.first == "XRefStm") continue;
        trailer.set(entry.first, entry.second);
    }
    trailer.set("Size", PdfObject{newSize});
    trailer.set("Prev", PdfObject{static_cast<std::int64_t>(source_.lastXrefOffset())});
    return trailer;
}

std::string IncrementalAppender::buildXrefTable(const std::map<int, std::size_t>& offsets,
                                                std::int64_t newSize,
                                                std::size_t xrefOffset) const {
    std::vector<int> numbers;
    numbers.reserve(offsets.size());
    for (const auto& entry : offsets) numbers.push_back(entry.first);

    std::string out = "xref\n";
    for (const NumberRun& run : groupContiguous(numbers)) {
        out += std::to_string(run.first);
        out += ' ';
        out += std::to_string(run.numbers.size());
        out += '\n';
        for (const int number : run.numbers) {
            const auto pending = pending_.find(number);
            const int generation = pending == pending_.end() ? 0 : pending->second.generation;
            // 每筆恰好 20 位元組（§7.5.4）。長度不對的話所有後續項目都會錯位，
            // 而多數解析器不會報錯，只會讀到垃圾。
            out += padded(offsets.at(number), 10);
            out += ' ';
            out += padded(static_cast<std::size_t>(generation), 5);
            out += " n \n";
        }
    }

    PdfDictionary trailer = buildTrailerDictionary(newSize);
    out += "trailer\n";
    out += serialize(PdfObject{std::move(trailer)});
    out += "\nstartxref\n";
    out += std::to_string(xrefOffset);
    out += "\n%%EOF\n";
    return out;
}

BuildResult IncrementalAppender::build() const {
    BuildResult result{};
    if (!opened_) {
        result.diagnostic = "尚未開啟原檔";
        return result;
    }
    if (pending_.empty()) {
        result.diagnostic = "沒有要附加的物件";
        return result;
    }

    // 取號正確性的最後一道關卡：新編號不得落在原檔已存在的編號上。
    for (const auto& entry : pending_) {
        if (entry.second.isNew && source_.hasObject(entry.first)) {
            result.diagnostic = "新物件編號 " + std::to_string(entry.first) + " 與既有物件相撞";
            return result;
        }
        if (!entry.second.isNew && !source_.hasObject(entry.first)) {
            result.diagnostic = "要更新的物件 " + std::to_string(entry.first) + " 不存在於原檔";
            return result;
        }
    }

    const std::string& original = source_.bytes();
    std::string out = original;
    // 原檔結尾多半是「%%EOF\n」，但不保證。附加段必須從新的一行開始，
    // 否則第一個物件編號會與 %%EOF 黏在一起變成無法解析的 token。
    if (!out.empty() && out.back() != '\n' && out.back() != '\r') out += '\n';

    std::map<int, std::size_t> offsets;
    for (const auto& entry : pending_) {
        offsets[entry.first] = out.size();
        out += serializeIndirect(entry.first, entry.second.generation, entry.second.object);
    }

    std::int64_t newSize = source_.trailerSize();
    for (const auto& entry : pending_) {
        newSize = std::max<std::int64_t>(newSize, entry.first + 1);
    }

    if (source_.style() == XrefStyle::Table) {
        const std::size_t xrefOffset = out.size();
        out += buildXrefTable(offsets, newSize, xrefOffset);
    } else {
        // xref 串流本身也是一個物件，必須被自己涵蓋，因此要先配號再算位移。
        int xrefNumber = nextNumber_;
        while (source_.hasObject(xrefNumber) || pending_.count(xrefNumber) != 0) ++xrefNumber;
        const std::size_t xrefOffset = out.size();
        offsets[xrefNumber] = xrefOffset;
        newSize = std::max<std::int64_t>(newSize, xrefNumber + 1);

        std::size_t maxOffset = 0;
        for (const auto& entry : offsets) maxOffset = std::max(maxOffset, entry.second);
        int offsetWidth = 4;
        while (offsetWidth < 8 && (maxOffset >> (offsetWidth * 8)) != 0) ++offsetWidth;

        std::vector<int> numbers;
        numbers.reserve(offsets.size());
        for (const auto& entry : offsets) numbers.push_back(entry.first);

        PdfArray index;
        std::string data;
        for (const NumberRun& run : groupContiguous(numbers)) {
            index.emplace_back(static_cast<std::int64_t>(run.first));
            index.emplace_back(static_cast<std::int64_t>(run.numbers.size()));
            for (const int number : run.numbers) {
                const auto pending = pending_.find(number);
                const int generation = pending == pending_.end() ? 0 : pending->second.generation;
                appendBigEndian(data, 1, 1);
                appendBigEndian(data, offsets.at(number), offsetWidth);
                appendBigEndian(data, static_cast<std::uint64_t>(generation), 2);
            }
        }

        PdfDictionary dict = buildTrailerDictionary(newSize);
        dict.set("Type", makeName("XRef"));
        dict.set("W", PdfArray{PdfObject{static_cast<std::int64_t>(1)},
                               PdfObject{static_cast<std::int64_t>(offsetWidth)},
                               PdfObject{static_cast<std::int64_t>(2)}});
        dict.set("Index", PdfObject{std::move(index)});
        // 刻意不壓縮：省下的位元組在增量段裡微不足道，而少一個濾鏡
        // 就少一種「我們自己寫出來卻讀不回去」的失敗模式。
        dict.remove("Filter");
        dict.remove("DecodeParms");

        out += serializeIndirect(xrefNumber, 0, PdfObject{PdfStream{std::move(dict), std::move(data)}});
        out += "startxref\n";
        out += std::to_string(xrefOffset);
        out += "\n%%EOF\n";
    }

    // 簽章保全的核心承諾，因此在這裡實際驗證而不是相信上面的程式碼。
    if (out.size() < original.size() || out.compare(0, original.size(), original) != 0) {
        result.diagnostic = "輸出前綴與原檔不符，已中止";
        return result;
    }

    result.ok = true;
    result.appendedBytes = static_cast<std::uint64_t>(out.size() - original.size());
    result.bytes = std::move(out);
    return result;
}

}  // namespace alioth::engine::objects
