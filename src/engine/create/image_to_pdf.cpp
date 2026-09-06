#include "engine/create/image_to_pdf.h"

#include <cstdint>
#include <utility>

namespace alioth::engine::create {

using domain::create::ImageImportOptions;
using domain::create::ImagePixelFormat;
using domain::create::ImagePlacement;
using domain::create::SourceImage;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfStream;

namespace {

std::string toBytes(const std::vector<std::uint8_t>& data) {
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

// 從 RGBA 拆出 RGB 與 A 兩份。兩趟迴圈而不是一趟寫兩個緩衝區，是因為
// 兩份資料的用途完全不同（一份進主影像、一份進 /SMask），分開比較不會
// 在之後加上 CMYK 或 16-bit 支援時把兩個分支糾纏在一起。
std::string extractRgb(const std::vector<std::uint8_t>& rgba) {
    std::string rgb;
    rgb.reserve(rgba.size() / 4 * 3);
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        rgb.push_back(static_cast<char>(rgba[i]));
        rgb.push_back(static_cast<char>(rgba[i + 1]));
        rgb.push_back(static_cast<char>(rgba[i + 2]));
    }
    return rgb;
}

std::string extractAlpha(const std::vector<std::uint8_t>& rgba) {
    std::string alpha;
    alpha.reserve(rgba.size() / 4);
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        alpha.push_back(static_cast<char>(rgba[i]));
    }
    return alpha;
}

PdfStream makeSampledImageStream(int width, int height, const char* colorSpace,
                                 std::string samples) {
    PdfStream stream;
    stream.dict.set("Type", objects::makeName("XObject"));
    stream.dict.set("Subtype", objects::makeName("Image"));
    stream.dict.set("Width", PdfObject{static_cast<std::int64_t>(width)});
    stream.dict.set("Height", PdfObject{static_cast<std::int64_t>(height)});
    stream.dict.set("ColorSpace", objects::makeName(colorSpace));
    stream.dict.set("BitsPerComponent", PdfObject{static_cast<std::int64_t>(8)});

    std::string compressed = deflateBytes(samples);
    if (!compressed.empty() && compressed.size() < samples.size()) {
        stream.dict.set("Filter", objects::makeName("FlateDecode"));
        stream.data = std::move(compressed);
    } else {
        // 壓不小就不壓。宣稱 FlateDecode 卻塞未壓縮位元組是那種
        // 「檔案看起來正常直到某個解析器照規格讀」的錯誤。
        stream.data = std::move(samples);
    }
    return stream;
}

std::string makeDrawContent(const ImagePlacement& placement, const std::string& resourceName) {
    // 影像的單位空間是 [0,1]×[0,1]，因此 cm 矩陣的縮放量直接就是目標尺寸。
    std::string content = "q\n";
    content += objects::formatReal(placement.rect.width());
    content += " 0 0 ";
    content += objects::formatReal(placement.rect.height());
    content += ' ';
    content += objects::formatReal(placement.rect.left);
    content += ' ';
    content += objects::formatReal(placement.rect.bottom);
    content += " cm\n/";
    content += objects::escapeName(resourceName);
    content += " Do\nQ\n";
    return content;
}

}  // namespace

int addImagePage(PdfDocumentBuilder& builder, const SourceImage& image,
                 const ImageImportOptions& options, std::string* diagnostic,
                 bool* producedSmask) {
    if (producedSmask != nullptr) *producedSmask = false;
    if (!image.valid()) {
        if (diagnostic != nullptr) {
            *diagnostic = "影像無效：尺寸為零，或位元組數與寬×高×通道數不符";
        }
        return 0;
    }

    const ImagePlacement placement = domain::create::computeImagePlacement(image, options);
    if (placement.pageWidthPt <= 0.0 || placement.pageHeightPt <= 0.0 ||
        placement.rect.isEmpty()) {
        if (diagnostic != nullptr) *diagnostic = "換算後的頁面或影像尺寸為零";
        return 0;
    }

    PdfStream imageStream;
    int smaskNumber = 0;

    if (image.format == ImagePixelFormat::JpegEncoded) {
        // /Width /Height 必須與 JPEG 內部的框架標頭一致，否則多數檢視器會
        // 顯示成斜條紋而不是報錯。這裡拿探測結果去對帳而不是覆寫呼叫端的值：
        // 兩者不符代表上游拿錯了資料，那是必須講出來的錯，不是可以默默修好的。
        const domain::create::JpegProbe probe = domain::create::probeJpeg(image.bytes);
        if (!probe.ok) {
            if (diagnostic != nullptr) *diagnostic = "無法在 JPEG 資料裡找到框架標頭";
            return 0;
        }
        if (probe.width != image.width || probe.height != image.height) {
            if (diagnostic != nullptr) {
                *diagnostic = "JPEG 標頭的尺寸（" + std::to_string(probe.width) + "×" +
                              std::to_string(probe.height) + "）與呼叫端宣告的（" +
                              std::to_string(image.width) + "×" + std::to_string(image.height) +
                              "）不符";
            }
            return 0;
        }
        if (probe.components == 4) {
            if (diagnostic != nullptr) {
                *diagnostic = "CMYK JPEG 需要 /Decode 反轉與 Adobe APP14 判讀，目前不支援";
            }
            return 0;
        }

        imageStream.dict.set("Type", objects::makeName("XObject"));
        imageStream.dict.set("Subtype", objects::makeName("Image"));
        imageStream.dict.set("Width", PdfObject{static_cast<std::int64_t>(image.width)});
        imageStream.dict.set("Height", PdfObject{static_cast<std::int64_t>(image.height)});
        imageStream.dict.set(
            "ColorSpace",
            objects::makeName(probe.components == 1 ? "DeviceGray" : "DeviceRGB"));
        imageStream.dict.set("BitsPerComponent", PdfObject{static_cast<std::int64_t>(8)});
        imageStream.dict.set("Filter", objects::makeName("DCTDecode"));
        // 這一行是整條路徑的重點：原始位元組原封不動。
        imageStream.data = toBytes(image.bytes);
    } else if (image.format == ImagePixelFormat::Gray8) {
        imageStream = makeSampledImageStream(image.width, image.height, "DeviceGray",
                                             toBytes(image.bytes));
    } else if (image.format == ImagePixelFormat::Rgb8) {
        imageStream =
            makeSampledImageStream(image.width, image.height, "DeviceRGB", toBytes(image.bytes));
    } else {
        imageStream = makeSampledImageStream(image.width, image.height, "DeviceRGB",
                                             extractRgb(image.bytes));
        PdfStream mask = makeSampledImageStream(image.width, image.height, "DeviceGray",
                                                extractAlpha(image.bytes));
        smaskNumber = builder.allocateObject();
        builder.setObject(smaskNumber, PdfObject{std::move(mask)});
        imageStream.dict.set("SMask", objects::makeRef(smaskNumber));
        if (producedSmask != nullptr) *producedSmask = true;
    }

    const int imageNumber = builder.allocateObject();
    builder.setObject(imageNumber, PdfObject{std::move(imageStream)});

    const std::string resourceName = "Im0";
    PdfDictionary xobjects;
    xobjects.set(resourceName, objects::makeRef(imageNumber));
    PdfDictionary resources;
    resources.set("XObject", PdfObject{std::move(xobjects)});

    return builder.addPage(placement.pageWidthPt, placement.pageHeightPt,
                           makeDrawContent(placement, resourceName), std::move(resources));
}

ImageImportResult createPdfFromImages(const std::vector<SourceImage>& images,
                                      const ImageImportOptions& options) {
    ImageImportResult result;
    if (images.empty()) {
        result.diagnostic = "沒有輸入影像";
        return result;
    }

    PdfDocumentBuilder builder;
    builder.setProducer("Alioth image import");

    for (std::size_t i = 0; i < images.size(); ++i) {
        std::string diagnostic;
        bool smask = false;
        if (addImagePage(builder, images[i], options, &diagnostic, &smask) == 0) {
            // 一張壞掉就整批失敗，而不是靜默跳過：使用者選了 20 張、
            // 拿到 19 頁的 PDF 卻沒有任何提示，是最難察覺的一種資料遺失。
            result.diagnostic = "第 " + std::to_string(i + 1) + " 張影像無法轉換：" + diagnostic;
            return result;
        }
        if (smask) ++result.smaskCount;
        result.placements.push_back(domain::create::computeImagePlacement(images[i], options));
    }

    DocumentBuildResult built = builder.build();
    if (!built.ok) {
        result.diagnostic = built.diagnostic;
        return result;
    }

    result.ok = true;
    result.bytes = std::move(built.bytes);
    result.pageCount = builder.pageCount();
    return result;
}

}  // namespace alioth::engine::create
