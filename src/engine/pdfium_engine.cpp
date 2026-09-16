#include "engine/pdfium_engine.h"

#include "engine/file_source.h"
#include "engine/pdfium_library.h"
#include "engine/pdfium_lock.h"

#include <fpdfview.h>
#include <fpdf_doc.h>
#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_signature.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <unordered_map>
#include <utility>

namespace alioth::engine {
namespace {

using domain::TaskPriority;

domain::DocumentError translateLoadError() {
    switch (FPDF_GetLastError()) {
        case FPDF_ERR_SUCCESS:  return domain::DocumentError::None;
        case FPDF_ERR_FILE:     return domain::DocumentError::FileNotFound;
        case FPDF_ERR_FORMAT:   return domain::DocumentError::NotAPdf;
        case FPDF_ERR_PASSWORD: return domain::DocumentError::WrongPassword;
        case FPDF_ERR_SECURITY: return domain::DocumentError::UnsupportedFeature;
        case FPDF_ERR_PAGE:     return domain::DocumentError::CorruptXref;
        default:                return domain::DocumentError::Unknown;
    }
}

// 依旋轉角度組出「頁面顯示空間 → 圖磚空間」的矩陣。
//
// PDFium 顯示空間 Y 軸向下、原點在頁面左上，與領域層的 PDF 座標（Y 向上）不同。
// 這裡刻意只處理顯示空間，頁面座標的 Y 翻轉統一由 domain::PageTransform 負責，
// 兩邊都做會得到看似正確、實際上下顛倒的結果。
FS_MATRIX tileMatrix(double pageWidthPt, double pageHeightPt, double scale,
                     domain::Rotation rotation, double tileX, double tileY) {
    const auto s = static_cast<float>(scale);
    const auto w = static_cast<float>(pageWidthPt * scale);
    const auto h = static_cast<float>(pageHeightPt * scale);
    FS_MATRIX m{s, 0.0f, 0.0f, s, 0.0f, 0.0f};
    switch (rotation) {
        case domain::Rotation::None:
            break;
        case domain::Rotation::Cw90:
            m = {0.0f, s, -s, 0.0f, h, 0.0f};
            break;
        case domain::Rotation::Cw180:
            m = {-s, 0.0f, 0.0f, -s, w, h};
            break;
        case domain::Rotation::Cw270:
            m = {0.0f, -s, s, 0.0f, 0.0f, w};
            break;
    }
    m.e -= static_cast<float>(tileX);
    m.f -= static_cast<float>(tileY);
    return m;
}

// 自訂背景與文字色（PRD-VIEW-007）。
//
// 依每個像素的亮度在「文字色 → 背景色」之間內插：亮度 0 取文字色、
// 255 取背景色、中間線性過渡。這樣抗鋸齒的字緣仍然是平滑的過渡，
// 而不是被二值化成鋸齒。
//
// 只對接近灰階的像素套用。彩色像素（照片、圖表、螢光筆）維持原色——
// 使用者換配色是因為白底刺眼，不是要把照片變成單色。
void applyCustomColors(PixelBuffer& buffer, const RenderOptions& options) noexcept {
    // 飽和度門檻：R/G/B 三者的極差超過這個值就視為彩色。
    // 32 是實測值，足以放過照片，又不會漏掉被 JPEG 壓出輕微色偏的黑字。
    constexpr int kChromaThreshold = 32;

    for (std::int32_t y = 0; y < buffer.height(); ++y) {
        std::uint8_t* row = buffer.scanline(y);
        for (std::int32_t x = 0; x < buffer.width(); ++x) {
            std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
            const int b = px[0];
            const int g = px[1];
            const int r = px[2];
            const int chroma = std::max({r, g, b}) - std::min({r, g, b});
            if (chroma > kChromaThreshold) continue;

            // 亮度用綠色通道近似即可：這裡的像素已經確定接近灰階。
            const int luminance = g;
            const auto blend = [luminance](std::uint8_t dark, std::uint8_t light) {
                return static_cast<std::uint8_t>((dark * (255 - luminance) + light * luminance) /
                                                 255);
            };
            px[0] = blend(options.textB, options.backgroundB);
            px[1] = blend(options.textG, options.backgroundG);
            px[2] = blend(options.textR, options.backgroundR);
        }
    }
}

// 夜間模式（PRD-VIEW-007）。PDFium 沒有對應旗標，於像素層反相。
void applyNightMode(PixelBuffer& buffer) noexcept {
    for (std::int32_t y = 0; y < buffer.height(); ++y) {
        std::uint8_t* row = buffer.scanline(y);
        for (std::int32_t x = 0; x < buffer.width(); ++x) {
            std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
            px[0] = static_cast<std::uint8_t>(255 - px[0]);
            px[1] = static_cast<std::uint8_t>(255 - px[1]);
            px[2] = static_cast<std::uint8_t>(255 - px[2]);
        }
    }
}


// PDFium 的字串 API 一律回傳 UTF-16LE 並以位元組計長。這個轉換寫錯不會崩潰，
// 只會讓中文書籤變成亂碼，所以集中在一處。
std::string utf16BytesToUtf8(const std::vector<unsigned short>& units) {
    std::u16string text;
    text.reserve(units.size());
    for (const unsigned short unit : units) {
        if (unit == 0) break;
        text.push_back(static_cast<char16_t>(unit));
    }
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t code = text[i];
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.size()) {
            const char32_t low = text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

std::string annotationString(FPDF_ANNOTATION annot, const char* key) {
    const unsigned long bytes = FPDFAnnot_GetStringValue(annot, key, nullptr, 0);
    if (bytes <= 2) return {};
    std::vector<unsigned short> buffer(bytes / 2 + 1, 0);
    FPDFAnnot_GetStringValue(annot, key, buffer.data(), bytes);
    return utf16BytesToUtf8(buffer);
}

// PDFium 只回傳 /Subtype 的列舉值，介面要顯示的是名稱。
// 這張表刻意只列本產品範圍內的型別；範圍外的顯示原始數值而不是猜一個名字。
const char* subtypeName(int subtype) {
    switch (subtype) {
        case FPDF_ANNOT_TEXT:       return "Text";
        case FPDF_ANNOT_LINK:       return "Link";
        case FPDF_ANNOT_FREETEXT:   return "FreeText";
        case FPDF_ANNOT_LINE:       return "Line";
        case FPDF_ANNOT_SQUARE:     return "Square";
        case FPDF_ANNOT_CIRCLE:     return "Circle";
        case FPDF_ANNOT_POLYGON:    return "Polygon";
        case FPDF_ANNOT_POLYLINE:   return "PolyLine";
        case FPDF_ANNOT_HIGHLIGHT:  return "Highlight";
        case FPDF_ANNOT_UNDERLINE:  return "Underline";
        case FPDF_ANNOT_SQUIGGLY:   return "Squiggly";
        case FPDF_ANNOT_STRIKEOUT:  return "StrikeOut";
        case FPDF_ANNOT_STAMP:      return "Stamp";
        case FPDF_ANNOT_CARET:      return "Caret";
        case FPDF_ANNOT_INK:        return "Ink";
        case FPDF_ANNOT_POPUP:      return "Popup";
        case FPDF_ANNOT_FILEATTACHMENT: return "FileAttachment";
        case FPDF_ANNOT_WIDGET:     return "Widget";
        default:                    return "";
    }
}

std::string bookmarkTitle(FPDF_BOOKMARK bookmark) {
    const unsigned long bytes = FPDFBookmark_GetTitle(bookmark, nullptr, 0);
    if (bytes <= 2) return {};
    std::vector<unsigned short> buffer(bytes / 2 + 1, 0);
    FPDFBookmark_GetTitle(bookmark, buffer.data(), bytes);
    return utf16BytesToUtf8(buffer);
}

}  // namespace

// 佇列中的一項工作。優先權相同時維持先進先出。
struct Task {
    TaskPriority priority{TaskPriority::Visible};
    std::uint64_t sequence{0};
    std::function<void()> run;
    CancellationToken token{};
    std::function<void()> onCancelled;
    // 可視區一變就丟掉的工作。圖磚是（算到一半的舊可視區沒有價值），
    // 但頁面幾何這類「版面賴以成立的中繼資料」不是：丟掉它不會有錯誤訊息，
    // 只會讓那幾頁永遠停在佔位尺寸。同理，同步等待結果的呼叫端（列印）
    // 一旦工作被丟掉就會永遠等下去。
    bool discardable{true};
};

struct PdfiumEngine::Impl {
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable idleCv;
    std::deque<Task> queue;
    std::uint64_t nextSequence{0};
    bool running{true};
    bool busy{false};

    // 以下成員只能由引擎執行緒觸碰。
    FPDF_DOCUMENT document{nullptr};
    // 檔案來源必須活得比文件把手久：PDFium 在整個文件生命週期內都會回頭讀它。
    FileSource source;
    std::unordered_map<std::int32_t, FPDF_PAGE> pageCache;
    domain::DocumentInfo info{};

    void enqueue(TaskPriority priority, CancellationToken token, std::function<void()> run,
                 std::function<void()> onCancelled = {}, bool discardable = true) {
        {
            std::lock_guard lock(mutex);
            if (!running) return;
            Task task;
            task.priority = priority;
            task.sequence = nextSequence++;
            task.run = std::move(run);
            task.token = std::move(token);
            task.onCancelled = std::move(onCancelled);
            task.discardable = discardable;
            // 優先權排序：可見圖磚 > 預取 > 縮圖；同權時依序號維持 FIFO。
            const auto pos = std::upper_bound(
                queue.begin(), queue.end(), task, [](const Task& a, const Task& b) {
                    if (a.priority != b.priority) return a.priority < b.priority;
                    return a.sequence < b.sequence;
                });
            queue.insert(pos, std::move(task));
        }
        cv.notify_one();
    }

    FPDF_PAGE acquirePage(std::int32_t index) {
        if (!document) return nullptr;
        if (const auto it = pageCache.find(index); it != pageCache.end()) {
            return it->second;
        }
        FPDF_PAGE page = FPDF_LoadPage(document, index);
        if (page) {
            // 頁面按需載入並限制常駐數量，否則 10,000 頁文件會把把手全部留住
            // （PRD §8.1：10,000 頁文字型文件記憶體 ≤ 768 MB）。
            constexpr std::size_t kMaxResidentPages = 24;
            if (pageCache.size() >= kMaxResidentPages) {
                const auto victim = pageCache.begin();
                FPDF_ClosePage(victim->second);
                pageCache.erase(victim);
            }
            pageCache.emplace(index, page);
        }
        return page;
    }

    void releaseAllPages() {
        for (auto& entry : pageCache) {
            FPDF_ClosePage(entry.second);
        }
        pageCache.clear();
    }

    void loop() {
        const PdfiumRuntime runtime;
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return !running || !queue.empty(); });
                if (!running && queue.empty()) break;
                task = std::move(queue.front());
                queue.pop_front();
                busy = true;
            }

            if (task.token.valid() && task.token.isCancelled()) {
                // 取消路徑不碰 PDFium，不需要也不該持有鎖——取消要能在
                // 別人正在渲染時立刻生效，那正是取消存在的意義。
                if (task.onCancelled) task.onCancelled();
            } else if (task.run) {
                // 行程級序列化（ADR-005）。同一個行程裡可能有兩個 PdfiumEngine
                // 實體同時動作——檢視器一個、列印一個——而 PDFium 的行程級狀態
                // 不容許那樣。粒度是一張圖磚，所以檢視器最壞是等一張列印圖磚。
                const PdfiumGuard guard;
                task.run();
            }

            {
                std::lock_guard lock(mutex);
                busy = false;
            }
            idleCv.notify_all();
        }
        releaseAllPages();
        if (document) {
            FPDF_CloseDocument(document);
            document = nullptr;
        }
        source.close();
    }
};

PdfiumEngine::PdfiumEngine() : impl_(std::make_unique<Impl>()) {
    impl_->worker = std::thread([this] { impl_->loop(); });
}

PdfiumEngine::~PdfiumEngine() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->running = false;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

void PdfiumEngine::openDocument(std::string path, std::string password,
                                std::function<void(OpenResult)> callback) {
    impl_->enqueue(TaskPriority::Visible, CancellationToken{},
                   [this, path = std::move(path), password = std::move(password),
                    callback = std::move(callback)] {
        impl_->releaseAllPages();
        if (impl_->document) {
            FPDF_CloseDocument(impl_->document);
            impl_->document = nullptr;
        }

        OpenResult result;

        // 走回呼式隨機存取而不是 FPDF_LoadDocument（WBS 2.2）。兩個理由：
        // 大檔不整份進記憶體，以及不持有會擋住存檔原子更名的檔案控制代碼。
        impl_->source.close();
        if (!impl_->source.open(path)) {
            result.error = domain::DocumentError::FileNotFound;
            if (callback) callback(std::move(result));
            return;
        }

        FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(
            static_cast<FPDF_FILEACCESS*>(impl_->source.fileAccess()),
            password.empty() ? nullptr : password.c_str());
        if (!doc) {
            result.error = translateLoadError();
            impl_->source.close();
            if (callback) callback(std::move(result));
            return;
        }

        impl_->document = doc;
        domain::DocumentInfo info;
        info.pageCount = FPDF_GetPageCount(doc);
        info.encrypted = FPDF_GetSecurityHandlerRevision(doc) >= 0;
        info.hasSignatures = FPDF_GetSignatureCount(doc) > 0;

        int fileVersion = 0;
        if (FPDF_GetFileVersion(doc, &fileVersion) && fileVersion > 0) {
            info.pdfVersion =
                std::to_string(fileVersion / 10) + "." + std::to_string(fileVersion % 10);
        }

        // 權限旗標：受限操作要在 UI 就灰化，而不是等到存檔才失敗（PRD-SEC-001）。
        const unsigned long perms = FPDF_GetDocPermissions(doc);
        if (info.encrypted) {
            info.permissions.print = (perms & (1UL << 2)) != 0;
            info.permissions.modify = (perms & (1UL << 3)) != 0;
            info.permissions.copy = (perms & (1UL << 4)) != 0;
            info.permissions.annotate = (perms & (1UL << 5)) != 0;
            info.permissions.fillForms = (perms & (1UL << 8)) != 0;
            info.permissions.extractForAccessibility = (perms & (1UL << 9)) != 0;
            info.permissions.assemble = (perms & (1UL << 10)) != 0;
            info.permissions.printHighQuality = (perms & (1UL << 11)) != 0;
        }

        impl_->info = info;
        result.info = info;
        if (callback) callback(std::move(result));
    });
}

void PdfiumEngine::closeDocument(std::function<void()> callback) {
    impl_->enqueue(TaskPriority::Visible, CancellationToken{},
                   [this, callback = std::move(callback)] {
        impl_->releaseAllPages();
        if (impl_->document) {
            FPDF_CloseDocument(impl_->document);
            impl_->document = nullptr;
        }
        impl_->source.close();
        impl_->info = {};
        if (callback) callback();
    });
}

void PdfiumEngine::renderTile(domain::TileKey key, RenderOptions options, TaskPriority priority,
                              CancellationToken token, RenderCallback callback) {
    auto cancelled = [key, callback] {
        if (callback) {
            RenderResult result;
            result.key = key;
            result.cancelled = true;
            callback(std::move(result));
        }
    };

    impl_->enqueue(
        priority, token,
        [this, key, options, token, callback = std::move(callback)] {
            RenderResult result;
            result.key = key;

            FPDF_PAGE page = impl_->acquirePage(key.pageIndex);
            if (!page) {
                if (callback) callback(std::move(result));
                return;
            }

            const double pageWidth = FPDF_GetPageWidthF(page);
            const double pageHeight = FPDF_GetPageHeightF(page);
            const double scale = domain::scaleOfKey(key.scaleKey);
            if (scale <= 0.0) {
                // 倍率鍵為 0 代表呼叫端組錯了鍵。畫出來會是一片空白，
                // 那種「沒壞但也不對」的結果比直接失敗難查得多（IL-4）。
                if (callback) callback(std::move(result));
                return;
            }

            auto buffer = std::make_shared<PixelBuffer>(domain::kTileSize, domain::kTileSize);
            if (buffer->isNull()) {
                if (callback) callback(std::move(result));
                return;
            }
            // 白底：PDF 頁面本身不保證畫背景。透明度格線模式刻意反過來——
            // 用全透明（alpha = 0）起始，讓頁面真正沒畫到的地方保留透明，
            // 呈現層才有辦法在圖磚底下畫棋盤格透出來（PRD-VIEW-018）。
            buffer->fill(options.transparencyGrid ? 0x00 : 0xFF);

            // 零複製：PDFium 直接寫進我們的緩衝區，stride 顯式傳入，
            // 絕不以「寬 × 4」推算（見 pixel_buffer.h 的說明）。
            FPDF_BITMAP bitmap =
                FPDFBitmap_CreateEx(buffer->width(), buffer->height(), FPDFBitmap_BGRA,
                                    buffer->data(), static_cast<int>(buffer->stride()));
            if (!bitmap) {
                if (callback) callback(std::move(result));
                return;
            }

            const FS_MATRIX matrix =
                tileMatrix(pageWidth, pageHeight, scale, key.rotation,
                           static_cast<double>(key.column) * domain::kTileSize,
                           static_cast<double>(key.row) * domain::kTileSize);
            const FS_RECTF clip{0.0f, 0.0f, static_cast<float>(buffer->width()),
                                static_cast<float>(buffer->height())};

            int flags = 0;
            if (options.drawAnnotations) flags |= FPDF_ANNOT;
            if (options.lcdText) flags |= FPDF_LCD_TEXT;
            // thinLines（PRD-VIEW-010）屬於註解外觀層的處置，不是 PDFium 旗標；
            // 待註解子系統接手，此處保留欄位以固定介面。
            (void)options.thinLines;
            // Stroke Adjust（PRD-VIEW-013）：關掉路徑抗鋸齒讓細線變銳利。
            // 見標頭的說明——這是近似，不是 PDF 規格的 stroke adjustment。
            if (options.strokeAdjust) flags |= FPDF_RENDER_NO_SMOOTHPATH;
            if (!options.smoothText) flags |= FPDF_RENDER_NO_SMOOTHTEXT;
            if (!options.smoothImages) flags |= FPDF_RENDER_NO_SMOOTHIMAGE;
            if (options.grayscale) flags |= FPDF_GRAYSCALE;

            // 這裡刻意不用 FPDF_RenderPageBitmap_Start/Continue 的漸進式路徑：
            // 圖磚化本身就是分段單位，一塊 512×512 的渲染時間遠低於 16 毫秒，
            // 取消的粒度由「佇列中丟棄未開始的任務」提供而非中斷單塊渲染。
            // 漸進式 API 又不吃矩陣，硬用會退化成整頁光柵化再裁切——正是 PRD-VIEW-001 禁止的事。
            FPDF_RenderPageBitmapWithMatrix(bitmap, page, &matrix, &clip, flags);
            FPDFBitmap_Destroy(bitmap);

            if (token.valid() && token.isCancelled()) {
                result.cancelled = true;
                if (callback) callback(std::move(result));
                return;
            }

            // 兩者互斥：夜間模式優先。同時套用等於先反相再重新上色，
            // 得到的顏色與使用者選的兩個色都沒有關係。
            if (options.nightMode) {
                applyNightMode(*buffer);
            } else if (options.customColors) {
                applyCustomColors(*buffer, options);
            }

            result.buffer = buffer;
            if (callback) callback(std::move(result));
        },
        std::move(cancelled));
}

// 頁面幾何刻意不可丟棄（Task::discardable）。它跑在 Background，而
// scheduleTiles 每次可視區變動都會 discardPending(Prefetch)——那個條件是
// 「優先權數值 >= Prefetch」，Background 數值更大，所以一併中彈。
// 後果有兩個，都不會有錯誤訊息：版面永遠停在 A4 佔位尺寸，以及同步等待
// 結果的列印路徑（print_service）永遠等不到 promise。
void PdfiumEngine::pageInfo(std::int32_t pageIndex,
                            std::function<void(std::optional<domain::PageInfo>)> callback) {
    impl_->enqueue(TaskPriority::Background, CancellationToken{},
                   [this, pageIndex, callback = std::move(callback)] {
        FPDF_PAGE page = impl_->acquirePage(pageIndex);
        if (!page) {
            if (callback) callback(std::nullopt);
            return;
        }
        domain::PageInfo info;
        info.index = pageIndex;
        info.sizePt = {FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page)};
        info.intrinsicRotation = static_cast<domain::Rotation>(FPDFPage_GetRotation(page) & 0x3);
        if (callback) callback(info);
    }, {}, /*discardable=*/false);
}


void PdfiumEngine::pageAnnotations(
    std::int32_t pageIndex,
    std::function<void(std::vector<domain::AnnotationSummary>)> callback) {
    impl_->enqueue(TaskPriority::Background, CancellationToken{},
                   [this, pageIndex, callback = std::move(callback)] {
        std::vector<domain::AnnotationSummary> summaries;
        FPDF_PAGE page = impl_->acquirePage(pageIndex);
        if (!page) {
            if (callback) callback(std::move(summaries));
            return;
        }

        const int count = FPDFPage_GetAnnotCount(page);
        summaries.reserve(static_cast<std::size_t>(std::max(0, count)));
        for (int i = 0; i < count; ++i) {
            FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
            if (!annot) continue;

            const int subtype = FPDFAnnot_GetSubtype(annot);
            // Popup 不是獨立的註解，是別則註解的附屬視窗。列在清單裡會變成
            // 每則便利貼出現兩次。
            if (subtype == FPDF_ANNOT_POPUP) {
                FPDFPage_CloseAnnot(annot);
                continue;
            }

            domain::AnnotationSummary summary;
            summary.pageIndex = pageIndex;
            summary.indexOnPage = i;
            const char* name = subtypeName(subtype);
            summary.subtype = *name != 0 ? name : std::to_string(subtype);
            summary.author = annotationString(annot, "T");
            summary.contents = annotationString(annot, "Contents");
            summary.modified = annotationString(annot, "M");

            FS_RECTF rect{};
            if (FPDFAnnot_GetRect(annot, &rect)) {
                summary.rect = domain::RectF{rect.left, rect.bottom, rect.right, rect.top}
                                   .normalized();
            }

            summaries.push_back(std::move(summary));
            FPDFPage_CloseAnnot(annot);
        }

        if (callback) callback(std::move(summaries));
    });
}

void PdfiumEngine::pageLinks(std::int32_t pageIndex,
                             std::function<void(std::vector<domain::LinkTarget>)> callback) {
    impl_->enqueue(TaskPriority::Background, CancellationToken{},
                   [this, pageIndex, callback = std::move(callback)] {
        std::vector<domain::LinkTarget> links;
        FPDF_PAGE page = impl_->acquirePage(pageIndex);
        if (!page || !impl_->document) {
            if (callback) callback(std::move(links));
            return;
        }

        int position = 0;
        FPDF_LINK link = nullptr;
        while (FPDFLink_Enumerate(page, &position, &link)) {
            if (!link) continue;

            domain::LinkTarget target;
            FS_RECTF rect{};
            if (FPDFLink_GetAnnotRect(link, &rect)) {
                target.rect =
                    domain::RectF{rect.left, rect.bottom, rect.right, rect.top}.normalized();
            }

            if (FPDF_DEST dest = FPDFLink_GetDest(impl_->document, link)) {
                const int destPage = FPDFDest_GetDestPageIndex(impl_->document, dest);
                if (destPage >= 0) target.pageIndex = destPage;
            }

            if (FPDF_ACTION action = FPDFLink_GetAction(link)) {
                const unsigned long type = FPDFAction_GetType(action);
                if (type == PDFACTION_URI) {
                    const unsigned long length =
                        FPDFAction_GetURIPath(impl_->document, action, nullptr, 0);
                    if (length > 1) {
                        std::string uri(length, char{});
                        FPDFAction_GetURIPath(impl_->document, action, uri.data(), length);
                        // 回傳長度含結尾的 NUL，留著會讓字串比較與顯示都出錯。
                        uri.resize(length - 1);
                        target.uri = std::move(uri);
                    }
                } else if (type == PDFACTION_GOTO) {
                    if (FPDF_DEST dest = FPDFAction_GetDest(impl_->document, action)) {
                        const int destPage = FPDFDest_GetDestPageIndex(impl_->document, dest);
                        if (destPage >= 0) target.pageIndex = destPage;
                    }
                }
                // Launch action 一律忽略：PRD §8.2 明令禁止執行外部程式。
                // 這裡不回報也不提示——那是一個使用者從未要求的動作。
            }

            if (target.isValid()) links.push_back(std::move(target));
        }

        if (callback) callback(std::move(links));
    });
}

void PdfiumEngine::outline(std::function<void(std::vector<domain::OutlineNode>)> callback) {
    impl_->enqueue(TaskPriority::Background, CancellationToken{}, [this, callback = std::move(callback)] {
        std::vector<domain::OutlineNode> roots;
        if (!impl_->document) {
            if (callback) callback(std::move(roots));
            return;
        }

        // 迭代而非遞迴走訪：書籤樹的深度來自不可信任的輸入，
        // 惡意檔案可以構造出上萬層的巢狀把堆疊爆掉（PDF 視為不可信任輸入）。
        //
        // 走訪堆疊記的是「索引路徑」而不是節點指標。原本的版本存的是
        // `&stored.children`，而 stored 是 std::vector 的元素——同一層再 push 一個
        // 兄弟就會重新配置，那個位址立刻懸空。觸發條件是「同層有兩個以上兄弟、
        // 且非最後一個帶子節點」，也就是幾乎所有真實文件的書籤樹。
        // 這個 use-after-free 是在寫書籤工具時被抓到的。
        constexpr int kMaxDepth = 32;
        struct Frame {
            FPDF_BOOKMARK bookmark;
            std::vector<std::size_t> path;  // 由根到父節點的索引路徑；空代表根層
            int depth;
        };

        // 依索引路徑取得該層的容器。每次取用時重新解析，所以中途的重新配置不影響正確性。
        const auto resolveSiblings = [&roots](const std::vector<std::size_t>& path)
            -> std::vector<domain::OutlineNode>* {
            std::vector<domain::OutlineNode>* level = &roots;
            for (const std::size_t index : path) {
                if (index >= level->size()) return nullptr;
                level = &(*level)[index].children;
            }
            return level;
        };

        std::vector<Frame> stack;
        if (FPDF_BOOKMARK first = FPDFBookmark_GetFirstChild(impl_->document, nullptr)) {
            stack.push_back(Frame{first, {}, 0});
        }

        while (!stack.empty()) {
            const Frame frame = stack.back();
            stack.pop_back();

            FPDF_BOOKMARK bookmark = frame.bookmark;
            while (bookmark) {
                std::vector<domain::OutlineNode>* siblings = resolveSiblings(frame.path);
                if (siblings == nullptr) break;

                domain::OutlineNode node;
                node.title = bookmarkTitle(bookmark);

                if (FPDF_DEST dest = FPDFBookmark_GetDest(impl_->document, bookmark)) {
                    const int page = FPDFDest_GetDestPageIndex(impl_->document, dest);
                    if (page >= 0) node.pageIndex = page;
                }

                // FPDFBookmark_GetDest 完全不看 /A。以 GoTo action 形態寫的書籤
                // 拿不到頁碼，因此再走一次 action 路徑補上。
                if (!node.pageIndex) {
                    if (FPDF_ACTION action = FPDFBookmark_GetAction(bookmark)) {
                        if (FPDFAction_GetType(action) == PDFACTION_GOTO) {
                            if (FPDF_DEST dest = FPDFAction_GetDest(impl_->document, action)) {
                                const int page = FPDFDest_GetDestPageIndex(impl_->document, dest);
                                if (page >= 0) node.pageIndex = page;
                            }
                        }
                    }
                }

                siblings->push_back(std::move(node));
                const std::size_t storedIndex = siblings->size() - 1;

                if (frame.depth + 1 < kMaxDepth) {
                    if (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(impl_->document, bookmark)) {
                        std::vector<std::size_t> childPath = frame.path;
                        childPath.push_back(storedIndex);
                        stack.push_back(Frame{child, std::move(childPath), frame.depth + 1});
                    }
                }

                bookmark = FPDFBookmark_GetNextSibling(impl_->document, bookmark);
            }
        }

        if (callback) callback(std::move(roots));
    });
}

void PdfiumEngine::renderThumbnail(std::int32_t pageIndex, std::int32_t maxEdgePixels,
                                   CancellationToken token, RenderCallback callback) {
    impl_->enqueue(
        TaskPriority::Thumbnail, token,
        [this, pageIndex, maxEdgePixels, callback = std::move(callback)] {
            RenderResult result;
            result.key = domain::TileKey{pageIndex, 0, 0, 0, domain::Rotation::None, false};

            FPDF_PAGE page = impl_->acquirePage(pageIndex);
            if (!page) {
                if (callback) callback(std::move(result));
                return;
            }

            const double pageWidth = FPDF_GetPageWidthF(page);
            const double pageHeight = FPDF_GetPageHeightF(page);
            if (pageWidth <= 0.0 || pageHeight <= 0.0) {
                if (callback) callback(std::move(result));
                return;
            }

            const double scale =
                static_cast<double>(maxEdgePixels) / std::max(pageWidth, pageHeight);
            const auto width = static_cast<std::int32_t>(std::max(1.0, pageWidth * scale));
            const auto height = static_cast<std::int32_t>(std::max(1.0, pageHeight * scale));

            auto buffer = std::make_shared<PixelBuffer>(width, height);
            if (buffer->isNull()) {
                if (callback) callback(std::move(result));
                return;
            }
            buffer->fill(0xFF);

            FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA,
                                                     buffer->data(),
                                                     static_cast<int>(buffer->stride()));
            if (!bitmap) {
                if (callback) callback(std::move(result));
                return;
            }
            FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, FPDF_ANNOT);
            FPDFBitmap_Destroy(bitmap);

            result.buffer = buffer;
            if (callback) callback(std::move(result));
        },
        [key = domain::TileKey{pageIndex, 0, 0, 0, domain::Rotation::None, false}, callback] {
            if (callback) {
                RenderResult result;
                result.key = key;
                result.cancelled = true;
                callback(std::move(result));
            }
        });
}

void PdfiumEngine::withDocument(std::function<void(void*)> work) {
    impl_->enqueue(TaskPriority::Background, CancellationToken{},
                   [this, work = std::move(work)] {
                       if (work) work(impl_->document);
                   });
}

void PdfiumEngine::discardPending(TaskPriority atOrBelow) {
    std::deque<Task> dropped;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto it = impl_->queue.begin(); it != impl_->queue.end();) {
            if (it->priority >= atOrBelow && it->discardable) {
                dropped.push_back(std::move(*it));
                it = impl_->queue.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& task : dropped) {
        if (task.onCancelled) task.onCancelled();
    }
}

void PdfiumEngine::waitForIdle() {
    std::unique_lock lock(impl_->mutex);
    impl_->idleCv.wait(lock, [this] { return impl_->queue.empty() && !impl_->busy; });
}

std::size_t PdfiumEngine::pendingTaskCount() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->queue.size();
}

}  // namespace alioth::engine
