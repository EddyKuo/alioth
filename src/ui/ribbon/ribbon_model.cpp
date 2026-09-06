#include "ui/ribbon/ribbon_model.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

namespace alioth::ui::ribbon {
namespace {

constexpr QLatin1StringView kKeyVersion{"version"};
constexpr QLatin1StringView kKeyPages{"pages"};
constexpr QLatin1StringView kKeyGroups{"groups"};
constexpr QLatin1StringView kKeyItems{"items"};
constexpr QLatin1StringView kKeyQuickAccess{"quickAccess"};
constexpr QLatin1StringView kKeyCollapsed{"collapsed"};
constexpr QLatin1StringView kKeyId{"id"};
constexpr QLatin1StringView kKeyTitle{"title"};
constexpr QLatin1StringView kKeyKeyTip{"keyTip"};
constexpr QLatin1StringView kKeyType{"type"};
constexpr QLatin1StringView kKeyActionId{"actionId"};
constexpr QLatin1StringView kKeyLabel{"label"};
constexpr QLatin1StringView kKeyIcon{"icon"};
constexpr QLatin1StringView kKeySize{"size"};

constexpr QLatin1StringView kSeparator{"separator"};
constexpr QLatin1StringView kAction{"action"};
constexpr QLatin1StringView kLarge{"large"};
constexpr QLatin1StringView kSmall{"small"};

void warn(ParseResult* result, const QString& message) {
    if (result != nullptr) result->warnings << message;
}

// 只在非空時寫入。缺席即預設值，這樣手寫的設定檔不必為了合法而填一堆空字串，
// 序列化往返也仍然等值。
void putIfNotEmpty(QJsonObject& object, QLatin1StringView key, const QString& value) {
    if (!value.isEmpty()) object.insert(key, value);
}

[[nodiscard]] QString stringField(const QJsonObject& object, QLatin1StringView key) {
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString{};
}

[[nodiscard]] QJsonObject itemToJson(const Item& item) {
    QJsonObject object;
    if (item.type == ItemType::Separator) {
        object.insert(kKeyType, kSeparator);
        return object;
    }
    object.insert(kKeyActionId, item.actionId);
    putIfNotEmpty(object, kKeyLabel, item.label);
    putIfNotEmpty(object, kKeyIcon, item.iconName);
    object.insert(kKeySize, item.size == ItemSize::Large ? kLarge : kSmall);
    putIfNotEmpty(object, kKeyKeyTip, item.keyTip);
    return object;
}

[[nodiscard]] bool itemFromJson(const QJsonValue& value, Item& out, ParseResult* result,
                                const QString& context) {
    if (!value.isObject()) {
        warn(result, QStringLiteral("%1：項目不是物件，已略過").arg(context));
        return false;
    }
    const QJsonObject object = value.toObject();
    if (stringField(object, kKeyType) == kSeparator) {
        out = Item{};
        out.type = ItemType::Separator;
        return true;
    }

    out = Item{};
    out.actionId = stringField(object, kKeyActionId);
    if (out.actionId.isEmpty()) {
        // 沒有 actionId 的項目點下去什麼都不會發生，留著只會讓使用者以為功能壞了。
        warn(result, QStringLiteral("%1：項目缺少 actionId，已略過").arg(context));
        return false;
    }
    out.label = stringField(object, kKeyLabel);
    out.iconName = stringField(object, kKeyIcon);
    out.keyTip = stringField(object, kKeyKeyTip);
    const QString size = stringField(object, kKeySize);
    if (size == kSmall) {
        out.size = ItemSize::Small;
    } else {
        if (!size.isEmpty() && size != kLarge) {
            warn(result, QStringLiteral("%1：未知的尺寸 \"%2\"，改用 large").arg(context, size));
        }
        out.size = ItemSize::Large;
    }
    return true;
}

[[nodiscard]] QJsonArray itemsToJson(const QList<Item>& items) {
    QJsonArray array;
    for (const Item& item : items) array.append(itemToJson(item));
    return array;
}

[[nodiscard]] QJsonObject groupToJson(const Group& group) {
    QJsonObject object;
    putIfNotEmpty(object, kKeyId, group.id);
    putIfNotEmpty(object, kKeyTitle, group.title);
    putIfNotEmpty(object, kKeyKeyTip, group.keyTip);
    object.insert(kKeyItems, itemsToJson(group.items));
    return object;
}

[[nodiscard]] bool groupFromJson(const QJsonValue& value, Group& out, ParseResult* result,
                                 const QString& context) {
    if (!value.isObject()) {
        warn(result, QStringLiteral("%1：群組不是物件，已略過").arg(context));
        return false;
    }
    const QJsonObject object = value.toObject();
    out = Group{};
    out.id = stringField(object, kKeyId);
    out.title = stringField(object, kKeyTitle);
    out.keyTip = stringField(object, kKeyKeyTip);

    const QJsonValue items = object.value(kKeyItems);
    if (!items.isArray()) {
        // 空群組是合法的（使用者可能正在組一個新群組），只是要留下痕跡。
        if (!items.isUndefined()) {
            warn(result, QStringLiteral("%1：items 不是陣列，視為空群組").arg(context));
        }
        return true;
    }
    const QJsonArray array = items.toArray();
    for (int i = 0; i < array.size(); ++i) {
        Item item;
        if (itemFromJson(array.at(i), item, result,
                         QStringLiteral("%1 項目 #%2").arg(context).arg(i))) {
            out.items.append(item);
        }
    }
    return true;
}

[[nodiscard]] QJsonObject pageToJson(const Page& page) {
    QJsonObject object;
    object.insert(kKeyId, page.id);
    putIfNotEmpty(object, kKeyTitle, page.title);
    putIfNotEmpty(object, kKeyKeyTip, page.keyTip);
    QJsonArray groups;
    for (const Group& group : page.groups) groups.append(groupToJson(group));
    object.insert(kKeyGroups, groups);
    return object;
}

[[nodiscard]] bool pageFromJson(const QJsonValue& value, Page& out, ParseResult* result,
                                const QString& context) {
    if (!value.isObject()) {
        warn(result, QStringLiteral("%1：分頁不是物件，已略過").arg(context));
        return false;
    }
    const QJsonObject object = value.toObject();
    out = Page{};
    out.id = stringField(object, kKeyId);
    if (out.id.isEmpty()) {
        // 分頁 id 是後續「切到某分頁」與設定檔合併的唯一依據，沒有就無法定位。
        warn(result, QStringLiteral("%1：分頁缺少 id，已略過").arg(context));
        return false;
    }
    out.title = stringField(object, kKeyTitle);
    out.keyTip = stringField(object, kKeyKeyTip);

    const QJsonValue groups = object.value(kKeyGroups);
    if (!groups.isArray()) {
        if (!groups.isUndefined()) {
            warn(result, QStringLiteral("%1：groups 不是陣列，視為空分頁").arg(context));
        }
        return true;
    }
    const QJsonArray array = groups.toArray();
    for (int i = 0; i < array.size(); ++i) {
        Group group;
        if (groupFromJson(array.at(i), group, result,
                          QStringLiteral("%1 群組 #%2").arg(context).arg(i))) {
            out.groups.append(group);
        }
    }
    return true;
}

}  // namespace

QJsonObject toJson(const Layout& layout) {
    QJsonObject root;
    root.insert(kKeyVersion, layout.version);
    root.insert(kKeyCollapsed, layout.collapsed);

    QJsonArray pages;
    for (const Page& page : layout.pages) pages.append(pageToJson(page));
    root.insert(kKeyPages, pages);

    QJsonArray quickAccess;
    for (const QString& id : layout.quickAccessActionIds) quickAccess.append(id);
    root.insert(kKeyQuickAccess, quickAccess);
    return root;
}

Layout fromJson(const QJsonObject& root, ParseResult* result) {
    if (result != nullptr) *result = ParseResult{};

    Layout layout;
    const QJsonValue version = root.value(kKeyVersion);
    if (version.isDouble()) {
        layout.version = version.toInt(1);
    } else if (!version.isUndefined()) {
        warn(result, QStringLiteral("version 不是數字，改用 1"));
    }
    layout.collapsed = root.value(kKeyCollapsed).toBool(false);

    const QJsonValue pages = root.value(kKeyPages);
    if (pages.isArray()) {
        QSet<QString> seen;
        const QJsonArray array = pages.toArray();
        for (int i = 0; i < array.size(); ++i) {
            Page page;
            if (!pageFromJson(array.at(i), page, result, QStringLiteral("分頁 #%1").arg(i))) {
                continue;
            }
            // 重複 id 會讓 setCurrentPage(id) 產生二義性，寧可丟掉後來的那個。
            if (seen.contains(page.id)) {
                warn(result, QStringLiteral("分頁 id \"%1\" 重複，已略過").arg(page.id));
                continue;
            }
            seen.insert(page.id);
            layout.pages.append(page);
        }
    } else if (!pages.isUndefined()) {
        warn(result, QStringLiteral("pages 不是陣列，視為空配置"));
    }

    const QJsonValue quickAccess = root.value(kKeyQuickAccess);
    if (quickAccess.isArray()) {
        const QJsonArray array = quickAccess.toArray();
        for (int i = 0; i < array.size(); ++i) {
            const QJsonValue entry = array.at(i);
            if (!entry.isString() || entry.toString().isEmpty()) {
                warn(result, QStringLiteral("快速存取項目 #%1 不是有效的 actionId，已略過").arg(i));
                continue;
            }
            layout.quickAccessActionIds << entry.toString();
        }
    } else if (!quickAccess.isUndefined()) {
        warn(result, QStringLiteral("quickAccess 不是陣列，視為空清單"));
    }
    return layout;
}

bool saveToFile(const Layout& layout, const QString& path, ParseResult* result) {
    if (result != nullptr) *result = ParseResult{};
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (result != nullptr) {
            result->error = QStringLiteral("無法寫入 %1：%2").arg(path, file.errorString());
        }
        return false;
    }
    const QJsonDocument document(toJson(layout));
    const QByteArray bytes = document.toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        if (result != nullptr) {
            result->error = QStringLiteral("寫入 %1 不完整：%2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

Layout loadFromFile(const QString& path, ParseResult* result) {
    if (result != nullptr) *result = ParseResult{};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (result != nullptr) {
            result->error = QStringLiteral("無法讀取 %1：%2").arg(path, file.errorString());
        }
        return Layout{};
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (result != nullptr) {
            result->error = parseError.error != QJsonParseError::NoError
                                ? QStringLiteral("%1 不是合法 JSON：%2")
                                      .arg(path, parseError.errorString())
                                : QStringLiteral("%1 的根節點不是物件").arg(path);
        }
        return Layout{};
    }
    return fromJson(document.object(), result);
}

void assignAutomaticKeyTips(Layout& layout) {
    // KeyTip 只認 ASCII 英數字：Alt 序列要能在任何鍵盤佈局上按得出來，
    // 而 CJK 標籤的第一個字元按不出來，那種情況一律退回序號。
    const auto pick = [](const QString& label, QSet<QChar>& used) {
        for (const QChar ch : label) {
            const QChar upper = ch.toUpper();
            if ((upper >= u'A' && upper <= u'Z') || (upper >= u'0' && upper <= u'9')) {
                if (!used.contains(upper)) {
                    used.insert(upper);
                    return QString(upper);
                }
            }
        }
        for (char fallback = '1'; fallback <= '9'; ++fallback) {
            const QChar candidate(QLatin1Char{fallback});
            if (!used.contains(candidate)) {
                used.insert(candidate);
                return QString(candidate);
            }
        }
        return QString{};
    };

    QSet<QChar> pageKeys;
    for (Page& page : layout.pages) {
        if (page.keyTip.isEmpty()) {
            page.keyTip = pick(page.title.isEmpty() ? page.id : page.title, pageKeys);
        } else {
            pageKeys.insert(page.keyTip.at(0).toUpper());
        }

        QSet<QChar> itemKeys;
        for (Group& group : page.groups) {
            for (Item& item : group.items) {
                if (item.type != ItemType::Action) continue;
                if (item.keyTip.isEmpty()) {
                    item.keyTip = pick(item.label.isEmpty() ? item.actionId : item.label, itemKeys);
                } else {
                    itemKeys.insert(item.keyTip.at(0).toUpper());
                }
            }
        }
    }
}

QStringList collectActionIds(const Layout& layout) {
    QStringList ids = layout.quickAccessActionIds;
    for (const Page& page : layout.pages) {
        for (const Group& group : page.groups) {
            for (const Item& item : group.items) {
                if (item.type == ItemType::Action && !ids.contains(item.actionId)) {
                    ids << item.actionId;
                }
            }
        }
    }
    ids.removeDuplicates();
    return ids;
}

}  // namespace alioth::ui::ribbon
