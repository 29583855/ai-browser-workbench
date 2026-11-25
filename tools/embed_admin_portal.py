"""把中文门户资源确定性嵌入原生后台，不依赖运行机的Node或Web服务器。日期：2026-11-05。"""
import argparse
from pathlib import Path


# 用途：按UTF8字节预算切分相邻C++原始字符串，兼容v142单字面量长度限制；日期：2026-11-05。
def literal(name, text):
    chunks = []
    current = ""
    for line in text.splitlines(keepends=True):
        if len((current + line).encode("utf-8")) > 8000 and current:
            chunks.append(current)
            current = ""
        current += line
    if current:
        chunks.append(current)
    parts = ["const char " + name + "[] =\n"]
    for index, chunk in enumerate(chunks):
        delimiter = "AWB" + str(index)
        assert ")" + delimiter + '"' not in chunk
        parts.append('    R"' + delimiter + '(' + chunk + ')' + delimiter + '"\n')
    return "".join(parts).rstrip("\n") + ";\n"


# 用途：生成或核验门户嵌入文件，保留HTML/JS源文件原编码；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    text = "#pragma once\nnamespace awb\n{\n// 门户资源由tools/embed_admin_portal.py生成；日期：2026-11-05。\n"
    for name, file in [("PortalHtml", "index.html"), ("PortalJs", "portal.js")]:
        text += literal(name, (root / "src/admin/web" / file).read_text("utf-8"))
    text += "} // namespace awb\n"
    target = root / "src/admin/portal.hpp"
    if args.check:
        assert target.read_bytes() == text.encode("utf-8"), "Regenerate embedded portal after editing web sources"
    else:
        target.write_bytes(text.encode("utf-8"))
    print("Embedded portal verified" if args.check else "Embedded portal generated")


if __name__ == "__main__":
    main()
