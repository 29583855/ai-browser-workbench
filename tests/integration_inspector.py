"""真实验证元素探查/高亮/预览、填写准入和原生选项操作。日期：2026-11-04。"""

import argparse
import base64
import hashlib
import json
import threading
from datetime import datetime
from pathlib import Path
from PIL import Image, ImageChops
from integration_browser import Client, FixtureServer


# 用途：在真实站点验证观察无副作用、表单事件和负向准入，证据不保存填写内容；日期：2026-11-04。
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
        profile = client.call("profile_create", {"name": "元素探查与表单", "allowedOrigins": [origin]})
        client.call("browser_start", {"profileId": profile["id"], "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile["id"], "owner": "inspect", "ttlMs": 300000})
        base = {"profileId": profile["id"], "lease": lease}

        # 用途：共用当前环境参数并保持测试输入不进入报告；日期：2026-11-04。
        def call(command, extra=None, error=None):
            return client.call(command, {**base, **(extra or {})}, error)

        # 用途：只读获取独立网页断言，不用客户端返回值代替DOM结果；日期：2026-11-04。
        def text(selector):
            ref = call("page_locate", {"selector": selector})["ref"]
            return call("page_text", {"ref": ref})["text"]

        call("browser_navigate", {"url": origin + "/inspect"})
        facts = call("page_inspect", {"selector": "#editable", "highlight": True})
        assert facts["fillReady"] and facts["highlighted"] and not facts["executed"]
        assert facts["tagName"] == "INPUT" and facts["geometry"]["width"] > 0
        assert [d["tool"] for d in facts["drafts"]] == ["page_click", "page_fill"]
        assert "secret-canary-" not in json.dumps(facts) and "value" not in facts
        for draft in facts["drafts"]:
            assert "value" not in draft["arguments"] and "lease" not in draft["arguments"]
        (output / "highlighted.png").write_bytes(base64.b64decode(call("page_screenshot")["base64"]))
        assert call("page_clear_highlight")["cleared"]
        (output / "cleared.png").write_bytes(base64.b64decode(call("page_screenshot")["base64"]))
        difference = ImageChops.difference(Image.open(output / "highlighted.png"), Image.open(output / "cleared.png")).getbbox()
        assert difference is not None and difference[2] - difference[0] > 50 and difference[3] - difference[1] > 10
        geometry = facts["geometry"]
        assert abs(difference[0] - geometry["x"]) <= 3 and abs(difference[1] - geometry["y"]) <= 3
        report["highlightPixelDifferenceBounds"] = list(difference)
        call("page_inspect", {"selector": ".duplicate", "highlight": True}, "LOCATOR_AMBIGUOUS")
        call("page_inspect", {"selector": "#missing"}, "ELEMENT_NOT_FOUND")
        report["checks"].append("unique-inspection-native-highlight-clear-and-secret-free-drafts")

        ref = call("page_locate", {"selector": "#click"})["ref"]
        assert call("page_preview", {"ref": ref, "action": "click"})["readyNow"]
        assert text("#clicks") == "0" and text("#input-events") == "0:0"
        for selector, reason in [("#covered", "covered-or-pointer-events"), ("#transparent", "not-visible"),
                                 ("#offscreen", "requires-scroll")]:
            target = call("page_locate", {"selector": selector})["ref"]
            preview = call("page_preview", {"ref": target, "action": "click"})
            assert not preview["executed"] and not preview["readyNow"] and reason in preview["blockedBy"]
        assert text("#scroll") == "0" and text("#clicks") == "0" and text("#focuses") == "0"
        for selector in ["#covered", "#transparent"]:
            target = call("page_locate", {"selector": selector})["ref"]
            rejected = call("page_click", {"ref": target}, "ELEMENT_NOT_ACTIONABLE")
            assert rejected["error"]["outcomeUnknown"] is False
        call("page_click", {"ref": ref})
        assert text("#clicks") == "1"
        report["checks"].append("read-only-preview-no-click-focus-or-scroll-and-recheck-before-dispatch")

        for selector in ["#readonly", "#hidden", "#file", "#fieldset", "#inert", "#opacity", "#display"]:
            info = call("page_inspect", {"selector": selector})
            assert not info["fillReady"]
            assert not call("page_preview", {"ref": info["ref"], "action": "fill"})["readyNow"]
            rejected = call("page_fill", {"ref": info["ref"], "value": "must-not-write"}, "ELEMENT_NOT_EDITABLE")
            assert rejected["error"]["outcomeUnknown"] is False
        assert text("#input-events") == "0:0"
        filled = call("page_fill", {"ref": facts["ref"], "value": "secret-canary-filled"})
        assert filled["filled"] and filled["valueMatchesRequested"] and text("#input-events") == "1:1"
        report["checks"].append("hidden-transparent-disabled-fieldset-inert-readonly-and-file-blocked-before-fill")

        single = call("page_inspect", {"selector": "#single"})
        assert single["selectReady"] and not single["multiple"] and single["optionCount"] == 8
        assert call("page_preview", {"ref": single["ref"], "action": "select"})["readyNow"]
        selected = call("page_select", {"ref": single["ref"], "values": ["cn"]})
        assert selected["selectedCount"] == 1 and text("#single-value") == "cn" and text("#select-events") == "1:1"
        for values, error in [(["missing"], "OPTION_NOT_FOUND"), (["dupe"], "OPTION_AMBIGUOUS"),
                              (["bad"], "OPTION_DISABLED"), (["locked"], "OPTION_DISABLED"),
                              ([], "SELECTION_COUNT_INVALID"), (["us", "cn"], "SELECTION_COUNT_INVALID"),
                              (["cn", "cn"], "INVALID_ARGUMENTS")]:
            rejected = call("page_select", {"ref": single["ref"], "values": values}, error)
            assert rejected["error"]["outcomeUnknown"] is False
        assert text("#single-value") == "cn" and text("#select-events") == "1:1"
        multiple = call("page_inspect", {"selector": "#multiple"})
        assert multiple["multiple"] and multiple["selectReady"]
        assert call("page_select", {"ref": multiple["ref"], "values": ["c", "a"]})["selectedCount"] == 2
        assert text("#multiple-value") == "a,c"
        call("page_select", {"ref": multiple["ref"], "values": ["b", "missing"]}, "OPTION_NOT_FOUND")
        assert text("#multiple-value") == "a,c" and text("#select-events") == "2:2"
        call("page_select", {"ref": multiple["ref"], "values": ["a"] * 33}, "INVALID_ARGUMENTS")
        call("page_preview", {"ref": multiple["ref"], "action": "delete"}, "INVALID_ARGUMENTS")
        assert call("page_select", {"ref": multiple["ref"], "values": []})["selectedCount"] == 0
        assert text("#multiple-value") == "" and text("#select-events") == "3:3"
        disabled = call("page_locate", {"selector": "#disabled-select"})["ref"]
        call("page_select", {"ref": disabled, "values": ["x"]}, "ELEMENT_NOT_SELECTABLE")
        assert call("page_select", {"ref": single["ref"], "values": ["secret-canary-selection"]})["selected"]
        assert "secret-canary-" not in json.dumps(call("page_inspect", {"selector": "#single"}))
        report["checks"].append("single-multiple-empty-and-secret-selection-exact-events-negative-cases-atomic")

        reactive = call("page_locate", {"selector": "#reactive"})["ref"]
        unknown = call("page_select", {"ref": reactive, "values": ["b"]}, "ACTION_POSTCONDITION_FAILED")
        assert unknown["error"]["outcomeUnknown"] is True and text("#reactive-events") == "1"
        assert text("#reactive-events") == "1"
        normalized = call("page_locate", {"selector": "#normalize"})["ref"]
        unknown = call("page_fill", {"ref": normalized, "value": "lower"}, "ACTION_POSTCONDITION_FAILED")
        assert unknown["error"]["outcomeUnknown"] is True and text("#normalized-events") == "1"
        report["checks"].append("reactive-selection-and-fill-postcondition-failure-unknown-no-event-retry")

        offscreen = call("page_locate", {"selector": "#offscreen"})["ref"]
        call("page_click", {"ref": offscreen})
        assert text("#clicks") == "2" and int(text("#scroll")) > 0
        old = facts["ref"]
        remove = call("page_locate", {"selector": "#remove"})["ref"]
        call("page_click", {"ref": remove})
        call("page_preview", {"ref": old, "action": "fill"}, "STALE_ELEMENT_REFERENCE")
        call("browser_navigate", {"url": origin + "/inspect"})
        call("page_preview", {"ref": single["ref"], "action": "select"}, "STALE_ELEMENT_REFERENCE")
        current = call("page_inspect", {"selector": "#single", "highlight": True})["ref"]
        call("page_open", {"url": origin + "/inspect"})
        call("page_preview", {"ref": current, "action": "select"}, "STALE_ELEMENT_REFERENCE")
        current = call("page_inspect", {"selector": "#single"})["ref"]
        other = client.call("profile_create", {"name": "跨环境拒绝", "allowedOrigins": [origin]})
        client.call("browser_start", {"profileId": other["id"], "headless": True})
        other_lease = client.call("lease_acquire", {"profileId": other["id"], "owner": "other", "ttlMs": 300000})
        client.call("page_preview", {"profileId": other["id"], "lease": other_lease,
                                     "ref": current, "action": "select"}, "STALE_ELEMENT_REFERENCE")
        client.call("browser_stop", {"profileId": other["id"]})
        client.call("lease_revoke", {"profileId": profile["id"]})
        for command, extra in [("page_inspect", {"selector": "#single"}), ("page_preview", {"ref": current, "action": "select"}),
                               ("page_clear_highlight", {}), ("page_select", {"ref": current, "values": ["cn"]})]:
            call(command, extra, "LEASE_REVOKED")
        client.call("browser_stop", {"profileId": profile["id"]})
        report["checks"].append("removed-navigation-page-switch-ref-invalidation-and-lease-fencing")
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
    finally:
        client.close()
        server.shutdown()
        server.server_close()
        report["commands"] = client.events
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "error": report.get("error")}))
    raise SystemExit(0 if report["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
