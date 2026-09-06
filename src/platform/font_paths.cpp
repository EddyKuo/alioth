#include "platform/font_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace alioth::platform {
namespace {

// 隨附字型的檔名。放在執行檔旁邊的 fonts/ 目錄，與 pdfium.dll 的作法一致。
constexpr const char* kBundledFileName = "NotoSansTC-Regular.ttf";

[[nodiscard]] QString bundledPath() {
    // 沒有 QCoreApplication 實體時 applicationDirPath() 會回傳空字串並印警告。
    // 那不是假設性的情況：QTEST_APPLESS_MAIN 的測試就是這樣跑的，而字型查找
    // 本身不需要事件迴圈。沒有實體就跳過隨附路徑，直接找系統字型。
    if (QCoreApplication::instance() == nullptr) return {};
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("fonts/") + QLatin1String(kBundledFileName));
}

}  // namespace

std::vector<QString> cjkFontCandidates() {
    std::vector<QString> candidates;
    // 一、隨附的那一份。優先權最高，理由見標頭。
    if (const QString bundled = bundledPath(); !bundled.isEmpty()) {
        candidates.push_back(bundled);
    }

#ifdef _WIN32
    // 二、系統字型。Windows 10 之後內建 Noto Sans TC；微軟正黑體是更舊的退路。
    //
    // 可變字型（-VF）放在靜態版之後：子集化會取它的預設實例，結果正確但字重
    // 未必是使用者預期的 Regular。有靜態版就先用靜態版。
    const QString fonts = QDir::fromNativeSeparators(qEnvironmentVariable("WINDIR")) +
                          QStringLiteral("/Fonts/");
    candidates.push_back(fonts + QStringLiteral("NotoSansTC-Regular.ttf"));
    candidates.push_back(fonts + QStringLiteral("NotoSansTC-VF.ttf"));
    candidates.push_back(fonts + QStringLiteral("NotoSansHK-VF.ttf"));
    // msjh.ttc 是 TrueType Collection，子集器目前不支援（會明確拒絕）。
    // 仍然列出來是為了讓診斷訊息說得出「我找過它但格式不支援」——
    // 那比「找不到任何字型」對使用者有用得多。
    candidates.push_back(fonts + QStringLiteral("msjh.ttc"));
#elif defined(__APPLE__)
    candidates.push_back(QStringLiteral("/System/Library/Fonts/PingFang.ttc"));
    candidates.push_back(QStringLiteral("/Library/Fonts/NotoSansTC-Regular.ttf"));
#else
    candidates.push_back(
        QStringLiteral("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"));
    candidates.push_back(QStringLiteral("/usr/share/fonts/truetype/noto/NotoSansTC-Regular.ttf"));
#endif
    return candidates;
}

CjkFontSource findCjkFont() {
    const std::vector<QString> candidates = cjkFontCandidates();
    const QString bundled = bundledPath();

    for (const QString& candidate : candidates) {
        if (!QFileInfo::exists(candidate)) continue;
        CjkFontSource source;
        source.path = candidate;
        source.bundled = candidate == bundled;
        // /BaseFont 用檔名主幹。它只是識別字串，不影響字形——真正決定長相的
        // 是內嵌的 /FontFile2。
        source.baseName = QFileInfo(candidate).completeBaseName();
        return source;
    }
    return CjkFontSource{};
}

}  // namespace alioth::platform
