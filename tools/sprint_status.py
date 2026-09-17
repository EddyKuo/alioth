"""產生 sprint/current/metrics.md（目前的測試與需求數字）。

存在的理由是一次真實的腐化：`sprint/current/status.md` 手寫著「13 個測試目標全綠」，
同一份文件後面又寫「24 個測試目標全綠」，而實際登錄數早就是三位數。兩句話都曾經
是對的，錯的是「把會變的數字寫進手寫文件」這件事本身——沒有人會在加一支測試時
回頭改它，而一份數字錯了的狀態文件不能拿來當排程或 release gate 的依據。

因此這裡的規則是：**會變的數字一律由這支腳本產生，手寫文件只寫判斷與理由。**

數字來源都是可執行的事實，不是人的記憶：
  - 測試登錄數與停用數：`ctest -N`（要有建置目錄；沒有就明說沒量到，不猜）
  - 需求統計：tools/traceability.py 的 STATUS 表（與對標矩陣同一個來源）

用法：
    python tools/sprint_status.py [preset]      預設 windows-x64-debug
"""

import re
import subprocess
import sys
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import traceability  # noqa: E402  匯入時會跑它自己的資料閘門，那正是我們要的


def test_counts(preset):
    """回傳 (登錄數, 停用數)；量不到時回傳 (None, None)。

    量不到就明說量不到。塞一個 0 或上次的數字進去，比留白更糟：留白看得出
    沒量到，假數字看不出來。
    """
    build_dir = ROOT / "out" / "build" / preset
    if not build_dir.is_dir():
        return None, None
    try:
        result = subprocess.run(
            ["ctest", "--test-dir", str(build_dir), "-N"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None, None
    if result.returncode != 0:
        return None, None

    total = None
    match = re.search(r"Total Tests:\s*(\d+)", result.stdout)
    if match:
        total = int(match.group(1))
    # ctest -N 會在停用的項目後面標 (Disabled)。停用的測試仍然算進 Total Tests，
    # 但它不提供任何保護——兩個數字必須分開寫，否則「182 個測試」會被讀成
    # 「182 份保護」。
    disabled = len(re.findall(r"\(Disabled\)", result.stdout))
    return total, disabled


def requirement_counts():
    """需求統計。

    刻意走 traceability 的 PRD 解析而不是直接數 STATUS 的鍵：STATUS 可能含
    PRD 表格裡沒有獨立編號的條目，直接數會比對標矩陣多出幾條——兩份文件
    對同一件事給出不同的數字，正是這次要修掉的毛病。
    """
    rows = traceability.parse_requirements(traceability.PRD.read_text(encoding="utf-8"))
    counts = {}
    for row in rows:
        status = traceability.STATUS.get(row["id"], ("未開始", ""))[0]
        counts[status] = counts.get(status, 0) + 1
    return counts


def main():
    preset = sys.argv[1] if len(sys.argv) > 1 else "windows-x64-debug"
    total, disabled = test_counts(preset)
    counts = requirement_counts()
    listed = sum(counts.values())

    lines = [
        "# 目前數字（自動產生）",
        "",
        "<!-- 由 `python tools/sprint_status.py` 產生，不要手動編輯。",
        "     手寫的數字會腐化：status.md 曾經同時寫著 13 與 24 個測試目標全綠。 -->",
        "",
        f"| 項目 | 數值 |",
        "|---|---|",
        f"| 產生日期 | {date.today().isoformat()} |",
        f"| 量測 preset | `{preset}` |",
    ]

    if total is None:
        lines.append("| 登錄測試數 | 未量到（沒有建置目錄，先跑 `build.bat`） |")
        lines.append("| 停用測試數 | 未量到 |")
    else:
        lines.append(f"| 登錄測試數 | {total} |")
        lines.append(f"| 其中停用（不提供保護） | {disabled} |")
        lines.append(f"| 實際執行 | {total - disabled} |")

    lines.append(f"| 表列需求數 | {listed} |")
    # 四種狀態一律列出，包含 0。少列一行會讓讀表的人以為那一類不存在，
    # 而「未開始 0」與「沒有未開始這一欄」是完全不同的訊息。
    for status in ("完成", "部分", "未開始", "範圍外"):
        lines.append(f"| 需求：{status} | {counts.get(status, 0)} |")

    lines += [
        "",
        "登錄數與執行數必須分開看：停用的測試仍然算進 ctest 的總數，",
        "但它不提供任何保護。兩者相等才是「測試數等於保護範圍」。",
        "",
        "需求統計與 `docs/TRACEABILITY.md` 同一個來源（`tools/traceability.py` 的 STATUS 表），",
        "所以兩處不可能對不上；STATUS 本身是人工維護的，備註欄寫的是做到什麼程度。",
        "",
    ]

    target = ROOT / "sprint" / "current" / "metrics.md"
    target.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {target}")
    if total is not None and disabled:
        print(f"注意：有 {disabled} 個停用的測試，它們不提供保護")
    return 0


if __name__ == "__main__":
    sys.exit(main())
