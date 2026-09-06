#pragma once

// 影像編解碼（WBS 14，PRD-ENH-003 / 004）。
//
// 這是本工作包唯一需要 Qt 的地方。理由是 /DCTDecode 就是 JPEG，而專案的
// 三個引擎級元件裡只有 Qt 有 JPEG 編碼器——PDFium 只解不編。
// 為了不讓 Qt 的型別滲進其他檔案，這一層的介面只用 PixelBuffer 與 std::string。
//
// 一個容易寫錯的地方：PDF 的影像 XObject 沒有 alpha 通道，透明度一律靠
// /SMask（另一張灰階影像）。因此編碼的輸出可能是「兩份位元組」，
// 呼叫端必須兩份一起寫進去；只寫彩色那份，原本半透明的地方會變成不透明的黑。

#include <cstdint>
#include <string>

#include "domain/enhance.h"
#include "engine/pixel_buffer.h"

namespace alioth::engine::enhance {

struct EncodedImage {
    bool ok{false};
    std::string diagnostic{};

    std::string data{};                 // 串流位元組（已套用 filter）
    std::string filter{};               // "DCTDecode" 或 "FlateDecode"
    std::string colorSpace{"DeviceRGB"};// "DeviceRGB" 或 "DeviceGray"
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t bitsPerComponent{8};

    // /SMask 的位元組（灰階、FlateDecode）。來源不透明時為空。
    std::string softMaskData{};

    [[nodiscard]] std::int64_t byteSize() const noexcept {
        return static_cast<std::int64_t>(data.size() + softMaskData.size());
    }
};

struct DecodedImage {
    bool ok{false};
    std::string diagnostic{};
    PixelBuffer pixels{};
    bool hasAlpha{false};
};

// 影像檔位元組（JPEG / PNG / BMP…）→ BGRA 像素。背景影像的來源走這條。
[[nodiscard]] DecodedImage decodeImageFile(const std::string& bytes);

// 已解開濾鏡的原始取樣值 → BGRA。重壓縮時，FlateDecode 的影像走這條。
// 只支援 8 位元的 DeviceGray / DeviceRGB：其餘（索引色、CMYK、1 位元）
// 一律明確失敗，因為猜錯色彩空間的結果是顏色整片錯亂而不是崩潰。
[[nodiscard]] DecodedImage decodeRawSamples(const std::string& samples, std::int32_t width,
                                            std::int32_t height, std::int32_t bitsPerComponent,
                                            const std::string& colorSpace);

// JPEG 位元組（/DCTDecode 的串流內容本身就是 JPEG）→ BGRA。
[[nodiscard]] DecodedImage decodeJpeg(const std::string& bytes);

[[nodiscard]] EncodedImage encodeJpeg(const PixelBuffer& pixels, std::int32_t quality);

// 無損。全灰的來源自動降成 DeviceGray：二值化後的掃描頁若仍以三通道寫出，
// 體積是三倍而畫面完全相同。
[[nodiscard]] EncodedImage encodeFlate(const PixelBuffer& pixels);

// 依設定挑編碼。Auto 會兩種都編一次再取小的——這比用啟發式猜可靠，
// 而且點陣化本來就只做一次，多編一次的成本遠低於猜錯的體積代價。
[[nodiscard]] EncodedImage encodeImage(const PixelBuffer& pixels,
                                       const domain::enhance::CompressionSettings& settings);

// 雙線性縮放。重取樣比降品質更有效：600 dpi 的掃描件降到 200 dpi 就少掉九成像素。
[[nodiscard]] PixelBuffer resample(const PixelBuffer& source, std::int32_t width,
                                   std::int32_t height);

}  // namespace alioth::engine::enhance
