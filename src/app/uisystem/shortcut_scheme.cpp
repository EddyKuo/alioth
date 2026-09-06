#include "app/uisystem/shortcut_scheme.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace alioth::app {

QString toString(ShortcutContext context) {
    switch (context) {
        case ShortcutContext::Global: return QStringLiteral("global");
        case ShortcutContext::Viewer: return QStringLiteral("viewer");
        case ShortcutContext::Annotation: return QStringLiteral("annotation");
        case ShortcutContext::FormFill: return QStringLiteral("form_fill");
        case ShortcutContext::PageOrganize: return QStringLiteral("page_organize");
    }
    return QStringLiteral("global");
}

std::optional<ShortcutContext> shortcutContextFromString(const QString& text) {
    if (text == QStringLiteral("global")) return ShortcutContext::Global;
    if (text == QStringLiteral("viewer")) return ShortcutContext::Viewer;
    if (text == QStringLiteral("annotation")) return ShortcutContext::Annotation;
    if (text == QStringLiteral("form_fill")) return ShortcutContext::FormFill;
    if (text == QStringLiteral("page_organize")) return ShortcutContext::PageOrganize;
    return std::nullopt;
}

namespace {

// 兩個綁定「看得見彼此」的規則：Global 對任何情境都可見；非 Global 只與相同情境
// 互相可見。這是 ShortcutContext 文件註解裡的規則的唯一實作點。
bool contextsVisible(ShortcutContext a, ShortcutContext b) {
    if (a == ShortcutContext::Global || b == ShortcutContext::Global) return true;
    return a == b;
}

}  // namespace

ShortcutScheme::ShortcutScheme() : bindings_(defaultBindings()) {}

std::vector<ShortcutBinding> ShortcutScheme::defaultBindings() {
    using C = ShortcutContext;
    return {
        // 檔案
        {QStringLiteral("file.open"), QKeySequence(QStringLiteral("Ctrl+O")), C::Global, QStringLiteral("開啟檔案")},
        {QStringLiteral("file.save"), QKeySequence(QStringLiteral("Ctrl+S")), C::Global, QStringLiteral("儲存")},
        {QStringLiteral("file.save_as"), QKeySequence(QStringLiteral("Ctrl+Shift+S")), C::Global, QStringLiteral("另存新檔")},
        {QStringLiteral("file.print"), QKeySequence(QStringLiteral("Ctrl+P")), C::Global, QStringLiteral("列印")},
        {QStringLiteral("file.close_tab"), QKeySequence(QStringLiteral("Ctrl+W")), C::Global, QStringLiteral("關閉頁籤")},
        {QStringLiteral("file.properties"), QKeySequence(QStringLiteral("Ctrl+D")), C::Global, QStringLiteral("文件內容")},
        // 編輯 / 復原
        {QStringLiteral("edit.undo"), QKeySequence(QStringLiteral("Ctrl+Z")), C::Global, QStringLiteral("復原")},
        {QStringLiteral("edit.redo"), QKeySequence(QStringLiteral("Ctrl+Shift+Z")), C::Global, QStringLiteral("重做")},
        {QStringLiteral("edit.copy"), QKeySequence(QStringLiteral("Ctrl+C")), C::Global, QStringLiteral("複製")},
        {QStringLiteral("edit.find"), QKeySequence(QStringLiteral("Ctrl+F")), C::Global, QStringLiteral("尋找")},
        {QStringLiteral("edit.find_next"), QKeySequence(QStringLiteral("F3")), C::Global, QStringLiteral("找下一個")},
        // 檢視 / 縮放（PDF-XChange、Acrobat 皆用 Ctrl+加減與 Ctrl+0/1）
        {QStringLiteral("view.zoom_in"), QKeySequence(QStringLiteral("Ctrl++")), C::Viewer, QStringLiteral("放大")},
        {QStringLiteral("view.zoom_out"), QKeySequence(QStringLiteral("Ctrl+-")), C::Viewer, QStringLiteral("縮小")},
        {QStringLiteral("view.zoom_actual"), QKeySequence(QStringLiteral("Ctrl+1")), C::Viewer, QStringLiteral("實際大小")},
        {QStringLiteral("view.fit_width"), QKeySequence(QStringLiteral("Ctrl+2")), C::Viewer, QStringLiteral("符合寬度")},
        {QStringLiteral("view.fit_page"), QKeySequence(QStringLiteral("Ctrl+3")), C::Viewer, QStringLiteral("符合頁面")},
        {QStringLiteral("view.rotate_cw"), QKeySequence(QStringLiteral("Ctrl+Shift+Plus")), C::Viewer, QStringLiteral("順時針旋轉")},
        {QStringLiteral("view.full_screen"), QKeySequence(QStringLiteral("Ctrl+L")), C::Viewer, QStringLiteral("全螢幕")},
        // 導覽
        {QStringLiteral("nav.first_page"), QKeySequence(QStringLiteral("Ctrl+Home")), C::Viewer, QStringLiteral("第一頁")},
        {QStringLiteral("nav.last_page"), QKeySequence(QStringLiteral("Ctrl+End")), C::Viewer, QStringLiteral("最後一頁")},
        {QStringLiteral("nav.next_page"), QKeySequence(QStringLiteral("PgDown")), C::Viewer, QStringLiteral("下一頁")},
        {QStringLiteral("nav.prev_page"), QKeySequence(QStringLiteral("PgUp")), C::Viewer, QStringLiteral("上一頁")},
        {QStringLiteral("nav.goto_page"), QKeySequence(QStringLiteral("Ctrl+G")), C::Viewer, QStringLiteral("跳至頁面")},
        {QStringLiteral("nav.back"), QKeySequence(QStringLiteral("Alt+Left")), C::Viewer, QStringLiteral("上一個檢視位置")},
        {QStringLiteral("nav.forward"), QKeySequence(QStringLiteral("Alt+Right")), C::Viewer, QStringLiteral("下一個檢視位置")},
        // 工具單鍵切換（Viewer 情境；表單填寫情境不含這些鍵，避免打字時誤觸——
        // 這是 ShortcutContext 分開 Viewer / FormFill 的主要理由）
        {QStringLiteral("tool.select"), QKeySequence(Qt::Key_Escape), C::Viewer, QStringLiteral("選取工具")},
        {QStringLiteral("tool.hand"), QKeySequence(Qt::Key_H), C::Viewer, QStringLiteral("手形工具")},
        {QStringLiteral("tool.highlight"), QKeySequence(Qt::Key_U), C::Annotation, QStringLiteral("螢光筆")},
        {QStringLiteral("tool.sticky_note"), QKeySequence(Qt::Key_C), C::Annotation, QStringLiteral("附註")},
        {QStringLiteral("tool.stamp"), QKeySequence(Qt::Key_K), C::Annotation, QStringLiteral("印章")},
        {QStringLiteral("tool.draw"), QKeySequence(Qt::Key_N), C::Annotation, QStringLiteral("繪圖")},
        // 頁面管理
        {QStringLiteral("page.insert"), QKeySequence(QStringLiteral("Ctrl+Shift+I")), C::PageOrganize, QStringLiteral("插入頁面")},
        {QStringLiteral("page.delete"), QKeySequence(Qt::Key_Delete), C::PageOrganize, QStringLiteral("刪除頁面")},
        {QStringLiteral("page.rotate_cw"), QKeySequence(QStringLiteral("Ctrl+Shift+Plus")), C::PageOrganize, QStringLiteral("頁面順時針旋轉")},
    };
}

void ShortcutScheme::resetToDefaults() { bindings_ = defaultBindings(); }

void ShortcutScheme::resetAction(const QString& actionId) {
    const auto defaults = defaultBindings();
    auto it = std::find_if(defaults.begin(), defaults.end(),
                            [&](const ShortcutBinding& b) { return b.actionId == actionId; });
    auto target = std::find_if(bindings_.begin(), bindings_.end(),
                                [&](const ShortcutBinding& b) { return b.actionId == actionId; });
    if (target == bindings_.end()) return;
    if (it != defaults.end()) {
        target->sequence = it->sequence;
        target->context = it->context;
    } else {
        // 動作不在預設表裡（例如外掛/自訂動作），還原就是清空。
        target->sequence = QKeySequence();
    }
}

BindResult ShortcutScheme::setBinding(const QString& actionId, const QKeySequence& sequence) {
    auto target = std::find_if(bindings_.begin(), bindings_.end(),
                                [&](const ShortcutBinding& b) { return b.actionId == actionId; });
    if (target == bindings_.end()) return BindResult::UnknownAction;

    if (!sequence.isEmpty()) {
        for (const auto& other : bindings_) {
            if (other.actionId == actionId) continue;
            if (other.isEmpty()) continue;
            if (other.sequence == sequence && contextsVisible(other.context, target->context)) {
                return BindResult::Conflict;
            }
        }
    }
    target->sequence = sequence;
    return BindResult::Ok;
}

void ShortcutScheme::clearBinding(const QString& actionId) {
    auto target = std::find_if(bindings_.begin(), bindings_.end(),
                                [&](const ShortcutBinding& b) { return b.actionId == actionId; });
    if (target != bindings_.end()) target->sequence = QKeySequence();
}

std::optional<ShortcutBinding> ShortcutScheme::binding(const QString& actionId) const {
    auto it = std::find_if(bindings_.begin(), bindings_.end(),
                            [&](const ShortcutBinding& b) { return b.actionId == actionId; });
    if (it == bindings_.end()) return std::nullopt;
    return *it;
}

std::vector<ShortcutConflict> ShortcutScheme::findConflicts() const {
    std::vector<ShortcutConflict> conflicts;
    // 以 (context bucket) 分組比對；Global 的可見性較廣，因此對每個非 Global 綁定
    // 也要跟 Global 的鍵位比。做法：對每個唯一鍵位序列，蒐集所有「彼此可見」的
    // 占用者，切成互相看得見的群組。
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const auto& a = bindings_[i];
        if (a.isEmpty()) continue;
        std::vector<QString> group{a.actionId};
        for (std::size_t j = i + 1; j < bindings_.size(); ++j) {
            const auto& b = bindings_[j];
            if (b.isEmpty()) continue;
            if (b.sequence == a.sequence && contextsVisible(a.context, b.context)) {
                group.push_back(b.actionId);
            }
        }
        if (group.size() > 1) {
            // 避免同一組合被記錄兩次：只在 i 是這一組裡最小索引時才輸出。
            bool alreadyReported = false;
            for (std::size_t k = 0; k < i; ++k) {
                if (!bindings_[k].isEmpty() && bindings_[k].sequence == a.sequence &&
                    contextsVisible(a.context, bindings_[k].context)) {
                    alreadyReported = true;
                    break;
                }
            }
            if (!alreadyReported) {
                ShortcutConflict conflict;
                conflict.sequence = a.sequence;
                conflict.context = a.context;
                conflict.actionIds = group;
                conflicts.push_back(conflict);
            }
        }
    }
    return conflicts;
}

QByteArray ShortcutScheme::exportToJson() const {
    QJsonArray array;
    for (const auto& b : bindings_) {
        QJsonObject obj;
        obj[QStringLiteral("actionId")] = b.actionId;
        obj[QStringLiteral("sequence")] = b.sequence.toString(QKeySequence::PortableText);
        obj[QStringLiteral("context")] = toString(b.context);
        obj[QStringLiteral("description")] = b.description;
        array.append(obj);
    }
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("bindings")] = array;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

ShortcutScheme::ImportResult ShortcutScheme::importFromJson(const QByteArray& json) {
    ImportResult result;
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("JSON 格式錯誤：%1").arg(parseError.errorString());
        return result;
    }
    const QJsonObject root = doc.object();
    const QJsonArray array = root.value(QStringLiteral("bindings")).toArray();

    std::vector<ShortcutBinding> imported;
    for (const auto& value : array) {
        if (!value.isObject()) continue;
        const QJsonObject obj = value.toObject();
        ShortcutBinding binding;
        binding.actionId = obj.value(QStringLiteral("actionId")).toString();
        binding.sequence = QKeySequence(obj.value(QStringLiteral("sequence")).toString(),
                                         QKeySequence::PortableText);
        binding.context = shortcutContextFromString(obj.value(QStringLiteral("context")).toString())
                               .value_or(ShortcutContext::Global);
        binding.description = obj.value(QStringLiteral("description")).toString();
        if (binding.actionId.isEmpty()) {
            result.error = QStringLiteral("匯入資料含空白的 actionId");
            return result;
        }
        imported.push_back(binding);
    }

    // 用一份暫時的表算衝突，成功才整批換掉現有表——「拒絕整批」而不是「盡量套用」。
    ShortcutScheme probe;
    probe.bindings_ = imported;
    const auto conflicts = probe.findConflicts();
    if (!conflicts.empty()) {
        result.conflicts = conflicts;
        return result;
    }

    bindings_ = imported;
    result.ok = true;
    return result;
}

}  // namespace alioth::app
