#include "engine/signature/dss_builder.h"

#include <openssl/err.h>
#include <openssl/evp.h>

#include <map>
#include <utility>

#include "engine/objects/pdf_object.h"

namespace alioth::engine::signature {

namespace obj = alioth::engine::objects;

namespace {

// key -> 已寫入的物件編號，讓同一份 DER（例如多個簽章共用的根 CA 憑證）
// 只落成一個串流物件，其餘位置全部參照它。
using InternTable = std::map<std::string, int>;

[[nodiscard]] int internBytes(objects::IncrementalAppender& appender,
                              const std::vector<std::uint8_t>& bytes, InternTable& table,
                              obj::PdfArray& globalArray) {
    const std::string key(bytes.begin(), bytes.end());
    if (const auto it = table.find(key); it != table.end()) return it->second;

    const int number = appender.allocateObject();
    obj::PdfStream stream;
    stream.data.assign(bytes.begin(), bytes.end());
    appender.setObject(number, obj::PdfObject{std::move(stream)});
    table.emplace(key, number);
    globalArray.emplace_back(obj::makeRef(number));
    return number;
}

}  // namespace

std::string vriKeyForContents(const std::vector<std::uint8_t>& contentsRawBytes) {
    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int length = 0;
    if (EVP_Digest(contentsRawBytes.data(), contentsRawBytes.size(), digest, &length, EVP_sha1(),
                   nullptr) != 1) {
        ERR_clear_error();
        return {};
    }
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(static_cast<std::size_t>(length) * 2);
    for (unsigned int i = 0; i < length; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

DssBuildResult appendDocumentSecurityStore(objects::IncrementalAppender& appender,
                                           const DssBuildOptions& options) {
    DssBuildResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "附加器尚未開啟原始文件";
        return result;
    }

    InternTable certTable;
    InternTable crlTable;
    InternTable ocspTable;
    obj::PdfArray certsArray;
    obj::PdfArray crlsArray;
    obj::PdfArray ocspsArray;

    for (const auto& bytes : options.sharedCerts) internBytes(appender, bytes, certTable, certsArray);
    for (const auto& bytes : options.sharedCrls) internBytes(appender, bytes, crlTable, crlsArray);
    for (const auto& bytes : options.sharedOcspResponses) {
        internBytes(appender, bytes, ocspTable, ocspsArray);
    }

    obj::PdfDictionary vri;
    for (const SignatureEvidence& evidence : options.perSignature) {
        if (evidence.sha1OfContentsHex.empty()) {
            result.diagnostic = "簽章證據缺少 /VRI 鍵（sha1OfContentsHex 為空）";
            return result;
        }

        obj::PdfArray sigCerts;
        obj::PdfArray sigCrls;
        obj::PdfArray sigOcsps;
        for (const auto& bytes : evidence.certs) {
            sigCerts.emplace_back(obj::makeRef(internBytes(appender, bytes, certTable, certsArray)));
        }
        for (const auto& bytes : evidence.crls) {
            sigCrls.emplace_back(obj::makeRef(internBytes(appender, bytes, crlTable, crlsArray)));
        }
        for (const auto& bytes : evidence.ocspResponses) {
            sigOcsps.emplace_back(obj::makeRef(internBytes(appender, bytes, ocspTable, ocspsArray)));
        }

        obj::PdfDictionary vriEntry;
        if (!sigCerts.empty()) vriEntry.set("Cert", obj::PdfObject{std::move(sigCerts)});
        if (!sigCrls.empty()) vriEntry.set("CRL", obj::PdfObject{std::move(sigCrls)});
        if (!sigOcsps.empty()) vriEntry.set("OCSP", obj::PdfObject{std::move(sigOcsps)});
        vri.set(evidence.sha1OfContentsHex, obj::PdfObject{std::move(vriEntry)});
    }

    obj::PdfDictionary dss;
    if (!certsArray.empty()) dss.set("Certs", obj::PdfObject{std::move(certsArray)});
    if (!crlsArray.empty()) dss.set("CRLs", obj::PdfObject{std::move(crlsArray)});
    if (!ocspsArray.empty()) dss.set("OCSPs", obj::PdfObject{std::move(ocspsArray)});
    if (!vri.empty()) dss.set("VRI", obj::PdfObject{std::move(vri)});

    const int dssNumber = appender.allocateObject();
    appender.setObject(dssNumber, obj::PdfObject{std::move(dss)});

    const obj::PdfObject* rootEntry = appender.source().trailer().find("Root");
    if (rootEntry == nullptr || !rootEntry->isRef()) {
        result.diagnostic = "trailer 缺少 /Root，無法定位目錄物件";
        return result;
    }
    const obj::PdfRef rootRef = rootEntry->asRef();

    obj::PdfObject catalogObject = appender.currentObject(rootRef.number);
    obj::PdfDictionary* catalogDict = catalogObject.asDictionary();
    if (catalogDict == nullptr) {
        result.diagnostic = "目錄物件（/Root）不是字典，檔案可能已損毀";
        return result;
    }
    obj::PdfDictionary catalog = *catalogDict;
    catalog.set("DSS", obj::makeRef(dssNumber));

    if (!appender.updateObject(rootRef.number, obj::PdfObject{std::move(catalog)})) {
        result.diagnostic = "更新目錄（/Root）失敗";
        return result;
    }

    result.ok = true;
    result.dssObject = dssNumber;
    result.certCount = certTable.size();
    result.crlCount = crlTable.size();
    result.ocspCount = ocspTable.size();
    return result;
}

}  // namespace alioth::engine::signature
