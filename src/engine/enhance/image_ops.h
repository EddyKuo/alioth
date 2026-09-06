#pragma once

// 掃描增強的像素演算法（WBS 14，PRD-ENH-002）。
//
// 本檔的每一個函式都是純函數：輸入像素緩衝、輸出像素緩衝或純量，不碰 PDF、
// 不碰 PDFium、也不碰 Qt。這個切分是刻意的——去斜偵測是本工作包唯一會
// 「安靜地做錯」的部分（角度差 0.3 度看起來仍然像是正的，直到列印才發現），
// 因此它必須能用合成影像密集驗證，而不是靠開一份掃描件目視檢查。
//
// 像素格式沿用引擎既有的 PixelBuffer（BGRA、預乘），不另立一套：
// 點陣化的輸出直接來自 PDFium 的點陣圖，中途換格式只會多一次全圖複製。

#include <array>
#include <cstdint>
#include <vector>

#include "domain/enhance.h"
#include "engine/pixel_buffer.h"

namespace alioth::engine::enhance {

// 單通道灰階影像。去斜分析只需要明暗，帶著三個顏色通道走會讓內層迴圈
// 多兩次無用的記憶體存取，而那個迴圈要跑幾十個候選角度。
struct GrayImage {
    std::int32_t width{0};
    std::int32_t height{0};
    std::vector<std::uint8_t> pixels{};

    [[nodiscard]] bool isNull() const noexcept { return width <= 0 || height <= 0 || pixels.empty(); }
    [[nodiscard]] std::uint8_t at(std::int32_t x, std::int32_t y) const noexcept {
        return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(x)];
    }
    [[nodiscard]] std::uint8_t* row(std::int32_t y) noexcept {
        return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
    }
};

// BGRA → 灰階。係數用 ITU-R BT.601 的亮度權重，而不是三通道平均：
// 平均會讓純藍的印章與純黃的螢光筆得到相同的灰階值，
// 去斜的投影剖面因此會把不存在的結構算進去。
[[nodiscard]] GrayImage toGrayscale(const PixelBuffer& source);

// 盒式降取樣到指定最長邊。用盒式平均而不是最近鄰：最近鄰在縮小時會直接丟掉
// 整列像素，細筆畫的文字行可能整行消失，投影剖面就沒有峰可找了。
[[nodiscard]] GrayImage downscaleToMaxEdge(const GrayImage& source, std::int32_t maxEdge);

[[nodiscard]] std::array<std::uint64_t, 256> grayHistogram(const GrayImage& image);

// 深複製。PixelBuffer 不可複製（它是零複製渲染的載體，隱含複製一定是錯誤），
// 因此需要複製時必須明講。
[[nodiscard]] PixelBuffer clonePixels(const PixelBuffer& source);

// 以指定灰階值填滿的緩衝區。測試與旋轉的背景填色共用。
[[nodiscard]] PixelBuffer makeBuffer(std::int32_t width, std::int32_t height, std::uint8_t gray);

// 去斜角偵測：投影剖面法（PRD-ENH-002）。
//
// 為什麼選投影剖面而不是 Hough：
//
//   Hough 找的是「直線」。掃描的文字頁上並沒有真正的直線，有的是一排排字；
//   Hough 要先做邊緣偵測再累積參數空間，對純文字頁的可靠度取決於邊緣門檻，
//   而那個門檻隨掃描機的對比而變。投影剖面直接量的是「哪個角度下，
//   墨水最集中在少數幾條水平帶上」，那正是文字行的定義，
//   不需要邊緣偵測，也就少了一個要調的參數。
//
//   代價是投影剖面對「行結構」的依賴：
//   - 單欄或多欄的文字頁：可靠
//   - 整頁表格／格線：仍可靠（格線本身就是強烈的行結構）
//   - 整頁照片、地圖、工程圖：**會失敗**，且失敗方式是回報一個由雜訊決定的
//     隨機角度。這正是 minScoreGain 門檻存在的理由——分數曲線太平時
//     一律回報「沒有傾斜」，寧可漏判也不要把正常的圖轉歪
//   - 傾斜超過 maxAngleDeg（例如整頁被掃成橫的）：本方法量不出來，
//     那屬於頁面旋轉而不是去斜，應由 /Rotate 處理
//   - 文字量極少的頁（只有頁碼）：墨水像素太少，同樣以門檻擋掉
//
// 回傳的角度為「內容目前的傾斜量」，正值代表在螢幕座標下逆時針傾斜；
// 校正要施加的旋轉是它的相反數（domain::enhance::correctionAngleDeg）。
[[nodiscard]] domain::enhance::DeskewResult detectSkew(
    const GrayImage& image, const domain::enhance::DeskewSettings& settings = {});

// PixelBuffer 版本，內含灰階化與降取樣。
[[nodiscard]] domain::enhance::DeskewResult detectSkew(
    const PixelBuffer& image, const domain::enhance::DeskewSettings& settings = {});

// 繞中心旋轉，輸出尺寸與輸入相同，露出的角落以 background 灰階填滿。
//
// 尺寸不變是刻意的：去斜之後的影像要放回原來的頁面框裡，
// 若跟著長大就得同時改 /MediaBox，那會牽動所有註解的座標。
// 正角度在螢幕座標（Y 向下）看起來是逆時針，與 detectSkew 的符號一致。
[[nodiscard]] PixelBuffer rotate(const PixelBuffer& source, double degrees,
                                 std::uint8_t background = 255);

// 對比、亮度、灰階、二值化。順序固定為「色調曲線 → 灰階 → 二值化」：
// 先二值化再調對比毫無意義（只剩兩個值），而先灰階再調對比會讓
// 有色印章的可讀性選擇權消失。
[[nodiscard]] PixelBuffer applyEnhancement(const PixelBuffer& source,
                                           const domain::enhance::EnhanceSettings& settings);

}  // namespace alioth::engine::enhance
