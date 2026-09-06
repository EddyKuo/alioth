#include "engine/signature/signature_creator.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/cms.h>
#include <openssl/err.h>
#include <openssl/ess.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/x509.h>

#include <cstdio>
#include <ctime>
#include <functional>
#include <utility>
#include <vector>

#include "engine/objects/incremental_appender.h"
#include "engine/objects/page_object_editor.h"
#include "engine/objects/pdf_object.h"
#include "engine/signature/byte_range.h"

namespace alioth::engine::signature {

namespace obj = alioth::engine::objects;

// 可見簽章外觀的影像 XObject。RGBA 的 alpha 抽成獨立的灰階 /SMask——
// PDF 的影像沒有交錯式 alpha，把 RGBA 直接當 RGB 寫會讓每個像素往後錯一格，
// 整張圖顏色偏掉。刻意不壓縮：手寫簽名圖通常只有幾十 KB，為了它引入
// zlib 的寫入路徑不划算。
[[nodiscard]] obj::PdfObject makeAppearanceImageObject(obj::IncrementalAppender& appender,
                                                       const AppearanceImage& image) {
    int smaskNumber = 0;
    if (image.channels == 4) {
        obj::PdfDictionary maskDict;
        maskDict.set("Type", obj::makeName("XObject"));
        maskDict.set("Subtype", obj::makeName("Image"));
        maskDict.set("Width", obj::PdfObject{static_cast<std::int64_t>(image.width)});
        maskDict.set("Height", obj::PdfObject{static_cast<std::int64_t>(image.height)});
        maskDict.set("ColorSpace", obj::makeName("DeviceGray"));
        maskDict.set("BitsPerComponent", obj::PdfObject{static_cast<std::int64_t>(8)});

        std::string alpha;
        const std::size_t pixels = static_cast<std::size_t>(image.width) *
                                   static_cast<std::size_t>(image.height);
        alpha.reserve(pixels);
        for (std::size_t i = 0; i < pixels; ++i) {
            alpha.push_back(static_cast<char>(image.pixels[i * 4 + 3]));
        }
        smaskNumber = appender.allocateObject();
        appender.setObject(smaskNumber,
                           obj::PdfObject{obj::PdfStream{std::move(maskDict), std::move(alpha)}});
    }

    obj::PdfDictionary dict;
    dict.set("Type", obj::makeName("XObject"));
    dict.set("Subtype", obj::makeName("Image"));
    dict.set("Width", obj::PdfObject{static_cast<std::int64_t>(image.width)});
    dict.set("Height", obj::PdfObject{static_cast<std::int64_t>(image.height)});
    dict.set("ColorSpace", obj::makeName("DeviceRGB"));
    dict.set("BitsPerComponent", obj::PdfObject{static_cast<std::int64_t>(8)});
    if (smaskNumber > 0) dict.set("SMask", obj::makeRef(smaskNumber));

    std::string rgb;
    const std::size_t pixels = static_cast<std::size_t>(image.width) *
                               static_cast<std::size_t>(image.height);
    rgb.reserve(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t base = i * static_cast<std::size_t>(image.channels);
        rgb.push_back(static_cast<char>(image.pixels[base]));
        rgb.push_back(static_cast<char>(image.pixels[base + 1]));
        rgb.push_back(static_cast<char>(image.pixels[base + 2]));
    }
    return obj::PdfObject{obj::PdfStream{std::move(dict), std::move(rgb)}};
}


namespace {

// /ByteRange 佔位符固定十位數寬（涵蓋到 9,999,999,999 位元組，約 9.3 GB，
// 遠超本產品的合理文件大小），數字之間以單一空白分隔，總長度固定，
// 這樣才能在量出實際位移之後原地覆寫成等寬的最終值而不移動任何其他位元組。
constexpr int kByteRangeDigitWidth = 10;

[[nodiscard]] std::string zeroPad(std::int64_t value, int width) {
    std::string text = std::to_string(value);
    if (static_cast<int>(text.size()) > width) return {};
    return std::string(static_cast<std::size_t>(width) - text.size(), '0') + text;
}

[[nodiscard]] std::string byteRangePlaceholderText() {
    const std::string zero = zeroPad(0, kByteRangeDigitWidth);
    return zero + " " + zero + " " + zero + " " + zero;
}

// PDF 日期字串（ISO 32000-1 §7.9.4）。刻意透過 OpenSSL 的 ASN1_TIME_to_tm
// 取得 UTC 欄位，不用 std::gmtime／localtime——那組介面在各平台的執行緒安全
// 版本名稱不同，作業系統差異只准出現在平台層；ASN1_TIME_to_tm 是可攜路徑，
// 與 pkcs7_verifier.cpp 驗證側的 asn1TimeToUnix 同一個理由。
[[nodiscard]] std::string pdfDate(std::int64_t unixTime) {
    const time_t when = unixTime > 0 ? static_cast<time_t>(unixTime) : std::time(nullptr);
    ASN1_TIME* time = ASN1_TIME_set(nullptr, when);
    if (!time) return {};
    struct tm value {};
    const int ok = ASN1_TIME_to_tm(time, &value);
    ASN1_TIME_free(time);
    if (!ok) {
        ERR_clear_error();
        return {};
    }
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "D:%04d%02d%02d%02d%02d%02dZ", value.tm_year + 1900,
                  value.tm_mon + 1, value.tm_mday, value.tm_hour, value.tm_min, value.tm_sec);
    return std::string(buffer);
}

[[nodiscard]] int docMdpPValue(CertifyLevel level) noexcept {
    switch (level) {
        case CertifyLevel::NoChangesAllowed: return 1;
        case CertifyLevel::FormFillingAllowed: return 2;
        case CertifyLevel::FormFillingAndAnnotations: return 3;
        case CertifyLevel::None: return 0;
    }
    return 0;
}

// DocMDP 的 /Reference 陣列（ISO 32000-2 §12.8.2.3，PRD-SIG-007）。
//
// 已知簡化，未對照 Acrobat 實機驗證（見交付報告）：沒有寫 /Data（一般實作
// 只在 FieldMDP 才會指向目錄物件，DocMDP 依規格是對整份文件生效，多數
// 現有實作——包含 iText／PDFBox 的輸出——同樣不帶 /Data，但這裡沒有能力
// 逐條核對 Acrobat 對缺少 /Data 的 DocMDP 是否完全照規格接受）。
[[nodiscard]] std::string buildDocMdpReferenceText(CertifyLevel level) {
    obj::PdfDictionary transformParams;
    transformParams.set("Type", obj::makeName("TransformParams"));
    transformParams.set("P", obj::PdfObject{static_cast<std::int64_t>(docMdpPValue(level))});
    transformParams.set("V", obj::makeName("1.2"));

    obj::PdfDictionary sigRef;
    sigRef.set("Type", obj::makeName("SigRef"));
    sigRef.set("TransformMethod", obj::makeName("DocMDP"));
    sigRef.set("DigestMethod", obj::makeName("SHA256"));
    sigRef.set("TransformParams", obj::PdfObject{std::move(transformParams)});

    obj::PdfArray referenceArray;
    referenceArray.emplace_back(obj::PdfObject{std::move(sigRef)});
    return "/Reference " + obj::serialize(obj::PdfObject{std::move(referenceArray)});
}

// 索取時間戳並嵌進 unsigned attribute 成功時回傳 true；tsaCallback 為空
// （呼叫端沒有要求時間戳）視為成功但不做任何事。任何一步失敗都回傳 false
// 並填 diagnostic，讓 signCadesBes 據此讓整個簽署失敗——見 signature_creator.h
// 對 timestamped 欄位的說明：明確要求了時間戳卻拿不到，不能靜默退回一份
// 沒有時間戳的簽章。
using TimestampEmbedFn =
    std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>& sha256Hash,
                                              std::string& diagnostic)>;

[[nodiscard]] bool embedSignatureTimestamp(CMS_ContentInfo* cms, const TimestampEmbedFn& tsaCallback,
                                           std::string& diagnostic) {
    if (!tsaCallback) return true;  // 未要求時間戳

    STACK_OF(CMS_SignerInfo)* signerInfos = CMS_get0_SignerInfos(cms);
    CMS_SignerInfo* signerInfo =
        (signerInfos != nullptr && sk_CMS_SignerInfo_num(signerInfos) > 0)
            ? sk_CMS_SignerInfo_value(signerInfos, 0)
            : nullptr;
    if (signerInfo == nullptr) {
        diagnostic = "找不到 CMS SignerInfo，無法嵌入時間戳";
        return false;
    }

    // 訊息摘要對象是簽章值本身（RFC 5126 CAdES-T 的慣例），不是被簽的內容——
    // 見 timestamp_client.h 開頭的說明，這是最容易搞混的一步。
    ASN1_OCTET_STRING* signatureValue = CMS_SignerInfo_get0_signature(signerInfo);
    if (signatureValue == nullptr) {
        diagnostic = "取得簽章值失敗，無法計算時間戳的訊息摘要";
        return false;
    }

    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int digestLength = 0;
    if (EVP_Digest(ASN1_STRING_get0_data(signatureValue),
                   static_cast<std::size_t>(ASN1_STRING_length(signatureValue)), digest,
                   &digestLength, EVP_sha256(), nullptr) != 1) {
        ERR_clear_error();
        diagnostic = "計算簽章值的 SHA-256 摘要失敗";
        return false;
    }
    const std::vector<std::uint8_t> messageImprint(digest, digest + digestLength);

    std::string tsaDiagnostic;
    const std::vector<std::uint8_t> tokenDer = tsaCallback(messageImprint, tsaDiagnostic);
    if (tokenDer.empty()) {
        diagnostic = "向時間戳伺服器索取時間戳失敗：" + tsaDiagnostic;
        return false;
    }

    if (CMS_unsigned_add1_attr_by_NID(signerInfo, NID_id_smime_aa_timeStampToken, V_ASN1_SEQUENCE,
                                      tokenDer.data(), static_cast<int>(tokenDer.size())) <= 0) {
        ERR_clear_error();
        diagnostic = "掛上時間戳 unsigned attribute 失敗";
        return false;
    }
    return true;
}

// CAdES-BES（RFC 5126 之基礎，ETSI EN 319 122-1 PAdES-B-B 依附其上）：
// 用 CMS_PARTIAL 建立空殼、加簽署者、掛上 ESS signingCertificateV2 signed
// attribute（RFC 5035）、最後才呼叫 CMS_final 讓簽章涵蓋最終的屬性集合。
// 這個順序不能反過來——先簽再加屬性會讓簽章值不再涵蓋新加的屬性，
// 驗證時會被判定為摘要不符。時間戳則相反：它是 unsigned attribute，必須在
// CMS_final 之後才加，否則 CMS_final 會把它也一併納入簽章涵蓋範圍，
// 而時間戳當下還沒算出簽章值，順序上不可能成立。
[[nodiscard]] std::vector<std::uint8_t> signCadesBes(const std::vector<std::uint8_t>& data,
                                                      X509* cert, EVP_PKEY* key,
                                                      STACK_OF(X509) * chain,
                                                      const TimestampEmbedFn& tsaCallback,
                                                      std::string& diagnostic) {
    std::vector<std::uint8_t> out;
    if (!cert || !key) {
        diagnostic = "缺少簽署憑證或私鑰";
        return out;
    }

    const unsigned int flags = CMS_PARTIAL | CMS_BINARY | CMS_DETACHED;
    CMS_ContentInfo* cms = CMS_sign(nullptr, nullptr, chain, nullptr, flags);
    if (!cms) {
        diagnostic = "CMS_sign 建立空殼失敗";
        ERR_clear_error();
        return out;
    }

    CMS_SignerInfo* signerInfo = CMS_add1_signer(cms, cert, key, EVP_sha256(), flags);
    if (!signerInfo) {
        diagnostic = "CMS_add1_signer 失敗";
        ERR_clear_error();
        CMS_ContentInfo_free(cms);
        return out;
    }

    ESS_SIGNING_CERT_V2* essCert =
        OSSL_ESS_signing_cert_v2_new_init(EVP_sha256(), cert, nullptr, 1);
    if (!essCert) {
        diagnostic = "建立 ESS signingCertificateV2 失敗";
        ERR_clear_error();
        CMS_ContentInfo_free(cms);
        return out;
    }
    unsigned char* essDer = nullptr;
    const int essLen = i2d_ESS_SIGNING_CERT_V2(essCert, &essDer);
    ESS_SIGNING_CERT_V2_free(essCert);
    if (essLen <= 0 || !essDer) {
        diagnostic = "序列化 ESS signingCertificateV2 失敗";
        ERR_clear_error();
        CMS_ContentInfo_free(cms);
        return out;
    }
    const int attrAdded = CMS_signed_add1_attr_by_NID(
        signerInfo, NID_id_smime_aa_signingCertificateV2, V_ASN1_SEQUENCE, essDer, essLen);
    OPENSSL_free(essDer);
    if (attrAdded <= 0) {
        diagnostic = "掛上 ESS signingCertificateV2 屬性失敗";
        ERR_clear_error();
        CMS_ContentInfo_free(cms);
        return out;
    }

    BIO* dataBio = BIO_new_mem_buf(data.data(), static_cast<int>(data.size()));
    if (!dataBio) {
        diagnostic = "建立資料 BIO 失敗";
        CMS_ContentInfo_free(cms);
        return out;
    }
    const int finalized = CMS_final(cms, dataBio, nullptr, flags);
    BIO_free(dataBio);
    if (finalized != 1) {
        diagnostic = "CMS_final 簽署失敗";
        ERR_clear_error();
        CMS_ContentInfo_free(cms);
        return out;
    }

    if (!embedSignatureTimestamp(cms, tsaCallback, diagnostic)) {
        CMS_ContentInfo_free(cms);
        return out;
    }

    unsigned char* derBuffer = nullptr;
    const int derLen = i2d_CMS_ContentInfo(cms, &derBuffer);
    CMS_ContentInfo_free(cms);
    if (derLen <= 0 || !derBuffer) {
        diagnostic = "序列化 CMS SignedData 失敗";
        ERR_clear_error();
        return out;
    }
    out.assign(derBuffer, derBuffer + derLen);
    OPENSSL_free(derBuffer);
    return out;
}

[[nodiscard]] CreateSignatureResult failure(std::string message) {
    CreateSignatureResult result{};
    result.diagnostic = std::move(message);
    return result;
}

}  // namespace

CreateSignatureResult createSignature(const std::string& sourceBytes,
                                      const SigningIdentity& identity,
                                      const CreateSignatureOptions& options) {
    if (!identity.valid()) return failure("簽署身分未就緒：缺少憑證或私鑰");
    if (options.reserveBytes < 256) return failure("/Contents 預留空間過小");
    if (options.certify != CertifyLevel::None && options.documentAlreadyHasSignatures) {
        // 認證簽章必須是文件的第一個簽章。呼叫端已經明講文件已有簽章，
        // 這裡不重新掃描確認，直接拒絕——寧可拒絕一個其實安全的請求，
        // 也不要讓呼叫端以為傳了 documentAlreadyHasSignatures=true 還能矇混過關。
        return failure("認證簽章（Certify）必須是文件的第一個簽章，此文件已有既有簽章");
    }

    obj::IncrementalAppender appender;
    std::string diagnostic;
    const obj::SourceStatus status = appender.open(sourceBytes, &diagnostic);
    if (status != obj::SourceStatus::Ok) {
        if (status == obj::SourceStatus::Encrypted) {
            return failure("加密文件不支援建立簽章（物件層寫入通道明確拒絕，ADR-002）");
        }
        return failure("無法解析原檔：" + std::string(obj::describe(status)) +
                       (diagnostic.empty() ? "" : "（" + diagnostic + "）"));
    }

    const obj::PdfObject* rootEntry = appender.source().trailer().find("Root");
    if (rootEntry == nullptr || !rootEntry->isRef()) {
        return failure("trailer 缺少 /Root，無法定位目錄物件");
    }
    const obj::PdfRef rootRef = rootEntry->asRef();

    obj::PdfObject catalogObject = appender.currentObject(rootRef.number);
    obj::PdfDictionary* catalogDict = catalogObject.asDictionary();
    if (catalogDict == nullptr) return failure("目錄物件（/Root）不是字典，檔案可能已損毀");
    obj::PdfDictionary catalog = *catalogDict;

    obj::PdfRef pageRef{};
    if (!obj::pageRefAt(appender, options.pageIndex, pageRef)) {
        return failure("頁碼超出範圍：" + std::to_string(options.pageIndex));
    }

    const int sigNumber = appender.allocateObject();
    const int widgetNumber = appender.allocateObject();

    // --- 組出簽章字典本體（PdfRawLiteral：詳見 pdf_object.h 的說明） ---
    std::string sigText = "<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /ETSI.CAdES.detached";
    sigText += " /M (" + obj::escapeLiteralString(pdfDate(options.signingTimeUnix)) + ")";
    if (!options.reason.empty()) {
        sigText += " /Reason (" + obj::escapeLiteralString(options.reason) + ")";
    }
    if (!options.location.empty()) {
        sigText += " /Location (" + obj::escapeLiteralString(options.location) + ")";
    }
    if (!options.contactInfo.empty()) {
        sigText += " /ContactInfo (" + obj::escapeLiteralString(options.contactInfo) + ")";
    }
    if (const std::string signerName = identity.subjectCommonName(); !signerName.empty()) {
        sigText += " /Name (" + obj::escapeLiteralString(signerName) + ")";
    }
    if (options.certify != CertifyLevel::None) {
        sigText += " " + buildDocMdpReferenceText(options.certify);
    }
    sigText += " /ByteRange [" + byteRangePlaceholderText() + "]";
    sigText += " /Contents <" +
              std::string(static_cast<std::size_t>(options.reserveBytes) * 2, '0') + ">";
    sigText += " >>";

    appender.setObject(sigNumber, obj::PdfObject{obj::PdfRawLiteral{sigText}});

    // --- Widget 註解。appearance.rectPt 為空時是無實體簽章（/Rect 全零、無 /AP），
    //     否則產生可見外觀（PRD-SIG-004 的補完，見 ADR-004）。 ---
    const domain::RectF appearanceRect = options.appearance.rectPt.normalized();
    const bool visible = appearanceRect.width() > 0.0 && appearanceRect.height() > 0.0;

    int appearanceNumber = 0;
    int appearanceImageNumber = 0;
    AppearanceResult appearance;
    if (visible) {
        appearance = buildSignatureAppearance(options.appearance);
        if (!appearance.ok) return failure("簽章外觀產生失敗：" + appearance.diagnostic);

        if (appearance.needsImage) {
            // 影像必須是獨立物件：/Resources /XObject 的值只能是參照。
            appearanceImageNumber = appender.allocateObject();
            appender.setObject(appearanceImageNumber,
                               makeAppearanceImageObject(appender, options.appearance.image));
        }

        obj::PdfDictionary resources;
        if (appearance.needsFont) {
            obj::PdfDictionary font;
            font.set("Type", obj::makeName("Font"));
            font.set("Subtype", obj::makeName("Type1"));
            font.set("BaseFont", obj::makeName("Helvetica"));
            font.set("Encoding", obj::makeName("WinAnsiEncoding"));
            obj::PdfDictionary fonts;
            fonts.set("Helv", obj::PdfObject{std::move(font)});
            resources.set("Font", obj::PdfObject{std::move(fonts)});
        }
        if (appearanceImageNumber > 0) {
            obj::PdfDictionary xobjects;
            // 名稱要與 signature_appearance.cpp 寫進內容串流的 /Im0 一致。
            // 對不上時外觀是空白的，而且不會有任何錯誤。
            xobjects.set("Im0", obj::makeRef(appearanceImageNumber));
            resources.set("XObject", obj::PdfObject{std::move(xobjects)});
        }

        obj::PdfDictionary form;
        form.set("Type", obj::makeName("XObject"));
        form.set("Subtype", obj::makeName("Form"));
        form.set("FormType", obj::PdfObject{static_cast<std::int64_t>(1)});
        form.set("BBox", obj::makeNumberArray({appearance.bbox.left, appearance.bbox.bottom,
                                               appearance.bbox.right, appearance.bbox.top}));
        form.set("Resources", obj::PdfObject{std::move(resources)});

        appearanceNumber = appender.allocateObject();
        appender.setObject(appearanceNumber,
                           obj::PdfObject{obj::PdfStream{std::move(form), appearance.content}});
    }

    obj::PdfDictionary widget;
    widget.set("Type", obj::makeName("Annot"));
    widget.set("Subtype", obj::makeName("Widget"));
    widget.set("FT", obj::makeName("Sig"));
    widget.set("T", obj::makeTextString(options.fieldName));
    widget.set("V", obj::makeRef(sigNumber));
    // 132 = Print(4) + Locked(128)：印得出來但簽署後不可再移動/刪除。
    widget.set("F", obj::PdfObject{static_cast<std::int64_t>(132)});
    if (visible) {
        widget.set("Rect", obj::makeNumberArray({appearanceRect.left, appearanceRect.bottom,
                                                 appearanceRect.right, appearanceRect.top}));
        obj::PdfDictionary ap;
        ap.set("N", obj::makeRef(appearanceNumber));
        widget.set("AP", obj::PdfObject{std::move(ap)});
    } else {
        // /Rect 全零讓它在畫面上不佔任何位置，等同無實體簽名。
        widget.set("Rect", obj::makeNumberArray({0.0, 0.0, 0.0, 0.0}));
    }
    widget.set("P", obj::makeRef(pageRef.number, pageRef.generation));
    appender.setObject(widgetNumber, obj::PdfObject{std::move(widget)});

    const obj::PageEditStatus annotStatus =
        obj::appendToPageArray(appender, pageRef, "Annots", obj::makeRef(widgetNumber));
    if (!annotStatus.ok) return failure("掛上簽章欄位到頁面 /Annots 失敗：" + annotStatus.diagnostic);

    // --- AcroForm：新建或附加到既有的 /Fields，並設 /SigFlags 3
    //     （bit1 SignatureExists + bit2 AppendOnly，ISO 32000-1 表 219） ---
    const obj::PdfObject* acroFormEntry = catalog.find("AcroForm");
    obj::PdfDictionary acroForm;
    int acroFormIndirectNumber = 0;
    if (acroFormEntry != nullptr && acroFormEntry->isRef()) {
        acroFormIndirectNumber = acroFormEntry->asRef().number;
        const obj::PdfObject resolved = appender.currentObject(acroFormIndirectNumber);
        if (const obj::PdfDictionary* dict = resolved.asDictionary(); dict != nullptr) {
            acroForm = *dict;
        }
    } else if (acroFormEntry != nullptr && acroFormEntry->isDictionary()) {
        acroForm = *acroFormEntry->asDictionary();
    }

    const obj::PdfObject* fieldsEntry = acroForm.find("Fields");
    if (fieldsEntry == nullptr) {
        obj::PdfArray fields;
        fields.emplace_back(obj::makeRef(widgetNumber));
        acroForm.set("Fields", obj::PdfObject{std::move(fields)});
    } else if (const obj::PdfArray* directFields = fieldsEntry->asArray();
              directFields != nullptr) {
        obj::PdfArray fields = *directFields;
        fields.emplace_back(obj::makeRef(widgetNumber));
        acroForm.set("Fields", obj::PdfObject{std::move(fields)});
    } else if (fieldsEntry->isRef()) {
        const int fieldsNumber = fieldsEntry->asRef().number;
        const obj::PdfObject fieldsResolved = appender.currentObject(fieldsNumber);
        const obj::PdfArray* fieldsArray = fieldsResolved.asArray();
        if (fieldsArray == nullptr) {
            return failure("AcroForm /Fields 是間接參照但解不出陣列，檔案可能已損毀");
        }
        obj::PdfArray fields = *fieldsArray;
        fields.emplace_back(obj::makeRef(widgetNumber));
        if (!appender.updateObject(fieldsNumber, obj::PdfObject{std::move(fields)})) {
            return failure("更新 AcroForm /Fields 失敗");
        }
    } else {
        return failure("AcroForm /Fields 型別無法辨識，檔案可能已損毀");
    }
    acroForm.set("SigFlags", obj::PdfObject{static_cast<std::int64_t>(3)});

    if (acroFormIndirectNumber != 0) {
        if (!appender.updateObject(acroFormIndirectNumber, obj::PdfObject{acroForm})) {
            return failure("更新 AcroForm 字典失敗");
        }
        catalog.set("AcroForm", obj::makeRef(acroFormIndirectNumber));
    } else {
        catalog.set("AcroForm", obj::PdfObject{std::move(acroForm)});
    }

    if (options.certify != CertifyLevel::None) {
        obj::PdfDictionary perms;
        if (const obj::PdfObject* permsEntry = catalog.find("Perms"); permsEntry != nullptr) {
            if (const obj::PdfDictionary* dict = permsEntry->asDictionary(); dict != nullptr) {
                perms = *dict;
                if (perms.has("DocMDP")) {
                    return failure("目錄已存在 /Perms /DocMDP，此文件可能已被認證過");
                }
            }
        }
        perms.set("DocMDP", obj::makeRef(sigNumber));
        catalog.set("Perms", obj::PdfObject{std::move(perms)});
    }

    if (!appender.updateObject(rootRef.number, obj::PdfObject{std::move(catalog)})) {
        return failure("更新目錄（/Root）失敗");
    }

    const obj::BuildResult built = appender.build();
    if (!built.ok) return failure("附加式寫入失敗：" + built.diagnostic);

    std::string bytes = built.bytes;
    const std::size_t originalSize = appender.source().bytes().size();

    const std::string byteRangeMarker = "/ByteRange [" + byteRangePlaceholderText() + "]";
    const std::size_t byteRangePos = bytes.find(byteRangeMarker, originalSize);
    if (byteRangePos == std::string::npos) {
        return failure("內部錯誤：找不到 /ByteRange 佔位符");
    }
    const std::size_t numbersStart = byteRangePos + std::string("/ByteRange [").size();

    const std::string contentsMarker = "/Contents <";
    const std::size_t contentsMarkerPos = bytes.find(contentsMarker, byteRangePos);
    if (contentsMarkerPos == std::string::npos) {
        return failure("內部錯誤：找不到 /Contents 佔位符");
    }
    const std::size_t contentsStart = contentsMarkerPos + contentsMarker.size() - 1;  // '<' 本身
    const std::size_t contentsEnd = bytes.find('>', contentsStart);
    if (contentsEnd == std::string::npos) {
        return failure("內部錯誤：/Contents 佔位符沒有結尾");
    }
    const std::size_t contentsEndExclusive = contentsEnd + 1;  // '>' 之後

    const std::size_t fileSize = bytes.size();
    const std::int64_t range2 = static_cast<std::int64_t>(contentsStart);
    const std::int64_t range3 = static_cast<std::int64_t>(contentsEndExclusive);
    const std::int64_t range4 = static_cast<std::int64_t>(fileSize - contentsEndExclusive);

    const std::string num1 = zeroPad(0, kByteRangeDigitWidth);
    const std::string num2 = zeroPad(range2, kByteRangeDigitWidth);
    const std::string num3 = zeroPad(range3, kByteRangeDigitWidth);
    const std::string num4 = zeroPad(range4, kByteRangeDigitWidth);
    if (num2.empty() || num3.empty() || num4.empty()) {
        return failure("檔案過大：/ByteRange 數值超出保留的位數上限");
    }

    // 依序原地覆寫，寬度與佔位符完全相同，不會移動任何其他位元組。
    bytes.replace(numbersStart, kByteRangeDigitWidth, num1);
    bytes.replace(numbersStart + (kByteRangeDigitWidth + 1) * 1, kByteRangeDigitWidth, num2);
    bytes.replace(numbersStart + (kByteRangeDigitWidth + 1) * 2, kByteRangeDigitWidth, num3);
    bytes.replace(numbersStart + (kByteRangeDigitWidth + 1) * 3, kByteRangeDigitWidth, num4);

    std::vector<std::uint8_t> signedBytes;
    signedBytes.reserve(contentsStart + (fileSize - contentsEndExclusive));
    signedBytes.insert(signedBytes.end(), bytes.begin(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(contentsStart));
    signedBytes.insert(signedBytes.end(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(contentsEndExclusive),
                       bytes.end());

    TimestampEmbedFn tsaCallback;
    if (options.timestamp.enabled) {
        if (!options.timestamp.transport) {
            return failure("要求了時間戳但未提供傳輸層（Transport），無法連線 TSA");
        }
        // 用值捕捉：options 在本函式結束前一直存活，但 tsaCallback 只在
        // signCadesBes 內同步呼叫，不會被儲存到之後才用，值捕捉純粹是
        // 為了不必操心生命週期而已。
        const TimestampOptions timestampOptions = options.timestamp;
        tsaCallback = [timestampOptions](const std::vector<std::uint8_t>& sha256Hash,
                                         std::string& tsaDiagnostic) -> std::vector<std::uint8_t> {
            TimestampRequester requester(timestampOptions.transport);
            const TimestampResult tsaResult = requester.requestToken(
                timestampOptions.tsaUrl, sha256Hash, timestampOptions.requestOptions);
            if (!tsaResult.ok) {
                tsaDiagnostic = tsaResult.diagnostic;
                return {};
            }
            return tsaResult.tokenDer;
        };
    }

    std::string signDiagnostic;
    const std::vector<std::uint8_t> der =
        signCadesBes(signedBytes, static_cast<X509*>(identity.nativeCert()),
                    static_cast<EVP_PKEY*>(identity.nativeKey()),
                    static_cast<STACK_OF(X509)*>(identity.nativeChain()), tsaCallback,
                    signDiagnostic);
    if (der.empty()) return failure("簽署失敗：" + signDiagnostic);
    if (static_cast<int>(der.size()) > options.reserveBytes) {
        return failure("簽章保留空間不足：需要 " + std::to_string(der.size()) +
                       " 位元組，只保留了 " + std::to_string(options.reserveBytes) + " 位元組");
    }

    static const char* kHex = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(der.size() * 2);
    for (const std::uint8_t byte : der) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 0x0F]);
    }
    bytes.replace(contentsStart + 1, hex.size(), hex);

    CreateSignatureResult result{};
    result.ok = true;
    result.appendedBytes = static_cast<std::uint64_t>(bytes.size() - originalSize);
    result.byteRange[0] = 0;
    result.byteRange[1] = range2;
    result.byteRange[2] = range3;
    result.byteRange[3] = range4;
    result.timestamped = options.timestamp.enabled;
    result.bytes = std::move(bytes);
    result.signatureFieldObject = widgetNumber;
    result.signatureDictObject = sigNumber;
    return result;
}

// ---------------------------------------------------------------------------
// 批次簽署
// ---------------------------------------------------------------------------

namespace {

// 用 result.byteRange 重新組出「被簽章涵蓋的位元組」與「/Contents 的原始
// DER」，不重新剖析 bytes 找 /Contents 在哪裡——那件事 createSignature 已經
// 做過一次。verifyDetachedPkcs7 要的 pkcs7Der 必須先用 trimDerPadding 去掉
// 尾端補零，否則 d2i_PKCS7 會因為多餘位元組而失敗，症狀會被誤診成簽章損毀
// （與 pkcs7_verifier.h 的既有說明一致）。
[[nodiscard]] SignatureReport verifyJustCreatedSignature(const CreateSignatureResult& created,
                                                          const TrustStore& trust,
                                                          const VerifyOptions& verifyOptions,
                                                          RevocationChecker* revocation) {
    const std::vector<int> rawByteRange{
        static_cast<int>(created.byteRange[0]), static_cast<int>(created.byteRange[1]),
        static_cast<int>(created.byteRange[2]), static_cast<int>(created.byteRange[3])};
    const ByteRangeCheck coverage =
        checkByteRange(rawByteRange, static_cast<std::int64_t>(created.bytes.size()));

    const auto* data = reinterpret_cast<const std::uint8_t*>(created.bytes.data());
    const std::vector<std::uint8_t> signedBytes =
        assembleSignedBytes(data, created.bytes.size(), coverage);

    // /Contents 十六進位字串的內容：byteRange[1] 是 '<' 之後那個位元組的位移，
    // byteRange[2] 是 '>' 本身的位移（見 createSignature 對 contentsStart /
    // contentsEndExclusive 的定義：contentsStart 指向 '<'，因此 +1 才是十六進位
    // 字元的起點；contentsEndExclusive 是 '>' 之後一個位置，因此 -1 才是十六進位
    // 字元的終點）。
    const std::int64_t hexBegin = created.byteRange[1] + 1;
    const std::int64_t hexEnd = created.byteRange[2] - 1;
    std::vector<std::uint8_t> contentsDer;
    if (hexEnd > hexBegin) {
        contentsDer.reserve(static_cast<std::size_t>(hexEnd - hexBegin) / 2);
        for (std::int64_t i = hexBegin; i + 1 < hexEnd; i += 2) {
            const auto hexDigit = [&](std::int64_t pos) -> int {
                const char c = created.bytes[static_cast<std::size_t>(pos)];
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            const int high = hexDigit(i);
            const int low = hexDigit(i + 1);
            if (high < 0 || low < 0) break;
            contentsDer.push_back(static_cast<std::uint8_t>((high << 4) | low));
        }
    }
    const std::vector<std::uint8_t> trimmed = trimDerPadding(contentsDer);

    return verifyDetachedPkcs7(signedBytes, trimmed, coverage, trust, verifyOptions, revocation);
}

}  // namespace

BatchSignResult signBatch(const SigningIdentity& identity, const std::vector<BatchSignItem>& items,
                          const TrustStore* trust, const VerifyOptions& verifyOptions,
                          RevocationChecker* revocation) {
    BatchSignResult result;
    result.items.reserve(items.size());
    for (const BatchSignItem& item : items) {
        BatchSignItemResult itemResult;
        itemResult.create = createSignature(item.sourceBytes, identity, item.options);
        if (itemResult.create.ok && trust != nullptr) {
            itemResult.verifyAttempted = true;
            itemResult.verify =
                verifyJustCreatedSignature(itemResult.create, *trust, verifyOptions, revocation);
        }
        result.items.push_back(std::move(itemResult));
    }
    return result;
}

}  // namespace alioth::engine::signature
