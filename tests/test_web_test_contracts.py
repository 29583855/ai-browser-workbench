"""验证设计契约的来源、跨资源、秘密与固定用例不变量。日期：2026-11-04。"""

import argparse
import copy
import hashlib
import importlib.metadata
import json
import subprocess
import sys
from datetime import datetime
from pathlib import Path

from jsonschema import Draft7Validator

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from validate_web_test_contracts import CONTRACTS, read_document, validate_bundle


# 用途：逐个设计反例检查固定错误码，并核对离线CLI不泄漏输入；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [],
              "jsonschemaVersion": importlib.metadata.version("jsonschema"), "scope": "T067-design-contracts",
              "browserCasesExecuted": 0, "runtimeIntegrationComplete": False}
    try:
        example = read_document(CONTRACTS / "example-bundle.json")
        assert validate_bundle(example) == []
        report["checks"].append("example-bundle-cross-resource-and-secret-ref-valid")
        for kind, value in [("project", example["project"]), ("environment", example["environments"][0]),
                            ("case", example["cases"][0]), ("dataset", example["dataSets"][0]), ("bundle", example)]:
            schema = read_document(CONTRACTS / (kind + ".schema.json"))
            Draft7Validator.check_schema(schema)
            Draft7Validator(schema).validate(value)
            assert '"$ref"' not in json.dumps(schema), "Schema must not resolve external resources"
            report["checks"].append("standalone-draft07-valid-" + kind)
        # 用途：修改独立副本，避免反例之间互相污染；日期：2026-11-04。
        def reject(name, path, value, code):
            document = copy.deepcopy(example)
            target = document
            for part in path[:-1]:
                target = target[part]
            target[path[-1]] = value
            errors = validate_bundle(document)
            assert any(error["code"] == code for error in errors), (name, errors)
            assert "contract-secret-canary" not in json.dumps(errors)
            report["checks"].append(name)

        rejects = [
            ("reject-unknown-execution-command", ["cases", 0, "executeShell"], "contract-secret-canary", "SCHEMA_ADDITIONALPROPERTIES"),
            ("reject-future-schema", ["schemaVersion"], "2.0", "SCHEMA_CONST"),
            ("reject-bool-revision", ["project", "revision"], True, "SCHEMA_TYPE"),
            ("reject-secret-in-public-binding", ["dataSets", 0, "rows", 0, "bindings", "password"], {"value": "contract-secret-canary"}, "SENSITIVE_REQUIRES_STRING_SECRET_REF"),
            ("reject-sensitive-generator", ["dataSets", 0, "rows", 0, "bindings", "password"], {"generator": "case-attempt-unique", "prefix": "secret"}, "SENSITIVE_REQUIRES_STRING_SECRET_REF"),
            ("reject-secret-from-another-project", ["dataSets", 0, "rows", 0, "bindings", "password"], {"secretRef": "secret://other-project/password"}, "SECRET_PROJECT_MISMATCH"),
            ("reject-cross-project-resource", ["cases", 0, "projectId"], "another-project", "CROSS_PROJECT_REFERENCE"),
            ("reject-orphan-project-ref", ["project", "caseIds"], ["missing"], "PROJECT_RESOURCE_SET_MISMATCH"),
            ("reject-missing-default-environment", ["project", "defaultEnvironmentId"], "missing", "DEFAULT_ENVIRONMENT_NOT_FOUND"),
            ("reject-missing-case-environment", ["cases", 0, "environmentIds"], ["missing"], "CASE_ENVIRONMENT_NOT_FOUND"),
            ("reject-missing-case-role", ["cases", 0, "roleId"], "missing", "CASE_ROLE_NOT_FOUND"),
            ("reject-missing-case-dataset", ["cases", 0, "dataSetId"], "missing", "CASE_DATASET_NOT_FOUND"),
            ("reject-undeclared-scenario", ["cases", 0, "scenarioId"], "SC02", "CASE_SCENARIO_NOT_DECLARED"),
            ("reject-duplicate-resource", ["environments"], example["environments"] * 2, "DUPLICATE_RESOURCE_ID"),
            ("reject-duplicate-data-row", ["dataSets", 0, "rows"], example["dataSets"][0]["rows"] * 2, "DUPLICATE_DATA_ROW"),
            ("reject-missing-parameter", ["dataSets", 0, "rows", 0, "bindings"], {"username": {"value": "public"}}, "DATA_BINDINGS_MISMATCH"),
            ("reject-wrong-parameter-type", ["dataSets", 0, "rows", 0, "bindings", "username"], {"value": False}, "DATA_VALUE_TYPE_MISMATCH"),
            ("reject-ambiguous-binding", ["dataSets", 0, "rows", 0, "bindings", "password"], {"value": "contract-secret-canary", "secretRef": "secret://sample-web/password"}, "SCHEMA_ONEOF"),
            ("reject-unverified-auth-reuse", ["cases", 0, "authentication", "mode"], "reuse-verified", "REUSE_REQUIRES_IDENTITY_VERIFICATION"),
            ("reject-new-identity-with-stored-state", ["cases", 0, "authentication", "identityRef"], {"secretRef": "secret://sample-web/state"}, "NEW_IDENTITY_CANNOT_REUSE_AUTH"),
            ("reject-write-retry", ["cases", 0, "execution", "writeRetry"], "always", "SCHEMA_CONST"),
            ("reject-unknown-auto-resume", ["cases", 0, "execution", "onUnknown"], "retry", "SCHEMA_CONST"),
            ("reject-cleanup-failure-ignore", ["cases", 0, "cleanup", "onFailure"], "ignore", "SCHEMA_CONST"),
            ("reject-cleanup-all-resources", ["cases", 0, "cleanup", "onlyCreatedResources"], False, "SCHEMA_CONST"),
            ("reject-excess-parallel-with-shared-identity", ["environments", 0, "resourceRequirements", "parallelRuns"], 2, "PARALLEL_REQUIRES_INDEPENDENT_PROFILES"),
            ("reject-implicit-payment", ["environments", 0, "resourceRequirements", "overCapacity"], "auto-buy", "SCHEMA_ENUM"),
            ("reject-base-not-allowed", ["environments", 0, "allowedOrigins"], ["https://example.test"], "BASE_ORIGIN_NOT_ALLOWED"),
        ]
        for args_row in rejects:
            reject(*args_row)
        for number, value in enumerate(["file:///C:/secret", "http://user:contract-secret-canary@localhost", "http://localhost/path",
                "http://localhost?x=1", "http://localhost#", "http://localhost:0", "http://localhost:65536", "http://LOCALHOST",
                "http://localhost:80", "http://localhost\\other", "http://localhost\n", "https://-invalid.test", "http://local..host"]):
            reject("reject-origin-" + str(number), ["environments", 0, "baseUrl"], value, "INVALID_ORIGIN")
        for number, value in enumerate(["https://other.test/login", "//other.test/login", "/%2fother.test", "/%5cother", "/%00"]):
            reject("reject-default-page-" + str(number), ["environments", 0, "defaultPage"], value, "INVALID_PAGE_PATH")
        for value in ["http://localhost:3000", "https://test.example.test", "http://[::1]:3000"]:
            document = copy.deepcopy(example)
            document["environments"][0].update(baseUrl=value, allowedOrigins=[value])
            assert validate_bundle(document) == []
            report["checks"].append("accept-origin-" + value)
        for name, text in [("duplicate", '{"schemaVersion":"1.0","schemaVersion":"2.0"}'),
                           ("non-finite", '{"value":NaN}'), ("overflow-number", '{"value":1e999}'),
                           ("deep", '[' * 18 + '0' + ']' * 18), ("oversized", ' ' * (1024 * 1024 + 1))]:
            path = args.output / (name + ".json")
            path.write_bytes(text.encode("utf-8"))
            result = subprocess.run([sys.executable, str(CONTRACTS.parents[2] / "tools/validate_web_test_contracts.py"), str(path)], capture_output=True, text=True)
            assert result.returncode == 1 and json.loads(result.stdout)["errors"] == [{"code": "INVALID_DOCUMENT", "path": "/"}], result.stderr
            report["checks"].append("cli-reject-" + name)
        catalog = read_document(CONTRACTS / "case-catalog.json")
        assert catalog["caseCount"] == 48 and catalog["baselineCaseCount"] == 24 and catalog["extensionCaseCount"] == 24
        assert [case["id"] for case in catalog["cases"]] == ["WT" + str(i).zfill(2) for i in range(1, 49)]
        for case in catalog["cases"]:
            assert all(case[key] for key in ["setup", "input", "expected", "counterexample", "cleanup", "implementationTasks"])
            assert case["executionStatus"] == "not-run" and not case["evidence"]
        report["checks"].append("48-fixed-cases-have-setup-counterexample-cleanup-and-no-false-execution")
        actions = read_document(CONTRACTS / "action-contract.json")
        assert len(actions["actions"]) == 12 and len(actions["scenarios"]) == 4
        assert all(action["retryWrites"] == "never" and action["onUnknown"] == "stop-and-reconcile" for action in actions["actions"])
        assert all(action["tools"] or action["status"] == "planned" for action in actions["actions"])
        report["checks"].append("12-action-contracts-and-four-scenarios-preserve-unsupported-boundaries")
        report["contractFiles"] = {path.relative_to(CONTRACTS).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                   for path in sorted(CONTRACTS.glob("*.json"))}
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
    (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "error": report.get("error")}))
    raise SystemExit(0 if report["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
