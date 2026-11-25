"""仅回环可达的内容自动化练习网站，使用真实SQLite持久化。日期：2026-11-04。"""

import argparse
import hashlib
import hmac
import json
import secrets
import sqlite3
import threading
from datetime import datetime, timezone
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

HERE = Path(__file__).resolve().parent


# 用途：生成带独立盐值的演示帐号密码摘要；日期：2026-11-04。
def password_hash(password, salt):
    return hashlib.pbkdf2_hmac(
        "sha256", password.encode("utf-8"), salt.encode("ascii"), 100000
    ).hex()


class ContentServer(ThreadingHTTPServer):
    # 用途：建立仅本机监听的服务、数据库和一次性故障注入配置；日期：2026-11-04。
    def __init__(self, port, database):
        self.database = Path(database).resolve()
        self.database.parent.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.faults = {}
        self.initialize()
        super().__init__(("127.0.0.1", port), ContentHandler)
        self.origin = "http://127.0.0.1:" + str(self.server_port)

    # 用途：按请求建立启用外键约束的独立SQLite连接；日期：2026-11-04。
    def connect(self):
        connection = sqlite3.connect(self.database, timeout=10)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys=ON")
        return connection

    # 用途：创建练习数据并保持重启后的文章、评论及点赞状态；日期：2026-11-04。
    def initialize(self):
        with self.connect() as db:
            db.executescript(
                """
                CREATE TABLE IF NOT EXISTS users(name TEXT PRIMARY KEY, salt TEXT, hash TEXT, role TEXT);
                CREATE TABLE IF NOT EXISTS sessions(token TEXT PRIMARY KEY, username TEXT REFERENCES users(name), csrf TEXT);
                CREATE TABLE IF NOT EXISTS articles(id TEXT PRIMARY KEY, author TEXT REFERENCES users(name),
                    title TEXT, body TEXT, category TEXT, state TEXT, task_key TEXT, created TEXT,
                    UNIQUE(author, task_key));
                CREATE TABLE IF NOT EXISTS likes(article TEXT REFERENCES articles(id), username TEXT REFERENCES users(name),
                    PRIMARY KEY(article, username));
                CREATE TABLE IF NOT EXISTS comments(id TEXT PRIMARY KEY, article TEXT REFERENCES articles(id),
                    author TEXT REFERENCES users(name), parent TEXT REFERENCES comments(id), body TEXT, task_key TEXT,
                    UNIQUE(author, task_key));
                CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY, kind TEXT, object_id TEXT, username TEXT);
            """
            )
            for name, role in [("editor", "author"), ("demo-reader", "reader")]:
                salt = secrets.token_hex(16)
                db.execute(
                    "INSERT OR IGNORE INTO users VALUES(?,?,?,?)",
                    (name, salt, password_hash("ContentLab-2026!", salt), role),
                )
            for suffix, title in [
                ("a", "浏览器自动化：登录测试"),
                ("b", "浏览器自动化：表单回归"),
                ("c", "浏览器自动化：元素定位"),
                ("d", "普通产品公告"),
            ]:
                identity = "seed-" + suffix
                db.execute(
                    "INSERT OR IGNORE INTO articles VALUES(?,?,?,?,?,?,?,?)",
                    (
                        identity,
                        "editor",
                        title,
                        "这是本地测试文章，用于验证页面自动操作。",
                        "技术",
                        "published",
                        identity,
                        "2026-11-04T00:00:00+00:00",
                    ),
                )
                db.execute(
                    "INSERT OR IGNORE INTO comments VALUES(?,?,?,?,?,?)",
                    (
                        "comment-" + suffix,
                        identity,
                        "editor",
                        None,
                        "这是一条待回复的示例评论。",
                        identity,
                    ),
                )

    # 用途：测试程序在进程内配置单次拒绝、会话过期或提交后回执丢失，不向网页开放管理接口；日期：2026-11-04。
    def inject(self, path, behavior):
        with self.lock:
            self.faults[path] = behavior


class BusinessError(Exception):
    # 用途：携带固定业务错误码及HTTP状态；日期：2026-11-04。
    def __init__(self, code, status=400):
        super().__init__(code)
        self.code = code
        self.status = status


class ContentHandler(BaseHTTPRequestHandler):
    # 用途：避免服务日志泄露Cookie、输入正文及口令；日期：2026-11-04。
    def log_message(self, _format, *_args):
        return

    # 用途：发送禁缓存、禁嗅探及受限页面资源响应；日期：2026-11-04。
    def send_data(
        self, status, body, content_type="application/json; charset=utf-8", cookie=None
    ):
        if isinstance(body, dict):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        elif isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; object-src 'none'; frame-ancestors 'none'",
        )
        if cookie:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            return

    # 用途：拒绝非预期Host，避免本机测试服务被任意域名引用；日期：2026-11-04。
    def valid_host(self):
        return self.headers.get("Host") == "127.0.0.1:" + str(self.server.server_port)

    # 用途：只从HttpOnly会话Cookie读取有效帐号，不信任网页提交的作者字段；日期：2026-11-04。
    def session(self, db):
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get("Cookie", ""))
            token = (
                cookie["content_session"].value if "content_session" in cookie else ""
            )
        except (KeyError, ValueError):
            return None
        return db.execute(
            "SELECT s.*,u.role FROM sessions s JOIN users u ON u.name=s.username WHERE token=?",
            (token,),
        ).fetchone()

    # 用途：验证文本字段类型、空白及长度上限；日期：2026-11-04。
    def field(self, data, name, maximum, optional=False):
        value = data.get(name, "")
        if (
            not isinstance(value, str)
            or len(value) > maximum
            or (not optional and not value.strip())
        ):
            raise BusinessError("INVALID_" + name.upper())
        return value

    # 用途：读取仅页面可见的文章与本人互动状态；日期：2026-11-04。
    def article(self, db, identity, username):
        row = db.execute(
            "SELECT * FROM articles WHERE id=? AND (state='published' OR author=?)",
            (identity, username),
        ).fetchone()
        if not row:
            raise BusinessError("ARTICLE_NOT_FOUND", 404)
        result = dict(row)
        result["liked"] = bool(
            db.execute(
                "SELECT 1 FROM likes WHERE article=? AND username=?",
                (identity, username),
            ).fetchone()
        )
        result["comments"] = [
            dict(c)
            for c in db.execute(
                "SELECT * FROM comments WHERE article=? ORDER BY rowid", (identity,)
            )
        ]
        return result

    # 用途：提供页面资源、会话、搜索和文章详情，不提供测试专用写入后门；日期：2026-11-04。
    def do_GET(self):
        if not self.valid_host():
            self.send_data(403, {"error": "HOST_DENIED"})
            return
        target = urlsplit(self.path)
        resources = {
            "/": ("index.html", "text/html; charset=utf-8"),
            "/app.js": ("app.js", "text/javascript; charset=utf-8"),
            "/style.css": ("style.css", "text/css; charset=utf-8"),
        }
        if target.path in resources:
            name, content_type = resources[target.path]
            self.send_data(200, (HERE / name).read_bytes(), content_type)
            return
        try:
            with self.server.connect() as db:
                current = self.session(db)
                if target.path == "/api/session":
                    self.send_data(
                        200,
                        {
                            "username": current["username"] if current else "anonymous",
                            "role": current["role"] if current else "guest",
                            "csrf": current["csrf"] if current else "",
                        },
                    )
                    return
                if not current:
                    raise BusinessError("LOGIN_REQUIRED", 401)
                query = parse_qs(target.query)
                if target.path == "/api/search":
                    keyword = query.get("q", [""])[0][:120]
                    try:
                        page = max(1, min(20, int(query.get("page", ["1"])[0])))
                    except ValueError:
                        raise BusinessError("INVALID_PAGE")
                    rows = [
                        dict(r)
                        for r in db.execute(
                            "SELECT id,title,author,created FROM articles WHERE state='published' ORDER BY rowid"
                        )
                        if keyword.casefold() in r["title"].casefold()
                    ]
                    selected = rows[(page - 1) * 2 : page * 2]
                    self.send_data(
                        200,
                        {
                            "items": selected,
                            "page": page,
                            "total": len(rows),
                            "hasNext": page * 2 < len(rows),
                        },
                    )
                elif target.path == "/api/article":
                    self.send_data(
                        200,
                        self.article(db, query.get("id", [""])[0], current["username"]),
                    )
                else:
                    raise BusinessError("NOT_FOUND", 404)
        except BusinessError as error:
            self.send_data(error.status, {"error": error.code})

    # 用途：执行经过会话、来源、CSRF及对象关系校验的真实业务写入；日期：2026-11-04。
    def do_POST(self):
        try:
            if (
                not self.valid_host()
                or self.headers.get("Origin") != self.server.origin
            ):
                raise BusinessError("ORIGIN_DENIED", 403)
            if self.headers.get("Content-Type", "").split(";")[0] != "application/json":
                raise BusinessError("JSON_REQUIRED", 415)
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 131072:
                raise BusinessError("BODY_LIMIT", 413)
            data = json.loads(self.rfile.read(length))
            if not isinstance(data, dict):
                raise BusinessError("OBJECT_REQUIRED")
            path = urlsplit(self.path).path
            cookie = None
            with self.server.lock, self.server.connect() as db:
                fault = self.server.faults.pop(path, None)
                current = self.session(db)
                if fault == "rate-limit":
                    raise BusinessError("RATE_LIMITED", 429)
                if fault == "expire-session" and current:
                    db.execute(
                        "DELETE FROM sessions WHERE token=?", (current["token"],)
                    )
                    db.commit()
                    raise BusinessError("LOGIN_REQUIRED", 401)
                if path == "/api/register":
                    name = self.field(data, "username", 64)
                    password = self.field(data, "password", 128)
                    if len(password) < 8:
                        raise BusinessError("PASSWORD_TOO_SHORT")
                    if db.execute(
                        "SELECT 1 FROM users WHERE name=?", (name,)
                    ).fetchone():
                        raise BusinessError("DUPLICATE_USER", 409)
                    salt = secrets.token_hex(16)
                    db.execute(
                        "INSERT INTO users VALUES(?,?,?,?)",
                        (name, salt, password_hash(password, salt), "author"),
                    )
                    result = {"state": "registered"}
                elif path == "/api/login":
                    name = self.field(data, "username", 64)
                    password = self.field(data, "password", 128)
                    user = db.execute(
                        "SELECT * FROM users WHERE name=?", (name,)
                    ).fetchone()
                    if not user or not hmac.compare_digest(
                        user["hash"], password_hash(password, user["salt"])
                    ):
                        raise BusinessError("INVALID_CREDENTIALS", 401)
                    token, csrf = secrets.token_hex(32), secrets.token_hex(32)
                    db.execute(
                        "INSERT INTO sessions VALUES(?,?,?)", (token, name, csrf)
                    )
                    cookie = (
                        "content_session="
                        + token
                        + "; HttpOnly; SameSite=Strict; Path=/"
                    )
                    result = {
                        "state": "logged-in",
                        "username": name,
                        "role": user["role"],
                        "csrf": csrf,
                    }
                else:
                    if not current:
                        raise BusinessError("LOGIN_REQUIRED", 401)
                    if not hmac.compare_digest(
                        self.headers.get("X-CSRF-Token", ""), current["csrf"]
                    ):
                        raise BusinessError("CSRF_DENIED", 403)
                    username = current["username"]
                    if path == "/api/logout":
                        db.execute(
                            "DELETE FROM sessions WHERE token=?", (current["token"],)
                        )
                        cookie = "content_session=; Max-Age=0; HttpOnly; SameSite=Strict; Path=/"
                        result = {"state": "logged-out"}
                    elif path == "/api/publish":
                        if current["role"] != "author":
                            raise BusinessError("PUBLISH_FORBIDDEN", 403)
                        title = self.field(data, "title", 200)
                        body = self.field(data, "body", 20000)
                        category = self.field(data, "category", 32)
                        task_key = self.field(data, "taskKey", 80)
                        state = self.field(data, "state", 24)
                        if state not in (
                            "published",
                            "draft",
                            "pending-review",
                        ) or category not in ("技术", "产品动态"):
                            raise BusinessError("INVALID_PUBLICATION")
                        existing = db.execute(
                            "SELECT * FROM articles WHERE author=? AND task_key=?",
                            (username, task_key),
                        ).fetchone()
                        if existing:
                            if any(
                                existing[k] != v
                                for k, v in [
                                    ("title", title),
                                    ("body", body),
                                    ("category", category),
                                    ("state", state),
                                ]
                            ):
                                raise BusinessError("TASK_CONTENT_CONFLICT", 409)
                            identity = existing["id"]
                        else:
                            identity = "article-" + secrets.token_hex(6)
                            db.execute(
                                "INSERT INTO articles VALUES(?,?,?,?,?,?,?,?)",
                                (
                                    identity,
                                    username,
                                    title,
                                    body,
                                    category,
                                    state,
                                    task_key,
                                    datetime.now(timezone.utc).isoformat(),
                                ),
                            )
                            db.execute(
                                "INSERT INTO events(kind,object_id,username) VALUES('publish',?,?)",
                                (identity, username),
                            )
                        result = self.article(db, identity, username)
                    elif path == "/api/like":
                        identity = self.field(data, "articleId", 80)
                        article = self.article(db, identity, username)
                        if article["state"] != "published":
                            raise BusinessError("ARTICLE_NOT_PUBLIC", 409)
                        cursor = db.execute(
                            "INSERT OR IGNORE INTO likes VALUES(?,?)",
                            (identity, username),
                        )
                        if cursor.rowcount:
                            db.execute(
                                "INSERT INTO events(kind,object_id,username) VALUES('like',?,?)",
                                (identity, username),
                            )
                        result = {"state": "liked", "articleId": identity}
                    elif path == "/api/comment":
                        identity = self.field(data, "articleId", 80)
                        article = self.article(db, identity, username)
                        if article["state"] != "published":
                            raise BusinessError("ARTICLE_NOT_PUBLIC", 409)
                        body = self.field(data, "body", 1000)
                        key = self.field(data, "taskKey", 80)
                        parent = self.field(data, "parentId", 80, True) or None
                        if (
                            parent
                            and not db.execute(
                                "SELECT 1 FROM comments WHERE id=? AND article=?",
                                (parent, identity),
                            ).fetchone()
                        ):
                            raise BusinessError("PARENT_MISMATCH", 409)
                        existing = db.execute(
                            "SELECT * FROM comments WHERE author=? AND task_key=?",
                            (username, key),
                        ).fetchone()
                        if existing and (
                            existing["article"],
                            existing["parent"],
                            existing["body"],
                        ) != (identity, parent, body):
                            raise BusinessError("TASK_CONTENT_CONFLICT", 409)
                        comment_id = (
                            existing["id"]
                            if existing
                            else "comment-" + secrets.token_hex(6)
                        )
                        if not existing:
                            db.execute(
                                "INSERT INTO comments VALUES(?,?,?,?,?,?)",
                                (comment_id, identity, username, parent, body, key),
                            )
                            db.execute(
                                "INSERT INTO events(kind,object_id,username) VALUES(?,?,?)",
                                (
                                    "reply" if parent else "comment",
                                    comment_id,
                                    username,
                                ),
                            )
                        result = {
                            "id": comment_id,
                            "articleId": identity,
                            "parentId": parent,
                            "author": username,
                            "body": body,
                        }
                    else:
                        raise BusinessError("NOT_FOUND", 404)
                db.commit()
            if fault == "lose-receipt":
                self.close_connection = True
                self.connection.close()
                return
            self.send_data(200, result, cookie=cookie)
        except BusinessError as error:
            self.send_data(error.status, {"error": error.code})
        except (ValueError, UnicodeDecodeError):
            self.send_data(400, {"error": "INVALID_JSON"})


# 用途：从命令行启动可重复练习的网站，退出时关闭监听；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser(description="本地内容自动化练习网站")
    parser.add_argument("--port", type=int, default=3010)
    parser.add_argument("--database", type=Path, default=HERE / "data/content.sqlite3")
    args = parser.parse_args()
    server = ContentServer(args.port, args.database)
    print("Content Lab: " + server.origin, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
