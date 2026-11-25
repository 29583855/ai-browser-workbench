"""以真实Chromium验证多页面会话、弹窗归属、引用失效及有界事件游标。日期：2026-11-04。"""

import argparse
import hashlib
import json
import threading
import time
from datetime import datetime
from pathlib import Path
from integration_browser import Client, FixtureServer


# 用途：只读轮询页面列表等待真实弹窗出现，不重复点击创建；日期：2026-11-04。
def wait_pages(client, base, count):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        pages = client.call("page_list", base)
        if len(pages["pages"]) == count:
            return pages
        time.sleep(0.03)
    raise AssertionError("Managed popup count was not observed")


# 用途：独立环境运行真实页面/弹窗/事件，错误不回退为假成功；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    client = Client(args.executable.resolve(), output / "profiles", args.browser.resolve())
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        origin = f"http://127.0.0.1:{server.server_port}"
        settings = {"userAgent": "AWB-Multipage/1.0", "languages": ["fr-FR", "fr"], "timezone": "Europe/Paris"}
        p = client.call("profile_create", {"name": "多页面会话", "allowedOrigins": [origin], "runtimeSettings": settings})
        client.call("browser_start", {"profileId": p["id"], "headless": True})
        lease = client.call("lease_acquire", {"profileId": p["id"], "owner": "pages", "ttlMs": 300000})
        base = {"profileId": p["id"], "lease": lease}
        pages = client.call("page_list", base)
        assert len(pages["pages"]) == 1
        first = pages["activePageId"]
        blank = client.call("page_open", {**base, "url": "about:blank"})["pageId"]
        client.call("page_close", {**base, "pageId": blank})
        client.call("browser_navigate", {**base, "url": origin + "/pages"})
        old = client.call("page_locate", {**base, "selector": "#identity"})["ref"]
        second = client.call("page_open", {**base, "url": origin + "/runtime"})["pageId"]
        assert first != second
        ref = client.call("page_locate", {**base, "selector": "#runtime"})["ref"]
        actual = json.loads(client.call("page_text", {**base, "ref": ref})["text"])
        assert actual == settings
        client.call("page_text", {**base, "ref": old}, "STALE_ELEMENT_REFERENCE")
        client.call("page_switch", {**base, "pageId": first})
        client.call("page_text", {**base, "ref": ref}, "STALE_ELEMENT_REFERENCE")
        ref = client.call("page_locate", {**base, "selector": "#identity"})["ref"]
        assert client.call("page_text", {**base, "ref": ref})["text"] == "page-one"
        client.call("page_open", {**base, "url": f"http://localhost:{server.server_port}/runtime"}, "ORIGIN_DENIED")
        assert len(client.call("page_list", base)["pages"]) == 2
        report["checks"].append("independent-sessions-blank-page-runtime-origin-and-stale-references")
        other = client.call("profile_create", {"name": "另一环境", "allowedOrigins": [origin]})
        client.call("browser_start", {"profileId": other["id"], "headless": True})
        other_lease = client.call("lease_acquire", {"profileId": other["id"], "owner": "other", "ttlMs": 300000})
        client.call("page_switch", {"profileId": other["id"], "lease": other_lease, "pageId": first}, "PAGE_NOT_FOUND")
        client.call("browser_stop", {"profileId": other["id"]})
        ref = client.call("page_locate", {**base, "selector": "#popup"})["ref"]
        client.call("page_click", {**base, "ref": ref})
        popup = next(page["pageId"] for page in wait_pages(client, base, 3)["pages"] if page["pageId"] not in [first, second])
        client.call("page_switch", {**base, "pageId": popup})
        assert client.call("browser_status", {"profileId": p["id"]})["runtime"]["effective"] == settings
        client.call("page_switch", {**base, "pageId": first})
        ref = client.call("page_locate", {**base, "selector": "#denied"})["ref"]
        client.call("page_click", {**base, "ref": ref})
        assert len(client.call("page_list", base)["pages"]) == 3
        report["checks"].append("popup-adoption-foreign-origin-exclusion-and-profile-boundary")
        events = client.call("browser_events", {**base, "limit": 2})
        epoch = events["cursor"]["eventEpoch"]
        assert events["events"] and events["more"] and not events["liveSubscription"]
        cursor = events["cursor"]
        seen = [event["sequence"] for event in events["events"]]
        while events["more"]:
            events = client.call("browser_events", {**base, **cursor, "limit": 2})
            seen += [event["sequence"] for event in events["events"]]
            cursor = events["cursor"]
        assert len(seen) == len(set(seen)) and seen == sorted(seen)
        client.call("browser_events", {**base, "after": cursor["after"] + 10000, "eventEpoch": epoch}, "EVENT_CURSOR_INVALID")
        client.call("browser_events", {**base, "after": 1}, "EVENT_CURSOR_INVALID")
        for index in range(90):
            client.call("browser_navigate", {**base, "url": origin + "/pages?event-query-canary-" + str(index)})
        gap = client.call("browser_events", {**base, "after": 0, "eventEpoch": epoch, "limit": 100})
        assert gap["gap"] and len(gap["events"]) <= 100 and gap["bufferLimit"] == 256
        assert "event-query-canary" not in json.dumps(gap) and all("url" not in event for event in gap["events"])
        report["checks"].append("event-pagination-no-duplicates-overflow-gap-and-no-page-secrets")
        client.call("browser_navigate", {**base, "url": origin + "/login"})
        stale = client.call("page_locate", {**base, "selector": "#identity"})["ref"]
        hang = client.call("page_locate", {**base, "selector": "#hang-submit"})["ref"]
        client.call("page_click", {**base, "ref": hang}, "ACTION_OUTCOME_UNKNOWN")
        assert server.fault_commit_count == 1
        assert not client.call("browser_status", {"profileId": p["id"]})["cdpConnected"]
        client.call("page_snapshot", base, "CDP_DISCONNECTED")
        restored = client.call("browser_reconnect", base)
        assert restored["reconnected"] and restored["replayedPageActions"] == 0
        assert restored["activePageId"] == first and server.fault_commit_count == 1
        client.call("page_text", {**base, "ref": stale}, "STALE_ELEMENT_REFERENCE")
        client.call("page_wait_text", {**base, "selector": "#identity", "expected": "anonymous", "timeoutMs": 5000})
        connection_events = client.call("browser_events", {**base, "after": gap["currentSequence"], "eventEpoch": epoch, "limit": 100})
        assert any(event["method"] == "workbench.connectionReplaced" for event in connection_events["events"])
        report["checks"].append("real-timeout-explicit-reconnect-preserves-pages-no-business-replay")
        client.call("lease_revoke", {"profileId": p["id"]})
        client.call("page_open", {**base, "url": origin + "/pages"}, "LEASE_REVOKED")
        client.call("page_close", {**base, "pageId": second}, "LEASE_REVOKED")
        client.call("browser_reconnect", base, "LEASE_REVOKED")
        base["lease"] = client.call("lease_acquire", {"profileId": p["id"], "owner": "resumed", "ttlMs": 300000})
        assert len(client.call("page_list", base)["pages"]) == 3
        extra_pages = []
        for _ in range(29):
            extra_pages.append(client.call("page_open", {**base, "url": "about:blank"})["pageId"])
        assert len(client.call("page_list", base)["pages"]) == 32
        client.call("page_open", {**base, "url": "about:blank"}, "PAGE_BUDGET_EXCEEDED")
        for identity in extra_pages:
            client.call("page_close", {**base, "pageId": identity})
        report["checks"].append("explicit-32-page-budget-and-close-reclaims-capacity")
        for identity in [popup, second, first]:
            client.call("page_close", {**base, "pageId": identity})
        assert client.call("page_list", base)["pages"] == []
        client.call("page_snapshot", base, "PAGE_REQUIRED")
        reopened = client.call("page_open", {**base, "url": origin + "/pages"})
        assert reopened["pageId"] != first
        client.call("page_switch", {**base, "pageId": first}, "PAGE_NOT_FOUND")
        client.call("browser_stop", {"profileId": p["id"]})
        client.call("browser_start", {"profileId": p["id"], "headless": True})
        base["lease"] = client.call("lease_acquire", {"profileId": p["id"], "owner": "restart", "ttlMs": 300000})
        client.call("browser_events", {**base, "after": 0, "eventEpoch": epoch}, "EVENT_CURSOR_INVALID")
        client.call("browser_stop", {"profileId": p["id"]})
        report["checks"].append("lease-fencing-close-last-reopen-and-instance-cursor-reset")
        report["result"] = "passed"
    finally:
        try:
            client.close()
        finally:
            server.shutdown()
            server.server_close()
            report["commands"] = client.events
            (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "report": str(output / "report.json")}))


if __name__ == "__main__":
    main()
