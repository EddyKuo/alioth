#include "engine/signature/byte_range.h"

#include <algorithm>
#include <limits>

namespace alioth::engine::signature {

const char* describe(ByteRangeVerdict verdict) noexcept {
    switch (verdict) {
        case ByteRangeVerdict::Valid:              return "涵蓋完整";
        case ByteRangeVerdict::Malformed:          return "格式錯誤";
        case ByteRangeVerdict::OutOfBounds:        return "超出檔案範圍";
        case ByteRangeVerdict::Overlapping:        return "區段重疊";
        case ByteRangeVerdict::IncompleteCoverage: return "只涵蓋部分檔案";
    }
    return "未知";
}

ByteRangeCheck checkByteRange(const std::vector<int>& raw, std::int64_t fileSize) {
    ByteRangeCheck check;
    check.fileSize = fileSize;

    if (raw.empty() || raw.size() % 2 != 0) {
        check.verdict = ByteRangeVerdict::Malformed;
        check.detail = "/ByteRange 的元素數量必須是成對的偶數，實際為 " +
                       std::to_string(raw.size()) + " 個。";
        return check;
    }
    if (fileSize <= 0) {
        check.verdict = ByteRangeVerdict::Malformed;
        check.detail = "檔案長度未知或為零，無法驗證 /ByteRange。";
        return check;
    }

    check.segments.reserve(raw.size() / 2);
    for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
        ByteRangeSegment segment{static_cast<std::int64_t>(raw[i]),
                                 static_cast<std::int64_t>(raw[i + 1])};
        if (segment.offset < 0 || segment.length < 0) {
            check.verdict = ByteRangeVerdict::Malformed;
            check.detail = "/ByteRange 含負數（位移 " + std::to_string(segment.offset) +
                           "、長度 " + std::to_string(segment.length) + "）。";
            return check;
        }
        check.segments.push_back(segment);
    }

    std::int64_t cursor = 0;
    for (const ByteRangeSegment& segment : check.segments) {
        // 溢位在這裡是真實風險：/ByteRange 的值來自不可信任的檔案，
        // 兩個接近 INT_MAX 的數字相加會回繞成負數，越界檢查就會被繞過。
        if (segment.length > std::numeric_limits<std::int64_t>::max() - segment.offset) {
            check.verdict = ByteRangeVerdict::Malformed;
            check.detail = "/ByteRange 的位移與長度相加溢位。";
            return check;
        }
        if (segment.end() > fileSize) {
            check.verdict = ByteRangeVerdict::OutOfBounds;
            check.detail = "/ByteRange 第 " + std::to_string(segment.offset) + " 起 " +
                           std::to_string(segment.length) + " 位元組的區段超出檔案尾端（檔案共 " +
                           std::to_string(fileSize) + " 位元組）。";
            return check;
        }
        if (segment.offset < cursor) {
            check.verdict = ByteRangeVerdict::Overlapping;
            check.detail = "/ByteRange 的區段彼此重疊或未依位移遞增，位移 " +
                           std::to_string(segment.offset) + " 落在前一段的範圍內。";
            return check;
        }
        if (segment.offset > cursor) {
            check.gaps.push_back(ByteRangeSegment{cursor, segment.offset - cursor});
        }
        cursor = segment.end();
        check.coveredBytes += segment.length;
    }

    if (cursor < fileSize) {
        check.gaps.push_back(ByteRangeSegment{cursor, fileSize - cursor});
    }

    if (!check.segments.empty() && check.segments.front().offset != 0) {
        check.verdict = ByteRangeVerdict::IncompleteCoverage;
        check.detail = "/ByteRange 未從檔案開頭起算，前 " +
                       std::to_string(check.segments.front().offset) + " 位元組未被簽章涵蓋。";
        return check;
    }

    // 正常的 detached 簽章只會留一個洞，就是 /Contents 十六進位字串本身。
    // 多於一個洞，或洞在檔案尾端，都代表有內容被排除在簽章之外。
    if (check.gaps.size() > 1 || (check.gaps.size() == 1 && check.gaps.front().end() == fileSize)) {
        check.verdict = ByteRangeVerdict::IncompleteCoverage;
        check.detail = "簽章只涵蓋部分檔案：共 " + std::to_string(check.uncoveredBytes()) +
                       " 位元組（" + std::to_string(check.gaps.size()) +
                       " 個區段）不在 /ByteRange 內，這些位元組可以被任意竄改而不影響驗證結果。";
        return check;
    }

    check.verdict = ByteRangeVerdict::Valid;
    check.detail = "/ByteRange 涵蓋整份檔案，僅保留簽章值本身的位置。";
    return check;
}

ByteRangeCheck coverageOfWholeInput(std::int64_t size) {
    ByteRangeCheck check;
    check.fileSize = size;
    if (size <= 0) {
        check.verdict = ByteRangeVerdict::Malformed;
        check.detail = "輸入為空，沒有可驗證的位元組。";
        return check;
    }
    check.verdict = ByteRangeVerdict::Valid;
    check.segments.push_back(ByteRangeSegment{0, size});
    check.coveredBytes = size;
    check.detail = "呼叫端保證這段位元組即為簽章涵蓋範圍。";
    return check;
}

std::vector<std::uint8_t> assembleSignedBytes(const std::uint8_t* data, std::size_t size,
                                              const ByteRangeCheck& check) {
    std::vector<std::uint8_t> out;
    if (!data) return out;
    // 只有語法合法的區段才有「被簽的內容」可言。重疊或越界時串出來的位元組
    // 沒有任何意義，卻會產生一個看似能驗的摘要——那正是最危險的結果。
    if (check.verdict == ByteRangeVerdict::Malformed ||
        check.verdict == ByteRangeVerdict::OutOfBounds ||
        check.verdict == ByteRangeVerdict::Overlapping) {
        return out;
    }

    out.reserve(static_cast<std::size_t>(std::max<std::int64_t>(check.coveredBytes, 0)));
    for (const ByteRangeSegment& segment : check.segments) {
        if (segment.offset < 0 || segment.length <= 0) continue;
        const auto begin = static_cast<std::size_t>(segment.offset);
        const auto count = static_cast<std::size_t>(segment.length);
        if (begin > size || count > size - begin) return {};
        out.insert(out.end(), data + begin, data + begin + count);
    }
    return out;
}

}  // namespace alioth::engine::signature
