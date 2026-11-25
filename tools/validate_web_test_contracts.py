"""离线校验Web测试设计数据，绝不执行工作流或读取秘密。日期：2026-11-04。"""

import argparse
import ipaddress
import json
import math
import re
from pathlib import Path
from urllib.parse import unquote, urljoin, urlsplit

from jsonschema import Draft7Validator

ROOT = Path(__file__).resolve().parent.parent
CONTRACTS = ROOT / "docs/contracts/web-testing-v1"


# 用途：拒绝重复键，避免同一配置被不同解析器解释为不同内容；日期：2026-11-04。
def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("DUPLICATE_JSON_KEY")
        result[key] = value
    return result


# 用途：拒绝JSON标准之外的NaN和Infinity而不输出原输入；日期：2026-11-04。
def reject_constant(_value):
    raise ValueError("NON_FINITE_JSON_NUMBER")


# 用途：有界读取UTF-8设计文件，不访问网络或跟随外部Schema；日期：2026-11-04。
def read_document(path):
    with path.open("rb") as stream:
        raw = stream.read(1024 * 1024 + 1)
    if len(raw) > 1024 * 1024:
        raise ValueError("DOCUMENT_TOO_LARGE")
    value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object, parse_constant=reject_constant)
    pending = [(value, 0)]
    while pending:
        item, depth = pending.pop()
        if depth > 16:
            raise ValueError("DOCUMENT_TOO_DEEP")
        if isinstance(item, (dict, list)):
            pending.extend((child, depth + 1) for child in (item.values() if isinstance(item, dict) else item))
        elif isinstance(item, float) and not math.isfinite(item):
            raise ValueError("NON_FINITE_JSON_NUMBER")
    return value


# 用途：仅接受显式HTTP(S)规范来源，拒绝凭据/路径/查询和隐式端口变体；日期：2026-11-04。
def origin(value):
    if not isinstance(value, str) or any(ord(char) <= 32 or ord(char) == 127 for char in value) or "\\" in value:
        raise ValueError("INVALID_ORIGIN")
    try:
        parts = urlsplit(value)
        if parts.scheme not in ["http", "https"] or not parts.hostname or parts.username is not None or parts.password is not None:
            raise ValueError("INVALID_ORIGIN")
        if parts.path or parts.query or parts.fragment or "?" in value or "#" in value:
            raise ValueError("INVALID_ORIGIN")
        host = parts.hostname
        if ":" in host:
            host = "[" + str(ipaddress.IPv6Address(host)) + "]"
        else:
            host = host.encode("idna").decode("ascii").lower()
            if not re.fullmatch(r"[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?", host) or ".." in host:
                raise ValueError("INVALID_ORIGIN")
            if any(not label or len(label) > 63 or label.startswith("-") or label.endswith("-") for label in host.split(".")):
                raise ValueError("INVALID_ORIGIN")
        port = parts.port
        if port is not None and not 1 <= port <= 65535:
            raise ValueError("INVALID_ORIGIN")
        normalized = parts.scheme + "://" + host
        if port is not None and port != (443 if parts.scheme == "https" else 80):
            normalized += ":" + str(port)
        if normalized != value:
            raise ValueError("NON_CANONICAL_ORIGIN")
        return normalized
    except (UnicodeError, TypeError, ValueError):
        raise ValueError("INVALID_ORIGIN") from None


# 用途：只接受同源相对页面路径，拒绝反斜线、控制符和编码网络路径；日期：2026-11-04。
def page_url(base, page):
    decoded = unquote(page)
    if not page.startswith("/") or decoded.startswith("//") or "\\" in decoded or any(ord(char) < 32 or ord(char) == 127 for char in decoded):
        raise ValueError("INVALID_PAGE_PATH")
    result = urljoin(base + "/", page)
    parts = urlsplit(result)
    if parts.scheme + "://" + parts.netloc != base:
        raise ValueError("PAGE_ORIGIN_DENIED")
    return result


# 用途：验证结构和跨资源不变量，错误仅返回固定码及字段路径；日期：2026-11-04。
def validate_bundle(document):
    schema = read_document(CONTRACTS / "bundle.schema.json")
    Draft7Validator.check_schema(schema)
    errors = []

    # 用途：保存无输入值的稳定错误定位；日期：2026-11-04。
    def fail(code, path):
        errors.append({"code": code, "path": path})

    for error in sorted(Draft7Validator(schema).iter_errors(document), key=lambda item: str(list(item.absolute_path))):
        path = "/" + "/".join(str(item).replace("~", "~0").replace("/", "~1") for item in error.absolute_path)
        fail("SCHEMA_" + str(error.validator).upper(), path)
    if errors:
        return errors
    project = document["project"]
    project_id = project["id"]
    resources = {}
    for field, refs in [("environments", "environmentIds"), ("cases", "caseIds"), ("dataSets", "dataSetIds")]:
        items = document[field]
        resources[field] = {item["id"]: item for item in items}
        if len(resources[field]) != len(items):
            fail("DUPLICATE_RESOURCE_ID", "/" + field)
        if set(resources[field]) != set(project[refs]):
            fail("PROJECT_RESOURCE_SET_MISMATCH", "/project/" + refs)
        for index, item in enumerate(items):
            if item["projectId"] != project_id:
                fail("CROSS_PROJECT_REFERENCE", "/" + field + "/" + str(index) + "/projectId")
    if project["defaultEnvironmentId"] not in resources["environments"]:
        fail("DEFAULT_ENVIRONMENT_NOT_FOUND", "/project/defaultEnvironmentId")

    # 用途：秘密引用必须限定当前项目，校验过程不解析秘密；日期：2026-11-04。
    def check_secret(value, path):
        if "secretRef" in value and not value["secretRef"].startswith("secret://" + project_id + "/"):
            fail("SECRET_PROJECT_MISMATCH", path)

    for index, env in enumerate(document["environments"]):
        path = "/environments/" + str(index)
        try:
            base = origin(env["baseUrl"])
            allowed = {origin(value) for value in env["allowedOrigins"]}
            if base not in allowed:
                fail("BASE_ORIGIN_NOT_ALLOWED", path + "/allowedOrigins")
            page_url(base, env["defaultPage"])
        except ValueError as error:
            fail(str(error), path + "/baseUrl-or-defaultPage")
        requirements = env["resourceRequirements"]
        if requirements["parallelRuns"] > requirements["independentProfiles"]:
            fail("PARALLEL_REQUIRES_INDEPENDENT_PROFILES", path + "/resourceRequirements")
        for key, value in env["variables"].items():
            check_secret(value, path + "/variables/" + key)
    for index, dataset in enumerate(document["dataSets"]):
        path = "/dataSets/" + str(index)
        parameters = dataset["parameters"]
        rows = dataset["rows"]
        if len({row["id"] for row in rows}) != len(rows):
            fail("DUPLICATE_DATA_ROW", path + "/rows")
        for ordinal, row in enumerate(rows):
            row_path = path + "/rows/" + str(ordinal) + "/bindings"
            if set(row["bindings"]) != set(parameters):
                fail("DATA_BINDINGS_MISMATCH", row_path)
            for key, binding in row["bindings"].items():
                check_secret(binding, row_path + "/" + key)
                if key not in parameters:
                    continue
                definition = parameters[key]
                if definition["sensitive"] and ("secretRef" not in binding or definition["type"] != "string"):
                    fail("SENSITIVE_REQUIRES_STRING_SECRET_REF", row_path + "/" + key)
                if "value" in binding:
                    if not Draft7Validator({"type": definition["type"]}).is_valid(binding["value"]):
                        fail("DATA_VALUE_TYPE_MISMATCH", row_path + "/" + key)
                if "generator" in binding and definition["type"] != "string":
                    fail("GENERATOR_REQUIRES_STRING", row_path + "/" + key)
    for index, case in enumerate(document["cases"]):
        path = "/cases/" + str(index)
        if not set(case["environmentIds"]).issubset(resources["environments"]):
            fail("CASE_ENVIRONMENT_NOT_FOUND", path + "/environmentIds")
        if case["dataSetId"] not in resources["dataSets"]:
            fail("CASE_DATASET_NOT_FOUND", path + "/dataSetId")
        if case["roleId"] not in project["roleIds"]:
            fail("CASE_ROLE_NOT_FOUND", path + "/roleId")
        if case["scenarioId"] not in project["scenarioIds"]:
            fail("CASE_SCENARIO_NOT_DECLARED", path + "/scenarioId")
        auth = case["authentication"]
        if auth["identityRef"] is not None:
            check_secret(auth["identityRef"], path + "/authentication/identityRef")
        if auth["mode"] == "reuse-verified":
            if auth["identityRef"] is None or auth["verifyWorkflow"] is None:
                fail("REUSE_REQUIRES_IDENTITY_VERIFICATION", path + "/authentication")
        elif auth["mode"] == "new-identity" and (auth["identityRef"] is not None or auth["verifyWorkflow"] is not None):
            fail("NEW_IDENTITY_CANNOT_REUSE_AUTH", path + "/authentication")
        elif auth["mode"] == "login-each-case" and auth["identityRef"] is not None:
            fail("LOGIN_EACH_CASE_CANNOT_REUSE_STATE", path + "/authentication")
    return errors


# 用途：提供设计文件离线预检入口，失败不打印数据或秘密值；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser(description="Validate design contracts only; does not run browser tests")
    parser.add_argument("document", type=Path)
    args = parser.parse_args()
    try:
        errors = validate_bundle(read_document(args.document))
    except (OSError, UnicodeError, ValueError, RecursionError):
        errors = [{"code": "INVALID_DOCUMENT", "path": "/"}]
    print(json.dumps({"result": "failed" if errors else "passed", "errors": errors,
                      "scope": "design-contract-validation", "workflowsExecuted": False, "secretsResolved": False}))
    raise SystemExit(1 if errors else 0)


if __name__ == "__main__":
    main()
