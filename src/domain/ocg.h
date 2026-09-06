#pragma once

// 圖層（OCG，Optional Content Group）面板的領域模型（PRD-VIEW-008）。
//
// 這一層只描述「面板上看到的樹狀結構與可見性狀態」，不認識 PDFium 也不認識
// PDF 語法——那些由 engine::layers::readOcgTree（見 engine/layers/ocg_reader.h）
// 負責解析並轉換成這裡的型別。
//
// 重要限制（已於 M0 實測，見 exceptions/EXC_20260906_RD_SA_ocg_render_gap.md）：
// PDFium 目前對外的公開 C API 沒有任何可以在渲染時指定執行期 OC（Optional
// Content）狀態的入口——FPDF_RenderPageBitmap 系列一律採用文件內建 /OCProperties
// /D 的預設可見性，且該預設在文件開啟當下就固定。因此本檔案定義的 visible 旗標
// 目前只驅動面板 UI 本身（勾選狀態、面板顯示/隱藏），**不會**回饋進渲染管線。
// 這不是本檔案的臆測，而是查過 third_party/pdfium/include 全部標頭後的結論：
// 找不到任何 OCG 可見性的 setter。PRD-VIEW-008 的「勾選後渲染即時更新」驗收
// 標準在目前的預編譯 PDFium 上做不到，需要 SA 決策（改用圖層攤平近似，或等待
// 自建 PDFium）。

#include <cstdint>
#include <string>
#include <vector>

namespace alioth::domain {

// 一個圖層節點。既可能是真正的 OCG（objectNumber > 0），也可能是 /Order
// 陣列裡用文字字串表示的純分組標題（objectNumber < 0，isGroupHeading = true，
// 只能展開/收合，不能勾選）。
struct OcgLayer {
    std::int32_t objectNumber{0};
    std::string name;
    bool visible{true};
    bool locked{false};
    bool isGroupHeading{false};
    // -1 表示不屬於任何互斥群組（/RBGroups，一組內同時最多一個可見）。
    std::int32_t radioGroup{-1};
    std::vector<std::int32_t> children;  // 對 OcgTree::layers 的索引
};

struct OcgTree {
    std::vector<OcgLayer> layers;
    std::vector<std::int32_t> roots;  // 對 layers 的索引，依面板顯示順序
    // 文件完全沒有 /OCProperties 時為 false：這是常態而不是錯誤，
    // 面板應該顯示「本文件沒有圖層」而不是空清單。
    bool present{false};

    [[nodiscard]] const OcgLayer* find(std::int32_t objectNumber) const noexcept {
        for (const auto& layer : layers) {
            if (layer.objectNumber == objectNumber) return &layer;
        }
        return nullptr;
    }
    [[nodiscard]] OcgLayer* find(std::int32_t objectNumber) noexcept {
        for (auto& layer : layers) {
            if (layer.objectNumber == objectNumber) return &layer;
        }
        return nullptr;
    }
};

// 依 /RBGroups 語意切換可見性：把 index 設為可見時，同一互斥群組（radioGroup
// 相同）的其餘節點自動設為不可見。鎖定的節點忽略本次請求（回傳空清單）——
// 鎖定與否由呼叫端在 UI 層決定要不要先擋下互動，這裡是最後一道防線。
//
// 回傳實際被改動可見性的節點索引，方便呼叫端只局部刷新面板而不必整棵重畫。
[[nodiscard]] inline std::vector<std::int32_t> setLayerVisible(OcgTree& tree, std::int32_t index,
                                                                bool visible) {
    std::vector<std::int32_t> changed;
    if (index < 0 || static_cast<std::size_t>(index) >= tree.layers.size()) return changed;
    OcgLayer& target = tree.layers[static_cast<std::size_t>(index)];
    if (target.locked || target.isGroupHeading) return changed;
    if (target.visible != visible) {
        target.visible = visible;
        changed.push_back(index);
    }
    if (visible && target.radioGroup >= 0) {
        for (std::size_t i = 0; i < tree.layers.size(); ++i) {
            if (static_cast<std::int32_t>(i) == index) continue;
            OcgLayer& other = tree.layers[i];
            if (other.radioGroup == target.radioGroup && other.visible) {
                other.visible = false;
                changed.push_back(static_cast<std::int32_t>(i));
            }
        }
    }
    return changed;
}

// OCMD（Optional Content Membership Dictionary，ISO 32000-1 §8.11.2.3）的
// 可見性政策。一個 OCMD 用 /P 決定它底下一組 OCG 要怎麼合併成單一個可見性：
// 沒有 /P 時規範預設是 AnyOn。
enum class OcmdPolicy : std::uint8_t {
    AnyOn,   // 任一成員可見即可見（預設）
    AllOn,   // 全部成員可見才可見
    AnyOff,  // 任一成員不可見即可見
    AllOff,  // 全部成員不可見才可見
};

// 純邏輯：給定政策與每個成員目前的可見性，算出 OCMD 本身的可見性。
// 不認得 PDF 語法——成員清單怎麼從 /OCGs 展開、/VE 可見性運算式要不要支援，
// 都是呼叫端（engine::layers::ocg_flatten）的事，這裡只負責布林合併，
// 因此可以離開 PDF 剖析器單獨測到底。
//
// 空清單（OCMD 的 /OCGs 缺漏或無法解析任何成員）沒有規範定義的答案；
// 依專案一貫的「寧可多顯示也不要少顯示」保守方向回傳 true。
[[nodiscard]] inline bool resolveOcmdPolicy(OcmdPolicy policy,
                                             const std::vector<bool>& memberVisible) noexcept {
    if (memberVisible.empty()) return true;
    switch (policy) {
        case OcmdPolicy::AnyOn:
            for (const bool visible : memberVisible) {
                if (visible) return true;
            }
            return false;
        case OcmdPolicy::AllOn:
            for (const bool visible : memberVisible) {
                if (!visible) return false;
            }
            return true;
        case OcmdPolicy::AnyOff:
            for (const bool visible : memberVisible) {
                if (!visible) return true;
            }
            return false;
        case OcmdPolicy::AllOff:
            for (const bool visible : memberVisible) {
                if (visible) return false;
            }
            return true;
    }
    return true;
}

}  // namespace alioth::domain
