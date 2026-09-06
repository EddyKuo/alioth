#pragma once

// 安全性原則建立與套用（PRD-SEC-003，R3，能做多少做多少）。
//
// 「原則」在 Acrobat 裡是一組可重複套用的安全設定：密碼、權限旗標、
// 描述文字，存起來之後可以一次套到很多份文件上，不必每份重新設定一次。
//
// 這裡誠實劃一條線：
//   - 建立、儲存、列舉、刪除原則 —— 可行，純粹是記憶體裡的資料管理。
//   - 套用「不要求密碼、不限制任何權限」的原則 —— 可行，但「套用」的意義
//     只是把原則名稱記成文件的中繼資料，不改動任何位元組。
//   - 套用「要求密碼」或「限制任一權限」的原則 —— **不可行**。權限旗標只有
//     在文件被加密的情況下才具有強制力（未加密文件的權限旗標形同虛設，
//     任何檢視器都可以忽略它），而目前定案的技術堆疊（預編譯 PDFium）
//     沒有輸出加密文件的公開 API——見 engine/save/security_saver.h 的
//     SetPasswordStatus::NotSupported，以及
//     exceptions/EXC_20260906_RD_SA_set_password_unsupported.md。
//     這裡不生出一個看起來能設定、實際上毫無強制力的假實作：那比完全不做
//     更危險，使用者會誤以為文件真的被保護了。

#include <string>
#include <vector>

#include "engine/save/security_saver.h"

namespace alioth::app {

struct SecurityPolicy {
    std::string name;         // 使用者自訂，同一個 store 內必須唯一
    std::string description;
    bool requiresPassword{false};
    engine::save::PermissionFlags permissions{};
};

// 純記憶體的原則清單管理。序列化交給呼叫端（通常是 app/settings.h 那類
// QSettings 包裝）——CRUD 邏輯與儲存格式是兩件事，混在一起會讓這裡的測試
// 被迫牽扯 QSettings。
class SecurityPolicyStore {
public:
    // 名稱重複或為空時回傳 false，不覆蓋既有原則——「新增」跟「覆蓋」是
    // 使用者兩個不同的意圖，混在一起容易誤刪別人辛苦調好的設定。
    bool add(SecurityPolicy policy);
    bool remove(const std::string& name);

    [[nodiscard]] const SecurityPolicy* find(const std::string& name) const noexcept;
    [[nodiscard]] const std::vector<SecurityPolicy>& all() const noexcept { return policies_; }
    [[nodiscard]] std::size_t size() const noexcept { return policies_.size(); }

private:
    std::vector<SecurityPolicy> policies_;
};

struct ApplyPolicyResult {
    bool ok{false};
    // true 代表「目前技術堆疊做不到」而不是操作本身有錯——呼叫端應該把
    // 這個狀態顯示成「不支援」而不是「失敗」，兩者對使用者的意義不同。
    bool blocked{false};
    std::string message;
};

// 套用原則。理由與判準見檔頭說明；只有完全開放（不要求密碼、且
// PermissionFlags 全部為 true）的原則會回傳 ok == true。
[[nodiscard]] ApplyPolicyResult applySecurityPolicy(const SecurityPolicy& policy);

}  // namespace alioth::app
