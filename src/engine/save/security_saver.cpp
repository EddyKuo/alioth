#include "engine/save/security_saver.h"
#include "engine/pdfium_lock.h"

#include <fpdf_save.h>
#include <fpdfview.h>

#include "engine/objects/pdf_object.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::save {
namespace {

FPDF_DOCUMENT toDocument(DocumentHandle handle) {
    return static_cast<FPDF_DOCUMENT>(handle);
}

// /Encrypt 字典裡 /CF /StdCF /CFM 的名稱 → 演算法。/V 4 起才有 /CF；
// V1/V2 沒有這個子字典，演算法完全由 /V 與金鑰長度決定。
EncryptionAlgorithm algorithmFromCfm(const std::string& cfm) {
    if (cfm == "AESV2") return EncryptionAlgorithm::Aes128;
    if (cfm == "AESV3") return EncryptionAlgorithm::Aes256;
    if (cfm == "V2") return EncryptionAlgorithm::Rc4_128;
    if (cfm == "None") return EncryptionAlgorithm::None;
    return EncryptionAlgorithm::Unknown;
}

}  // namespace

const char* describe(EncryptionAlgorithm algorithm) noexcept {
    switch (algorithm) {
        case EncryptionAlgorithm::None:    return "未加密";
        case EncryptionAlgorithm::Rc4_40:  return "RC4 40 位元";
        case EncryptionAlgorithm::Rc4_128: return "RC4 128 位元";
        case EncryptionAlgorithm::Aes128:  return "AES-128";
        case EncryptionAlgorithm::Aes256:  return "AES-256";
        case EncryptionAlgorithm::Unknown: return "未知加密方式";
    }
    return "未知狀態";
}

PermissionFlags PermissionFlags::fromRawFlags(std::uint32_t raw) noexcept {
    // 位元位置依 ISO 32000-1 表 22（由 1 起算的位元編號，這裡換算成位移）。
    PermissionFlags flags;
    flags.print = (raw & (1u << 2)) != 0;
    flags.modify = (raw & (1u << 3)) != 0;
    flags.copy = (raw & (1u << 4)) != 0;
    flags.annotate = (raw & (1u << 5)) != 0;
    flags.fillForms = (raw & (1u << 8)) != 0;
    flags.extractForAccessibility = (raw & (1u << 9)) != 0;
    flags.assemble = (raw & (1u << 10)) != 0;
    flags.printHighRes = (raw & (1u << 11)) != 0;
    return flags;
}

SecurityInfo inspectEncryption(const std::string& pdfBytes) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    SecurityInfo info;

    objects::PdfSourceDocument source;
    std::string diagnostic;
    // 回傳值可能是 Ok 或 Encrypted：兩者都代表 trailer 已經解析成功，
    // 差別只在物件層寫入通道會不會繼續往下開放寫入。這裡只讀 trailer，
    // 不需要 Ok。其餘狀態（NotPdf、BadXref…）代表連 trailer 都讀不到，
    // 沒有加密資訊可言。
    const objects::SourceStatus status = source.open(pdfBytes, &diagnostic);
    if (status != objects::SourceStatus::Ok && status != objects::SourceStatus::Encrypted) {
        return info;  // encrypted=false、algorithmKnown=false：無法確認
    }

    if (!source.trailer().has("Encrypt")) {
        info.encrypted = false;
        info.algorithm = EncryptionAlgorithm::None;
        info.algorithmKnown = true;
        return info;
    }
    info.encrypted = true;

    const objects::PdfObject* encryptEntry = source.trailer().find("Encrypt");
    if (encryptEntry == nullptr) return info;
    const objects::PdfObject resolved = source.resolve(*encryptEntry);
    const objects::PdfDictionary* dict = resolved.asDictionary();
    if (dict == nullptr) return info;  // 有 /Encrypt 鍵但解不出字典：演算法不明

    const objects::PdfObject* vEntry = dict->find("V");
    const std::int64_t v = vEntry != nullptr ? vEntry->asInteger(0) : 0;

    if (v == 1) {
        info.algorithm = EncryptionAlgorithm::Rc4_40;
        info.algorithmKnown = true;
    } else if (v == 2) {
        const objects::PdfObject* lengthEntry = dict->find("Length");
        const std::int64_t bits = lengthEntry != nullptr ? lengthEntry->asInteger(40) : 40;
        info.algorithm = bits >= 128 ? EncryptionAlgorithm::Rc4_128 : EncryptionAlgorithm::Rc4_40;
        info.algorithmKnown = true;
    } else if (v == 4 || v == 5) {
        // /CF /StdCF /CFM 是唯一能區分 RC4/AES/AES-256 的欄位，多層字典找起來
        // 囉唆，但沒有捷徑：/V 只到「有 crypt filter」為止，不含演算法名稱。
        std::string cfm;
        if (const objects::PdfObject* cfEntry = dict->find("CF"); cfEntry != nullptr) {
            const objects::PdfObject cf = source.resolve(*cfEntry);
            if (const objects::PdfDictionary* cfDict = cf.asDictionary(); cfDict != nullptr) {
                if (const objects::PdfObject* stdCfEntry = cfDict->find("StdCF");
                    stdCfEntry != nullptr) {
                    const objects::PdfObject stdCf = source.resolve(*stdCfEntry);
                    if (const objects::PdfDictionary* stdCfDict = stdCf.asDictionary();
                        stdCfDict != nullptr) {
                        if (const objects::PdfObject* cfmEntry = stdCfDict->find("CFM");
                            cfmEntry != nullptr) {
                            cfm = cfmEntry->asName();
                        }
                    }
                }
            }
        }
        if (!cfm.empty()) {
            info.algorithm = algorithmFromCfm(cfm);
            info.algorithmKnown = info.algorithm != EncryptionAlgorithm::Unknown;
        } else if (v == 5) {
            // /V 5 沒有 /CF 時幾乎必是 AES-256（R5/R6 是唯一定義 /V 5 的修訂）。
            info.algorithm = EncryptionAlgorithm::Aes256;
            info.algorithmKnown = true;
        }
    }
    // v 為其他值：自訂或未來的安全處理器，algorithm 維持 Unknown。

    return info;
}

void fillPermissions(DocumentHandle document, SecurityInfo& info) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    FPDF_DOCUMENT doc = toDocument(document);
    if (!doc) return;
    info.securityHandlerRevision = FPDF_GetSecurityHandlerRevision(doc);
    info.permissions = PermissionFlags::fromRawFlags(
        static_cast<std::uint32_t>(FPDF_GetDocPermissions(doc)));
}

SetPasswordResult setPassword(const std::string& /*newUserPassword*/,
                              const std::string& /*newOwnerPassword*/,
                              std::uint32_t /*permissions*/) {
    SetPasswordResult result;
    result.status = SetPasswordStatus::NotSupported;
    result.message =
        "設定密碼在目前的技術堆疊下不支援：PDFium 的公開存檔介面沒有加密輸出的選項，"
        "而自建加密輸出需要一整條 ISO 32000-1 §7.6 的加密管線，超出物件層寫入通道"
        "（ADR-002）目前的範圍。詳見 "
        "exceptions/EXC_20260906_RD_SA_set_password_unsupported.md。";
    return result;
}

SaveResult removePassword(DocumentHandle document, const std::string& targetPath) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    // removePassword 只對「已經解鎖」的把手有意義：呼叫端必須先用正確密碼
    // 開啟文件，這裡才能把它整份重寫成不加密版本。SaveOptions::removeSecurity
    // 對應的正是 FPDF_REMOVE_SECURITY——這條路徑本來就存在，這裡只是給它一個
    // 語意明確的入口，而不是要求呼叫端自己記得要傳哪個旗標。
    SaveOptions options;
    options.removeSecurity = true;
    return IncrementalSaver::saveAsCopy(document, targetPath, options);
}

}  // namespace alioth::engine::save
