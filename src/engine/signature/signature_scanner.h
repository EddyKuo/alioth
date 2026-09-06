#pragma once

// 簽章列舉與驗證的門面（WBS 6.5–6.7，PRD-SIG-001 / 002）。
//
// 執行緒規則與其他 PDFium 子系統相同：本類別持有自己的專用執行緒與獨立的
// 文件把手。簽章驗證會做大量雜湊與憑證運算，塞進渲染佇列會直接讓畫面卡住。
//
// 它同時持有第二個把手：一個唯讀的檔案控制代碼，用來讀取 /ByteRange 指定的
// 原始位元組。PDFium 不提供讀取原始位元組的 API，而簽章驗證的本質就是
// 「對原始位元組算雜湊」——這一段只能自己來。
//
// 記憶體取捨：只把 /ByteRange 涵蓋的區段讀進記憶體，不整份讀檔。
// 涵蓋範圍正常時那幾乎等於整份檔案，因此大型文件的簽章驗證仍會有一次
// 與檔案同級的記憶體尖峰。串流式雜湊需要自訂 BIO 串接，屬於後續最佳化。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "domain/document.h"
#include "engine/signature/revocation.h"
#include "engine/signature/signature_types.h"
#include "engine/signature/trust_store.h"

namespace alioth::engine::signature {

class SignatureScanner {
public:
    SignatureScanner();
    ~SignatureScanner();

    SignatureScanner(const SignatureScanner&) = delete;
    SignatureScanner& operator=(const SignatureScanner&) = delete;

    void open(std::string path, std::string password,
              std::function<void(domain::DocumentError)> callback);
    void close(std::function<void()> callback = {});

    // 無簽章的文件回報 0。這個數字來自 FPDF_GetSignatureCount，
    // 不從 /Annots 推測——widget 上有簽章欄位不代表已經簽過。
    [[nodiscard]] std::int32_t signatureCount() const noexcept;

    // trust 與 revocation 由呼叫端擁有，必須活到回呼結束。
    // 傳指標而不是複製，是因為信任存放區可能很大且應該全程式共用一份。
    void scan(const TrustStore* trust, VerifyOptions options, RevocationChecker* revocation,
              std::function<void(std::vector<SignatureReport>)> callback);

    void waitForIdle();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine::signature
