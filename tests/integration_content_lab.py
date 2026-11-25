"""通过产品CLI驱动真实浏览器验证内容站点，SQLite仅作独立结果核对。日期：2026-11-04。"""

import argparse
import copy
import hashlib
import json
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LAB = ROOT / "examples/content-lab"
sys.path.insert(0, str(LAB))
from runner import BrowserScript, ToolError
from server import ContentServer


# 用途：读取数据库真实业务记录，验证器不通过数据库制造业务成功；日期：2026-11-04。
def facts(server):
    with server.connect() as db:
        return {
            "articles": [
                dict(r)
                for r in db.execute(
                    "SELECT id,author,title,body,category,state,task_key FROM articles ORDER BY rowid"
                )
            ],
            "comments": [
                dict(r) for r in db.execute("SELECT * FROM comments ORDER BY rowid")
            ],
            "likes": [
                dict(r) for r in db.execute("SELECT * FROM likes ORDER BY rowid")
            ],
            "events": [
                dict(r)
                for r in db.execute(
                    "SELECT kind,object_id,username FROM events ORDER BY id"
                )
            ],
        }


# 用途：从预期错误读取固定错误码，出现成功或其他错误均不视为通过；日期：2026-11-04。
def rejected(client, command, arguments, expected):
    try:
        client.call(command, arguments)
    except ToolError as error:
        assert error.code == expected, error.code
        return
    raise AssertionError("Expected tool rejection: " + expected)


# 用途：准备新稿并通过真实编辑表单提交指定业务状态；日期：2026-11-04。
def publish_form(client, key, button, expected):
    client.click("#create-article", "#editor-status", "ready")
    client.fill("#publish-key", key)
    client.fill("#article-title", "独立状态验证 " + key)
    client.fill("#article-body", "状态测试正文")
    client.click(button, "#publish-status", expected)


# 用途：执行正向、重复、权限、断回执和持久化场景并完整保留失败报告；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--private-root", type=Path, required=True)
    parser.add_argument("--headed", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    args.private_root.mkdir(parents=True, exist_ok=False)
    server = ContentServer(0, args.private_root / "content.sqlite3")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    report = {
        "date": "2026-11-04",
        "result": "failed",
        "checks": [],
        "workflows": [],
        "origin": server.origin,
        "method": "real managed Chromium through existing C++ CLI; database readback only",
        "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
        "workflowSha256": hashlib.sha256(
            (LAB / "workflow-publish-search.json").read_bytes()
        ).hexdigest(),
        "privateRuntimeDataPublished": False,
    }
    client = BrowserScript(
        args.executable,
        args.browser,
        args.private_root / "browser",
        server.origin,
        args.headed,
    )
    supplied = json.loads((LAB / "inputs.example.json").read_text("utf-8"))
    try:
        client.start()
        assert client.register(supplied) == "registered"
        assert client.register(supplied) == "DUPLICATE_USER"
        client.fill("#password", "Incorrect-Only-2026!")
        client.click("#login", "#auth-status", "INVALID_CREDENTIALS")
        client.expect("#identity", "anonymous")
        report["checks"].append("registration-duplicate-and-wrong-password")

        status = client.workflow(supplied)
        report["workflows"].append(
            {
                "name": "publish-search",
                "runId": status["runId"],
                "state": status["state"],
                "steps": len(status["steps"]),
            }
        )
        assert status["state"] == "succeeded", status
        assert len(status["steps"]) == 19
        identity = client.text("#article-id")
        rows = client.collect()
        assert len(rows) == 4 and len({r["id"] for r in rows}) == 4
        assert {r["id"] for r in rows} == {"seed-a", "seed-b", "seed-c", identity}
        report["searchResults"] = rows
        with server.connect() as db:
            article = dict(
                db.execute("SELECT * FROM articles WHERE id=?", (identity,)).fetchone()
            )
        assert (
            article["title"],
            article["body"],
            article["author"],
            article["category"],
            article["state"],
        ) == (
            supplied["title"],
            supplied["body"],
            supplied["user"],
            supplied["category"],
            "published",
        )
        client.expect("#settings-status", "unchanged")
        rejected(
            client,
            "page_locate",
            {**client.base, "selector": "#article-publish, #settings-publish"},
            "LOCATOR_AMBIGUOUS",
        )
        report["checks"].append(
            "nineteen-step-dsl-publish-full-body-author-category-and-search-two-pages"
        )
        report["checks"].append(
            "same-name-publish-buttons-ambiguous-rejected-settings-untouched"
        )
        client.screenshot(args.output / "published-search.png")

        first = client.interact(supplied["taskKey"])
        assert first["like"] == "liked"
        second = client.interact(supplied["taskKey"])
        assert second["like"] == "already-satisfied"
        state = facts(server)
        assert len(state["likes"]) == 1
        own_comments = [c for c in state["comments"] if c["author"] == supplied["user"]]
        assert len(own_comments) == 2
        assert {c["parent"] for c in own_comments} == {None, "comment-a"}
        assert all(c["article"] == "seed-a" for c in own_comments)
        assert len([e for e in state["events"] if e["kind"] == "like"]) == 1
        rejected(
            client,
            "page_locate",
            {**client.base, "selector": "#comments button"},
            "LOCATOR_AMBIGUOUS",
        )
        report["checks"].append(
            "like-skipped-on-second-run-comments-idempotent-correct-parent-bound"
        )
        client.screenshot(args.output / "comments-replies.png")

        again = client.workflow(supplied)
        assert again["state"] == "succeeded", again
        report["workflows"].append(
            {
                "name": "repeat-acknowledged-task",
                "runId": again["runId"],
                "state": again["state"],
                "steps": len(again["steps"]),
            }
        )
        assert (
            len(
                [
                    a
                    for a in facts(server)["articles"]
                    if a["task_key"] == supplied["taskKey"]
                    and a["author"] == supplied["user"]
                ]
            )
            == 1
        )
        report["checks"].append(
            "repeat-completed-task-same-content-does-not-create-second-article"
        )

        client.fill("#search-query", "不存在的关键词-89731")
        client.click("#search", "#search-status", "results:0:page:1")
        assert client.collect() == []
        report["checks"].append("empty-search-ends-without-interaction")
        publish_form(client, "draft-test", "#article-draft", "draft")
        client.expect("#article-state", "draft")
        publish_form(client, "review-test", "#article-review", "pending-review")
        client.expect("#article-state", "pending-review")
        report["checks"].append("draft-and-pending-review-never-reported-as-published")

        publish_form(client, "draft-test", "#article-publish", "TASK_CONTENT_CONFLICT")
        assert (
            len([a for a in facts(server)["articles"] if a["task_key"] == "draft-test"])
            == 1
        )
        report["checks"].append("same-task-different-state-or-content-rejected")

        fault_input = copy.deepcopy(supplied)
        fault_input["taskKey"] = "lost-publish-receipt"
        fault_input["expectedSearchStatus"] = "results:5:page:1"
        server.inject("/api/publish", "lose-receipt")
        unknown = client.workflow(fault_input)
        report["workflows"].append(
            {
                "name": "lost-publish-receipt",
                "runId": unknown["runId"],
                "state": unknown["state"],
                "errorCode": unknown.get("errorCode"),
            }
        )
        assert unknown["state"] == "outcome-unknown", unknown
        assert all(s["stepId"] != "search" for s in unknown["steps"])
        persisted = [
            a
            for a in facts(server)["articles"]
            if a["task_key"] == fault_input["taskKey"]
        ]
        assert len(persisted) == 1
        client.open_article(persisted[0]["id"])
        client.expect("#article-state", "published")
        client.expect("#detail-title", supplied["title"])
        report["checks"].append(
            "publish-http-receipt-lost-one-commit-workflow-unknown-readonly-reconcile-no-resubmit"
        )

        client.open_article("seed-a")
        client.fill("#comment-key", "lost-comment-receipt")
        client.fill("#comment-body", "回执丢失时只发送一次。")
        server.inject("/api/comment", "lose-receipt")
        client.click("#comment-send", "#comment-status", "RECEIPT_UNKNOWN")
        unknown_comments = [
            c
            for c in facts(server)["comments"]
            if c["task_key"] == "lost-comment-receipt"
        ]
        assert len(unknown_comments) == 1
        client.open_article("seed-a")
        client.expect(
            '[data-comment-id="' + unknown_comments[0]["id"] + '"] p',
            "回执丢失时只发送一次。",
        )
        report["checks"].append(
            "comment-http-receipt-lost-one-commit-readonly-reconcile-no-resubmit"
        )

        before = len(facts(server)["comments"])
        client.fill("#comment-key", "rate-limit-comment")
        client.fill("#comment-body", "本条应被拒绝。")
        server.inject("/api/comment", "rate-limit")
        client.click("#comment-send", "#comment-status", "RATE_LIMITED")
        assert len(facts(server)["comments"]) == before
        report["checks"].append("rate-limit-stops-write-no-new-comment")
        server.inject("/api/comment", "expire-session")
        client.click("#comment-send", "#comment-status", "LOGIN_REQUIRED")
        assert len(facts(server)["comments"]) == before
        client.navigate()
        client.expect("#identity", "anonymous")
        report["checks"].append("expired-session-rejected-no-new-comment")

        client.fill("#username", "demo-reader")
        client.fill("#password", "ContentLab-2026!")
        client.click("#login", "#auth-status", "logged-in")
        client.expect("#identity", "demo-reader")
        before = len(facts(server)["articles"])
        publish_form(client, "reader-publish", "#article-publish", "PUBLISH_FORBIDDEN")
        assert len(facts(server)["articles"]) == before
        report["checks"].append(
            "reader-role-cannot-publish-author-is-not-client-controlled"
        )

        client.navigate()
        client.expect("#auth-status", "ready")
        old = client.call("page_locate", {**client.base, "selector": "#username"})[
            "ref"
        ]
        client.navigate()
        client.expect("#auth-status", "ready")
        rejected(
            client,
            "page_fill",
            {**client.base, "ref": old, "value": "must-not-fill"},
            "STALE_ELEMENT_REFERENCE",
        )
        report["checks"].append("page-navigation-invalidates-old-element-reference")

        saved = facts(server)
        port = server.server_port
        server.shutdown()
        server.server_close()
        server = ContentServer(port, args.private_root / "content.sqlite3")
        threading.Thread(target=server.serve_forever, daemon=True).start()
        assert facts(server) == saved
        client.navigate()
        client.expect("#identity", "demo-reader")
        client.open_article(identity)
        client.expect("#detail-title", supplied["title"])
        report["checks"].append(
            "website-restart-preserves-session-articles-likes-and-comment-parentage"
        )
        report["serverFacts"] = saved
        report["result"] = "passed"
    except Exception as error:
        report["errorType"] = type(error).__name__
        report["lastCommands"] = client.events[-8:]
        raise
    finally:
        try:
            client.close()
        finally:
            server.shutdown()
            server.server_close()
            report["commands"] = client.events
            (args.output / "report.json").write_bytes(
                (json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode(
                    "utf-8"
                )
            )
    print(
        json.dumps(
            {
                "result": report["result"],
                "checks": len(report["checks"]),
                "calls": len(client.events),
            },
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
