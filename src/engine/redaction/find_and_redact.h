#pragma once

// Find and Redact（PRD-ANN-033，WBS 11）。
//
// 這一層**只產生標記**，不套用。這不是實作進度問題而是需求本身：
// 搜尋命中會有誤判（同一組數字既可能是身分證字號也可能是零件編號），
// 直接刪掉就沒有回頭路。因此流程一律是「搜尋 → 標記 → 人工檢視 → 套用」，
// 中間那一步在型別上由 domain::IrreversibleConsent 強制。
//
// 命中範圍取自 PDFium 的字元外框（TextExtractor），與塗黑套用時自行重放
// 內容串流所算出來的外框是兩條獨立的路徑。這是刻意的：兩者若共用同一份
// 幾何推導，推導錯了也不會有人發現。

#include <optional>
#include <string>

#include "domain/annotation.h"
#include "domain/redaction.h"

namespace alioth::engine::redaction {

struct FindRedactOptions {
    bool matchCase{false};
    bool matchWholeWord{false};

    // 命中外框往外擴張的點數。字元外框緊貼字身，完全不擴張時字的描邊
    // 會露在覆蓋矩形外面，看起來像沒塗乾淨。
    double padding{1.0};

    domain::ColorRgb fillColor{0.0, 0.0, 0.0};
    std::string author{};
    std::string subject{};
    std::string note{};
    std::optional<std::string> overlayText{};

    // 單次操作的命中上限。搜尋「a」這種查詢在大文件上會產生數十萬個標記，
    // 那既不是使用者的本意，也會讓後續套用的成本失控。
    int maxMarks{20000};
};

struct FindRedactResult {
    bool ok{false};
    std::string diagnostic{};
    domain::RedactionMarkSet marks{};
    int matchCount{0};
    bool truncated{false};  // 因 maxMarks 而提早停止
};

// 對整份文件搜尋並產生標記。
//
// 同步阻塞：搜尋與標記是一次批次操作，呼叫端拿到全部結果才有東西可以檢視。
// 內部仍走 TextExtractor 自己的執行緒與獨立文件把手（CLAUDE.md 硬性限制 1）。
[[nodiscard]] FindRedactResult findAndMarkRedactions(const std::string& path,
                                                     const std::string& password,
                                                     const std::string& queryUtf8,
                                                     const FindRedactOptions& options = {});

}  // namespace alioth::engine::redaction
