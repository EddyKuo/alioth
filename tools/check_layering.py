"""分層檢查（CLAUDE.md 的架構硬約束）。

五條規則，違反了會在 CI 擋下來而不是等到 code review：

1. 只有引擎層的 target 可以連結 pdfium
2. 領域層不得連結 Qt（領域層的測試因此不需要 QApplication）
3. 只有平台層可以有 #ifdef _WIN32
4. 每個 PDFium 呼叫點都要在 PdfiumGuard 底下
5. 產品程式碼不得接線並行 PDFium 路徑（ADR-005）

這些規則原本靠 CMake 的 target 相依「自然」維持，但那只擋得住連結錯誤，
擋不住有人在 src/ui 裡寫 #include <fpdfview.h> 然後透過某個 PUBLIC 相依
意外編得過。這支腳本直接掃原始碼，因此擋得住。

輸出寫進 stdout；有違規時退出碼為 1。
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

# 引擎層以外的目錄。這些底下不得出現 PDFium 的標頭。
NON_ENGINE_DIRS = ["domain", "app", "ui", "platform"]

PDFIUM_INCLUDE = re.compile(r'#\s*include\s*[<"](fpdf[a-z_]*\.h|public/fpdf[a-z_/]*\.h)[>"]')
QT_INCLUDE = re.compile(r'#\s*include\s*[<"]Q[A-Za-z]')
WIN32_IFDEF = re.compile(r"#\s*if(def)?\s+.*_WIN32")


def source_files(directory):
    for base, _dirs, files in os.walk(directory):
        for name in files:
            if name.endswith((".h", ".cpp")):
                yield os.path.join(base, name)


def relative(path):
    return os.path.relpath(path, ROOT).replace("\\", "/")


LINE_COMMENT = re.compile(r"//.*$", re.MULTILINE)
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(text):
    """把註解去掉再比對。

    註解裡提到 `#ifdef _WIN32`（例如說明「為什麼這裡不該寫它」）不是違規，
    但正則不會分辨。第一次跑這支腳本時就是被自己寫的說明註解誤判的。
    """
    return LINE_COMMENT.sub("", BLOCK_COMMENT.sub("", text))


FPDF_CALL = re.compile(r"\bFPDF[A-Za-z]*_[A-Za-z0-9]+\s*\(")
PDFIUM_GUARD = re.compile(r"\bPdfiumGuard\b")

# 呼叫 PDFium 但不自己取序列化鎖的檔案，連同「為什麼不需要」。
#
# 這份清單存在的理由：ADR-005 量到 PDFium **可並存、不可並行**，
# 兩條執行緒同時進去會安靜地掉資料。因此每一個 PDFium 呼叫點都必須落在
# 某一把 PdfiumGuard 底下。免除只有兩種正當理由：
#   (a) 它本來就在某個已加鎖的工作內部執行（呼叫端已經持有鎖）
#   (b) 它根本不在這個行程裡跑
#
# 加新檔案到這裡之前先問：它是在哪一條執行緒上跑的？答不出來就不要加免除。
PDFIUM_GUARD_EXEMPT = {
    "src/engine/pdfium_library.cpp":
        "只做 FPDF_InitLibraryWithConfig，由 call_once 保證只跑一次",
    "src/engine/file_source.cpp":
        "m_GetBlock 回呼，由 PDFium 在已持鎖的工作內部呼叫",
    "src/engine/text/text_search.cpp":
        "只在 TextExtractor::withTextPage 的回呼裡執行，該處已持鎖",
    "src/engine/shellthumb/thumbnail_renderer.cpp":
        "載進 explorer.exe，與本程式不同行程；那個行程裡只有它一個使用者",
    "src/engine/forms/form_environment.cpp":
        "只在 FormDocument 的工作內部執行，該處已持鎖",
    "src/engine/save/save_testing.cpp":
        "測試輔助，單執行緒同步使用",
}


def check_pdfium_serialisation():
    """每個 PDFium 呼叫點都必須在某一把 PdfiumGuard 底下（ADR-005）。

    這條規則抓的是「新增一個同步的 PDFium 使用者、忘了序列化」——那種缺陷
    不會崩潰，只會在某個交錯順序下安靜地少給資料，而且通常在使用者的機器上
    才出現。靠 code review 抓不到，因為缺的東西是看不見的。
    """
    violations = []
    engine = os.path.join(SRC, "engine")
    if not os.path.isdir(engine):
        return violations

    for path in source_files(engine):
        if not path.endswith(".cpp"):
            continue
        text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
        if not FPDF_CALL.search(text):
            continue
        if PDFIUM_GUARD.search(text):
            continue
        name = relative(path)
        if name in PDFIUM_GUARD_EXEMPT:
            continue
        violations.append(
            "{}：呼叫 PDFium 但沒有取 PdfiumGuard（ADR-005）。"
            "若確定它在已持鎖的工作內部執行，請加進 PDFIUM_GUARD_EXEMPT 並寫明理由".format(name))
    return violations


PARALLEL_SEARCH_USE = re.compile(r"\bParallelSearchSession\b")

# 定義 ParallelSearchSession 的檔案本身。它們是 ADR-005 的墓碑：保留下來是為了讓
# 「為什麼不這樣做」有實體可指，不是為了被呼叫。
PARALLEL_SEARCH_DEFINITION = {
    "src/engine/text/parallel_search.h",
    "src/engine/text/parallel_search.cpp",
}


def check_no_parallel_pdfium():
    """產品程式碼不得接線並行 PDFium 路徑（ADR-005）。

    ADR-005 量到的事實是：PDFium 的文件把手可並存、不可並行，兩條執行緒同時
    進去會**安靜地掉資料**，之後整個行程的開檔開始失敗。壞掉的方式不是崩潰，
    所以「跑起來沒事」不能當證據，只能靠這種靜態檢查擋在接線的那一刻。

    重現器本身（tests/diagnostics/）不在檢查範圍內——它的工作就是重現那個現象。
    """
    violations = []
    for path in source_files(SRC):
        rel = relative(path)
        if rel in PARALLEL_SEARCH_DEFINITION:
            continue
        text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
        if PARALLEL_SEARCH_USE.search(text):
            violations.append(
                "{}：不得接線並行 PDFium 搜尋（ADR-005；改用單一擷取器預建文字索引）"
                .format(rel))
    return violations


def main():
    violations = []

    for name in NON_ENGINE_DIRS:
        directory = os.path.join(SRC, name)
        if not os.path.isdir(directory):
            continue
        for path in source_files(directory):
            text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
            if PDFIUM_INCLUDE.search(text):
                violations.append(
                    "{}：{} 層不得引入 PDFium 標頭".format(relative(path), name))

    domain = os.path.join(SRC, "domain")
    for path in source_files(domain):
        text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
        match = QT_INCLUDE.search(text)
        if match:
            violations.append(
                "{}：領域層不得依賴 Qt（{}）".format(relative(path), match.group(0).strip()))

    # #ifdef _WIN32 只允許出現在平台層。其他層需要平台差異時，
    # 應該在平台層開一個介面，而不是就地分歧。
    for name in ["domain", "app", "ui"]:
        directory = os.path.join(SRC, name)
        if not os.path.isdir(directory):
            continue
        for path in source_files(directory):
            text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
            if WIN32_IFDEF.search(text):
                violations.append(
                    "{}：作業系統差異只能收在平台層".format(relative(path)))

    violations.extend(check_pdfium_serialisation())
    violations.extend(check_no_parallel_pdfium())

    if violations:
        print("分層檢查失敗：")
        for line in violations:
            print("  " + line)
        return 1

    print("分層檢查通過")
    return 0


if __name__ == "__main__":
    sys.exit(main())
