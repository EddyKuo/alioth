#include "engine/signature/signature_types.h"

namespace alioth::engine::signature {

const char* describe(SignatureTrust trust) noexcept {
    switch (trust) {
        case SignatureTrust::Trusted:   return "有效且受信任";
        case SignatureTrust::Untrusted: return "有效但無法確認信任";
        case SignatureTrust::Invalid:   return "無效";
    }
    return "未知";
}

const char* describe(RevocationStatus status) noexcept {
    switch (status) {
        case RevocationStatus::NotChecked: return "未查詢";
        case RevocationStatus::Good:       return "未被吊銷";
        case RevocationStatus::Revoked:    return "已被吊銷";
        case RevocationStatus::Unknown:    return "無法確認";
    }
    return "未知";
}

SignatureTrust classify(const SignatureReport& report, RevocationPolicy policy) {
    // 紅燈的條件由嚴到寬排列。任何一條成立就到此為止——
    // 讓「有效」成為唯一需要通過所有檢查的結果，而不是預設值。
    if (!report.contentsPresent || !report.parsed) return SignatureTrust::Invalid;
    // /ByteRange 本身壞掉（重疊、越界、語法錯誤）與「合法但只涵蓋部分檔案」
    // 是兩件事。前者代表這份簽章無法解讀，是紅燈；後者見下面的說明。
    if (!report.coverage.ok() && !report.coverage.partiallyCovered()) {
        return SignatureTrust::Invalid;
    }
    if (!report.digestMatches || !report.cryptographicallyValid) return SignatureTrust::Invalid;
    if (report.revocation == RevocationStatus::Revoked) return SignatureTrust::Invalid;
    if (policy == RevocationPolicy::HardFail &&
        report.revocation != RevocationStatus::Good) {
        return SignatureTrust::Invalid;
    }

    // 部分涵蓋：密碼學驗證通過，但檔案裡有一段不在簽章保護範圍內。
    //
    // 這裡刻意是黃燈而不是紅燈。紅燈的語意是「這份簽章無效」，而它並不無效——
    // 摘要對得上、憑證鏈也可能是可信的。真正的情況是**我們無法確認**那段
    // 附加內容是無害的（使用者自己加的註解）還是惡意的（增量儲存攻擊），
    // 而「無法確認」正是黃燈的定義。
    //
    // 要分辨兩者需要修改分析（ISO 32000-2 §12.8.2.3 的精神）：逐一比對增量段
    // 新增／覆寫的物件是否只落在允許的類別。那是一個獨立的子系統，尚未實作。
    // 在它做出來之前，把使用者自己加一個螢光筆之後的合約標成「無效」是誤導；
    // 標成綠燈則是真正的危險。黃燈加上 findings 裡逐條列出的未涵蓋位元組數，
    // 是目前唯一誠實的答案。
    //
    // 這個判定不需要第四種狀態，因此 PRD-SIG-001 的三態驗收標準仍然成立。
    if (report.coverage.partiallyCovered()) return SignatureTrust::Untrusted;

    // 憑證鏈不受信任一律是黃燈。這條規則沒有例外：
    // 自簽憑證在密碼學上完全有效，但「有效」與「可信」不是同一件事，
    // 把它算成綠燈等於讓任何人都能偽造出一份看起來被認證過的合約。
    if (!report.chainTrusted) return SignatureTrust::Untrusted;

    // 憑證已過期或尚未生效。沒有可信時間戳（LTV 屬於 R3）就無法證明
    // 簽署當下憑證仍在有效期內，因此只能降為黃燈而不是判定無效。
    if (report.certificateExpired || report.certificateNotYetValid) {
        return SignatureTrust::Untrusted;
    }

    if (policy != RevocationPolicy::Skip && report.revocation != RevocationStatus::Good) {
        return SignatureTrust::Untrusted;
    }

    return SignatureTrust::Trusted;
}

void finalize(SignatureReport& report, RevocationPolicy policy) {
    if (!report.contentsPresent) {
        report.findings.emplace_back("簽章欄位沒有 /Contents，無法驗證。");
    } else if (!report.parsed) {
        report.findings.emplace_back("PKCS#7 / CMS 結構解析失敗，簽章資料已損毀或格式不受支援。");
    }

    if (!report.coverage.ok()) {
        if (report.coverage.partiallyCovered()) {
            report.findings.emplace_back("簽章只涵蓋部分檔案：" + report.coverage.detail);
        } else {
            report.findings.emplace_back("/ByteRange " +
                                         std::string(describe(report.coverage.verdict)) + "：" +
                                         report.coverage.detail);
        }
    }

    if (report.parsed && report.contentsPresent) {
        if (!report.digestMatches) {
            report.findings.emplace_back("文件內容與簽章時的摘要不符，檔案在簽署後被竄改。");
        } else if (!report.cryptographicallyValid) {
            report.findings.emplace_back("簽章的密碼學驗證失敗。");
        }
    }

    if (report.cryptographicallyValid && !report.chainTrusted) {
        report.findings.emplace_back(
            "憑證鏈無法回溯到受信任的根憑證，簽署者身分未經任何第三方確認。");
    }
    if (report.certificateExpired) {
        report.findings.emplace_back("簽署憑證已過期；缺少可信時間戳，無法證明簽署當下憑證仍有效。");
    }
    if (report.certificateNotYetValid) {
        report.findings.emplace_back("簽署憑證尚未生效。");
    }

    switch (report.revocation) {
        case RevocationStatus::Revoked:
            report.findings.emplace_back("簽署憑證已被發證機構吊銷。");
            break;
        case RevocationStatus::Unknown:
            report.findings.emplace_back("查詢過吊銷狀態但沒有明確答案（CRL/OCSP 無法取得或不涵蓋此憑證）。");
            break;
        case RevocationStatus::NotChecked:
            if (policy != RevocationPolicy::Skip) {
                report.findings.emplace_back("未查詢吊銷狀態，無法確認憑證是否仍然有效。");
            }
            break;
        case RevocationStatus::Good:
            break;
    }

    report.trust = classify(report, policy);
}

}  // namespace alioth::engine::signature
