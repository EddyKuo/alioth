#include "engine/forms/form_document.h"

#include <fpdfview.h>
#include <fpdf_annot.h>
#include <fpdf_flatten.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "engine/file_source.h"
#include "engine/pdfium_library.h"
#include "engine/pdfium_lock.h"

namespace alioth::engine::forms {
namespace {

std::vector<char32_t> decodeUtf8(std::string_view text) {
    std::vector<char32_t> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        char32_t code = lead;
        std::size_t extra = 0;
        if ((lead & 0xE0) == 0xC0) { code = lead & 0x1Fu; extra = 1; }
        else if ((lead & 0xF0) == 0xE0) { code = lead & 0x0Fu; extra = 2; }
        else if ((lead & 0xF8) == 0xF0) { code = lead & 0x07u; extra = 3; }
        if (extra > 0 && i + extra < text.size()) {
            for (std::size_t k = 1; k <= extra; ++k) {
                code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
            }
            i += extra;
        }
        ++i;
        out.push_back(code);
    }
    return out;
}

// UTF-8 → 以 NUL 結尾的 UTF-16LE，供 FPDF_WIDESTRING 使用。
std::vector<unsigned short> toWide(std::string_view text) {
    std::vector<unsigned short> out;
    for (const char32_t code : decodeUtf8(text)) {
        if (code >= 0x10000) {
            const char32_t v = code - 0x10000;
            out.push_back(static_cast<unsigned short>(0xD800 + (v >> 10)));
            out.push_back(static_cast<unsigned short>(0xDC00 + (v & 0x3FF)));
        } else {
            out.push_back(static_cast<unsigned short>(code));
        }
    }
    out.push_back(0);
    return out;
}

std::string fromWide(const std::vector<unsigned short>& units) {
    std::string out;
    for (std::size_t i = 0; i < units.size(); ++i) {
        char32_t code = units[i];
        if (code == 0) break;
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < units.size()) {
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

// PDFium 的字串 API 一律「先問長度再取值」，長度以位元組計而不是字元數。
// 這個模式抄錯會少一個字或多一個 NUL，包成函式免得每個呼叫點各錯一次。
template <typename Fn>
std::string readWideString(Fn&& fn) {
    const unsigned long bytes = fn(nullptr, 0);
    if (bytes <= 2) return {};
    std::vector<unsigned short> buffer(bytes / 2 + 1, 0);
    fn(buffer.data(), bytes);
    return fromWide(buffer);
}

FormFieldType translateFieldType(int type) {
    switch (type) {
        case FPDF_FORMFIELD_PUSHBUTTON:  return FormFieldType::PushButton;
        case FPDF_FORMFIELD_CHECKBOX:    return FormFieldType::CheckBox;
        case FPDF_FORMFIELD_RADIOBUTTON: return FormFieldType::RadioButton;
        case FPDF_FORMFIELD_COMBOBOX:    return FormFieldType::ComboBox;
        case FPDF_FORMFIELD_LISTBOX:     return FormFieldType::ListBox;
        case FPDF_FORMFIELD_TEXTFIELD:   return FormFieldType::TextField;
        case FPDF_FORMFIELD_SIGNATURE:   return FormFieldType::Signature;
        case FPDF_FORMFIELD_UNKNOWN:     return FormFieldType::Unknown;
        default:                         return FormFieldType::Xfa;
    }
}

int nativeFieldType(FormFieldType type) {
    switch (type) {
        case FormFieldType::PushButton:  return FPDF_FORMFIELD_PUSHBUTTON;
        case FormFieldType::CheckBox:    return FPDF_FORMFIELD_CHECKBOX;
        case FormFieldType::RadioButton: return FPDF_FORMFIELD_RADIOBUTTON;
        case FormFieldType::ComboBox:    return FPDF_FORMFIELD_COMBOBOX;
        case FormFieldType::ListBox:     return FPDF_FORMFIELD_LISTBOX;
        case FormFieldType::TextField:   return FPDF_FORMFIELD_TEXTFIELD;
        case FormFieldType::Signature:   return FPDF_FORMFIELD_SIGNATURE;
        default:                         return FPDF_FORMFIELD_UNKNOWN;
    }
}

FormType translateFormType(int type) {
    switch (type) {
        case FORMTYPE_ACRO_FORM:      return FormType::AcroForm;
        case FORMTYPE_XFA_FULL:       return FormType::XfaFull;
        case FORMTYPE_XFA_FOREGROUND: return FormType::XfaForeground;
        default:                      return FormType::None;
    }
}

FormFieldFlags translateFlags(int flags) {
    FormFieldFlags out;
    out.readOnly = (flags & FPDF_FORMFLAG_READONLY) != 0;
    out.required = (flags & FPDF_FORMFLAG_REQUIRED) != 0;
    out.noExport = (flags & FPDF_FORMFLAG_NOEXPORT) != 0;
    out.multiline = (flags & FPDF_FORMFLAG_TEXT_MULTILINE) != 0;
    out.password = (flags & FPDF_FORMFLAG_TEXT_PASSWORD) != 0;
    out.comboEditable = (flags & FPDF_FORMFLAG_CHOICE_EDIT) != 0;
    out.multiSelect = (flags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT) != 0;
    return out;
}

// FPDF_SaveAsCopy 的輸出接口。攤平後另存新檔專用。
// 用 std::ofstream 而不是 std::fopen：後者在 MSVC 上是被標記為不安全的介面，
// 而安全的替代品 fopen_s 只有 Windows 有——那會在引擎轉接層留下平台分支。
struct FileWriter : FPDF_FILEWRITE {
    std::ofstream* stream{nullptr};
    bool failed{false};

    static int write(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
        auto* writer = static_cast<FileWriter*>(self);
        if (!writer->stream) return 0;
        writer->stream->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!writer->stream->good()) {
            writer->failed = true;
            return 0;
        }
        return 1;
    }
};

}  // namespace

struct FormTask {
    std::function<void()> run;
};

// 頁面是否已經對表單環境宣告過。分開記是因為 FPDFDOC_InitFormFillEnvironment
// 期間 PDFium 可能反過來呼叫 FFI_GetPage，那時 handle 還沒回來，
// FORM_OnAfterLoadPage 送不出去；漏送的後果是那一頁的 widget 永遠不建立，
// 之後所有事件都靜默無效——沒有任何錯誤訊息。
struct PageEntry {
    FPDF_PAGE page{nullptr};
    bool announced{false};
};

struct FormDocument::Impl {
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable idleCv;
    std::deque<FormTask> queue;
    bool running{true};
    bool busy{false};
    std::int32_t pageCount{0};

    // 以下成員只能由表單執行緒觸碰。
    FPDF_DOCUMENT document{nullptr};
    FileSource source;
    FormEnvironment environment;
    std::unordered_map<std::int32_t, PageEntry> pages;

    void enqueue(std::function<void()> run) {
        {
            std::lock_guard lock(mutex);
            if (!running) return;
            queue.push_back(FormTask{std::move(run)});
        }
        cv.notify_one();
    }

    FPDF_PAGE acquirePage(std::int32_t index) {
        if (!document || index < 0) return nullptr;
        PageEntry* entry = nullptr;
        if (const auto it = pages.find(index); it != pages.end()) {
            entry = &it->second;
        } else {
            FPDF_PAGE page = FPDF_LoadPage(document, index);
            if (!page) return nullptr;
            entry = &pages.emplace(index, PageEntry{page, false}).first->second;
        }
        if (!entry->announced && environment.valid()) {
            FORM_OnAfterLoadPage(entry->page, static_cast<FPDF_FORMHANDLE>(environment.handle()));
            entry->announced = true;
        }
        return entry->page;
    }

    std::int32_t indexOfPage(void* page) const {
        for (const auto& entry : pages) {
            if (entry.second.page == page) return entry.first;
        }
        return -1;
    }

    void releasePages() {
        for (auto& entry : pages) {
            if (entry.second.announced && environment.valid()) {
                FORM_OnBeforeClosePage(entry.second.page,
                                       static_cast<FPDF_FORMHANDLE>(environment.handle()));
            }
            FPDF_ClosePage(entry.second.page);
        }
        pages.clear();
    }

    void closeDocument() {
        releasePages();
        environment.detach();
        if (document) {
            FPDF_CloseDocument(document);
            document = nullptr;
        }
        source.close();
        std::lock_guard lock(mutex);
        pageCount = 0;
    }

    // 取得指定 widget 的把手。呼叫端負責 FPDFPage_CloseAnnot。
    FPDF_ANNOTATION acquireWidget(std::int32_t pageIndex, std::int32_t annotIndex) {
        FPDF_PAGE page = acquirePage(pageIndex);
        if (!page) return nullptr;
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, annotIndex);
        if (!annot) return nullptr;
        if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_WIDGET) {
            FPDFPage_CloseAnnot(annot);
            return nullptr;
        }
        return annot;
    }

    FormFieldInfo describeWidget(std::int32_t pageIndex, std::int32_t annotIndex,
                                 FPDF_ANNOTATION annot) {
        auto handle = static_cast<FPDF_FORMHANDLE>(environment.handle());
        FormFieldInfo info;
        info.pageIndex = pageIndex;
        info.annotIndex = annotIndex;
        info.type = translateFieldType(FPDFAnnot_GetFormFieldType(handle, annot));
        info.name = readWideString([&](unsigned short* buf, unsigned long len) {
            return FPDFAnnot_GetFormFieldName(handle, annot, buf, len);
        });
        info.alternateName = readWideString([&](unsigned short* buf, unsigned long len) {
            return FPDFAnnot_GetFormFieldAlternateName(handle, annot, buf, len);
        });
        info.value = readWideString([&](unsigned short* buf, unsigned long len) {
            return FPDFAnnot_GetFormFieldValue(handle, annot, buf, len);
        });
        info.exportValue = readWideString([&](unsigned short* buf, unsigned long len) {
            return FPDFAnnot_GetFormFieldExportValue(handle, annot, buf, len);
        });
        info.flags = translateFlags(FPDFAnnot_GetFormFieldFlags(handle, annot));

        if (info.isButton()) {
            info.checked = FPDFAnnot_IsChecked(handle, annot) != 0;
        }

        if (info.isChoice()) {
            const int count = FPDFAnnot_GetOptionCount(handle, annot);
            for (int i = 0; i < count; ++i) {
                info.options.push_back(readWideString([&](unsigned short* buf, unsigned long len) {
                    return FPDFAnnot_GetOptionLabel(handle, annot, i, buf, len);
                }));
                if (FPDFAnnot_IsOptionSelected(handle, annot, i)) {
                    info.selectedIndices.push_back(i);
                }
            }
        }

        FS_RECTF rect{};
        if (FPDFAnnot_GetRect(annot, &rect)) {
            // PDFium 的 /Rect 不保證已正規化，直接拿來當左下右上會出現負高度。
            info.rectPt = domain::RectF{std::min(rect.left, rect.right),
                                        std::min(rect.bottom, rect.top),
                                        std::max(rect.left, rect.right),
                                        std::max(rect.bottom, rect.top)};
        }
        return info;
    }

    std::vector<FormFieldInfo> fieldsOnPage(std::int32_t pageIndex) {
        std::vector<FormFieldInfo> out;
        if (!environment.valid()) return out;
        FPDF_PAGE page = acquirePage(pageIndex);
        if (!page) return out;
        const int count = FPDFPage_GetAnnotCount(page);
        for (int i = 0; i < count; ++i) {
            FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
            if (!annot) continue;
            if (FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_WIDGET) {
                out.push_back(describeWidget(pageIndex, i, annot));
            }
            FPDFPage_CloseAnnot(annot);
        }
        return out;
    }

    // 文字類欄位的寫入走「聚焦 → 全選 → 取代」而不是直接改 /V。
    // 直接改字典會讓外觀串流與值不同步，畫面上還是舊字；
    // 走事件路徑則由 PDFium 重建 /AP，這也是 WBS 6.1 事件橋接存在的理由。
    bool replaceTextValue(std::int32_t pageIndex, FPDF_ANNOTATION annot, std::string_view value) {
        auto handle = static_cast<FPDF_FORMHANDLE>(environment.handle());
        FPDF_PAGE page = acquirePage(pageIndex);
        if (!page || !handle) return false;
        if (!FORM_SetFocusedAnnot(handle, annot)) return false;
        FORM_SelectAllText(handle, page);
        const auto wide = toWide(value);
        FORM_ReplaceSelection(handle, page, wide.data());
        // 不殺焦點就不會寫回 /V，讀回來的仍是舊值。
        FORM_ForceToKillFocus(handle);
        return true;
    }
};

FormDocument::FormDocument() : impl_(std::make_unique<Impl>()) {
    impl_->worker = std::thread([this] {
        const PdfiumRuntime runtime;
        for (;;) {
            FormTask task;
            {
                std::unique_lock lock(impl_->mutex);
                impl_->cv.wait(lock, [this] { return !impl_->running || !impl_->queue.empty(); });
                if (!impl_->running && impl_->queue.empty()) break;
                task = std::move(impl_->queue.front());
                impl_->queue.pop_front();
                impl_->busy = true;
            }
            // 行程級序列化（ADR-005）。表單與檢視器渲染是兩條不同的執行緒，
            // 而 PDFium 的行程級狀態不容許兩條執行緒同時進去。
            if (task.run) {
                const PdfiumGuard guard;
                task.run();
            }
            {
                std::lock_guard lock(impl_->mutex);
                impl_->busy = false;
            }
            impl_->idleCv.notify_all();
        }
        impl_->closeDocument();
    });
}

FormDocument::~FormDocument() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->running = false;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void FormDocument::open(std::string path, std::string password,
                        std::function<void(domain::DocumentError)> callback) {
    impl_->enqueue([this, path = std::move(path), password = std::move(password),
                    callback = std::move(callback)] {
        impl_->closeDocument();

        // 與渲染子系統一樣走 FPDF_LoadCustomDocument：不整份讀進記憶體，
        // 也不持有會擋住存檔原子更名的檔案控制代碼。
        if (!impl_->source.open(path)) {
            if (callback) callback(domain::DocumentError::FileNotFound);
            return;
        }
        FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(
            static_cast<FPDF_FILEACCESS*>(impl_->source.fileAccess()),
            password.empty() ? nullptr : password.c_str());
        if (!doc) {
            impl_->source.close();
            const unsigned long error = FPDF_GetLastError();
            domain::DocumentError translated = domain::DocumentError::Unknown;
            switch (error) {
                case FPDF_ERR_FILE:     translated = domain::DocumentError::FileNotFound; break;
                case FPDF_ERR_FORMAT:   translated = domain::DocumentError::NotAPdf; break;
                case FPDF_ERR_PASSWORD: translated = domain::DocumentError::WrongPassword; break;
                case FPDF_ERR_PAGE:     translated = domain::DocumentError::CorruptXref; break;
                default: break;
            }
            if (callback) callback(translated);
            return;
        }

        impl_->document = doc;
        {
            std::lock_guard lock(impl_->mutex);
            impl_->pageCount = FPDF_GetPageCount(doc);
        }

        // 表單環境對「沒有表單的文件」也會成功建立，這是 PDFium 的行為。
        // 因此不能用 attach 的成敗判斷有沒有表單——那要看 FPDF_GetFormType。
        //
        // 反過來說，attach 真的失敗時只代表表單功能失效，閱讀不受影響，
        // 所以不把它升級成開檔失敗；identify() 會回報無表單，UI 據此灰化表單工具。
        const bool environmentReady = impl_->environment.attach(
            doc, [this](std::int32_t index) -> void* { return impl_->acquirePage(index); },
            [this](void* page) { return impl_->indexOfPage(page); });
        (void)environmentReady;

        // 刻意不呼叫 FPDF_LoadXFA。CLAUDE.md 的立場是不支援 XFA，
        // 只顯示後備內容並提示（PRD-FORM-003）。
        if (callback) callback(domain::DocumentError::None);
    });
}

void FormDocument::close(std::function<void()> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        impl_->closeDocument();
        if (callback) callback();
    });
}

std::int32_t FormDocument::pageCount() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->pageCount;
}

void FormDocument::identify(std::function<void(FormIdentification)> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        FormIdentification result;
        if (!impl_->document) {
            if (callback) callback(result);
            return;
        }
        result.formType = translateFormType(FPDF_GetFormType(impl_->document));
        result.hasAcroFormDictionary = result.formType != FormType::None;

        const std::int32_t pages = FPDF_GetPageCount(impl_->document);
        for (std::int32_t i = 0; i < pages; ++i) {
            const auto fields = impl_->fieldsOnPage(i);
            if (fields.empty()) continue;
            result.fieldCount += static_cast<std::int32_t>(fields.size());
            result.pageIndicesWithFields.push_back(i);
        }
        result.pagesWithFields = static_cast<std::int32_t>(result.pageIndicesWithFields.size());
        if (callback) callback(std::move(result));
    });
}

void FormDocument::xfaReport(std::function<void(XfaReport)> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        if (!impl_->document) {
            if (callback) callback(XfaReport{});
            return;
        }
        const FormType type = translateFormType(FPDF_GetFormType(impl_->document));
        bool acroFields = false;
        const std::int32_t pages = FPDF_GetPageCount(impl_->document);
        for (std::int32_t i = 0; i < pages && !acroFields; ++i) {
            for (const auto& field : impl_->fieldsOnPage(i)) {
                if (field.type != FormFieldType::Unknown && field.type != FormFieldType::Xfa) {
                    acroFields = true;
                    break;
                }
            }
        }
        if (callback) callback(describeXfa(type, acroFields));
    });
}

void FormDocument::fields(std::int32_t pageIndex,
                          std::function<void(std::vector<FormFieldInfo>)> callback) {
    impl_->enqueue([this, pageIndex, callback = std::move(callback)] {
        auto result = impl_->fieldsOnPage(pageIndex);
        if (callback) callback(std::move(result));
    });
}

void FormDocument::allFields(std::function<void(std::vector<FormFieldInfo>)> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        std::vector<FormFieldInfo> out;
        if (impl_->document) {
            const std::int32_t pages = FPDF_GetPageCount(impl_->document);
            for (std::int32_t i = 0; i < pages; ++i) {
                auto page = impl_->fieldsOnPage(i);
                out.insert(out.end(), page.begin(), page.end());
            }
        }
        if (callback) callback(std::move(out));
    });
}

void FormDocument::fieldAt(std::int32_t pageIndex, domain::PointF pagePoint,
                           std::function<void(std::optional<FormFieldInfo>)> callback) {
    impl_->enqueue([this, pageIndex, pagePoint, callback = std::move(callback)] {
        std::optional<FormFieldInfo> result;
        FPDF_PAGE page = impl_->acquirePage(pageIndex);
        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        if (page && handle) {
            FS_POINTF point{static_cast<float>(pagePoint.x), static_cast<float>(pagePoint.y)};
            FPDF_ANNOTATION annot = FPDFAnnot_GetFormFieldAtPoint(handle, page, &point);
            if (annot) {
                const int index = FPDFPage_GetAnnotIndex(page, annot);
                result = impl_->describeWidget(pageIndex, index, annot);
                FPDFPage_CloseAnnot(annot);
            }
        }
        if (callback) callback(std::move(result));
    });
}

void FormDocument::setTextValue(std::int32_t pageIndex, std::int32_t annotIndex, std::string value,
                                std::function<void(FormFillResult)> callback) {
    impl_->enqueue([this, pageIndex, annotIndex, value = std::move(value),
                    callback = std::move(callback)] {
        FormFillResult result;
        FPDF_ANNOTATION annot = impl_->acquireWidget(pageIndex, annotIndex);
        if (!annot) {
            result.error = "找不到指定的表單欄位（頁碼或欄位索引不正確）。";
            if (callback) callback(std::move(result));
            return;
        }
        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        const FormFieldType type = translateFieldType(FPDFAnnot_GetFormFieldType(handle, annot));
        const FormFieldFlags flags = translateFlags(FPDFAnnot_GetFormFieldFlags(handle, annot));
        if (flags.readOnly) {
            result.error = "此欄位為唯讀，文件不允許填寫。";
        } else if (type != FormFieldType::TextField && type != FormFieldType::ComboBox) {
            result.error = "只有文字欄位與可編輯下拉方塊能以文字填寫。";
        } else if (!impl_->replaceTextValue(pageIndex, annot, value)) {
            result.error = "表單環境拒絕了這次寫入，欄位可能無法取得焦點。";
        } else {
            result.ok = true;
        }
        FPDFPage_CloseAnnot(annot);
        if (callback) callback(std::move(result));
    });
}

void FormDocument::setChecked(std::int32_t pageIndex, std::int32_t annotIndex, bool checked,
                              std::function<void(FormFillResult)> callback) {
    impl_->enqueue([this, pageIndex, annotIndex, checked, callback = std::move(callback)] {
        FormFillResult result;
        FPDF_ANNOTATION annot = impl_->acquireWidget(pageIndex, annotIndex);
        FPDF_PAGE page = impl_->acquirePage(pageIndex);
        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        if (!annot || !page || !handle) {
            result.error = "找不到指定的表單欄位（頁碼或欄位索引不正確）。";
            if (annot) FPDFPage_CloseAnnot(annot);
            if (callback) callback(std::move(result));
            return;
        }

        const FormFieldType type = translateFieldType(FPDFAnnot_GetFormFieldType(handle, annot));
        const FormFieldFlags flags = translateFlags(FPDFAnnot_GetFormFieldFlags(handle, annot));
        if (flags.readOnly) {
            result.error = "此欄位為唯讀，文件不允許填寫。";
        } else if (type != FormFieldType::CheckBox && type != FormFieldType::RadioButton) {
            result.error = "只有核取方塊與單選按鈕能設定勾選狀態。";
        } else if ((FPDFAnnot_IsChecked(handle, annot) != 0) == checked) {
            // 已經是目標狀態就不送事件：重送會把核取方塊切回去。
            result.ok = true;
        } else {
            FS_RECTF rect{};
            FPDFAnnot_GetRect(annot, &rect);
            const double x = (static_cast<double>(rect.left) + rect.right) / 2.0;
            const double y = (static_cast<double>(rect.top) + rect.bottom) / 2.0;
            // 走真正的滑鼠事件而不是改字典：勾選會連動同群組其他 widget 的
            // /AS 與外觀，那段邏輯在 PDFium 裡面，自己改一定漏。
            FORM_OnLButtonDown(handle, page, 0, x, y);
            FORM_OnLButtonUp(handle, page, 0, x, y);
            FORM_ForceToKillFocus(handle);
            result.ok = (FPDFAnnot_IsChecked(handle, annot) != 0) == checked;
            if (!result.ok) result.error = "勾選狀態未依預期改變，欄位可能被文件鎖定。";
        }
        FPDFPage_CloseAnnot(annot);
        if (callback) callback(std::move(result));
    });
}

void FormDocument::setSelectedIndices(std::int32_t pageIndex, std::int32_t annotIndex,
                                      std::vector<std::int32_t> indices,
                                      std::function<void(FormFillResult)> callback) {
    impl_->enqueue([this, pageIndex, annotIndex, indices = std::move(indices),
                    callback = std::move(callback)] {
        FormFillResult result;
        FPDF_ANNOTATION annot = impl_->acquireWidget(pageIndex, annotIndex);
        FPDF_PAGE page = impl_->acquirePage(pageIndex);
        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        if (!annot || !page || !handle) {
            result.error = "找不到指定的表單欄位（頁碼或欄位索引不正確）。";
            if (annot) FPDFPage_CloseAnnot(annot);
            if (callback) callback(std::move(result));
            return;
        }

        const FormFieldType type = translateFieldType(FPDFAnnot_GetFormFieldType(handle, annot));
        const FormFieldFlags flags = translateFlags(FPDFAnnot_GetFormFieldFlags(handle, annot));
        const int optionCount = FPDFAnnot_GetOptionCount(handle, annot);
        if (flags.readOnly) {
            result.error = "此欄位為唯讀，文件不允許填寫。";
        } else if (type != FormFieldType::ComboBox && type != FormFieldType::ListBox) {
            result.error = "只有下拉方塊與清單方塊能設定選取項目。";
        } else if (!FORM_SetFocusedAnnot(handle, annot)) {
            result.error = "欄位無法取得焦點，選取狀態未變更。";
        } else {
            result.ok = true;
            for (int i = 0; i < optionCount; ++i) {
                const bool wanted =
                    std::find(indices.begin(), indices.end(), i) != indices.end();
                if (FORM_IsIndexSelected(handle, page, i) == (wanted ? 1 : 0)) continue;
                if (!FORM_SetIndexSelected(handle, page, i, wanted ? 1 : 0)) {
                    result.ok = false;
                    result.error = "PDFium 拒絕變更第 " + std::to_string(i) + " 個選項的選取狀態。";
                    break;
                }
            }
            FORM_ForceToKillFocus(handle);
        }
        FPDFPage_CloseAnnot(annot);
        if (callback) callback(std::move(result));
    });
}

void FormDocument::mouseDown(std::int32_t pageIndex, domain::PointF pagePoint, int modifiers,
                             std::function<void(bool)> callback) {
    impl_->enqueue([this, pageIndex, pagePoint, modifiers, callback = std::move(callback)] {
        FPDF_PAGE page = impl_->acquirePage(pageIndex);
        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        const bool ok = page && handle &&
                        FORM_OnLButtonDown(handle, page, modifiers, pagePoint.x, pagePoint.y);
        if (callback) callback(ok);
    });
}

void FormDocument::mouseUp(std::int32_t pageIndex, domain::PointF pagePoint, int modifiers,
                           std::function<void(bool)> callback) {
    impl_->enqueue([this, pageIndex, pagePoint, modifiers, callback = std::move(callback)] {
        FPDF_PAGE page = impl_->acquirePage(pageIndex);
        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        const bool ok = page && handle &&
                        FORM_OnLButtonUp(handle, page, modifiers, pagePoint.x, pagePoint.y);
        if (callback) callback(ok);
    });
}

void FormDocument::typeText(std::int32_t pageIndex, std::string utf8, int modifiers,
                            std::function<void(bool)> callback) {
    impl_->enqueue([this, pageIndex, utf8 = std::move(utf8), modifiers,
                    callback = std::move(callback)] {
        FPDF_PAGE page = impl_->acquirePage(pageIndex);
        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        bool ok = page != nullptr && handle != nullptr;
        if (ok) {
            for (const char32_t code : decodeUtf8(utf8)) {
                if (!FORM_OnChar(handle, page, static_cast<int>(code), modifiers)) {
                    ok = false;
                    break;
                }
            }
        }
        if (callback) callback(ok);
    });
}

void FormDocument::killFocus(std::function<void(bool)> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        const bool ok = handle && FORM_ForceToKillFocus(handle);
        if (callback) callback(ok);
    });
}

void FormDocument::setFieldHighlight(std::optional<FormFieldType> type, std::uint32_t rgb,
                                     std::uint8_t alpha, std::function<void()> callback) {
    impl_->enqueue([this, type, rgb, alpha, callback = std::move(callback)] {
        impl_->environment.setFieldHighlight(type ? nativeFieldType(*type) : -1, rgb, alpha);
        if (callback) callback();
    });
}

void FormDocument::clearFieldHighlight(std::function<void()> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        impl_->environment.removeFieldHighlight();
        if (callback) callback();
    });
}

void FormDocument::exportData(FormDataFormat format, std::string sourcePath,
                              std::function<void(std::string)> callback) {
    impl_->enqueue([this, format, sourcePath = std::move(sourcePath),
                    callback = std::move(callback)] {
        std::vector<FormFieldInfo> all;
        if (impl_->document) {
            const std::int32_t pages = FPDF_GetPageCount(impl_->document);
            for (std::int32_t i = 0; i < pages; ++i) {
                auto page = impl_->fieldsOnPage(i);
                all.insert(all.end(), page.begin(), page.end());
            }
        }
        auto text = exportFormData(all, format, sourcePath);
        if (callback) callback(std::move(text));
    });
}

void FormDocument::importData(std::string text, FormDataFormat format,
                              std::function<void(FormDataImport)> callback) {
    impl_->enqueue([this, text = std::move(text), format, callback = std::move(callback)] {
        FormDataImport parsed = importFormData(text, format);
        if (!parsed.ok || !impl_->document) {
            if (callback) callback(std::move(parsed));
            return;
        }

        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        const std::int32_t pages = FPDF_GetPageCount(impl_->document);
        for (std::int32_t pageIndex = 0; pageIndex < pages; ++pageIndex) {
            for (const auto& field : impl_->fieldsOnPage(pageIndex)) {
                const auto entry = std::find_if(
                    parsed.entries.begin(), parsed.entries.end(),
                    [&](const FormDataEntry& e) { return e.name == field.name; });
                if (entry == parsed.entries.end()) continue;
                if (field.flags.readOnly) continue;

                FPDF_ANNOTATION annot = impl_->acquireWidget(pageIndex, field.annotIndex);
                if (!annot) continue;

                switch (field.type) {
                    case FormFieldType::TextField:
                    case FormFieldType::ComboBox:
                        if (field.type == FormFieldType::ComboBox && !field.options.empty()) {
                            const auto it = std::find(field.options.begin(), field.options.end(),
                                                      entry->primary());
                            if (it != field.options.end()) {
                                const int index =
                                    static_cast<int>(std::distance(field.options.begin(), it));
                                if (FORM_SetFocusedAnnot(handle, annot)) {
                                    FPDF_PAGE page = impl_->acquirePage(pageIndex);
                                    if (page) FORM_SetIndexSelected(handle, page, index, 1);
                                    FORM_ForceToKillFocus(handle);
                                }
                                break;
                            }
                        }
                        impl_->replaceTextValue(pageIndex, annot, entry->primary());
                        break;
                    case FormFieldType::ListBox: {
                        FPDF_PAGE page = impl_->acquirePage(pageIndex);
                        if (page && FORM_SetFocusedAnnot(handle, annot)) {
                            for (int i = 0; i < static_cast<int>(field.options.size()); ++i) {
                                const bool wanted =
                                    std::find(entry->values.begin(), entry->values.end(),
                                              field.options[static_cast<std::size_t>(i)]) !=
                                    entry->values.end();
                                if (FORM_IsIndexSelected(handle, page, i) != (wanted ? 1 : 0)) {
                                    FORM_SetIndexSelected(handle, page, i, wanted ? 1 : 0);
                                }
                            }
                            FORM_ForceToKillFocus(handle);
                        }
                        break;
                    }
                    case FormFieldType::CheckBox:
                    case FormFieldType::RadioButton: {
                        const std::string& state = entry->primary();
                        const bool wanted = !state.empty() && state != "Off";
                        if ((FPDFAnnot_IsChecked(handle, annot) != 0) != wanted) {
                            FPDF_PAGE page = impl_->acquirePage(pageIndex);
                            FS_RECTF rect{};
                            FPDFAnnot_GetRect(annot, &rect);
                            if (page) {
                                const double x = (static_cast<double>(rect.left) + rect.right) / 2.0;
                                const double y = (static_cast<double>(rect.top) + rect.bottom) / 2.0;
                                FORM_OnLButtonDown(handle, page, 0, x, y);
                                FORM_OnLButtonUp(handle, page, 0, x, y);
                                FORM_ForceToKillFocus(handle);
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
                FPDFPage_CloseAnnot(annot);
            }
        }
        if (callback) callback(std::move(parsed));
    });
}

void FormDocument::resetForm(std::function<void(FormFillResult)> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        FormFillResult result;
        if (!impl_->document || !impl_->environment.valid()) {
            result.error = "尚未開啟文件，無法重設表單。";
            if (callback) callback(std::move(result));
            return;
        }
        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        const std::int32_t pages = FPDF_GetPageCount(impl_->document);
        for (std::int32_t pageIndex = 0; pageIndex < pages; ++pageIndex) {
            for (const auto& field : impl_->fieldsOnPage(pageIndex)) {
                if (field.flags.readOnly) continue;
                FPDF_ANNOTATION annot = impl_->acquireWidget(pageIndex, field.annotIndex);
                if (!annot) continue;

                // 重設的目標值是 /DV。PDFium 沒有欄位層級的 API，只能直接讀
                // widget 字典上的 /DV——因此**繼承自父欄位的 /DV 讀不到**，
                // 那種欄位會被重設成空值。這是已知偏差，見 form_document.h 的說明。
                const std::string defaultValue =
                    readWideString([&](unsigned short* buf, unsigned long len) {
                        return FPDFAnnot_GetStringValue(annot, "DV", buf, len);
                    });

                switch (field.type) {
                    case FormFieldType::TextField:
                        impl_->replaceTextValue(pageIndex, annot, defaultValue);
                        break;
                    case FormFieldType::ComboBox:
                    case FormFieldType::ListBox: {
                        FPDF_PAGE page = impl_->acquirePage(pageIndex);
                        if (page && FORM_SetFocusedAnnot(handle, annot)) {
                            const int count = FPDFAnnot_GetOptionCount(handle, annot);
                            for (int i = 0; i < count; ++i) {
                                const bool wanted =
                                    !defaultValue.empty() &&
                                    readWideString([&](unsigned short* buf, unsigned long len) {
                                        return FPDFAnnot_GetOptionLabel(handle, annot, i, buf, len);
                                    }) == defaultValue;
                                if (FORM_IsIndexSelected(handle, page, i) != (wanted ? 1 : 0)) {
                                    FORM_SetIndexSelected(handle, page, i, wanted ? 1 : 0);
                                }
                            }
                            FORM_ForceToKillFocus(handle);
                        }
                        break;
                    }
                    case FormFieldType::CheckBox:
                    case FormFieldType::RadioButton: {
                        const bool wanted = !defaultValue.empty() && defaultValue != "Off";
                        if ((FPDFAnnot_IsChecked(handle, annot) != 0) != wanted) {
                            FPDF_PAGE page = impl_->acquirePage(pageIndex);
                            FS_RECTF rect{};
                            FPDFAnnot_GetRect(annot, &rect);
                            if (page) {
                                const double x = (static_cast<double>(rect.left) + rect.right) / 2.0;
                                const double y = (static_cast<double>(rect.top) + rect.bottom) / 2.0;
                                FORM_OnLButtonDown(handle, page, 0, x, y);
                                FORM_OnLButtonUp(handle, page, 0, x, y);
                                FORM_ForceToKillFocus(handle);
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
                FPDFPage_CloseAnnot(annot);
            }
        }
        result.ok = true;
        if (callback) callback(std::move(result));
    });
}

void FormDocument::flatten(std::function<void(FormFillResult)> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        FormFillResult result;
        if (!impl_->document) {
            result.error = "尚未開啟文件，無法攤平表單。";
            if (callback) callback(std::move(result));
            return;
        }

        auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
        if (handle) FORM_ForceToKillFocus(handle);

        const std::int32_t pages = FPDF_GetPageCount(impl_->document);
        std::int32_t failures = 0;
        for (std::int32_t i = 0; i < pages; ++i) {
            FPDF_PAGE page = impl_->acquirePage(i);
            if (!page) { ++failures; continue; }
            // FLAT_NORMALDISPLAY：以螢幕顯示外觀攤平。FLAT_PRINT 會採用列印外觀，
            // 兩者在有 /D（按下狀態）外觀的 widget 上結果不同。
            const int status = FPDFPage_Flatten(page, FLAT_NORMALDISPLAY);
            if (status == FLATTEN_FAIL) ++failures;
        }

        // 攤平把 widget 併進內容串流並移除註解，表單環境的內部狀態因此全部過期。
        // 不重建就繼續用會讓後續列舉讀到已被釋放的 widget。
        impl_->releasePages();
        impl_->environment.detach();
        const bool environmentReady = impl_->environment.attach(
            impl_->document,
            [this](std::int32_t index) -> void* { return impl_->acquirePage(index); },
            [this](void* page) { return impl_->indexOfPage(page); });
        (void)environmentReady;

        result.ok = failures == 0;
        if (!result.ok) {
            result.error = "有 " + std::to_string(failures) + " 頁攤平失敗，表單欄位仍然存在。";
        }
        if (callback) callback(std::move(result));
    });
}

void FormDocument::saveCopy(std::string path, std::function<void(FormFillResult)> callback) {
    impl_->enqueue([this, path = std::move(path), callback = std::move(callback)] {
        FormFillResult result;
        if (!impl_->document) {
            result.error = "尚未開啟文件，無法另存新檔。";
            if (callback) callback(std::move(result));
            return;
        }
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            result.error = "無法建立輸出檔案：" + path;
            if (callback) callback(std::move(result));
            return;
        }
        FileWriter writer{};
        writer.version = 1;
        writer.WriteBlock = &FileWriter::write;
        writer.stream = &stream;
        // 攤平是破壞性改寫，不能走增量儲存（增量儲存的前提是純附加）。
        const bool ok = FPDF_SaveAsCopy(impl_->document, &writer, FPDF_NO_INCREMENTAL) != 0;
        stream.flush();
        stream.close();
        result.ok = ok && !writer.failed;
        if (!result.ok) result.error = "PDFium 寫出文件失敗。";
        if (callback) callback(std::move(result));
    });
}

void FormDocument::renderPage(std::int32_t pageIndex, std::int32_t widthPx, std::int32_t heightPx,
                              bool includeFormOverlay,
                              std::function<void(PixelBufferPtr)> callback) {
    impl_->enqueue([this, pageIndex, widthPx, heightPx, includeFormOverlay,
                    callback = std::move(callback)] {
        FPDF_PAGE page = impl_->acquirePage(pageIndex);
        if (!page || widthPx <= 0 || heightPx <= 0) {
            if (callback) callback(nullptr);
            return;
        }
        auto buffer = std::make_shared<PixelBuffer>(widthPx, heightPx);
        if (buffer->isNull()) {
            if (callback) callback(nullptr);
            return;
        }
        buffer->fill(0xFF);

        // stride 顯式傳入，不以「寬 × 4」推算（見 pixel_buffer.h）。
        FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(widthPx, heightPx, FPDFBitmap_BGRA,
                                                 buffer->data(), static_cast<int>(buffer->stride()));
        if (!bitmap) {
            if (callback) callback(nullptr);
            return;
        }
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, widthPx, heightPx, 0, FPDF_ANNOT);
        if (includeFormOverlay) {
            auto handle = static_cast<FPDF_FORMHANDLE>(impl_->environment.handle());
            // FPDF_FFLDraw 必須在頁面內容畫完之後才呼叫，否則 widget 會被內容蓋掉。
            if (handle) FPDF_FFLDraw(handle, bitmap, page, 0, 0, widthPx, heightPx, 0, FPDF_ANNOT);
        }
        FPDFBitmap_Destroy(bitmap);
        if (callback) callback(PixelBufferPtr{std::move(buffer)});
    });
}

void FormDocument::eventSnapshot(std::function<void(FormEventSnapshot)> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        FormEventSnapshot snapshot;
        snapshot.changeCount = impl_->environment.changeCount();
        snapshot.invalidations = impl_->environment.invalidations();
        snapshot.blockedNavigations = impl_->environment.blockedNavigations();
        if (callback) callback(std::move(snapshot));
    });
}

void FormDocument::clearEvents(std::function<void()> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        impl_->environment.clearInvalidations();
        impl_->environment.clearBlockedNavigations();
        impl_->environment.resetChangeCount();
        if (callback) callback();
    });
}

void FormDocument::waitForIdle() {
    std::unique_lock lock(impl_->mutex);
    impl_->idleCv.wait(lock, [this] { return impl_->queue.empty() && !impl_->busy; });
}

std::size_t FormDocument::pendingTaskCount() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->queue.size();
}

}  // namespace alioth::engine::forms
