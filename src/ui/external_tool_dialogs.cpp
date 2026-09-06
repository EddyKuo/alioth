#include "ui/external_tool_dialogs.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/settings.h"

namespace alioth::ui {

ExternalToolEditDialog::ExternalToolEditDialog(const app::ExternalTool& initial, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("外部程式設定"));

    name_ = new QLineEdit(initial.name, this);
    executablePath_ = new QLineEdit(initial.executablePath, this);
    argumentsTemplate_ = new QLineEdit(initial.argumentsTemplate, this);
    workingDirectory_ = new QLineEdit(initial.workingDirectory, this);

    auto* browseButton = new QToolButton(this);
    browseButton->setText(tr("瀏覽…"));
    connect(browseButton, &QToolButton::clicked, this, &ExternalToolEditDialog::browseForExecutable);

    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(executablePath_);
    pathRow->addWidget(browseButton);

    auto* form = new QFormLayout;
    form->addRow(tr("名稱"), name_);
    form->addRow(tr("執行檔"), pathRow);
    form->addRow(tr("參數樣板"), argumentsTemplate_);
    form->addRow(tr("工作目錄（留空使用執行檔所在目錄）"), workingDirectory_);

    auto* hint = new QLabel(
        tr("參數樣板只允許 {file} 這一個佔位字元，會被換成目前開啟文件的路徑；"
           "含空白的參數請用雙引號包起來。"),
        this);
    hint->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(buttons);
}

void ExternalToolEditDialog::browseForExecutable() {
    // 檔案篩選字串刻意不用 "*.exe"：可執行檔的副檔名是作業系統的事
    // （CLAUDE.md 規定 #ifdef _WIN32 只能出現在 platform 層），這裡用
    // 「所有檔案」讓對話框本身保持跨平台無關。
    const QString path = QFileDialog::getOpenFileName(this, tr("選擇執行檔"),
                                                       executablePath_->text(),
                                                       tr("所有檔案 (*)"));
    if (!path.isEmpty()) executablePath_->setText(path);
}

app::ExternalTool ExternalToolEditDialog::tool() const {
    app::ExternalTool result;
    result.name = name_->text().trimmed();
    result.executablePath = executablePath_->text().trimmed();
    result.argumentsTemplate = argumentsTemplate_->text();
    result.workingDirectory = workingDirectory_->text().trimmed();
    return result;
}

namespace {

QString toolListLabel(const app::ExternalTool& tool) {
    const app::ExternalToolValidationError error = app::validateExternalTool(tool);
    if (error == app::ExternalToolValidationError::None) return tool.name;
    // 失效的工具（例如換了電腦、原本的執行檔路徑不存在了）仍然列出來，
    // 不無聲消失，但要讓使用者一眼看出「這個現在不能用」。
    return tool.name + QStringLiteral("　[") + app::describeValidationError(error) +
           QStringLiteral("]");
}

}  // namespace

ExternalToolManagerDialog::ExternalToolManagerDialog(app::Settings* settings, QWidget* parent)
    : QDialog(parent), settings_(settings), tools_(settings->externalTools()) {
    setWindowTitle(tr("管理外部程式"));
    resize(480, 360);

    list_ = new QListWidget(this);
    refreshList();

    auto* addButton = new QPushButton(tr("新增(&A)…"), this);
    auto* editButton = new QPushButton(tr("編輯(&E)…"), this);
    auto* removeButton = new QPushButton(tr("移除(&R)"), this);
    connect(addButton, &QPushButton::clicked, this, &ExternalToolManagerDialog::addTool);
    connect(editButton, &QPushButton::clicked, this,
            &ExternalToolManagerDialog::editSelectedTool);
    connect(removeButton, &QPushButton::clicked, this,
            &ExternalToolManagerDialog::removeSelectedTool);
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { editSelectedTool(); });

    auto* buttonColumn = new QVBoxLayout;
    buttonColumn->addWidget(addButton);
    buttonColumn->addWidget(editButton);
    buttonColumn->addWidget(removeButton);
    buttonColumn->addStretch();

    auto* listRow = new QHBoxLayout;
    listRow->addWidget(list_, /*stretch=*/1);
    listRow->addLayout(buttonColumn);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::clicked, this, &QDialog::accept);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(listRow);
    layout->addWidget(buttons);
}

void ExternalToolManagerDialog::refreshList() {
    list_->clear();
    for (const app::ExternalTool& tool : tools_) {
        list_->addItem(toolListLabel(tool));
    }
}

void ExternalToolManagerDialog::addTool() {
    ExternalToolEditDialog dialog(app::ExternalTool{}, this);
    if (dialog.exec() != QDialog::Accepted) return;
    tools_.push_back(dialog.tool());
    persist();
}

void ExternalToolManagerDialog::editSelectedTool() {
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(tools_.size())) return;
    ExternalToolEditDialog dialog(tools_[static_cast<std::size_t>(row)], this);
    if (dialog.exec() != QDialog::Accepted) return;
    tools_[static_cast<std::size_t>(row)] = dialog.tool();
    persist();
}

void ExternalToolManagerDialog::removeSelectedTool() {
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(tools_.size())) return;
    tools_.erase(tools_.begin() + row);
    persist();
}

void ExternalToolManagerDialog::persist() {
    settings_->setExternalTools(tools_);
    refreshList();
    emit toolsChanged();
}

bool confirmExternalToolLaunch(const app::ExternalTool& tool,
                               const app::ExternalToolLaunchRequest& request, QWidget* parent) {
    // 安全限制 #3：啟動前必須顯示完整命令列，不是只顯示工具名稱。
    // 使用唯讀的 QPlainTextEdit 而不是 QLabel：命令列可能很長，使用者應該
    // 能選取、複製下來自行核對，而不是被截斷看不全。
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QObject::tr("啟動外部程式"));
    box.setText(QObject::tr("即將啟動「%1」，完整命令列如下：").arg(tool.name));
    box.setInformativeText(
        app::formatCommandLineForConfirmation(request));
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(QObject::tr("啟動"));
    box.button(QMessageBox::Cancel)->setText(QObject::tr("取消"));
    box.setDefaultButton(QMessageBox::Cancel);
    return box.exec() == QMessageBox::Ok;
}

}  // namespace alioth::ui
