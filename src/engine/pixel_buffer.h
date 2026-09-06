#pragma once

// BGRA 預乘像素緩衝區。
//
// 這是零複製渲染的載體：PDFium 透過 FPDFBitmap_CreateEx 直接寫入本緩衝區，
// 呈現層再以 QImage 包裝同一塊記憶體，全程不複製像素。
//
// stride 一律顯式攜帶，不得由呼叫端以「寬 × 4」推算。BGRA 下兩者通常相等，
// 但一旦引入對齊或子區域檢視就會不等，而錯誤的 stride 產生的是斜切畫面
// 而非崩潰——這種錯誤在測試中極難察覺。

#include <cstddef>
#include <cstdint>
#include <memory>

namespace alioth::engine {

class PixelBuffer {
public:
    PixelBuffer() = default;

    PixelBuffer(std::int32_t width, std::int32_t height)
        : width_(width), height_(height), stride_(static_cast<std::size_t>(width) * 4) {
        if (width > 0 && height > 0) {
            data_ = std::make_unique<std::uint8_t[]>(stride_ * static_cast<std::size_t>(height));
        }
    }

    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }
    [[nodiscard]] std::size_t stride() const noexcept { return stride_; }
    [[nodiscard]] std::size_t sizeBytes() const noexcept {
        return stride_ * static_cast<std::size_t>(height_ > 0 ? height_ : 0);
    }
    [[nodiscard]] bool isNull() const noexcept { return data_ == nullptr; }

    [[nodiscard]] std::uint8_t* data() noexcept { return data_.get(); }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_.get(); }

    [[nodiscard]] std::uint8_t* scanline(std::int32_t y) noexcept {
        return data_.get() + stride_ * static_cast<std::size_t>(y);
    }

    void fill(std::uint8_t value) noexcept;

private:
    std::int32_t width_{0};
    std::int32_t height_{0};
    std::size_t stride_{0};
    std::unique_ptr<std::uint8_t[]> data_;
};

using PixelBufferPtr = std::shared_ptr<const PixelBuffer>;

}  // namespace alioth::engine
