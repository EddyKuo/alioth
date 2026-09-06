#include "engine/forms/form_environment.h"

#include <fpdfview.h>
#include <fpdf_formfill.h>

#include <cstring>
#include <ctime>

namespace alioth::engine::forms {
namespace {

// PDFium 的 UTF-16LE 字串轉 UTF-8。回呼參數多半是 URL 之類的短字串，
// 但仍然要正確處理代理對：非 ASCII 網域若被截成亂碼才交給使用者確認，
// 確認對話框就失去意義了。
std::string wideToUtf8(const unsigned short* units) {
    std::string out;
    if (!units) return out;
    for (std::size_t i = 0; units[i] != 0; ++i) {
        char32_t code = units[i];
        if (code >= 0xD800 && code <= 0xDBFF && units[i + 1] != 0) {
            const char32_t low = units[i + 1];
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

}  // namespace

// FPDF_FORMFILLINFO 必須是本結構的**第一個成員**：PDFium 只會把 pThis 傳回來，
// 我們靠位址回推整個 Impl。順序寫錯不會編譯失敗，只會在第一次回呼時踩到隨機記憶體。
struct FormEnvironment::Impl {
    FPDF_FORMFILLINFO info{};

    FPDF_FORMHANDLE handle{nullptr};
    FPDF_DOCUMENT document{nullptr};
    PageLoader loader;
    PageIndexResolver resolver;

    std::vector<InvalidatedRegion> invalidations;
    std::vector<BlockedNavigation> blocked;
    std::int32_t changes{0};
    std::int32_t nextTimerId{1};
    bool highlight{false};

    static Impl* from(FPDF_FORMFILLINFO* self) { return reinterpret_cast<Impl*>(self); }

    void recordInvalidation(FPDF_PAGE page, double left, double top, double right, double bottom) {
        InvalidatedRegion region;
        region.pageIndex = resolver ? resolver(page) : -1;
        // FFI_Invalidate 的 top / bottom 語意隨頁面方向而定，一律正規化成
        // RectF 的「bottom < top」不變量，避免下游拿到負高度的矩形。
        const double lo = bottom < top ? bottom : top;
        const double hi = bottom < top ? top : bottom;
        region.rectPt = domain::RectF{left, lo, right, hi};
        invalidations.push_back(region);
    }

    void blockNavigation(BlockedNavigation::Kind kind, std::string payload,
                         std::int32_t pageIndex) {
        BlockedNavigation nav;
        nav.kind = kind;
        nav.payload = std::move(payload);
        nav.pageIndex = pageIndex;
        blocked.push_back(std::move(nav));
    }

    // ----- 以下是交給 PDFium 的 C 回呼。放成靜態成員是為了能碰私有成員。 -----

    static void cbRelease(FPDF_FORMFILLINFO*) {}

    static void cbInvalidate(FPDF_FORMFILLINFO* self, FPDF_PAGE page, double left, double top,
                             double right, double bottom) {
        from(self)->recordInvalidation(page, left, top, right, bottom);
    }

    static void cbOutputSelectedRect(FPDF_FORMFILLINFO* self, FPDF_PAGE page, double left,
                                     double top, double right, double bottom) {
        from(self)->recordInvalidation(page, left, top, right, bottom);
    }

    static void cbSetCursor(FPDF_FORMFILLINFO*, int) {}

    // 不接受 PDFium 驅動的計時器：唯一用途是插字游標閃爍，而游標繪製由呈現層負責。
    // 回傳遞增的假 id 而不是 0——0 在部分呼叫路徑上被當成失敗，行為較難預測。
    static int cbSetTimer(FPDF_FORMFILLINFO* self, int, TimerCallback) {
        return from(self)->nextTimerId++;
    }

    static void cbKillTimer(FPDF_FORMFILLINFO*, int) {}

    // 回報的是 UTC 而不是當地時間。兩個理由：時區轉換的 CRT 介面在各平台不同，
    // 而作業系統差異只准出現在平台層；再者這個回呼的唯一消費者是 JS 與 XFA 的
    // 日期欄位，兩者本專案都不支援，因此差異不會被任何人看見。
    // 日曆換算用純算術（Howard Hinnant 的 civil_from_days），不呼叫 CRT。
    static FPDF_SYSTEMTIME cbGetLocalTime(FPDF_FORMFILLINFO*) {
        FPDF_SYSTEMTIME out{};
        const auto now = static_cast<long long>(std::time(nullptr));
        long long days = now / 86400;
        long long secondsOfDay = now % 86400;
        if (secondsOfDay < 0) {
            secondsOfDay += 86400;
            --days;
        }
        // 1970-01-01 是星期四。
        long long weekday = (days + 4) % 7;
        if (weekday < 0) weekday += 7;

        long long z = days + 719468;
        const long long era = (z >= 0 ? z : z - 146096) / 146097;
        const auto doe = static_cast<unsigned long long>(z - era * 146097);
        const unsigned long long yoe =
            (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const long long y = static_cast<long long>(yoe) + era * 400;
        const unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned long long mp = (5 * doy + 2) / 153;
        const unsigned long long d = doy - (153 * mp + 2) / 5 + 1;
        const unsigned long long m = mp + (mp < 10 ? 3 : -9);

        out.wYear = static_cast<unsigned short>(y + (m <= 2 ? 1 : 0));
        out.wMonth = static_cast<unsigned short>(m);
        out.wDay = static_cast<unsigned short>(d);
        out.wDayOfWeek = static_cast<unsigned short>(weekday);
        out.wHour = static_cast<unsigned short>(secondsOfDay / 3600);
        out.wMinute = static_cast<unsigned short>((secondsOfDay / 60) % 60);
        out.wSecond = static_cast<unsigned short>(secondsOfDay % 60);
        out.wMilliseconds = 0;
        return out;
    }

    static void cbOnChange(FPDF_FORMFILLINFO* self) { ++from(self)->changes; }

    static FPDF_PAGE cbGetPage(FPDF_FORMFILLINFO* self, FPDF_DOCUMENT document, int index) {
        Impl* impl = from(self);
        if (document != impl->document || !impl->loader) return nullptr;
        return static_cast<FPDF_PAGE>(impl->loader(index));
    }

    static FPDF_PAGE cbGetCurrentPage(FPDF_FORMFILLINFO* self, FPDF_DOCUMENT document) {
        return cbGetPage(self, document, 0);
    }

    // 表單環境問的是「檢視旋轉」而不是頁面自帶的 /Rotate。檢視旋轉屬於應用層狀態，
    // 引擎層沒有它；回報未旋轉是唯一不會說謊的答案。
    static int cbGetRotation(FPDF_FORMFILLINFO*, FPDF_PAGE) { return 0; }

    static void cbExecuteNamedAction(FPDF_FORMFILLINFO* self, FPDF_BYTESTRING action) {
        from(self)->blockNavigation(BlockedNavigation::Kind::NamedAction, action ? action : "", -1);
    }

    static void cbSetTextFieldFocus(FPDF_FORMFILLINFO*, FPDF_WIDESTRING, FPDF_DWORD, FPDF_BOOL) {}

    static void cbDoUriAction(FPDF_FORMFILLINFO* self, FPDF_BYTESTRING uri) {
        from(self)->blockNavigation(BlockedNavigation::Kind::Uri, uri ? uri : "", -1);
    }

    static void cbDoUriActionWithModifier(FPDF_FORMFILLINFO* self, FPDF_BYTESTRING uri, int) {
        cbDoUriAction(self, uri);
    }

    static void cbDoGoToAction(FPDF_FORMFILLINFO* self, int pageIndex, int, float*, int) {
        from(self)->blockNavigation(BlockedNavigation::Kind::GoTo, {}, pageIndex);
    }

    static void cbDisplayCaret(FPDF_FORMFILLINFO* self, FPDF_PAGE page, FPDF_BOOL, double left,
                               double top, double right, double bottom) {
        from(self)->recordInvalidation(page, left, top, right, bottom);
    }

    static int cbGetCurrentPageIndex(FPDF_FORMFILLINFO*, FPDF_DOCUMENT) { return 0; }

    static void cbSetCurrentPage(FPDF_FORMFILLINFO* self, FPDF_DOCUMENT, int pageIndex) {
        from(self)->blockNavigation(BlockedNavigation::Kind::GoTo, {}, pageIndex);
    }

    static void cbGotoUrl(FPDF_FORMFILLINFO* self, FPDF_DOCUMENT, FPDF_WIDESTRING url) {
        from(self)->blockNavigation(BlockedNavigation::Kind::Uri, wideToUtf8(url), -1);
    }

    // 沒有真正的可視區時回報整頁而不是零矩形：零矩形會讓 PDFium 判定所有 widget
    // 都在畫面外，於是外觀更新被靜默跳過。
    static void cbGetPageViewRect(FPDF_FORMFILLINFO*, FPDF_PAGE page, double* left, double* top,
                                  double* right, double* bottom) {
        if (left) *left = 0.0;
        if (bottom) *bottom = 0.0;
        if (right) *right = page ? static_cast<double>(FPDF_GetPageWidthF(page)) : 0.0;
        if (top) *top = page ? static_cast<double>(FPDF_GetPageHeightF(page)) : 0.0;
    }

    static void cbPageEvent(FPDF_FORMFILLINFO*, int, FPDF_DWORD) {}

    static FPDF_BOOL cbPopupMenu(FPDF_FORMFILLINFO*, FPDF_PAGE, FPDF_WIDGET, int, float, float) {
        return 0;
    }

    static void cbOnFocusChange(FPDF_FORMFILLINFO*, FPDF_ANNOTATION, int) {}
};

FormEnvironment::FormEnvironment() : impl_(std::make_unique<Impl>()) {}

FormEnvironment::~FormEnvironment() { detach(); }

bool FormEnvironment::attach(void* document, PageLoader loader, PageIndexResolver resolver) {
    detach();
    if (!document) return false;

    impl_->document = static_cast<FPDF_DOCUMENT>(document);
    impl_->loader = std::move(loader);
    impl_->resolver = std::move(resolver);

    FPDF_FORMFILLINFO& info = impl_->info;
    std::memset(&info, 0, sizeof(info));
    // version 2 才會收到 FFI_OnFocusChange 等實驗性回呼。本建置未含 XFA，
    // 規格允許 1 或 2；選 2 是為了拿到焦點變更通知（Fields 面板需要）。
    info.version = 2;
    info.Release = &Impl::cbRelease;
    info.FFI_Invalidate = &Impl::cbInvalidate;
    info.FFI_OutputSelectedRect = &Impl::cbOutputSelectedRect;
    info.FFI_SetCursor = &Impl::cbSetCursor;
    info.FFI_SetTimer = &Impl::cbSetTimer;
    info.FFI_KillTimer = &Impl::cbKillTimer;
    info.FFI_GetLocalTime = &Impl::cbGetLocalTime;
    info.FFI_OnChange = &Impl::cbOnChange;
    info.FFI_GetPage = &Impl::cbGetPage;
    info.FFI_GetCurrentPage = &Impl::cbGetCurrentPage;
    info.FFI_GetRotation = &Impl::cbGetRotation;
    info.FFI_ExecuteNamedAction = &Impl::cbExecuteNamedAction;
    info.FFI_SetTextFieldFocus = &Impl::cbSetTextFieldFocus;
    info.FFI_DoURIAction = &Impl::cbDoUriAction;
    info.FFI_DoGoToAction = &Impl::cbDoGoToAction;
    info.FFI_DisplayCaret = &Impl::cbDisplayCaret;
    info.FFI_GetCurrentPageIndex = &Impl::cbGetCurrentPageIndex;
    info.FFI_SetCurrentPage = &Impl::cbSetCurrentPage;
    info.FFI_GotoURL = &Impl::cbGotoUrl;
    info.FFI_GetPageViewRect = &Impl::cbGetPageViewRect;
    info.FFI_PageEvent = &Impl::cbPageEvent;
    info.FFI_PopupMenu = &Impl::cbPopupMenu;
    info.FFI_OnFocusChange = &Impl::cbOnFocusChange;
    info.FFI_DoURIActionWithKeyboardModifier = &Impl::cbDoUriActionWithModifier;

    // 這兩行是安全立場而不是最佳化：沒有 JS 平台、明確宣告不要 XFA。
    // 對應的是「絕不呼叫 FPDF_LoadXFA」——兩者缺一，換一份 dll 就會破功。
    info.m_pJsPlatform = nullptr;
    info.xfa_disabled = 1;

    impl_->handle = FPDFDOC_InitFormFillEnvironment(impl_->document, &info);
    if (!impl_->handle) {
        impl_->document = nullptr;
        impl_->loader = nullptr;
        impl_->resolver = nullptr;
        return false;
    }
    return true;
}

void FormEnvironment::detach() {
    if (impl_->handle) {
        FPDFDOC_ExitFormFillEnvironment(impl_->handle);
        impl_->handle = nullptr;
    }
    impl_->document = nullptr;
    impl_->loader = nullptr;
    impl_->resolver = nullptr;
    impl_->invalidations.clear();
    impl_->blocked.clear();
    impl_->changes = 0;
    impl_->highlight = false;
}

bool FormEnvironment::valid() const noexcept { return impl_->handle != nullptr; }

void* FormEnvironment::handle() const noexcept { return impl_->handle; }

void FormEnvironment::setFieldHighlight(std::int32_t fieldType, std::uint32_t rgb,
                                        std::uint8_t alpha) {
    if (!impl_->handle) return;
    const int type = fieldType < 0 ? FPDF_FORMFIELD_UNKNOWN : fieldType;
    FPDF_SetFormFieldHighlightColor(impl_->handle, type, rgb & 0x00FFFFFFu);
    FPDF_SetFormFieldHighlightAlpha(impl_->handle, alpha);
    impl_->highlight = true;
}

void FormEnvironment::removeFieldHighlight() {
    if (!impl_->handle) return;
    FPDF_RemoveFormFieldHighlight(impl_->handle);
    impl_->highlight = false;
}

bool FormEnvironment::highlightEnabled() const noexcept { return impl_->highlight; }

const std::vector<InvalidatedRegion>& FormEnvironment::invalidations() const noexcept {
    return impl_->invalidations;
}

void FormEnvironment::clearInvalidations() { impl_->invalidations.clear(); }

std::int32_t FormEnvironment::changeCount() const noexcept { return impl_->changes; }

void FormEnvironment::resetChangeCount() { impl_->changes = 0; }

const std::vector<BlockedNavigation>& FormEnvironment::blockedNavigations() const noexcept {
    return impl_->blocked;
}

void FormEnvironment::clearBlockedNavigations() { impl_->blocked.clear(); }

}  // namespace alioth::engine::forms
