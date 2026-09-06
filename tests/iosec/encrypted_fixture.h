#pragma once

// 產生一份「真的」被標準安全處理器保護的 PDF：Filter /Standard、V 1、R 2、
// RC4 40 位元，使用者密碼留空、擁有者密碼另設。
//
// 為什麼不能像 tests/objects/object_fixture.h 那樣用假的 /O /U 佔位符：
// 那份語料只需要讓我們自己的物件層寫入通道認出 /Encrypt 並拒絕，PDFium
// 完全不會嘗試解密它。但 PRD-SEC-002 的「移除密碼」要驗的是 PDFium 真的能
// 打開一份加密文件、驗出正確的權限旗標，再整份解密重寫——這條路徑不打開
// 一份「真的」通得過標準安全處理器驗證演算法的文件就測不到。
//
// 演算法依 ISO 32000-1 附錄 7.6.3（Algorithm 2/3/4，R2）：
//   Algorithm 3（擁有者值 O）：O = RC4(MD5(pad(擁有者密碼))[0:5], pad(使用者密碼))
//   Algorithm 2（檔案金鑰）  ：key = MD5(pad(使用者密碼) + O + P(4 bytes LE) + ID0)[0:5]
//   Algorithm 4（使用者值 U）：U = RC4(key, 標準填充字串)
// 內容串流刻意沒有真的用這把金鑰加密——載入與密碼驗證只碰物件層的
// /O /U /P /ID，不會在載入當下解開內容串流（那是渲染時才做的事），
// 因此這裡不需要為了「打得開」而真的加密內容，這份語料的用途只到
// 「驗證與權限」為止，不驗染色結果。

#include <openssl/evp.h>

#include <QByteArray>

#include <cstdint>
#include <cstring>
#include <vector>

namespace alioth::test {

namespace detail {

inline const unsigned char kStandardPad[32] = {
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
    0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A,
};

inline std::vector<unsigned char> padPassword(const std::string& password) {
    std::vector<unsigned char> out(password.begin(), password.end());
    if (out.size() > 32) out.resize(32);
    const std::size_t need = 32 - out.size();
    out.insert(out.end(), kStandardPad, kStandardPad + need);
    return out;
}

inline std::vector<unsigned char> md5(const std::vector<unsigned char>& data) {
    std::vector<unsigned char> digest(EVP_MAX_MD_SIZE);
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_md5(), nullptr);
    digest.resize(length);
    return digest;
}

// 標準安全處理器唯一使用的串流加密法，同時是加密與解密（對稱、自反）。
inline std::vector<unsigned char> rc4(const std::vector<unsigned char>& key,
                                      const std::vector<unsigned char>& data) {
    unsigned char state[256];
    for (int i = 0; i < 256; ++i) state[i] = static_cast<unsigned char>(i);
    int j = 0;
    for (int i = 0; i < 256; ++i) {
        j = (j + state[i] + key[static_cast<std::size_t>(i) % key.size()]) & 0xFF;
        std::swap(state[i], state[j]);
    }
    std::vector<unsigned char> out(data.size());
    int i = 0;
    j = 0;
    for (std::size_t n = 0; n < data.size(); ++n) {
        i = (i + 1) & 0xFF;
        j = (j + state[i]) & 0xFF;
        std::swap(state[i], state[j]);
        const unsigned char k = state[(state[i] + state[j]) & 0xFF];
        out[n] = static_cast<unsigned char>(data[n] ^ k);
    }
    return out;
}

inline QByteArray toHex(const std::vector<unsigned char>& bytes) {
    QByteArray hex;
    hex.reserve(static_cast<int>(bytes.size()) * 2);
    static const char* kDigits = "0123456789ABCDEF";
    for (const unsigned char b : bytes) {
        hex.append(kDigits[b >> 4]);
        hex.append(kDigits[b & 0x0F]);
    }
    return hex;
}

}  // namespace detail

struct EncryptedPdfSpec {
    QByteArray bytes;
    std::int32_t permissionsRaw{0};  // 寫進 /P 的那個帶號整數的原始位元樣式
};

// userPassword 留空代表任何人都能打開（唯一受限的是權限旗標）；
// ownerPassword 決定「解除限制」需要的密碼，本測試不需要用到它來開檔。
inline EncryptedPdfSpec makeRc4EncryptedPdf(const std::string& userPassword,
                                            const std::string& ownerPassword,
                                            std::int32_t permissions) {
    using namespace detail;

    const std::vector<unsigned char> paddedUser = padPassword(userPassword);
    const std::vector<unsigned char> paddedOwner = padPassword(ownerPassword);

    const std::vector<unsigned char> ownerKey = [&] {
        auto digest = md5(paddedOwner);
        digest.resize(5);
        return digest;
    }();
    const std::vector<unsigned char> oValue = rc4(ownerKey, paddedUser);

    const std::vector<unsigned char> id(16, 0x5A);  // 任意 16 位元組，兩個 /ID 條目共用同一份

    std::vector<unsigned char> keyInput = paddedUser;
    keyInput.insert(keyInput.end(), oValue.begin(), oValue.end());
    const auto p = static_cast<std::uint32_t>(permissions);
    keyInput.push_back(static_cast<unsigned char>(p & 0xFF));
    keyInput.push_back(static_cast<unsigned char>((p >> 8) & 0xFF));
    keyInput.push_back(static_cast<unsigned char>((p >> 16) & 0xFF));
    keyInput.push_back(static_cast<unsigned char>((p >> 24) & 0xFF));
    keyInput.insert(keyInput.end(), id.begin(), id.end());

    const std::vector<unsigned char> fileKey = [&] {
        auto digest = md5(keyInput);
        digest.resize(5);
        return digest;
    }();

    const std::vector<unsigned char> standardPad(kStandardPad, kStandardPad + 32);
    const std::vector<unsigned char> uValue = rc4(fileKey, standardPad);

    const QByteArray oHex = toHex(oValue);
    const QByteArray uHex = toHex(uValue);
    const QByteArray idHex = toHex(id);

    const QByteArray content = "0 0 0 rg\n0 0 100 200 re\nf\n";

    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
        "/Resources << >> >>");
    objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                      content + "endstream");
    objects.push_back("<< /Filter /Standard /V 1 /R 2 /O <" + oHex + "> /U <" + uHex + "> /P " +
                      QByteArray::number(permissions) + " >>");

    QByteArray pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<int> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(static_cast<int>(i) + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const int xrefOffset = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(static_cast<int>(objects.size()) + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           " /Root 1 0 R /Encrypt 5 0 R /ID [<" + idHex + "> <" + idHex + ">] >>\nstartxref\n" +
           QByteArray::number(xrefOffset) + "\n%%EOF\n";

    EncryptedPdfSpec spec;
    spec.bytes = pdf;
    spec.permissionsRaw = permissions;
    return spec;
}

}  // namespace alioth::test
