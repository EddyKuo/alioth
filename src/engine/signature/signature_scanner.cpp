#include "engine/signature/signature_scanner.h"

#include <fpdfview.h>
#include <fpdf_signature.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "engine/file_source.h"
#include "engine/pdfium_library.h"
#include "engine/pdfium_lock.h"
#include "engine/signature/byte_range.h"
#include "engine/signature/pkcs7_verifier.h"
#include "platform/shared_file.h"

namespace alioth::engine::signature {
namespace {

std::string utf16ToUtf8(const std::vector<unsigned short>& units) {
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

// 依 /ByteRange 的區段從檔案讀出被簽章涵蓋的位元組。
//
// 這裡再檢查一次界線，即使 checkByteRange 已經驗過：讀檔是實際會踩到記憶體的
// 一步，而 /ByteRange 來自不可信任的檔案。兩道檢查的成本是幾個比較，
// 漏掉一道的成本是越界讀取。
std::vector<std::uint8_t> readSegments(const platform::SharedReadFile& file,
                                       const ByteRangeCheck& check) {
    std::vector<std::uint8_t> out;
    if (!check.ok() && check.verdict != ByteRangeVerdict::IncompleteCoverage) return out;

    const auto fileSize = static_cast<std::int64_t>(file.size());
    for (const ByteRangeSegment& segment : check.segments) {
        if (segment.offset < 0 || segment.length <= 0) continue;
        if (segment.end() > fileSize) return {};
        const auto begin = out.size();
        out.resize(begin + static_cast<std::size_t>(segment.length));
        const std::size_t read = file.read(static_cast<std::uint64_t>(segment.offset),
                                           out.data() + begin,
                                           static_cast<std::size_t>(segment.length));
        if (read != static_cast<std::size_t>(segment.length)) return {};
    }
    return out;
}

}  // namespace

struct ScanTask {
    std::function<void()> run;
};

struct SignatureScanner::Impl {
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable idleCv;
    std::deque<ScanTask> queue;
    bool running{true};
    bool busy{false};
    std::int32_t signatureCount{0};

    // 以下成員只能由簽章執行緒觸碰。
    FPDF_DOCUMENT document{nullptr};
    FileSource source;              // 給 PDFium 的回呼式檔案存取
    platform::SharedReadFile bytes;  // 給我們自己算雜湊用的原始位元組

    void enqueue(std::function<void()> run) {
        {
            std::lock_guard lock(mutex);
            if (!running) return;
            queue.push_back(ScanTask{std::move(run)});
        }
        cv.notify_one();
    }

    void closeDocument() {
        if (document) {
            FPDF_CloseDocument(document);
            document = nullptr;
        }
        source.close();
        bytes.close();
        std::lock_guard lock(mutex);
        signatureCount = 0;
    }
};

SignatureScanner::SignatureScanner() : impl_(std::make_unique<Impl>()) {
    impl_->worker = std::thread([this] {
        const PdfiumRuntime runtime;
        for (;;) {
            ScanTask task;
            {
                std::unique_lock lock(impl_->mutex);
                impl_->cv.wait(lock, [this] { return !impl_->running || !impl_->queue.empty(); });
                if (!impl_->running && impl_->queue.empty()) break;
                task = std::move(impl_->queue.front());
                impl_->queue.pop_front();
                impl_->busy = true;
            }
            // 行程級序列化（ADR-005）。簽章驗證與檢視器渲染是兩條不同的執行緒，
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

SignatureScanner::~SignatureScanner() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->running = false;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void SignatureScanner::open(std::string path, std::string password,
                            std::function<void(domain::DocumentError)> callback) {
    impl_->enqueue([this, path = std::move(path), password = std::move(password),
                    callback = std::move(callback)] {
        impl_->closeDocument();

        if (!impl_->source.open(path) || !impl_->bytes.open(path)) {
            impl_->closeDocument();
            if (callback) callback(domain::DocumentError::FileNotFound);
            return;
        }

        FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(
            static_cast<FPDF_FILEACCESS*>(impl_->source.fileAccess()),
            password.empty() ? nullptr : password.c_str());
        if (!doc) {
            const unsigned long error = FPDF_GetLastError();
            impl_->closeDocument();
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
            // FPDF_GetSignatureCount 失敗回傳 -1，直接存進去會讓呼叫端
            // 把「讀取失敗」當成「有 -1 個簽章」。正規化成 0。
            const int count = FPDF_GetSignatureCount(doc);
            impl_->signatureCount = count > 0 ? count : 0;
        }
        if (callback) callback(domain::DocumentError::None);
    });
}

void SignatureScanner::close(std::function<void()> callback) {
    impl_->enqueue([this, callback = std::move(callback)] {
        impl_->closeDocument();
        if (callback) callback();
    });
}

std::int32_t SignatureScanner::signatureCount() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->signatureCount;
}

void SignatureScanner::scan(const TrustStore* trust, VerifyOptions options,
                            RevocationChecker* revocation,
                            std::function<void(std::vector<SignatureReport>)> callback) {
    impl_->enqueue([this, trust, options, revocation, callback = std::move(callback)] {
        std::vector<SignatureReport> reports;
        if (!impl_->document || !trust) {
            if (callback) callback(std::move(reports));
            return;
        }

        const int count = FPDF_GetSignatureCount(impl_->document);
        const auto fileSize = static_cast<std::int64_t>(impl_->bytes.size());

        for (int i = 0; i < count; ++i) {
            FPDF_SIGNATURE sig = FPDF_GetSignatureObject(impl_->document, i);
            if (!sig) continue;

            std::vector<int> rawRange;
            if (const unsigned long ints = FPDFSignatureObj_GetByteRange(sig, nullptr, 0);
                ints > 0) {
                rawRange.resize(ints);
                FPDFSignatureObj_GetByteRange(sig, rawRange.data(), ints);
            }

            std::vector<std::uint8_t> contents;
            if (const unsigned long bytes = FPDFSignatureObj_GetContents(sig, nullptr, 0);
                bytes > 0) {
                contents.resize(bytes);
                FPDFSignatureObj_GetContents(sig, contents.data(), bytes);
            }

            std::string subFilter;
            if (const unsigned long bytes = FPDFSignatureObj_GetSubFilter(sig, nullptr, 0);
                bytes > 1) {
                std::vector<char> buffer(bytes, 0);
                FPDFSignatureObj_GetSubFilter(sig, buffer.data(), bytes);
                subFilter.assign(buffer.data());
            }

            std::string signingTime;
            if (const unsigned long bytes = FPDFSignatureObj_GetTime(sig, nullptr, 0); bytes > 1) {
                std::vector<char> buffer(bytes, 0);
                FPDFSignatureObj_GetTime(sig, buffer.data(), bytes);
                signingTime.assign(buffer.data());
            }

            std::string reason;
            if (const unsigned long bytes = FPDFSignatureObj_GetReason(sig, nullptr, 0);
                bytes > 2) {
                std::vector<unsigned short> buffer(bytes / 2 + 1, 0);
                FPDFSignatureObj_GetReason(sig, buffer.data(), bytes);
                reason = utf16ToUtf8(buffer);
            }

            const ByteRangeCheck coverage = checkByteRange(rawRange, fileSize);
            const std::vector<std::uint8_t> signedBytes = readSegments(impl_->bytes, coverage);
            const std::vector<std::uint8_t> der = trimDerPadding(contents);

            SignatureReport report =
                verifyDetachedPkcs7(signedBytes, der, coverage, *trust, options, revocation);
            report.index = i;
            report.subFilter = std::move(subFilter);
            report.signingTimeRaw = std::move(signingTime);
            report.reason = std::move(reason);
            reports.push_back(std::move(report));
        }

        if (callback) callback(std::move(reports));
    });
}

void SignatureScanner::waitForIdle() {
    std::unique_lock lock(impl_->mutex);
    impl_->idleCv.wait(lock, [this] { return impl_->queue.empty() && !impl_->busy; });
}

}  // namespace alioth::engine::signature
