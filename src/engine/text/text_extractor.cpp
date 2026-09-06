#include "engine/text/text_extractor.h"

#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "engine/file_source.h"
#include "engine/pdfium_library.h"
#include "engine/pdfium_lock.h"
#include "engine/text/text_encoding.h"

namespace alioth::engine::text {
namespace {

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

// 視覺行的判定門檻：兩個字元外框的垂直重疊需達較矮者的一半才算同一行。
// 取一半是為了容忍下標、上標與混排字級；門檻再低會把相鄰兩行黏成一行，
// 而黏成一行的後果是跨行選取只產生一個 quad，螢光筆蓋掉整塊版面。
constexpr double kSameLineOverlapRatio = 0.5;

// 行距超過字高的這個倍數視為換段／換欄。純幾何啟發式：
// PDFium 不提供段落結構，而 /StructTree 在多數實務 PDF 裡不可靠。
constexpr double kBlockGapRatio = 0.75;

bool isLineBreak(char32_t c) noexcept { return c == U'\n' || c == U'\r'; }

domain::PageTextLayer buildLayer(FPDF_TEXTPAGE textPage, std::int32_t pageIndex) {
    const int count = FPDFText_CountChars(textPage);
    domain::PageTextLayer empty{pageIndex, {}};
    if (count <= 0) return empty;

    std::vector<domain::TextChar> chars;
    chars.reserve(static_cast<std::size_t>(count));

    std::int32_t lineIndex = 0;
    std::int32_t blockIndex = 0;
    domain::RectF lineBox{};
    bool lineHasBox = false;
    bool breakPending = false;

    for (int i = 0; i < count; ++i) {
        domain::TextChar ch;
        ch.index = i;
        ch.unicode = static_cast<char32_t>(FPDFText_GetUnicode(textPage, i));

        double left = 0.0, right = 0.0, bottom = 0.0, top = 0.0;
        if (FPDFText_GetCharBox(textPage, i, &left, &right, &bottom, &top)) {
            ch.box = domain::RectF{left, bottom, right, top}.normalized();
        }

        const bool degenerate = ch.box.isEmpty();
        if (isLineBreak(ch.unicode)) breakPending = true;

        if (!degenerate) {
            bool newLine = breakPending;
            if (!newLine && lineHasBox) {
                const double overlap =
                    std::min(lineBox.top, ch.box.top) - std::max(lineBox.bottom, ch.box.bottom);
                const double minHeight = std::min(lineBox.height(), ch.box.height());
                newLine = overlap < kSameLineOverlapRatio * minHeight;
            }
            if (newLine && lineHasBox) {
                ++lineIndex;
                // 頁面 Y 向上，往下讀就是 Y 遞減，因此空隙是「上一行底 − 這一行頂」。
                const double gap = lineBox.bottom - ch.box.top;
                if (gap > kBlockGapRatio * ch.box.height()) ++blockIndex;
                lineBox = ch.box;
            } else {
                lineBox = lineHasBox ? lineBox.united(ch.box) : ch.box;
            }
            lineHasBox = true;
            breakPending = false;
        }

        // 換行字元本身歸在它結束的那一行，三擊選行時再由領域層裁掉行尾控制字元。
        ch.lineIndex = lineIndex;
        ch.blockIndex = blockIndex;
        chars.push_back(ch);
    }

    return domain::PageTextLayer{pageIndex, std::move(chars)};
}

}  // namespace

TextPage::TextPage(void* fpdfPage, void* fpdfTextPage, std::int32_t pageIndex) noexcept
    : page_(fpdfPage), textPage_(fpdfTextPage), pageIndex_(pageIndex) {}

TextPage::TextPage(TextPage&& other) noexcept
    : page_(other.page_),
      textPage_(other.textPage_),
      pageIndex_(other.pageIndex_),
      layer_(std::move(other.layer_)) {
    other.page_ = nullptr;
    other.textPage_ = nullptr;
}

TextPage& TextPage::operator=(TextPage&& other) noexcept {
    if (this != &other) {
        reset();
        page_ = other.page_;
        textPage_ = other.textPage_;
        pageIndex_ = other.pageIndex_;
        layer_ = std::move(other.layer_);
        other.page_ = nullptr;
        other.textPage_ = nullptr;
    }
    return *this;
}

TextPage::~TextPage() { reset(); }

// 釋放順序不可顛倒：文字層是頁面的衍生物，先關頁面會留下懸空的文字層把手。
void TextPage::reset() noexcept {
    if (textPage_) {
        FPDFText_ClosePage(static_cast<FPDF_TEXTPAGE>(textPage_));
        textPage_ = nullptr;
    }
    if (page_) {
        FPDF_ClosePage(static_cast<FPDF_PAGE>(page_));
        page_ = nullptr;
    }
    layer_.reset();
}

const domain::PageTextLayer& TextPage::layer() const {
    if (!layer_) {
        layer_ = std::make_unique<domain::PageTextLayer>(
            textPage_ ? buildLayer(static_cast<FPDF_TEXTPAGE>(textPage_), pageIndex_)
                      : domain::PageTextLayer{pageIndex_, {}});
    }
    return *layer_;
}

std::int32_t charCount(const TextPage& page) {
    if (!page.valid()) return 0;
    const int count = FPDFText_CountChars(static_cast<FPDF_TEXTPAGE>(page.handle()));
    return count < 0 ? 0 : count;
}

domain::TextRange pageRange(const TextPage& page) {
    return domain::TextRange{0, charCount(page)};
}

std::int32_t charIndexAt(const TextPage& page, const domain::PointF& pagePoint, double tolerance) {
    if (!page.valid()) return -1;
    const int index = FPDFText_GetCharIndexAtPos(static_cast<FPDF_TEXTPAGE>(page.handle()),
                                                 pagePoint.x, pagePoint.y, tolerance, tolerance);
    // PDFium 用 -1 表示沒命中、-3 表示錯誤。對呼叫端而言兩者一樣是「沒有字元」。
    return index < 0 ? -1 : index;
}

domain::RectF charBox(const TextPage& page, std::int32_t index) {
    if (!page.valid() || index < 0) return {};
    double left = 0.0, right = 0.0, bottom = 0.0, top = 0.0;
    if (!FPDFText_GetCharBox(static_cast<FPDF_TEXTPAGE>(page.handle()), index, &left, &right,
                             &bottom, &top)) {
        return {};
    }
    return domain::RectF{left, bottom, right, top}.normalized();
}

std::vector<domain::QuadPoint> quadsForRange(const TextPage& page, domain::TextRange range) {
    if (!page.valid()) return {};
    return page.layer().quads(range);
}

std::string textForRange(const TextPage& page, domain::TextRange range) {
    if (!page.valid()) return {};
    return page.layer().text(range);
}

std::string boundedText(const TextPage& page, const domain::RectF& area) {
    if (!page.valid() || area.isEmpty()) return {};
    auto* textPage = static_cast<FPDF_TEXTPAGE>(page.handle());
    // 注意參數順序是 left, top, right, bottom——與 PDF /Rect 的 left,bottom,right,top 不同，
    // 傳錯的話會安靜地回傳空字串而不是報錯。
    const int needed =
        FPDFText_GetBoundedText(textPage, area.left, area.top, area.right, area.bottom, nullptr, 0);
    if (needed <= 0) return {};

    std::vector<unsigned short> buffer(static_cast<std::size_t>(needed) + 1, 0);
    const int written = FPDFText_GetBoundedText(textPage, area.left, area.top, area.right,
                                                area.bottom, buffer.data(),
                                                static_cast<int>(buffer.size()));
    if (written <= 0) return {};
    return toUtf8(buffer.data(), static_cast<std::size_t>(std::min(written, needed)));
}

domain::TextRange wordRangeAt(const TextPage& page, std::int32_t index) {
    if (!page.valid()) return {};
    return page.layer().wordRangeAt(index);
}

domain::TextRange lineRangeAt(const TextPage& page, std::int32_t index) {
    if (!page.valid()) return {};
    return page.layer().lineRangeAt(index);
}

// 佇列中的一項工作。文字擷取沒有渲染那樣的優先權差異，維持 FIFO 即可。
struct TextExtractor::Impl {
    // 檔案來源必須活得比文件把手久：PDFium 整個生命週期都會回頭讀它。
    FileSource source;
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable idleCv;
    std::deque<std::function<void()>> queue;
    bool running{true};
    bool busy{false};
    std::atomic<std::int32_t> pageCount{0};

    // 以下成員只能由文字執行緒觸碰。
    FPDF_DOCUMENT document{nullptr};
    std::deque<TextPage> pages;

    void enqueue(std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            if (!running) return;
            queue.push_back(std::move(task));
        }
        cv.notify_one();
    }

    const TextPage* acquirePage(std::int32_t index) {
        if (!document || index < 0) return nullptr;
        const auto it = std::find_if(pages.begin(), pages.end(), [index](const TextPage& p) {
            return p.pageIndex() == index;
        });
        if (it != pages.end()) return &*it;

        FPDF_PAGE page = FPDF_LoadPage(document, index);
        if (!page) return nullptr;
        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        if (!textPage) {
            FPDF_ClosePage(page);
            return nullptr;
        }

        // 常駐頁數上限。文字層在大頁面上可以是數 MB，10,000 頁文件全留住會爆掉。
        constexpr std::size_t kMaxResidentPages = 8;
        if (pages.size() >= kMaxResidentPages) pages.pop_front();
        pages.emplace_back(TextPage{page, textPage, index});
        return &pages.back();
    }

    void releaseAllPages() { pages.clear(); }

    void closeDocument() {
        releaseAllPages();
        if (document) {
            FPDF_CloseDocument(document);
            document = nullptr;
        }
        // 檔案來源必須在文件關掉之後才關：PDFium 在關檔過程中還可能回頭讀。
        source.close();
        pageCount.store(0, std::memory_order_relaxed);
    }

    void loop() {
        // 全域初始化由共用的 PdfiumRuntime 負責，兩個子系統共用同一份引用計數。
        const PdfiumRuntime runtime;
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return !running || !queue.empty(); });
                if (!running && queue.empty()) break;
                task = std::move(queue.front());
                queue.pop_front();
                busy = true;
            }
            // 行程級序列化（ADR-005）。文字擷取與渲染是兩條不同的執行緒，
            // 而使用者在搜尋或建索引時仍然會捲動。
            //
            // 粒度是一頁的文字層。建索引把整份文件切成一頁一件工作，
            // 正是為了讓這把鎖不會被長時間持有——一次全排會讓捲動卡到索引建完。
            if (task) {
                const PdfiumGuard guard;
                task();
            }
            {
                std::lock_guard lock(mutex);
                busy = false;
            }
            idleCv.notify_all();
        }
        // 所有 PDFium 物件都在這條執行緒上建立，也必須在這條執行緒上銷毀。
        closeDocument();
    }
};

TextExtractor::TextExtractor() : impl_(std::make_unique<Impl>()) {
    impl_->worker = std::thread([this] { impl_->loop(); });
}

TextExtractor::~TextExtractor() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->running = false;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void TextExtractor::open(std::string path, std::string password,
                         std::function<void(domain::DocumentError)> callback) {
    impl_->enqueue([this, path = std::move(path), password = std::move(password),
                    callback = std::move(callback)] {
        impl_->closeDocument();

        // 與 PdfiumEngine 一樣走回呼式隨機存取（WBS 2.2）。這裡的關鍵不是記憶體，
        // 而是檔案控制代碼：FPDF_LoadDocument 持有的鎖會讓存檔時的原子更名失敗，
        // 而「檢視器開著文件」正是存檔的唯一情境。
        if (!impl_->source.open(path)) {
            if (callback) callback(domain::DocumentError::FileNotFound);
            return;
        }

        FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(
            static_cast<FPDF_FILEACCESS*>(impl_->source.fileAccess()),
            password.empty() ? nullptr : password.c_str());
        if (!doc) {
            impl_->source.close();
            if (callback) callback(translateLoadError());
            return;
        }
        impl_->document = doc;
        impl_->pageCount.store(FPDF_GetPageCount(doc), std::memory_order_relaxed);
        if (callback) callback(domain::DocumentError::None);
    });
}

void TextExtractor::close(std::function<void()> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        impl_->closeDocument();
        if (callback) callback();
    });
}

std::int32_t TextExtractor::pageCount() const noexcept {
    return impl_->pageCount.load(std::memory_order_relaxed);
}

void TextExtractor::withTextPage(std::int32_t pageIndex,
                                 std::function<void(const TextPage*)> work) {
    impl_->enqueue([this, pageIndex, work = std::move(work)] {
        const TextPage* page = impl_->acquirePage(pageIndex);
        // 頁面不存在時仍要呼叫回呼並傳 nullptr：靜默不回呼會讓呼叫端永遠等下去（IL-4）。
        if (work) work(page);
    });
}

void TextExtractor::waitForIdle() {
    std::unique_lock lock(impl_->mutex);
    impl_->idleCv.wait(lock, [this] { return impl_->queue.empty() && !impl_->busy; });
}

std::size_t TextExtractor::pendingTaskCount() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->queue.size();
}

}  // namespace alioth::engine::text
