#include "app/security_policy.h"

#include <algorithm>

namespace alioth::app {

namespace {

[[nodiscard]] bool isFullyOpen(const engine::save::PermissionFlags& flags) noexcept {
    return flags.print && flags.modify && flags.copy && flags.annotate && flags.fillForms &&
           flags.extractForAccessibility && flags.assemble && flags.printHighRes;
}

}  // namespace

bool SecurityPolicyStore::add(SecurityPolicy policy) {
    if (policy.name.empty()) return false;
    if (find(policy.name) != nullptr) return false;
    policies_.push_back(std::move(policy));
    return true;
}

bool SecurityPolicyStore::remove(const std::string& name) {
    const auto it = std::find_if(policies_.begin(), policies_.end(),
                                 [&](const SecurityPolicy& p) { return p.name == name; });
    if (it == policies_.end()) return false;
    policies_.erase(it);
    return true;
}

const SecurityPolicy* SecurityPolicyStore::find(const std::string& name) const noexcept {
    const auto it = std::find_if(policies_.begin(), policies_.end(),
                                 [&](const SecurityPolicy& p) { return p.name == name; });
    return it == policies_.end() ? nullptr : &(*it);
}

ApplyPolicyResult applySecurityPolicy(const SecurityPolicy& policy) {
    if (policy.requiresPassword || !isFullyOpen(policy.permissions)) {
        return ApplyPolicyResult{
            false, true,
            "目前技術堆疊（預編譯 PDFium）無法輸出加密文件，密碼保護與權限限制"
            "都需要文件本身被加密才有強制力，因此「" +
                policy.name +
                "」無法真正套用。詳見 "
                "exceptions/EXC_20260906_RD_SA_set_password_unsupported.md"};
    }
    // 完全開放的原則不需要加密就能「套用」：這裡刻意不做任何位元組層的動作，
    // 只回報成功，讓呼叫端（UI／文件中繼資料）自行記錄「已套用哪個原則」。
    return ApplyPolicyResult{true, false, {}};
}

}  // namespace alioth::app
