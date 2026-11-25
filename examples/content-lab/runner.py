"""使用现有C++14 CLI/DSL执行本机内容网站自动化，不直接调用网站写接口。日期：2026-11-04。"""

import argparse
import base64
import hashlib
import json
import queue
import subprocess
import threading
import time
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

HERE = Path(__file__).resolve().parent


class ToolError(RuntimeError):
    # 用途：保留固定工具错误码，避免把输入值写入错误报告；日期：2026-11-04。
    def __init__(self, error):
        super().__init__(error["code"])
        self.code = error["code"]


class BrowserScript:
    # 用途：创建独立私有数据根和CLI进程，所有页面动作通过产品工具执行；日期：2026-11-04。
    def __init__(self, executable, browser, root, origin, headed=False):
        parsed = urlsplit(origin)
        if (
            parsed.scheme != "http"
            or parsed.hostname != "127.0.0.1"
            or not parsed.port
            or parsed.path
            or parsed.query
            or parsed.fragment
        ):
            raise ValueError("This example only operates a loopback Content Lab origin")
        self.origin = origin
        self.executable = Path(executable).resolve()
        self.process = subprocess.Popen(
            [
                str(self.executable),
                "--root",
                str(Path(root).resolve()),
                "--browser",
                str(Path(browser).resolve()),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        self.messages = queue.Queue()
        self.sequence = 0
        self.events = []
        self.base = {}
        self.profile = None
        self.headed = headed
        threading.Thread(target=self.read_output, daemon=True).start()

    # 用途：后台读取JSONL响应，主线程只做有界等待；日期：2026-11-04。
    def read_output(self):
        for line in self.process.stdout:
            self.messages.put(line)
        self.messages.put(None)

    # 用途：调用真实工具，仅保留命令和错误码而不记录参数或会话令牌；日期：2026-11-04。
    def call(self, command, arguments=None):
        self.sequence += 1
        request = {
            "id": self.sequence,
            "command": command,
            "arguments": arguments or {},
        }
        self.process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.process.stdin.flush()
        line = self.messages.get(timeout=35)
        if line is None:
            raise RuntimeError("CLI exited before returning a result")
        response = json.loads(line)
        if response["id"] != self.sequence:
            raise RuntimeError("CLI response sequence mismatch")
        self.events.append(
            {
                "sequence": self.sequence,
                "command": command,
                "result": "passed" if response["ok"] else response["error"]["code"],
            }
        )
        if not response["ok"]:
            raise ToolError(response["error"])
        return response["result"]

    # 用途：建立专用受管环境并获取五分钟排他控制权；日期：2026-11-04。
    def start(self):
        self.profile = self.call(
            "profile_create",
            {"name": "内容网站脚本练习", "allowedOrigins": [self.origin]},
        )
        self.call(
            "browser_start",
            {"profileId": self.profile["id"], "headless": not self.headed},
        )
        lease = self.call(
            "lease_acquire",
            {
                "profileId": self.profile["id"],
                "owner": "content-lab-script",
                "ttlMs": 300000,
            },
        )
        self.base = {"profileId": self.profile["id"], "lease": lease}
        self.navigate()
        self.expect("#auth-status", "ready")

    # 用途：每次操作前重新定位，避免保存跨页面旧引用；日期：2026-11-04。
    def action(self, command, selector, **extra):
        reference = self.call("page_locate", {**self.base, "selector": selector})["ref"]
        return self.call(command, {**self.base, "ref": reference, **extra})

    # 用途：只读等待确切业务文本，绝不在超时时重复提交；日期：2026-11-04。
    def expect(self, selector, expected):
        return self.call(
            "page_wait_text",
            {
                **self.base,
                "selector": selector,
                "expected": expected,
                "timeoutMs": 5000,
            },
        )

    # 用途：从当前页面读取公开文本；日期：2026-11-04。
    def text(self, selector):
        return self.action("page_text", selector)["text"]

    # 用途：通过产品填写真实表单字段；日期：2026-11-04。
    def fill(self, selector, value):
        return self.action("page_fill", selector, value=value)

    # 用途：点击一次并按调用方提供的业务状态核对；日期：2026-11-04。
    def click(self, selector, status=None, expected=None):
        result = self.action("page_click", selector)
        if status:
            self.expect(status, expected)
        return result

    # 用途：打开当前测试站点，调用方负责随后核对具体身份和状态；日期：2026-11-04。
    def navigate(self, path="/"):
        return self.call("browser_navigate", {**self.base, "url": self.origin + path})

    # 用途：通过注册表单创建帐号，仅将重复注册视为已存在；日期：2026-11-04。
    def register(self, supplied):
        self.fill("#username", supplied["user"])
        self.fill("#password", supplied["password"])
        self.click("#register")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            state = self.text("#auth-status")
            if state in ("registered", "DUPLICATE_USER"):
                return state
            if state != "busy":
                raise RuntimeError("Registration failed: " + state)
            time.sleep(0.05)
        raise TimeoutError("Registration not confirmed")

    # 用途：发布并执行可在桌面导入的DSL文件，回执未知或失败时不重新运行；日期：2026-11-04。
    def workflow(self, supplied, definition=None):
        flow = (
            json.loads((HERE / "workflow-publish-search.json").read_text("utf-8"))
            if definition is None
            else definition
        )
        flow["policy"]["allowedOrigins"] = [self.origin]
        for step in flow["steps"]:
            if step["type"] == "navigate":
                step["url"] = self.origin + "/"
        checked = self.call(
            "workflow_validate", {"definition": flow, "input": supplied}
        )
        if not checked["valid"]:
            raise RuntimeError(
                "Workflow definition/input validation failed; inspect workflow_validate errors"
            )
        published = self.call("workflow_publish", {"definition": flow})
        running = self.call(
            "workflow_run",
            {**self.base, "workflowId": published["workflowId"], "input": supplied},
        )
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            status = self.call("workflow_status", {"runId": running["runId"]})
            if status["state"] in (
                "succeeded",
                "failed",
                "outcome-unknown",
                "cancelled",
                "interrupted",
            ):
                return status
            time.sleep(0.1)
        raise TimeoutError(
            "Workflow still running; inspect workflow_runs before any rerun"
        )

    # 用途：读取已适配站点的固定列，分页有界采集并按实际文章ID去重；日期：2026-11-04。
    def collect(self, max_pages=3, max_results=20):
        rows, seen = [], set()
        total = int(self.text("#search-total"))
        for page in range(1, max_pages + 1):
            count = min(2, max(0, total - (page - 1) * 2))
            for index in range(1, count + 1):
                selector = "#results .result:nth-child(" + str(index) + ") .result-"
                row = {
                    name: self.text(selector + name)
                    for name in ("id", "title", "author", "url")
                }
                parsed = urlsplit(row["url"])
                if parsed.scheme + "://" + parsed.netloc != self.origin or parse_qs(
                    parsed.query
                ).get("article") != [row["id"]]:
                    raise RuntimeError("Search result identity/origin mismatch")
                if row["id"] not in seen:
                    seen.add(row["id"])
                    rows.append(row)
                if len(rows) >= max_results:
                    return rows
            if self.text("#has-next") != "true":
                break
            if page < max_pages:
                self.click(
                    "#next-page",
                    "#search-status",
                    "results:" + str(total) + ":page:" + str(page + 1),
                )
        return rows

    # 用途：按明确标识打开目标文章，避免按标题或搜索行序号绑定互动；日期：2026-11-04。
    def open_article(self, identity):
        self.fill("#open-id", identity)
        self.click("#open-article", "#detail-status", "opened:" + identity)
        self.expect("#article-id", identity)

    # 用途：对一个明确选中的测试文章点赞、评论并回复指定父评论；日期：2026-11-04。
    def interact(self, task_key):
        self.open_article("seed-a")
        already_liked = self.text("#like-state") == "liked"
        if not already_liked:
            self.click("#like", "#like-state", "liked")
        self.fill("#comment-key", task_key + "-comment")
        self.fill("#comment-body", "自动化测试评论：已核对文章内容。")
        self.click("#comment-send", "#comment-status", "submitted")
        self.click('[data-comment-id="comment-a"] button', "#reply-parent", "comment-a")
        self.fill("#reply-key", task_key + "-reply")
        self.fill("#reply-body", "自动化测试回复：此回复绑定comment-a。")
        self.click("#reply-send", "#reply-status", "submitted")
        return {
            "articleId": "seed-a",
            "like": "already-satisfied" if already_liked else "liked",
            "comment": "submitted",
            "replyParentId": "comment-a",
            "reply": "submitted",
        }

    # 用途：保存真实受管浏览器截图；日期：2026-11-04。
    def screenshot(self, path):
        result = self.call("page_screenshot", self.base)
        Path(path).write_bytes(base64.b64decode(result["base64"], validate=True))

    # 用途：关闭专用浏览器和CLI，不影响用户其他程序；日期：2026-11-04。
    def close(self):
        try:
            if self.process.poll() is None and self.profile:
                self.call("browser_stop", {"profileId": self.profile["id"]})
        finally:
            if self.process.poll() is None:
                self.process.stdin.close()
                try:
                    self.process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            if self.process.returncode:
                raise RuntimeError("CLI exited abnormally")


# 用途：运行开箱即用的本机浏览器脚本并输出不含凭据的报告；日期：2026-11-04。
def main():
    root = HERE.parent.parent
    parser = argparse.ArgumentParser(description="真实浏览器登录/发布/搜索/互动脚本")
    parser.add_argument(
        "--executable",
        type=Path,
        default=root / "build/native-vs2019/x64/Release/workbench-cli.exe",
    )
    parser.add_argument(
        "--browser", type=Path, default=root / "build/runtime/chrome-win64/chrome.exe"
    )
    parser.add_argument("--origin", default="http://127.0.0.1:3010")
    parser.add_argument("--inputs", type=Path, default=HERE / "inputs.example.json")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--headed", action="store_true", help="显示受管浏览器以观察自动操作"
    )
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    supplied = json.loads(args.inputs.read_text("utf-8"))
    report = {
        "date": "2026-11-04",
        "result": "failed",
        "origin": args.origin,
        "method": "C++ CLI workflow plus site-specific browser tool orchestration; no direct site write API",
        "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
    }
    client = BrowserScript(
        args.executable,
        args.browser,
        args.output / "private-browser-data",
        args.origin,
        args.headed,
    )
    try:
        client.start()
        report["registration"] = client.register(supplied)
        status = client.workflow(supplied)
        report["workflow"] = {
            "runId": status["runId"],
            "state": status["state"],
            "steps": len(status["steps"]),
        }
        if status["state"] != "succeeded":
            raise RuntimeError(
                "Workflow ended: " + status["state"] + "; reconcile before rerun"
            )
        report["publishedArticleId"] = client.text("#article-id")
        report["results"] = client.collect()
        if not any(row["id"] == "seed-a" for row in report["results"]):
            raise RuntimeError(
                "Explicit interaction target is not in the search results"
            )
        report["interaction"] = client.interact(supplied["taskKey"])
        client.screenshot(args.output / "browser.png")
        report["result"] = "passed"
    except Exception as error:
        report["error"] = (
            str(error)
            if isinstance(error, (ToolError, RuntimeError))
            else type(error).__name__
        )
        raise
    finally:
        try:
            client.close()
        finally:
            report["commands"] = client.events
            (args.output / "report.json").write_bytes(
                (json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode(
                    "utf-8"
                )
            )
    print(
        json.dumps(
            {"result": report["result"], "report": str(args.output / "report.json")},
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
