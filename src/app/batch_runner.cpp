#include "app/batch_runner.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "domain/page_operations.h"
#include "engine/bookmarks/bookmark_document.h"
#include "engine/enhance/barcode_stamp.h"
#include "engine/enhance/color_transform.h"
#include "engine/objects/incremental_appender.h"
#include "engine/pages/page_editor.h"
#include "engine/save/incremental_saver.h"

namespace alioth::app {

namespace {

[[nodiscard]] std::string readAllBytes(const std::string& path, bool* exists) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (exists != nullptr) *exists = false;
        return {};
    }
    if (exists != nullptr) *exists = true;
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

[[nodiscard]] bool writeAllBytes(const std::string& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    return static_cast<bool>(file);
}

[[nodiscard]] std::string describePages(const std::vector<std::int32_t>& pages) {
    if (pages.empty()) return "全部頁面";
    std::ostringstream out;
    out << "頁面 ";
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (i > 0) out << ",";
        out << (pages[i] + 1);  // 給使用者看的訊息用 1 起算
    }
    return out.str();
}

[[nodiscard]] std::string describeStep(const domain::macro::MacroAction& step) {
    switch (step.kind) {
        case domain::macro::MacroActionKind::RotatePages:
            return "旋轉 " + describePages(step.pages) + " " +
                  std::to_string(step.rotationDegrees) + " 度";
        case domain::macro::MacroActionKind::ConvertColor: {
            std::string mode;
            switch (step.colorSettings.mode) {
                case domain::enhance::ColorTransformMode::Grayscale: mode = "灰階"; break;
                case domain::enhance::ColorTransformMode::Desaturate: mode = "去飽和"; break;
                case domain::enhance::ColorTransformMode::ReplaceColor: mode = "換色"; break;
            }
            return "色彩轉換（" + mode + "）於 " + describePages(step.pages);
        }
        case domain::macro::MacroActionKind::AddBarcodeStamp:
            return "貼上條碼「" + step.barcodeText + "」於 " + describePages(step.pages);
        case domain::macro::MacroActionKind::BookmarkAddAffix:
            return "書籤標題加前綴／後綴（\"" + step.bookmarkAffix.prefix + "\" / \"" +
                  step.bookmarkAffix.suffix + "\"）";
        case domain::macro::MacroActionKind::BookmarkEveryNPages:
            return "每 " + std::to_string(step.bookmarkInterval) + " 頁加書籤";
        case domain::macro::MacroActionKind::BookmarkConvertCase:
            return "書籤標題大小寫轉換";
        case domain::macro::MacroActionKind::BookmarkFindReplace:
            return "書籤標題尋找取代（\"" + step.bookmarkFindReplace.find + "\" -> \"" +
                  step.bookmarkFindReplace.replace + "\"）";
        case domain::macro::MacroActionKind::BookmarkMergeDuplicates:
            return "合併重複書籤";
    }
    return "未知動作";
}

// 對單一檔案套用一個 RotatePages 動作：需要 PageEditor（PDFium 把手），
// 因此走「寫暫存檔 → open → 操作 → saveAsCopy → 讀回」，與其他兩種動作
// （純附加、不需要 PDFium）分開處理，理由見標頭註解。
[[nodiscard]] bool applyRotate(std::string& currentBytes, const domain::macro::MacroAction& step,
                               std::string& diagnostic) {
    std::error_code ec;
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
    const std::filesystem::path tempIn =
        tempDir / ("alioth-batch-in-" + std::to_string(reinterpret_cast<std::uintptr_t>(&step)) + ".pdf");
    const std::filesystem::path tempOut =
        tempDir / ("alioth-batch-out-" + std::to_string(reinterpret_cast<std::uintptr_t>(&step)) + ".pdf");

    if (!writeAllBytes(tempIn.string(), currentBytes)) {
        diagnostic = "無法建立暫存檔";
        return false;
    }

    engine::pages::PageEditor editor;
    bool success = false;
    if (editor.open(tempIn.string())) {
        const domain::pages::PageRotation rotation =
            domain::pages::rotationFromQuarterTurns(step.rotationDegrees / 90);
        // domain::pages::validate 不把空陣列當成「全部頁面」（那是
        // ColorTransformSettings／barcode 動作的慣例，PageEditor 的頁面操作
        // 沒有這個慣例），因此空陣列在這裡要自己展開成完整頁碼清單。
        std::vector<int> targetPages(step.pages.begin(), step.pages.end());
        if (targetPages.empty()) {
            const int pageCount = editor.pageCount();
            targetPages.resize(static_cast<std::size_t>(pageCount));
            for (int i = 0; i < pageCount; ++i) targetPages[static_cast<std::size_t>(i)] = i;
        }
        const engine::pages::PageEditResult applied =
            editor.rotatePages(targetPages, rotation, /*relative=*/true);
        if (applied.ok()) {
            const engine::pages::PageSaveResult saved = editor.saveAsCopy(tempOut.string());
            if (saved.ok()) {
                bool exists = false;
                currentBytes = readAllBytes(tempOut.string(), &exists);
                success = exists;
                if (!success) diagnostic = "旋轉後讀回暫存檔失敗";
            } else {
                diagnostic = "旋轉後存檔失敗：" + saved.save.message;
            }
        } else {
            diagnostic = std::string("旋轉失敗：") + engine::pages::describe(applied.status);
        }
    } else {
        diagnostic = "無法開啟暫存檔（可能是加密文件或損毀的 PDF）";
    }

    std::filesystem::remove(tempIn, ec);
    std::filesystem::remove(tempOut, ec);
    return success;
}

[[nodiscard]] bool applyConvertColor(std::string& currentBytes,
                                     const domain::macro::MacroAction& step,
                                     std::string& diagnostic) {
    engine::objects::IncrementalAppender appender;
    std::string openDiagnostic;
    if (appender.open(currentBytes, &openDiagnostic) != engine::objects::SourceStatus::Ok) {
        diagnostic = "無法開啟文件：" + openDiagnostic;
        return false;
    }
    domain::enhance::ColorTransformSettings settings = step.colorSettings;
    settings.pages = step.pages;
    const engine::enhance::ColorTransformResult result =
        engine::enhance::convertColors(appender, settings);
    if (!result.ok) {
        diagnostic = "色彩轉換失敗：" + result.diagnostic;
        return false;
    }
    const engine::objects::BuildResult built = appender.build();
    if (!built.ok) {
        diagnostic = "色彩轉換後產生檔案失敗：" + built.diagnostic;
        return false;
    }
    currentBytes = built.bytes;
    return true;
}

[[nodiscard]] bool applyAddBarcodeStamp(std::string& currentBytes,
                                       const domain::macro::MacroAction& step,
                                       std::string& diagnostic) {
    engine::objects::IncrementalAppender appender;
    std::string openDiagnostic;
    if (appender.open(currentBytes, &openDiagnostic) != engine::objects::SourceStatus::Ok) {
        diagnostic = "無法開啟文件：" + openDiagnostic;
        return false;
    }
    const std::size_t pageCount = appender.source().pages().size();
    std::vector<std::int32_t> targetPages = step.pages;
    if (targetPages.empty()) {
        targetPages.resize(pageCount);
        for (std::size_t i = 0; i < pageCount; ++i) targetPages[i] = static_cast<std::int32_t>(i);
    }
    for (const std::int32_t pageIndex : targetPages) {
        const engine::enhance::BarcodeStampResult stamped =
            engine::enhance::stampBarcode(appender, pageIndex, step.barcodeText, step.barcodeRect);
        if (!stamped.ok) {
            diagnostic = "貼上條碼失敗（頁 " + std::to_string(pageIndex + 1) + "）：" + stamped.diagnostic;
            return false;
        }
    }
    const engine::objects::BuildResult built = appender.build();
    if (!built.ok) {
        diagnostic = "貼上條碼後產生檔案失敗：" + built.diagnostic;
        return false;
    }
    currentBytes = built.bytes;
    return true;
}

// 五種書籤巨集動作共用同一個骨架：開檔讀出書籤樹 → 用 bookmark_ops.h 既有的
// 純函數改樹 → commitOutline() 整棵重寫 → build()。這裡只負責分派到哪個
// bookmark_ops 函式，實際的樹狀邏輯（大小寫、尋找取代……）在 domain 層已經
// 測過，這裡不重新驗證那些細節，只驗證「巨集接得上」。
[[nodiscard]] bool applyBookmarkMacro(std::string& currentBytes,
                                      const domain::macro::MacroAction& step,
                                      std::string& diagnostic) {
    engine::bookmarks::BookmarkDocument document;
    std::string openDiagnostic;
    if (document.open(currentBytes, &openDiagnostic) != engine::objects::SourceStatus::Ok) {
        diagnostic = "無法開啟文件：" + openDiagnostic;
        return false;
    }

    switch (step.kind) {
        case domain::macro::MacroActionKind::BookmarkAddAffix:
            domain::bookmarks::applyAffix(document.tree(), step.bookmarkAffix);
            break;
        case domain::macro::MacroActionKind::BookmarkEveryNPages: {
            domain::bookmarks::EveryNPagesOptions options;
            options.pageCount = document.pageCount();
            options.interval = step.bookmarkInterval;
            options.firstPage = step.bookmarkFirstPage;
            options.pageLabelOffset = step.bookmarkPageLabelOffset;
            options.titlePattern = step.bookmarkTitlePattern;
            domain::bookmarks::BookmarkTree generated =
                domain::bookmarks::generateEveryNPages(options);
            // 附加到既有書籤之後，不是取代——使用者原本手動整理的書籤不該
            // 因為跑了一次「每 N 頁加書籤」就整棵消失。
            document.tree().insert(document.tree().end(),
                                   std::make_move_iterator(generated.begin()),
                                   std::make_move_iterator(generated.end()));
            break;
        }
        case domain::macro::MacroActionKind::BookmarkConvertCase:
            domain::bookmarks::applyCase(document.tree(), step.bookmarkCaseMode);
            break;
        case domain::macro::MacroActionKind::BookmarkFindReplace:
            domain::bookmarks::findReplaceTitles(document.tree(), step.bookmarkFindReplace);
            break;
        case domain::macro::MacroActionKind::BookmarkMergeDuplicates:
            domain::bookmarks::mergeDuplicates(document.tree(), step.bookmarkMergeDuplicates);
            break;
        default:
            diagnostic = "內部錯誤：非書籤動作誤入 applyBookmarkMacro";
            return false;
    }

    const engine::bookmarks::OutlineWriteResult written = document.commitOutline();
    if (!written.ok) {
        diagnostic = "寫回書籤失敗：" + written.diagnostic;
        return false;
    }

    const engine::objects::BuildResult built = document.build();
    if (!built.ok) {
        diagnostic = "書籤巨集後產生檔案失敗：" + built.diagnostic;
        return false;
    }
    currentBytes = built.bytes;
    return true;
}

}  // namespace

std::string BatchRunner::formatOutputPath(const std::string& inputPath, const std::string& pattern) {
    const std::filesystem::path input(inputPath);
    const std::string stem = input.stem().string();
    const std::string ext = input.extension().string();

    std::string name;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern.compare(i, 6, "{name}") == 0) { name += stem; i += 6; continue; }
        if (pattern.compare(i, 5, "{ext}") == 0) { name += ext; i += 5; continue; }
        name.push_back(pattern[i]);
        ++i;
    }
    const std::filesystem::path output = input.parent_path() / name;
    return output.string();
}

BatchRunResult BatchRunner::run(const domain::macro::MacroDefinition& macro,
                                const std::vector<std::string>& inputPaths,
                                const BatchRunOptions& options) const {
    BatchRunResult result;

    const std::string macroProblem = macro.validate();
    if (!macroProblem.empty()) {
        result.diagnostic = "巨集不合法：" + macroProblem;
        return result;
    }

    for (const std::string& inputPath : inputPaths) {
        BatchFileResult fileResult;
        fileResult.inputPath = inputPath;
        fileResult.outputPath = formatOutputPath(inputPath, options.outputPattern);
        for (const domain::macro::MacroAction& step : macro.steps) {
            fileResult.stepDescriptions.push_back(describeStep(step));
        }

        // 用 std::filesystem::path 比較而非原始字串：輸入路徑可能來自不同
        // 呼叫端（例如 Qt 一律用 "/"），字串比較會把「同一個檔案、分隔符
        // 寫法不同」誤判成兩個不同路徑，讓本該擋下的覆蓋逃過檢查。
        if (std::filesystem::path(fileResult.outputPath) == std::filesystem::path(inputPath)) {
            fileResult.diagnostic = "輸出路徑與輸入路徑相同，為了不覆蓋原檔已拒絕執行；請調整 outputPattern";
            result.files.push_back(std::move(fileResult));
            continue;
        }

        if (options.dryRun) {
            // dry-run 只描述計畫、不觸碰檔案系統，連「輸入檔案存不存在」都不檢查——
            // 那個檢查本身也是一種 I/O，dry-run 的合約是「保證不碰檔案」，
            // 用一個例外換取使用者更少的信任成本並不值得。
            fileResult.ok = true;
            result.files.push_back(std::move(fileResult));
            continue;
        }

        bool exists = false;
        std::string currentBytes = readAllBytes(inputPath, &exists);
        if (!exists) {
            fileResult.diagnostic = "找不到輸入檔案";
            result.files.push_back(std::move(fileResult));
            continue;
        }

        bool stepOk = true;
        std::string stepDiagnostic;
        for (const domain::macro::MacroAction& step : macro.steps) {
            switch (step.kind) {
                case domain::macro::MacroActionKind::RotatePages:
                    stepOk = applyRotate(currentBytes, step, stepDiagnostic);
                    break;
                case domain::macro::MacroActionKind::ConvertColor:
                    stepOk = applyConvertColor(currentBytes, step, stepDiagnostic);
                    break;
                case domain::macro::MacroActionKind::AddBarcodeStamp:
                    stepOk = applyAddBarcodeStamp(currentBytes, step, stepDiagnostic);
                    break;
                case domain::macro::MacroActionKind::BookmarkAddAffix:
                case domain::macro::MacroActionKind::BookmarkEveryNPages:
                case domain::macro::MacroActionKind::BookmarkConvertCase:
                case domain::macro::MacroActionKind::BookmarkFindReplace:
                case domain::macro::MacroActionKind::BookmarkMergeDuplicates:
                    stepOk = applyBookmarkMacro(currentBytes, step, stepDiagnostic);
                    break;
            }
            if (!stepOk) break;
        }

        if (!stepOk) {
            fileResult.diagnostic = stepDiagnostic;
            result.files.push_back(std::move(fileResult));
            continue;
        }

        if (!writeAllBytes(fileResult.outputPath, currentBytes)) {
            fileResult.diagnostic = "寫出輸出檔案失敗";
            result.files.push_back(std::move(fileResult));
            continue;
        }

        fileResult.ok = true;
        result.files.push_back(std::move(fileResult));
    }

    result.ok = true;  // 巨集本身合法；個別檔案的成敗看 files[i].ok
    return result;
}

}  // namespace alioth::app
