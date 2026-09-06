#include "platform/cloud_folders.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

#ifdef _WIN32
#include <QSettings>
#endif

namespace alioth::platform {
namespace {

// 路徑比對用的正規化。這裡不解析符號連結也不碰檔案系統：雲端檔案在
// 「僅線上」狀態時，任何檔案系統查詢都可能觸發下載，而使用者只是把
// 滑鼠移到檔名上而已。
QString normalizeForPrefix(const QString& path) {
    QString value = QDir::cleanPath(path);
#ifdef _WIN32
    value = value.toLower();
#endif
    while (value.endsWith(QLatin1Char('/'))) value.chop(1);
    return value;
}

bool isUnder(const QString& path, const QString& root) {
    if (root.isEmpty()) return false;
    const QString normalizedPath = normalizeForPrefix(path);
    const QString normalizedRoot = normalizeForPrefix(root);
    if (normalizedPath == normalizedRoot) return true;
    // 前綴後面一定要接分隔符號，否則 "C:/OneDriveBackup" 會被判成
    // "C:/OneDrive" 底下的檔案。
    return normalizedPath.startsWith(normalizedRoot + QLatin1Char('/'));
}

void appendIfExists(std::vector<CloudFolder>& out, CloudProvider provider, const QString& path,
                    const QString& accountHint = {}) {
    if (path.isEmpty()) return;
    const QFileInfo info(path);
    if (!info.isDir()) return;
    for (const CloudFolder& existing : out) {
        if (normalizeForPrefix(existing.rootPath) == normalizeForPrefix(path)) return;
    }
    out.push_back(CloudFolder{provider, QDir::cleanPath(path), accountHint});
}

}  // namespace

QString providerDisplayName(CloudProvider provider) {
    switch (provider) {
        case CloudProvider::OneDrive:         return QObject::tr("OneDrive");
        case CloudProvider::OneDriveBusiness: return QObject::tr("OneDrive（公司帳號）");
        case CloudProvider::GoogleDrive:      return QObject::tr("Google 雲端硬碟");
        case CloudProvider::Dropbox:          return QObject::tr("Dropbox");
        case CloudProvider::Box:              return QObject::tr("Box");
        case CloudProvider::SharePoint:       return QObject::tr("SharePoint");
        case CloudProvider::None:             break;
    }
    return {};
}

std::vector<CloudFolder> detectCloudFolders() {
    std::vector<CloudFolder> folders;
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();

    // 各家用戶端登入後都會設環境變數。用它們而不是掃描磁碟：掃描既慢，
    // 又會走進使用者不希望我們碰的目錄。
    appendIfExists(folders, CloudProvider::OneDrive,
                   environment.value(QStringLiteral("OneDriveConsumer")));
    appendIfExists(folders, CloudProvider::OneDriveBusiness,
                   environment.value(QStringLiteral("OneDriveCommercial")));
    // OneDrive 只有一個帳號時只設 OneDrive；此時分不出個人或公司，
    // 保守歸為個人版並在說明文字上不宣稱帳號類型。
    appendIfExists(folders, CloudProvider::OneDrive,
                   environment.value(QStringLiteral("OneDrive")));

#ifdef _WIN32
    // Google 雲端硬碟與 Dropbox 把同步根目錄寫在使用者登錄裡。
    // 讀不到就當作沒安裝——這條路徑刻意不做任何猜測，猜錯會讓我們把
    // 一般的本機資料夾標示成「會同步到雲端」，那個誤導比沒標示嚴重。
    {
        QSettings dropbox(QStringLiteral("HKEY_CURRENT_USER\\Software\\Dropbox"),
                          QSettings::NativeFormat);
        appendIfExists(folders, CloudProvider::Dropbox,
                       dropbox.value(QStringLiteral("InstallPath")).toString());
    }
    {
        QSettings drive(QStringLiteral("HKEY_CURRENT_USER\\Software\\Google\\DriveFS"),
                        QSettings::NativeFormat);
        appendIfExists(folders, CloudProvider::GoogleDrive,
                       drive.value(QStringLiteral("DefaultMountPoint")).toString());
    }
    {
        QSettings box(QStringLiteral("HKEY_CURRENT_USER\\Software\\Box\\Box"),
                      QSettings::NativeFormat);
        appendIfExists(folders, CloudProvider::Box,
                       box.value(QStringLiteral("SyncRootFolder")).toString());
    }
#endif

    return folders;
}

CloudProvider providerForPath(const QString& path, const std::vector<CloudFolder>& folders) {
    if (path.isEmpty()) return CloudProvider::None;

    // 最長前綴優先：SharePoint 的文件庫常常同步在 OneDrive 根目錄底下，
    // 先比對到誰就回誰的話，結果取決於偵測順序而不是實際位置。
    CloudProvider best = CloudProvider::None;
    int bestLength = -1;
    for (const CloudFolder& folder : folders) {
        if (!isUnder(path, folder.rootPath)) continue;
        const int length = normalizeForPrefix(folder.rootPath).size();
        if (length > bestLength) {
            bestLength = length;
            best = folder.provider;
        }
    }
    return best;
}

QString cloudNoticeForPath(const QString& path, const std::vector<CloudFolder>& folders) {
    const CloudProvider provider = providerForPath(path, folders);
    if (provider == CloudProvider::None) return {};
    // 使用者需要知道「存下去會同步出去」——那會改變他要不要在裡面寫敏感註解。
    return QObject::tr("這份文件位於 %1，存檔後會自動同步")
        .arg(providerDisplayName(provider));
}

}  // namespace alioth::platform
