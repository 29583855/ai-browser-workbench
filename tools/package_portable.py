"""组装可复制的VS2019测试包，保留上游字节并生成逐文件校验清单。日期：2026-11-04。"""

import argparse
import hashlib
import json
import re
import shutil
import struct
import zipfile
from pathlib import Path, PurePosixPath
from datetime import datetime


# 用途：按块计算SHA-256，避免浏览器大文件重复占用内存；日期：2026-11-04。
def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


# 用途：保持文件原字节并拒绝源目录中的重解析点；日期：2026-11-04。
def copy_tree(source, destination):
    if source.is_junction() or source.is_symlink():
        raise ValueError(f"Reparse source is forbidden: {source}")
    destination.mkdir(parents=True, exist_ok=True)
    for item in source.iterdir():
        if item.name == "__pycache__" or item.suffix == ".pyc":
            continue
        if item.is_symlink() or item.is_junction():
            raise ValueError(f"Reparse source is forbidden: {item}")
        if item.is_dir():
            copy_tree(item, destination / item.name)
        else:
            shutil.copyfile(item, destination / item.name)


# 用途：仅接受实际x64 PE程序和DLL，拒绝误打包其他架构；日期：2026-11-04。
def verify_pe(path):
    with path.open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise ValueError(f"Invalid PE executable: {path}")
        stream.seek(0x3C)
        offset = struct.unpack("<I", stream.read(4))[0]
        stream.seek(offset)
        if stream.read(4) != b"PE\0\0" or struct.unpack("<H", stream.read(2))[0] != 0x8664:
            raise ValueError(f"Expected x64 PE executable: {path}")


# 用途：写入UTF-8无BOM的结构化清单，不改变原有源文件；日期：2026-11-04。
def write_json(path, value):
    path.write_bytes((json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))


# 用途：固定输入源码及运行时，在全新目录组装Release便携包并压缩；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vs-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--browser-archive", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    version = re.search(r'Version\[\] = "([0-9.]+-poc)"', (root / "include/awb/version.hpp").read_text("utf-8"))
    if not version:
        raise ValueError("Source version is missing")
    # 用途：打包前核对当前操作/API文档版本，避免历史提示混入新交付；日期：2026-11-04。
    for name in ["13_执行记录与验收台账.md", "16_功能操作说明.md", "17_运行环境搭建配置.md",
                 "18_源码编译开发环境搭建.md", "20_本地HTTP_API参考.md"]:
        header = (root / "docs" / name).read_text("utf-8").splitlines()[2]
        if "版本：" + version[1].removesuffix("-poc") not in header:
            raise ValueError("Current document version differs from executable: " + name)
    output = args.output.resolve()
    archive = output.parent / (output.name + ".zip")
    if output.exists() or archive.exists():
        raise ValueError("Package output and ZIP must be new paths; existing data is never replaced.")
    if output == root or any(output.is_relative_to(root / folder) for folder in
                             ["src", "include", "third_party", "vs2019", "tools", "tests", "docs", "sdk"]):
        raise ValueError("Package output cannot be a source directory.")
    private_names = {"workbench.sqlite3", "credentials.dpapi", "Cookies", "Login Data", "local-api.json", ".owner.lock", "control-plane.sqlite3", "commercial.dpapi", "bootstrap.key", "admin-endpoint.json", "payment-config.json", ".admin.lock", "control-plane.sqlite3-wal", "control-plane.sqlite3-shm", "official-payments.json", "oidc-config.json", "server-config.json", "node-state.dpapi", "node-inbox.sqlite3", "node-inbox.sqlite3-wal", "node-inbox.sqlite3-shm", ".node.lock", "config.json"}
    for evidence_file in (root / "docs/execution/evidence").rglob("*"):
        if evidence_file.is_file() and evidence_file.name in private_names:
            raise ValueError("Archive private test runtime data outside documentation before packaging: " + str(evidence_file))
    runtime_metadata = root / "build/runtime/runtime-origin.json"
    if not runtime_metadata.is_file():
        runtime_metadata = root.parent / "browser/runtime-origin.json"
    runtime = json.loads(runtime_metadata.read_text("utf-8"))
    browser_archive = args.browser_archive or root / "build/runtime/chrome-win64.zip"
    if browser_archive.stat().st_size != runtime["bytes"] or digest(browser_archive) != runtime["sha256"]:
        raise ValueError("Browser archive provenance mismatch")
    redist = args.vs_root / "VC/Redist/MSVC/14.29.30133/x64/Microsoft.VC142.CRT"
    if not redist.is_dir():
        raise ValueError("Matching VC142 x64 redistributable is required")
    license_file = args.vs_root / "Licenses/2052/Redist.txt"
    if not license_file.is_file():
        raise ValueError("Visual Studio redistributable notice not found")
    lock = json.loads((root / "third_party/dependencies.lock.json").read_text("utf-8"))
    for dependency in lock["dependencies"]:
        for item in dependency["files"]:
            if digest(root / item["path"]) != item["sha256"]:
                raise ValueError("Dependency hash mismatch: " + item["path"])
    output.mkdir(parents=True)
    binaries = ["workbench.exe", "workbench-cli.exe", "workbench-mcp.exe", "awb-core-tests.exe", "awb-storage-tests.exe", "workbench-admin.exe", "workbench-node.exe"]
    for name in binaries:
        source = root / "build/native-vs2019/x64/Release" / name
        verify_pe(source)
        shutil.copyfile(source, output / name)
    for source in sorted(redist.glob("*.dll")):
        verify_pe(source)
        shutil.copyfile(source, output / source.name)
    with zipfile.ZipFile(browser_archive) as source:
        for item in source.infolist():
            path = PurePosixPath(item.filename)
            if path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] != "chrome-win64":
                raise ValueError("Unexpected browser archive entry")
        source.extractall(output / "browser")
    verify_pe(output / "browser/chrome-win64/chrome.exe")
    copy_tree(root / "tools/portable", output)
    source_root = output / "source"
    source_root.mkdir()
    for folder in ["src", "include", "third_party", "vs2019", "tools", "tests", "docs", "examples", "sdk"]:
        copy_tree(root / folder, source_root / folder)
    for name in ["AIBrowserWorkbench.sln", "Build-VS2019.cmd", "CMakeLists.txt", "README.md", "NOTICE.md", ".clang-format", ".gitattributes", ".gitignore"]:
        shutil.copyfile(root / name, source_root / name)
    (output / "文档入口.md").write_bytes((
        "# AI 浏览器工作台交付文档\n\n日期：2026-11-06。版本：" + version[1] + "。Windows x64 Release便携开发测试包。\n\n"
        "## 快速开始\n\n"
        "1. 首次运行先双击 `Self-Test.cmd`，确认自检通过。\n"
        "2. 双击 `Start-Workbench.cmd` 启动桌面；浏览器、VC运行库及数据目录请保持在本目录结构内。\n"
        "3. 需要组织、席位、账务或多节点管理时双击 `Start-Admin.cmd`。首次管理员初始化密钥位于 `data/admin/bootstrap.key`。\n\n"
        "普通本机运行无需安装Visual Studio、Python或Node.js。练习网站和开发脚本需要额外安装Python 3.12。\n\n"
        "## 当前版本说明\n\n"
        "本包用于Windows x64开发测试，不是完整商业发行版。官方支付和OIDC参数默认关闭；真实商户、企业IdP、TLS/mTLS证书及第二台物理机需要外部环境验收。\n\n"
        "## 当前手册\n\n"
        "- [功能说明](source/docs/47_软件功能说明.md)\n"
        "- [软件使用与运行环境配置](source/docs/48_软件使用与运行环境配置指南.md)\n"
        "- [源码编译环境搭建](source/docs/49_源码编译环境搭建指南.md)\n"
        "- [微信公众号宣传文案](source/docs/51_微信公众号宣传文案.md)\n"
        "- [GitHub项目首页源码](source/README.md)\n"
        "- [97工具生成手册](source/docs/contracts/TOOLS_V1.md)\n"
        "- [Local HTTP API参考](source/docs/20_本地HTTP_API参考.md)\n"
        "- [官方支付、企业登录与多机操作部署](source/docs/45_官方支付企业登录与多机自动化操作部署指南.md)\n"
        "- [实施测试与验收范围](source/docs/44_官方支付OIDC与多机调度实施验收.md)\n"
        "- [独立部署包复核记录](source/docs/46_集成版独立部署包复核记录.md)\n"
        "- [旧版本计划与任务历史](source/docs/50_项目README历史记录.md)\n\n"
        "测试人员请使用本包首次启动生成的私有data。不要把包含账号、Cookie、支付密钥或DPAPI身份的运行目录公开分发。\n\n"
        "## 历史测试记录\n\n"
        "- [0.1.11凭据、调试、模型、知识包及测试](source/docs/35_自动化基础能力补全与测试报告.md)\n"
        "- [0.1.12后台、支付、席位与计时操作](source/docs/42_后台支付席位与计时实现测试及操作指南.md)\n"
        "- [全部未完成项逐项收口清单](source/docs/36_未完成项逐项收口清单.md)\n"
        "- [官方支付、企业登录与多机操作部署](source/docs/45_官方支付企业登录与多机自动化操作部署指南.md)\n"
        "- [本轮实施验收](source/docs/44_官方支付OIDC与多机调度实施验收.md)\n"
        "- [独立部署包复核记录](source/docs/46_集成版独立部署包复核记录.md)\n"
        "- [97工具生成手册](source/docs/contracts/TOOLS_V1.md)\n"
        "- [本轮文件流程、数组输出及环境库测试](source/docs/38_未实现收口更新与回归验证.md)\n"
        "- [Python/TS SDK与登录示例](source/sdk/AGENT_API_V1.md)\n"
        "- [测试复盘、过程、结果与缺陷修复](source/docs/15_测试复盘与缺陷修复报告.md)\n"
        "- [0.1.2功能增量与测试报告](source/docs/19_功能增量与测试报告.md)\n"
        "- [0.1.3多页面、事件与显式重连](source/docs/21_多页面事件与显式重连测试报告.md)\n"
        "- [0.1.4元素探查、动作预览与表单操作](source/docs/22_元素探查与表单操作测试报告.md)\n"
        "- [0.1.5流程执行器与检查点](source/docs/23_流程执行器与检查点测试报告.md)\n"
        "- [0.1.6诊断收集与步骤关联](source/docs/24_诊断收集与步骤关联测试报告.md)\n"
        "- [0.1.7结构化提取与动作批处理](source/docs/25_结构化提取与动作批处理测试报告.md)\n"
        "- [Web测试项目、用例与环境设计契约](source/docs/26_Web测试项目用例与环境契约.md)\n"
        "- [0.1.8桌面界面美化与交互验证](source/docs/27_桌面界面美化与交互验证报告.md)\n"
        "- [可视化RPA逐项开发计划](source/docs/31_可视化录制拖拽编排与网站自动适配开发计划.md)\n"
        "- [0.1.9可视化实施、测试及错误修复](source/docs/32_可视化RPA实施与测试记录.md)\n"
        "- [录制、画布、发布回放与草稿恢复指南](source/docs/33_可视化录制与画布预览操作指南.md)\n"
        "- [0.1.10项目复盘、回归测试与修复](source/docs/34_项目功能复盘与回归测试报告.md)\n"
        "- [功能操作说明](source/docs/16_功能操作说明.md)\n"
        "- [运行环境搭建与配置](source/docs/17_运行环境搭建配置.md)\n"
        "- [源码编译与开发环境搭建](source/docs/18_源码编译开发环境搭建.md)\n"
        "- [本地HTTP API参考](source/docs/20_本地HTTP_API参考.md)\n"
    ).encode("utf-8"))
    (output / "portable.mode").write_bytes(b"AI_BROWSER_WORKBENCH_PORTABLE_V1\n")
    (output / "data").mkdir()
    (output / "licenses").mkdir()
    shutil.copyfile(license_file, output / "licenses/VisualStudio2019-Redist.txt")
    shutil.copyfile(root / "third_party/nlohmann/LICENSE.MIT", output / "licenses/nlohmann-LICENSE.MIT")
    shutil.copyfile(root / "third_party/sqlite/copyright.html", output / "licenses/sqlite-copyright.html")
    shutil.copyfile(root / "NOTICE.md", output / "licenses/PROJECT-NOTICE.md")
    write_json(output / "browser/runtime-origin.json", runtime)
    write_json(output / "mcp-config.example.json", {
        "instructions": "Replace PACKAGE_DIRECTORY with the absolute extracted folder, use forward slashes, and do not share the desktop data root.",
        "mcpServers": {"ai-browser-workbench": {
            "command": "PACKAGE_DIRECTORY/workbench-mcp.exe",
            "args": ["--root", "PACKAGE_DIRECTORY/data/mcp"],
        }},
    })
    manifest = {
        "format": 1, "date": datetime.now().astimezone().date().isoformat(), "version": version[1], "platform": "Windows x64",
        "minimumOs": "Windows 10 x64 (Chrome for Testing compatibility); Windows 11 x64 recommended",
        "compiler": "VS2019 v142 14.29.30133 / C++14", "configuration": "Release / MD",
        "mode": "portable development test package", "commercialRelease": False,
        "browserVersion": runtime["version"], "browserPurpose": "copied local development testing; not a commercial browser redistribution release",
        "needsVisualStudioOnTestMachine": False, "needsPythonNodeOnTestMachine": False,
        "mutableDirectories": ["data", "test-results"], "excludedFromManifest": ["package-manifest.json"],
        "files": [],
    }
    for item in sorted(output.rglob("*")):
        if item.is_file():
            manifest["files"].append({"path": item.relative_to(output).as_posix(), "bytes": item.stat().st_size, "sha256": digest(item)})
    write_json(output / "package-manifest.json", manifest)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as target:
        for item in sorted(output.rglob("*")):
            if item.is_file() or not any(item.iterdir()):
                target.write(item, item.relative_to(output.parent).as_posix())
    checksum = {"file": archive.name, "sha256": digest(archive), "bytes": archive.stat().st_size}
    write_json(archive.with_suffix(".zip.sha256.json"), checksum)
    print(json.dumps({"folder": str(output), "zip": str(archive), "files": len(manifest["files"]), **checksum}, ensure_ascii=False))


if __name__ == "__main__":
    main()
