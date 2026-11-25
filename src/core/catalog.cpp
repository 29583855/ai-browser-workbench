#include "awb/catalog.hpp"
#include "awb/browser.hpp"
#include <bcrypt.h>
#include <chrono>
#include <limits>
#include <mutex>
#include <set>
#include <sqlite/sqlite3.h>
#include <thread>

namespace awb
{
namespace
{
constexpr int ApplicationId = 0x41574250;
constexpr int SchemaVersion = 7;
constexpr size_t MaximumProfiles = 100000;
const char *const Migrations[] = {
    R"SQL(CREATE TABLE catalog_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) STRICT;
INSERT INTO catalog_meta VALUES('mode','development-poc');
CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, sha256 TEXT NOT NULL) STRICT;
CREATE TABLE profiles(
id TEXT PRIMARY KEY CHECK(length(id)=32 AND id NOT GLOB '*[^0-9a-f]*'),
name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 128),
proxy_server TEXT NOT NULL DEFAULT '') STRICT;
CREATE TABLE profile_origins(
profile_id TEXT NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 31),
origin TEXT NOT NULL,
PRIMARY KEY(profile_id,ordinal), UNIQUE(profile_id,origin)) STRICT;)SQL",
    R"SQL(ALTER TABLE profiles ADD COLUMN revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>=1);
ALTER TABLE profiles ADD COLUMN created_at TEXT NOT NULL DEFAULT 'unknown-legacy';
ALTER TABLE profiles ADD COLUMN updated_at TEXT NOT NULL DEFAULT 'unknown-legacy';)SQL",
    R"SQL(ALTER TABLE profiles ADD COLUMN state TEXT NOT NULL DEFAULT 'active'
CHECK(state IN ('active','archived','trashed','purging'));
CREATE TABLE profile_references(
profile_id TEXT NOT NULL REFERENCES profiles(id) ON DELETE RESTRICT,
owner_id TEXT NOT NULL CHECK(length(owner_id) BETWEEN 1 AND 128),
PRIMARY KEY(profile_id,owner_id)) STRICT;)SQL",
    R"SQL(ALTER TABLE profiles ADD COLUMN runtime_json TEXT NOT NULL DEFAULT '{}';)SQL",
    R"SQL(CREATE TABLE profile_library(
id TEXT PRIMARY KEY CHECK(length(id)=32 AND id NOT GLOB '*[^0-9a-f]*'),
kind TEXT NOT NULL CHECK(kind IN ('group','template')),
name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 128),
revision INTEGER NOT NULL CHECK(revision>=1),
configuration_json TEXT NOT NULL,
updated_at TEXT NOT NULL) STRICT;
CREATE TABLE profile_group_members(
group_id TEXT NOT NULL REFERENCES profile_library(id) ON DELETE CASCADE,
profile_id TEXT NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
PRIMARY KEY(group_id,profile_id)) STRICT;)SQL",
    R"SQL(CREATE TABLE profile_template_versions(
template_id TEXT NOT NULL REFERENCES profile_library(id) ON DELETE CASCADE,
revision INTEGER NOT NULL CHECK(revision>=1),
name TEXT NOT NULL,
configuration_json TEXT NOT NULL,
created_at TEXT NOT NULL,
PRIMARY KEY(template_id,revision)) STRICT;
INSERT INTO profile_template_versions SELECT id,revision,name,configuration_json,updated_at
FROM profile_library WHERE kind='template';
CREATE TABLE profile_batches(
id TEXT PRIMARY KEY CHECK(length(id)=32 AND id NOT GLOB '*[^0-9a-f]*'),
request_hash TEXT NOT NULL CHECK(length(request_hash)=64),
record_json TEXT NOT NULL,
created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),
updated_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now'))) STRICT;)SQL",
    R"SQL(ALTER TABLE profile_batches ADD COLUMN archived INTEGER NOT NULL DEFAULT 0 CHECK(archived IN (0,1));
CREATE INDEX profile_batches_retention ON profile_batches(archived,created_at,id);)SQL"};
const char *const MigrationHashes[] = {"43526be70fad99767f0545df5408142bd9ba072af3d522cc5052f37a657bd2ee",
                                       "3ea255f26cbb16394ea8e7a418d34adcca1bc8d8f14a468d665b143f3436c5a0",
                                       "51c7eeddfc1e39269f99caf5e9f516216cca653b3ad9595dc926263bf257988f",
                                       "344e9ddbfc91c502c5f5f116df42f6f9d00e7e0341c34b0307d9e8358fb2cb20",
                                       "b4c43957fe04520996ea796c0ef1bf359a5efc20c51714ddc09e34918271aa8e",
                                       "2ce118b4604471b014bd8c5dcbec2bbc410f889bb01dbc52c5f7542a2543e7aa",
                                       "cd2ab8cc12240bf692beace3deeb971446c3a22c3152fbd29ebd60aaf62c6cf1"};

// 用途：对批次规范请求计算SHA-256以校验相同请求ID，日志不保存执行参数；日期：2026-11-05。
std::string BatchDigest(const Json &request)
{
    const auto bytes = request.dump();
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw Error("BATCH_HASH_FAILED", "Cannot initialize batch request hashing");
    unsigned char digest[32]{};
    const auto status =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(bytes.data())),
                   static_cast<ULONG>(bytes.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
        throw Error("BATCH_HASH_FAILED", "Cannot hash batch request");
    const char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

// 用途：将SQLite失败映射为无SQL和业务值泄漏的结构化错误；日期：2026-11-03。
void DatabaseError(int result, const char *operation, bool uncertain = false)
{
    const int primary = result & 255;
    const char *code = "STORAGE_ERROR";
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED)
        code = "STORAGE_BUSY";
    else if (primary == SQLITE_FULL)
        code = "STORAGE_FULL";
    else if (primary == SQLITE_READONLY)
        code = "STORAGE_READ_ONLY";
    else if (primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB)
        code = "STORAGE_CORRUPT";
    else if (primary == SQLITE_IOERR)
        code = "STORAGE_IO_ERROR";
    else if (primary == SQLITE_CONSTRAINT)
        code = "STORAGE_CONSTRAINT";
    else if (primary == SQLITE_CANTOPEN)
        code = "STORAGE_OPEN_FAILED";
    throw Error(code, std::string(operation) + " failed (SQLite " + std::to_string(result) + ")", uncertain);
}
// 用途：拒绝数据库及旁路日志的重解析点、目录和硬链接；日期：2026-11-03。
std::wstring DatabasePath(const std::wstring &input)
{
    const auto path = SafeAbsolute(input);
    for (const auto &suffix : {L"", L"-wal", L"-shm", L"-journal"})
    {
        const auto file = SafeAbsolute(path + suffix);
        const DWORD attributes = GetFileAttributesW(file.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw Error("STORAGE_OPEN_FAILED", "Cannot inspect database path");
            continue;
        }
        Handle handle(CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) || handle.get() == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handle.get(), &info) || info.nNumberOfLinks != 1)
            throw Error("PATH_INVALID", "Database paths must be ordinary files with one link");
    }
    return path;
}

struct Connection
{
    sqlite3 *db = nullptr;
    // 用途：仅使用本地文件和全互斥连接，限制单值/SQL预算并关闭扩展；日期：2026-11-03。
    Connection(const std::wstring &path, bool readonly)
    {
        const int result = sqlite3_open_v2(
            Utf8(DatabasePath(path)).c_str(), &db,
            (readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE) | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (result != SQLITE_OK)
        {
            sqlite3_close(db);
            db = nullptr;
            DatabaseError(result, "Open catalog");
        }
        sqlite3_extended_result_codes(db, 1);
        sqlite3_busy_timeout(db, 2000);
        sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 1024 * 1024);
        sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, 65536);
        sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
    }
    // 用途：释放连接，不在析构阶段隐藏抛出失败；日期：2026-11-03。
    ~Connection()
    {
        sqlite3_close(db);
    }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
};

struct Statement
{
    sqlite3_stmt *statement = nullptr;
    // 用途：预编译固定SQL，业务值一律通过绑定传入；日期：2026-11-03。
    Statement(sqlite3 *db, const char *sql)
    {
        const int result = sqlite3_prepare_v2(db, sql, -1, &statement, nullptr);
        if (result != SQLITE_OK)
            DatabaseError(result, "Prepare catalog statement");
    }
    // 用途：释放预编译语句及其持有锁；日期：2026-11-03。
    ~Statement()
    {
        sqlite3_finalize(statement);
    }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    // 用途：绑定UTF-8文本，不拼接SQL或依赖调用者缓冲区寿命；日期：2026-11-03。
    void Bind(int index, const std::string &value)
    {
        const int result = sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()),
                                             SQLITE_TRANSIENT);
        if (result != SQLITE_OK)
            DatabaseError(result, "Bind catalog value");
    }
    // 用途：执行一步并区分行、结束及错误，绝不吞掉写入失败；日期：2026-11-03。
    bool Row()
    {
        const int result = sqlite3_step(statement);
        if (result == SQLITE_ROW)
            return true;
        if (result != SQLITE_DONE)
            DatabaseError(result, "Execute catalog statement");
        return false;
    }
    // 用途：按实际字节读取字段，保留非法NUL供上层拒绝；日期：2026-11-03。
    std::string Text(int column)
    {
        const auto value = sqlite3_column_text(statement, column);
        const int bytes = sqlite3_column_bytes(statement, column);
        return value ? std::string(reinterpret_cast<const char *>(value), static_cast<size_t>(bytes)) : "";
    }
};

// 用途：执行固定DDL/PRAGMA，错误只暴露结构化码；日期：2026-11-03。
void Sql(sqlite3 *db, const char *sql)
{
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (result != SQLITE_OK)
        DatabaseError(result, "Catalog SQL");
}
// 用途：读取固定单整数查询，用于身份/版本/数量核验；日期：2026-11-03。
int Integer(sqlite3 *db, const char *sql)
{
    Statement query(db, sql);
    if (!query.Row())
        throw Error("STORAGE_CORRUPT", "Catalog query returned no value");
    return sqlite3_column_int(query.statement, 0);
}
// 用途：通过完整页校验及外键检查，损坏时禁止回退空索引；日期：2026-11-03。
void Integrity(sqlite3 *db)
{
    Statement check(db, "PRAGMA integrity_check");
    if (!check.Row() || check.Text(0) != "ok" || check.Row())
        throw Error("STORAGE_CORRUPT", "Catalog integrity check failed");
    Statement foreign_keys(db, "PRAGMA foreign_key_check");
    if (foreign_keys.Row())
        throw Error("STORAGE_CORRUPT", "Catalog contains broken profile references");
}
// 用途：拒绝其他应用、未来版本和被改写的迁移记录；日期：2026-11-03。
int Verify(sqlite3 *db)
{
    if (Integer(db, "PRAGMA application_id") != ApplicationId)
        throw Error("STORAGE_IDENTITY_INVALID", "File is not a Workbench catalog");
    const int version = Integer(db, "PRAGMA user_version");
    if (version < 1 || version > SchemaVersion)
        throw Error("STORAGE_VERSION_UNSUPPORTED", "Catalog schema version is unsupported");
    Statement mode(db, "SELECT value FROM catalog_meta WHERE key='mode'");
    if (!mode.Row() || mode.Text(0) != "development-poc")
        throw Error("STORAGE_IDENTITY_INVALID", "Catalog mode is unsupported");
    Statement history(db, "SELECT version,sha256 FROM schema_migrations ORDER BY version");
    int expected = 1;
    while (history.Row())
    {
        if (expected > version || sqlite3_column_int(history.statement, 0) != expected ||
            history.Text(1) != MigrationHashes[expected - 1])
            throw Error("STORAGE_MIGRATION_INVALID", "Catalog migration history was altered");
        ++expected;
    }
    if (expected != version + 1)
        throw Error("STORAGE_MIGRATION_INVALID", "Catalog migration history is incomplete");
    Integrity(db);
    return version;
}

struct Transaction
{
    sqlite3 *db;
    bool &healthy;
    // 用途：立即取得写事务，失败时不开始局部修改；日期：2026-11-03。
    Transaction(sqlite3 *connection, bool &health, bool write = true) : db(connection), healthy(health)
    {
        Sql(db, write ? "BEGIN IMMEDIATE" : "BEGIN");
    }
    // 用途：未提交路径无条件尝试回滚，失败后封闭连接后续操作；日期：2026-11-03。
    ~Transaction()
    {
        if (!sqlite3_get_autocommit(db) &&
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK)
            healthy = false;
    }
    // 用途：仅确认提交成功才返回，I/O提交异常要求重启核对；日期：2026-11-03。
    void Commit()
    {
        const int result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        if (result != SQLITE_OK)
        {
            const bool unknown = (result & 255) == SQLITE_IOERR;
            if (unknown)
                healthy = false;
            DatabaseError(result, "Commit catalog transaction", unknown);
        }
    }
};

// 用途：对配置执行与服务创建相同的边界校验并规范化；日期：2026-11-03。
Json Metadata(const Json &input)
{
    if (!input.is_object() || !input.contains("id") || !input.at("id").is_string() ||
        !input.contains("name") || !input.at("name").is_string() || !input.contains("allowedOrigins") ||
        !input.at("allowedOrigins").is_array())
        throw Error("PROFILE_MANIFEST_INVALID", "Profile metadata shape is invalid");
    for (auto it = input.begin(); it != input.end(); ++it)
        if (it.key() != "id" && it.key() != "name" && it.key() != "allowedOrigins" &&
            it.key() != "proxyServer" && it.key() != "runtimeSettings")
            throw Error("PROFILE_MANIFEST_INVALID", "Unknown profile configuration field");
    const auto id = input.at("id").get<std::string>();
    const auto name = input.at("name").get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos || name.empty() ||
        name.find('\0') != std::string::npos || UnicodeLength(name) > 128 ||
        input.at("allowedOrigins").empty() || input.at("allowedOrigins").size() > 32)
        throw Error("PROFILE_MANIFEST_INVALID", "Profile metadata exceeds allowed bounds");
    Json origins = Json::array();
    std::set<std::string> unique;
    for (const auto &value : input.at("allowedOrigins"))
    {
        if (!value.is_string())
            throw Error("PROFILE_MANIFEST_INVALID", "Profile origin must be a string");
        const auto text = value.get<std::string>();
        if (text.find('\0') != std::string::npos || UnicodeLength(text) > 8192)
            throw Error("PROFILE_MANIFEST_INVALID", "Profile origin exceeds allowed bounds");
        const auto origin = Browser::Origin(text);
        if (unique.insert(origin).second)
            origins.push_back(origin);
    }
    if (input.contains("proxyServer") && !input.at("proxyServer").is_string())
        throw Error("PROFILE_MANIFEST_INVALID", "Profile proxy must be a string");
    const auto proxy = input.value("proxyServer", "");
    if (proxy.find('\0') != std::string::npos || UnicodeLength(proxy) > 2048)
        throw Error("PROFILE_MANIFEST_INVALID", "Profile proxy exceeds allowed bounds");
    Browser::ValidateProxyServer(proxy);
    Json result{{"id", id}, {"name", name}, {"allowedOrigins", origins}};
    if (!proxy.empty())
        result["proxyServer"] = proxy;
    if (input.contains("runtimeSettings"))
    {
        Browser::ValidateRuntimeSettings(input.at("runtimeSettings"));
        if (!input.at("runtimeSettings").empty())
            result["runtimeSettings"] = input.at("runtimeSettings");
    }
    return result;
}
// 用途：写入一条已校验环境及规范化来源，事务由上层统一管理；日期：2026-11-03。
void Insert(sqlite3 *db, const Json &metadata)
{
    Statement profile(db, "INSERT INTO profiles(id,name,proxy_server,runtime_json,created_at,updated_at) "
                          "VALUES(?1,?2,?3,?4,strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
                          "strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
    profile.Bind(1, metadata.at("id"));
    profile.Bind(2, metadata.at("name"));
    profile.Bind(3, metadata.value("proxyServer", ""));
    profile.Bind(4, metadata.value("runtimeSettings", Json::object()).dump());
    profile.Row();
    int ordinal = 0;
    for (const auto &origin : metadata.at("allowedOrigins"))
    {
        Statement row(db, "INSERT INTO profile_origins VALUES(?1,?2,?3)");
        row.Bind(1, metadata.at("id"));
        const int result = sqlite3_bind_int(row.statement, 2, ordinal++);
        if (result != SQLITE_OK)
            DatabaseError(result, "Bind origin ordinal");
        row.Bind(3, origin);
        row.Row();
    }
}
// 用途：从关系表重建有界环境列表，同时拒绝非法站点配置；日期：2026-11-03。
Json ReadProfiles(sqlite3 *db, bool details = false, const std::string &id = "")
{
    Json result = Json::array();
    const auto version = Integer(db, "PRAGMA user_version");
    const bool current = version >= 3;
    const std::string query =
        version >= 4
            ? "SELECT id,name,proxy_server,revision,created_at,updated_at,state,runtime_json FROM profiles"
        : current ? "SELECT id,name,proxy_server,revision,created_at,updated_at,state FROM profiles"
                  : "SELECT id,name,proxy_server FROM profiles";
    Statement rows(db, (query + (id.empty() ? " ORDER BY id" : " WHERE id=?1")).c_str());
    if (!id.empty())
        rows.Bind(1, id);
    while (rows.Row())
    {
        if (result.size() >= MaximumProfiles)
            throw Error("STORAGE_BUDGET_EXCEEDED", "Catalog exceeds local safety budget");
        Json profile{{"id", rows.Text(0)}, {"name", rows.Text(1)}, {"allowedOrigins", Json::array()}};
        const auto proxy = rows.Text(2);
        if (!proxy.empty())
            profile["proxyServer"] = proxy;
        Statement origins(db, "SELECT origin FROM profile_origins WHERE profile_id=?1 ORDER BY ordinal");
        origins.Bind(1, rows.Text(0));
        while (origins.Row())
        {
            if (profile.at("allowedOrigins").size() >= 32)
                throw Error("PROFILE_MANIFEST_INVALID", "Profile has too many origins");
            profile["allowedOrigins"].push_back(origins.Text(0));
        }
        if (version >= 4)
            profile["runtimeSettings"] = Json::parse(rows.Text(7));
        profile = Metadata(profile);
        if (details && current)
        {
            profile["revision"] = sqlite3_column_int64(rows.statement, 3);
            profile["createdAt"] = rows.Text(4);
            profile["updatedAt"] = rows.Text(5);
            profile["state"] = rows.Text(6);
        }
        result.push_back(std::move(profile));
    }
    return result;
}
// 用途：独占创建目标文件，绝不覆盖已存在数据库；日期：2026-11-03。
void CreateNew(const std::wstring &path)
{
    DatabasePath(path);
    Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
        throw Error("STORAGE_DESTINATION_EXISTS", "Cannot exclusively create destination catalog");
}
// 用途：仅清理本次新建目标的数据库旁路文件，不递归删除目录；日期：2026-11-03。
void RemoveCreated(const std::wstring &path)
{
    for (const auto &suffix : {L"-wal", L"-shm", L"-journal", L""})
        DeleteFileW((path + suffix).c_str());
}
// 用途：通过备份API复制一致快照，忙碌等待有截止，失败清理新目标；日期：2026-11-03。
void CopyDatabase(sqlite3 *source, const std::wstring &input)
{
    const auto destination = DatabasePath(input);
    CreateNew(destination);
    try
    {
        {
            Connection target(destination, false);
            Sql(target.db, "PRAGMA synchronous=FULL");
            auto *backup = sqlite3_backup_init(target.db, "main", source, "main");
            if (!backup)
                DatabaseError(sqlite3_extended_errcode(target.db), "Initialize catalog backup");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            int result = SQLITE_OK;
            do
            {
                result = sqlite3_backup_step(backup, 64);
                if (result == SQLITE_BUSY || result == SQLITE_LOCKED)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while ((result == SQLITE_OK || result == SQLITE_BUSY || result == SQLITE_LOCKED) &&
                     std::chrono::steady_clock::now() < deadline);
            const int finished = sqlite3_backup_finish(backup);
            if (result != SQLITE_DONE)
                DatabaseError(result == SQLITE_OK ? SQLITE_BUSY : result, "Copy catalog backup");
            if (finished != SQLITE_OK)
                DatabaseError(finished, "Finish catalog backup");
            Sql(target.db, "PRAGMA journal_mode=DELETE");
            Verify(target.db);
            ReadProfiles(target.db);
        }
        Handle file(CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE || !FlushFileBuffers(file.get()))
            throw Error("STORAGE_IO_ERROR", "Cannot flush completed catalog snapshot");
    }
    catch (...)
    {
        RemoveCreated(destination);
        throw;
    }
}
} // namespace

struct Catalog::Impl
{
    std::unique_ptr<Connection> connection;
    std::mutex mutex;
    bool healthy = true;
    // 用途：调用者持有连接锁时核验故障封闭状态；日期：2026-11-03。
    void RequireHealthy() const
    {
        if (!healthy)
            throw Error("STORAGE_REOPEN_REQUIRED",
                        "Restart service to reconcile catalog after storage failure");
    }
};

// 用途：识别受管数据库，先备份旧版本再事务升级，绝不静默重建损坏库；日期：2026-11-03。
Catalog::Catalog(const std::wstring &input) : impl_(std::make_unique<Impl>())
{
    const auto path = DatabasePath(input);
    const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!exists)
        CreateNew(path);
    try
    {
        impl_->connection = std::make_unique<Connection>(path, false);
        auto *db = impl_->connection->db;
        if (sqlite3_db_readonly(db, "main") != 0)
            throw Error("STORAGE_READ_ONLY", "Managed catalog must be writable");
        Sql(db, "PRAGMA foreign_keys=ON");
        int version = exists ? Verify(db) : 0;
        if (version && version < SchemaVersion)
            CopyDatabase(db, path + L".before-v" + std::to_wstring(SchemaVersion) + L"." + Wide(RandomId()) +
                                 L".sqlite3");
        if (version < SchemaVersion)
        {
            Transaction transaction(db, impl_->healthy);
            for (; version < SchemaVersion; ++version)
            {
                Sql(db, Migrations[version]);
                Statement history(db, "INSERT INTO schema_migrations VALUES(?1,?2)");
                sqlite3_bind_int(history.statement, 1, version + 1);
                history.Bind(2, MigrationHashes[version]);
                history.Row();
                Sql(db, ("PRAGMA user_version=" + std::to_string(version + 1)).c_str());
            }
            Sql(db, "PRAGMA application_id=0x41574250");
            transaction.Commit();
        }
        {
            Statement wal(db, "PRAGMA journal_mode=WAL");
            if (!wal.Row() || wal.Text(0) != "wal")
                throw Error("STORAGE_WAL_UNAVAILABLE", "Catalog requires local WAL support");
        }
        Sql(db, "PRAGMA synchronous=FULL");
        Sql(db, "PRAGMA wal_autocheckpoint=1000");
        Verify(db);
        ReadProfiles(db);
    }
    catch (...)
    {
        impl_->connection.reset();
        if (!exists)
            RemoveCreated(path);
        throw;
    }
}
// 用途：默认销毁私有连接，SQLite负责关闭WAL及释放锁；日期：2026-11-03。
Catalog::~Catalog() = default;

// 用途：防止提交结果不明后继续使用可能过时的内存状态；日期：2026-11-03。
void Catalog::CheckHealthy()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
}
// 用途：在串行连接锁下读取持久状态；日期：2026-11-03。
Json Catalog::Profiles(bool details)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    Transaction transaction(impl_->connection->db, impl_->healthy, false);
    auto profiles = ReadProfiles(impl_->connection->db, details);
    transaction.Commit();
    return profiles;
}
// 用途：持锁读取单个已验证配置，ID不存在时不返回空对象；日期：2026-11-04。
Json Catalog::Profile(const std::string &id)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    Transaction transaction(impl_->connection->db, impl_->healthy, false);
    const auto profiles = ReadProfiles(impl_->connection->db, true, id);
    if (profiles.empty())
        throw Error("PROFILE_NOT_FOUND", "Profile does not exist");
    transaction.Commit();
    return profiles[0];
}
// 用途：在写事务内执行版本比较、状态转换和完整配置更新；日期：2026-11-04。
Json Catalog::ChangeProfile(const std::string &id, long long revision, const Json &patch,
                            const std::string &state)
{
    if (!patch.is_object() || revision < 1)
        throw Error("INVALID_ARGUMENTS", "Profile change requires an object and positive revision");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    auto *db = impl_->connection->db;
    Transaction transaction(db, impl_->healthy);
    auto rows = ReadProfiles(db, true, id);
    if (rows.empty())
        throw Error("PROFILE_NOT_FOUND", "Profile does not exist");
    auto current = rows[0];
    if (current.at("revision") != revision)
        throw Error("REVISION_CONFLICT", "Refresh the profile before applying changes");
    const auto before = current.at("state").get<std::string>();
    const auto after = state.empty() ? before : state;
    const bool editable = before == "active" || before == "archived";
    const bool transition = (state.empty() && editable) || (before == "active" && after == "archived") ||
                            (editable && after == "trashed") ||
                            ((before == "archived" || before == "trashed") && after == "active") ||
                            (before == "trashed" && after == "purging");
    if (!transition)
        throw Error("PROFILE_STATE_INVALID", "Profile lifecycle transition is not allowed");
    if (!state.empty() && !patch.empty())
        throw Error("INVALID_ARGUMENTS", "State transitions cannot also edit configuration");
    if (after != "active")
    {
        Statement references(db, "SELECT 1 FROM profile_references WHERE profile_id=?1 LIMIT 1");
        references.Bind(1, id);
        if (references.Row())
            throw Error("PROFILE_REFERENCED", "A saved resource still references this profile");
    }
    Json metadata{{"id", id},
                  {"name", current.at("name")},
                  {"allowedOrigins", current.at("allowedOrigins")},
                  {"proxyServer", current.value("proxyServer", "")},
                  {"runtimeSettings", current.value("runtimeSettings", Json::object())}};
    for (auto it = patch.begin(); it != patch.end(); ++it)
    {
        if (it.key() != "name" && it.key() != "allowedOrigins" && it.key() != "proxyServer" &&
            it.key() != "runtimeSettings")
            throw Error("INVALID_ARGUMENTS", "Profile field is immutable or unrecognized");
        metadata[it.key()] = it.value();
    }
    metadata = Metadata(metadata);
    Statement update(
        db, "UPDATE profiles SET name=?2,proxy_server=?3,state=?4,runtime_json=?5,revision=revision+1,"
            "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1");
    update.Bind(1, id);
    update.Bind(2, metadata.at("name"));
    update.Bind(3, metadata.value("proxyServer", ""));
    update.Bind(4, after);
    update.Bind(5, metadata.value("runtimeSettings", Json::object()).dump());
    update.Row();
    Statement erase(db, "DELETE FROM profile_origins WHERE profile_id=?1");
    erase.Bind(1, id);
    erase.Row();
    int ordinal = 0;
    for (const auto &origin : metadata.at("allowedOrigins"))
    {
        Statement row(db, "INSERT INTO profile_origins VALUES(?1,?2,?3)");
        row.Bind(1, id);
        sqlite3_bind_int(row.statement, 2, ordinal++);
        row.Bind(3, origin);
        row.Row();
    }
    const auto result = ReadProfiles(db, true, id)[0];
    transaction.Commit();
    return result;
}
// 用途：文件清理确认后才移除索引；外键和状态再次防止误删引用环境；日期：2026-11-04。
void Catalog::FinishPurge(const std::string &id, long long revision)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    auto *db = impl_->connection->db;
    Transaction transaction(db, impl_->healthy);
    const auto rows = ReadProfiles(db, true, id);
    if (rows.empty() || rows[0].at("revision") != revision || rows[0].at("state") != "purging")
        throw Error("REVISION_CONFLICT", "Purge state or revision has changed");
    Statement erase(db, "DELETE FROM profiles WHERE id=?1");
    erase.Bind(1, id);
    erase.Row();
    transaction.Commit();
}
// 用途：只有活动环境可新增持久引用，引用删除与生命周期操作串行；日期：2026-11-04。
void Catalog::SetProfileReference(const std::string &id, const std::string &owner, bool present)
{
    if (owner.empty() || owner.find('\0') != std::string::npos || UnicodeLength(owner) > 128)
        throw Error("INVALID_ARGUMENTS", "Invalid resource reference owner");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    auto *db = impl_->connection->db;
    Transaction transaction(db, impl_->healthy);
    const auto rows = ReadProfiles(db, true, id);
    if (rows.empty())
        throw Error("PROFILE_NOT_FOUND", "Referenced profile does not exist");
    if (present && rows[0].at("state") != "active")
        throw Error("PROFILE_STATE_INVALID", "Only active profiles can be referenced");
    Statement change(db, present ? "INSERT OR IGNORE INTO profile_references VALUES(?1,?2)"
                                 : "DELETE FROM profile_references WHERE profile_id=?1 AND owner_id=?2");
    change.Bind(1, id);
    change.Bind(2, owner);
    change.Row();
    transaction.Commit();
}
// 用途：验证后事务写入，任何来源写入失败都回滚整条环境；日期：2026-11-03。
void Catalog::InsertProfile(const Json &input)
{
    const auto metadata = Metadata(input);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    Transaction transaction(impl_->connection->db, impl_->healthy);
    if (Integer(impl_->connection->db, "SELECT count(*) FROM profiles") >= MaximumProfiles)
        throw Error("STORAGE_BUDGET_EXCEEDED", "Catalog exceeds local safety budget");
    Insert(impl_->connection->db, metadata);
    transaction.Commit();
}
// 用途：先校验整批配置再事务写入，重复身份或任意SQL失败回滚全批；日期：2026-11-05。
void Catalog::InsertProfiles(const Json &input)
{
    if (!input.is_array() || input.empty() || input.size() > 32)
        throw Error("INVALID_ARGUMENTS", "Import requires 1..32 profile configurations");
    Json rows = Json::array();
    std::set<std::string> ids;
    for (const auto &item : input)
    {
        const auto metadata = Metadata(item);
        if (!ids.insert(metadata.at("id")).second)
            throw Error("INVALID_ARGUMENTS", "Duplicate import identity");
        rows.push_back(metadata);
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    auto *db = impl_->connection->db;
    Transaction transaction(db, impl_->healthy);
    if (Integer(db, "SELECT count(*) FROM profiles") + rows.size() > MaximumProfiles)
        throw Error("STORAGE_BUDGET_EXCEEDED", "Catalog exceeds local safety budget");
    for (const auto &metadata : rows)
        Insert(db, metadata);
    transaction.Commit();
}
// 用途：复制配置仅接受白名单字段，复用环境规范化规则且不带本地身份；日期：2026-11-05。
Json Catalog::Configuration(const Json &input)
{
    if (!input.is_object() || input.contains("id"))
        throw Error("PROFILE_MANIFEST_INVALID", "Configuration must omit profile identity");
    auto metadata = input;
    metadata["id"] = "00000000000000000000000000000000";
    metadata = Metadata(metadata);
    metadata.erase("id");
    return metadata;
}
// 用途：事务管理分组成员与模板revision，过期写入和删除均拒绝；日期：2026-11-05。
Json Catalog::Library(const std::string &kind, const std::string &operation, const Json &arguments)
{
    if ((kind != "group" && kind != "template") ||
        (operation != "list" && operation != "create" && operation != "update" && operation != "delete" &&
         operation != "history" && operation != "rollback"))
        throw Error("INVALID_ARGUMENTS", "Unsupported local profile library operation");
    if ((operation == "history" || operation == "rollback") && kind != "template")
        throw Error("INVALID_ARGUMENTS", "Only templates have immutable configuration history");
    Json configuration = Json::object();
    if (kind == "template" && (operation == "create" || operation == "update"))
        configuration = Configuration(arguments.at("configuration"));
    if (operation == "create" || operation == "update")
    {
        const auto name = arguments.at("name").get<std::string>();
        if (name.empty() || name.find('\0') != std::string::npos || UnicodeLength(name) > 128)
            throw Error("INVALID_ARGUMENTS", "Library name must contain 1..128 characters");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    auto *db = impl_->connection->db;
    Transaction transaction(db, impl_->healthy, operation != "list" && operation != "history");
    const auto id = operation == "create" ? RandomId() : arguments.value("id", "");
    if (operation == "update" || operation == "delete" || operation == "rollback" || operation == "history")
    {
        Statement current(db, "SELECT revision FROM profile_library WHERE id=?1 AND kind=?2");
        current.Bind(1, id);
        current.Bind(2, kind);
        if (!current.Row())
            throw Error("LIBRARY_NOT_FOUND", "Profile library resource does not exist");
        if (operation != "history" &&
            sqlite3_column_int64(current.statement, 0) != arguments.at("expectedRevision").get<long long>())
            throw Error("REVISION_CONFLICT", "Refresh the library resource before changing it");
    }
    if (operation == "history")
    {
        Statement versions(db, "SELECT revision,name,configuration_json,created_at FROM "
                               "profile_template_versions WHERE template_id=?1 ORDER BY revision DESC");
        versions.Bind(1, id);
        Json items = Json::array();
        while (versions.Row())
            items.push_back({{"revision", sqlite3_column_int64(versions.statement, 0)},
                             {"name", versions.Text(1)},
                             {"configuration", Configuration(Json::parse(versions.Text(2)))},
                             {"createdAt", versions.Text(3)}});
        transaction.Commit();
        return {{"id", id}, {"items", items}, {"immutable", true}};
    }
    if (kind == "template" && (operation == "update" || operation == "rollback"))
    {
        Statement count(db, "SELECT count(*) FROM profile_template_versions WHERE template_id=?1");
        count.Bind(1, id);
        count.Row();
        if (sqlite3_column_int(count.statement, 0) >= 100)
            throw Error("STORAGE_BUDGET_EXCEEDED", "Template history exceeds 100 immutable revisions");
    }
    if (operation == "rollback")
    {
        Statement source(db, "SELECT name,configuration_json FROM profile_template_versions "
                             "WHERE template_id=?1 AND revision=?2");
        source.Bind(1, id);
        sqlite3_bind_int64(source.statement, 2, arguments.at("sourceRevision").get<long long>());
        if (!source.Row())
            throw Error("TEMPLATE_VERSION_NOT_FOUND", "Template revision is not retained");
        configuration = Configuration(Json::parse(source.Text(1)));
        Statement restore(db, "UPDATE profile_library SET name=?2,configuration_json=?3,"
                              "revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                              "WHERE id=?1 AND kind='template'");
        restore.Bind(1, id);
        restore.Bind(2, source.Text(0));
        restore.Bind(3, configuration.dump());
        restore.Row();
    }
    if (operation == "create" || operation == "update")
    {
        if (operation == "create" && Integer(db, "SELECT count(*) FROM profile_library") >= 1000)
            throw Error("STORAGE_BUDGET_EXCEEDED", "Local profile library exceeds 1000 resources");
        Statement put(
            db, operation == "create"
                    ? "INSERT INTO profile_library VALUES(?1,?2,?3,1,?4,strftime('%Y-%m-%dT%H:%M:%fZ','now'))"
                    : "UPDATE profile_library SET name=?3,revision=revision+1,configuration_json=?4,"
                      "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1 AND kind=?2");
        put.Bind(1, id);
        put.Bind(2, kind);
        put.Bind(3, arguments.at("name"));
        put.Bind(4, configuration.dump());
        put.Row();
        if (kind == "group")
        {
            Statement erase(db, "DELETE FROM profile_group_members WHERE group_id=?1");
            erase.Bind(1, id);
            erase.Row();
            const auto &members = arguments.at("profileIds");
            if (!members.is_array() || members.size() > 100)
                throw Error("INVALID_ARGUMENTS", "Groups accept at most 100 local profiles");
            std::set<std::string> unique;
            for (const auto &member : members)
            {
                if (!member.is_string() || !unique.insert(member.get<std::string>()).second)
                    throw Error("INVALID_ARGUMENTS", "Group members must be unique local profile IDs");
                const auto profiles = ReadProfiles(db, true, member);
                if (profiles.empty() || profiles[0].at("state") == "purging")
                    throw Error("PROFILE_NOT_FOUND", "Group member is missing or being removed");
                Statement add(db, "INSERT INTO profile_group_members VALUES(?1,?2)");
                add.Bind(1, id);
                add.Bind(2, member);
                add.Row();
            }
        }
    }
    if (operation == "delete")
    {
        Statement erase(db, "DELETE FROM profile_library WHERE id=?1 AND kind=?2");
        erase.Bind(1, id);
        erase.Bind(2, kind);
        erase.Row();
        transaction.Commit();
        return {{"deleted", true}, {"id", id}, {"profilesDeleted", false}};
    }
    if (kind == "template" && operation != "list")
    {
        Statement save(db, "INSERT INTO profile_template_versions SELECT "
                           "id,revision,name,configuration_json,updated_at FROM profile_library WHERE id=?1");
        save.Bind(1, id);
        save.Row();
    }
    Statement rows(db, operation == "list" ? "SELECT id,name,revision,configuration_json,updated_at FROM "
                                             "profile_library WHERE kind=?1 ORDER BY id"
                                           : "SELECT id,name,revision,configuration_json,updated_at FROM "
                                             "profile_library WHERE kind=?1 AND id=?2");
    rows.Bind(1, kind);
    if (operation != "list")
        rows.Bind(2, id);
    Json items = Json::array();
    while (rows.Row())
    {
        Json item{{"id", rows.Text(0)},
                  {"name", rows.Text(1)},
                  {"revision", sqlite3_column_int64(rows.statement, 2)},
                  {"updatedAt", rows.Text(4)}};
        if (kind == "template")
            item["configuration"] = Configuration(Json::parse(rows.Text(3)));
        else
        {
            item["profileIds"] = Json::array();
            Statement members(
                db, "SELECT profile_id FROM profile_group_members WHERE group_id=?1 ORDER BY profile_id");
            members.Bind(1, rows.Text(0));
            while (members.Row())
                item["profileIds"].push_back(members.Text(0));
        }
        items.push_back(item);
    }
    transaction.Commit();
    return operation == "list" ? Json{{"items", items}, {"scope", "local-development-root"}} : items.at(0);
}
// 用途：统一计算批次各项状态，恢复与归档不丢失已确认结果的计数；日期：2026-11-05。
static void BatchCounts(Json &record)
{
    size_t succeeded = 0, failed = 0, skipped = 0;
    for (const auto &item : record.at("items"))
    {
        succeeded += item.at("state") == "succeeded";
        failed += item.at("state") == "failed" || item.at("state") == "outcome-unknown";
        skipped += item.at("state") == "not-run";
    }
    record["succeeded"] = succeeded;
    record["failed"] = failed;
    record["notRun"] = skipped;
}
// 用途：把每项派发前后状态写入事务，分页归档保留去重摘要且禁止未知结果清理；日期：2026-11-05。
Json Catalog::Batches(const std::string &operation, const Json &arguments)
{
    const std::set<std::string> supported{"begin", "dispatch", "complete", "finish",
                                          "get",   "list",     "recover",  "archive"};
    if (!supported.count(operation))
        throw Error("INVALID_ARGUMENTS", "Unsupported batch journal operation");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    auto *db = impl_->connection->db;
    Transaction transaction(db, impl_->healthy, operation != "get" && operation != "list");
    if (operation == "list")
    {
        const auto scope = arguments.value("scope", "active");
        const auto limit_value = arguments.value("limit", Json(100));
        if ((scope != "active" && scope != "archived" && scope != "all") ||
            !limit_value.is_number_integer() || limit_value < 1 || limit_value > 100)
            throw Error("INVALID_ARGUMENTS", "Batch list scope or page limit is invalid");
        sqlite3_int64 before = (std::numeric_limits<sqlite3_int64>::max)();
        if (arguments.contains("before"))
        {
            Statement cursor(db, "SELECT rowid FROM profile_batches WHERE id=?1");
            cursor.Bind(1, arguments.at("before"));
            if (!cursor.Row())
                throw Error("BATCH_CURSOR_INVALID", "Batch cursor does not identify a retained receipt");
            before = sqlite3_column_int64(cursor.statement, 0);
        }
        const int limit = limit_value.get<int>();
        Statement rows(db, "SELECT id,record_json,created_at,updated_at,archived FROM profile_batches "
                           "WHERE (?1=-1 OR archived=?1) AND rowid<?2 ORDER BY rowid DESC LIMIT ?3");
        sqlite3_bind_int(rows.statement, 1, scope == "all" ? -1 : (scope == "archived" ? 1 : 0));
        sqlite3_bind_int64(rows.statement, 2, before);
        sqlite3_bind_int(rows.statement, 3, limit + 1);
        Json items = Json::array();
        bool more = false;
        while (rows.Row())
        {
            if (items.size() == static_cast<size_t>(limit))
            {
                more = true;
                break;
            }
            auto record = Json::parse(rows.Text(1));
            record["createdAt"] = rows.Text(2);
            record["updatedAt"] = rows.Text(3);
            record["archived"] = sqlite3_column_int(rows.statement, 4) != 0;
            items.push_back(record);
        }
        const Json cursor = more ? items.back().at("batchId") : Json(nullptr);
        const int active = Integer(db, "SELECT count(*) FROM profile_batches WHERE archived=0");
        const int archived = Integer(db, "SELECT count(*) FROM profile_batches WHERE archived=1");
        transaction.Commit();
        return {{"items", items},
                {"limit", limit},
                {"scope", scope},
                {"hasMore", more},
                {"nextCursor", cursor},
                {"activeCount", active},
                {"archivedCount", archived},
                {"automaticRetryAllowed", false}};
    }
    if (operation == "recover")
    {
        Json unfinished = Json::array();
        {
            Statement rows(db, "SELECT record_json FROM profile_batches WHERE archived=0");
            while (rows.Row())
            {
                auto record = Json::parse(rows.Text(0));
                if (!record.at("finished").get<bool>())
                    unfinished.push_back(record);
            }
        }
        for (auto &record : unfinished)
        {
            bool unknown = false;
            for (auto &item : record.at("items"))
            {
                if (item.at("state") == "dispatching")
                    item["state"] = "outcome-unknown";
                else if (item.at("state") == "pending")
                    item["state"] = "not-run";
                unknown = unknown || item.at("state") == "outcome-unknown";
            }
            BatchCounts(record);
            record["state"] = unknown ? "outcome-unknown" : "interrupted";
            record["finished"] = true;
            record["recoveredAfterRestart"] = true;
            Statement save(db, "UPDATE profile_batches SET record_json=?2,"
                               "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1");
            save.Bind(1, record.at("batchId"));
            save.Bind(2, record.dump());
            save.Row();
        }
        transaction.Commit();
        return {{"recovered", unfinished.size()}, {"automaticRetryAllowed", false}};
    }
    const auto id = arguments.at("id").get<std::string>();
    Statement current(
        db,
        "SELECT request_hash,record_json,created_at,updated_at,archived FROM profile_batches WHERE id=?1");
    current.Bind(1, id);
    const bool exists = current.Row();
    if (operation == "begin")
    {
        const auto fingerprint = BatchDigest(
            Json{{"operations", arguments.at("operations")}, {"onError", arguments.at("onError")}});
        if (exists)
        {
            if (current.Text(0) != fingerprint)
                throw Error("BATCH_ID_CONFLICT", "Request ID already belongs to a different batch");
            auto record = Json::parse(current.Text(1));
            record["archived"] = sqlite3_column_int(current.statement, 4) != 0;
            transaction.Commit();
            return {{"created", false}, {"batch", record}};
        }
        if (Integer(db, "SELECT count(*) FROM profile_batches WHERE archived=0") >= 1000 ||
            Integer(db, "SELECT count(*) FROM profile_batches") >= 100000)
            throw Error("STORAGE_BUDGET_EXCEEDED",
                        "Batch journal exceeds 1000 active or 100000 lifetime receipts");
        Json record{{"batchId", id},
                    {"state", "running"},
                    {"finished", false},
                    {"items", Json::array()},
                    {"automaticRetryAllowed", false},
                    {"atomic", false},
                    {"rollbackPerformed", false},
                    {"onError", arguments.at("onError")}};
        for (const auto &item : arguments.at("operations"))
            record["items"].push_back({{"index", record["items"].size()},
                                       {"command", item.at("command")},
                                       {"profileId", item.at("arguments").at("profileId")},
                                       {"state", "pending"}});
        Statement put(db, "INSERT INTO profile_batches(id,request_hash,record_json) VALUES(?1,?2,?3)");
        put.Bind(1, id);
        put.Bind(2, fingerprint);
        put.Bind(3, record.dump());
        put.Row();
        transaction.Commit();
        return {{"created", true}, {"batch", record}};
    }
    if (!exists)
        throw Error("BATCH_NOT_FOUND", "Batch journal entry does not exist");
    auto record = Json::parse(current.Text(1));
    if (operation == "get")
    {
        record["createdAt"] = current.Text(2);
        record["updatedAt"] = current.Text(3);
        record["archived"] = sqlite3_column_int(current.statement, 4) != 0;
        transaction.Commit();
        return record;
    }
    if (operation == "archive")
    {
        if (arguments.value("confirmBatchId", "") != id)
            throw Error("BATCH_CONFIRMATION_REQUIRED", "Repeat the exact batch ID before compacting details");
        if (sqlite3_column_int(current.statement, 4))
        {
            transaction.Commit();
            return record;
        }
        if (!record.at("finished").get<bool>())
            throw Error("BATCH_STATE_INVALID", "Unfinished batch details cannot be archived");
        if (record.at("state") == "outcome-unknown")
            throw Error("BATCH_RECONCILIATION_REQUIRED",
                        "Unknown outcomes retain all reconciliation details");
        for (const auto &item : record.at("items"))
            if (item.at("state") == "pending" || item.at("state") == "dispatching" ||
                item.at("state") == "outcome-unknown")
                throw Error("BATCH_RECONCILIATION_REQUIRED", "Unconfirmed items must retain their details");
        BatchCounts(record);
        record["itemCount"] = record.at("items").size();
        record["items"] = Json::array();
        record["archived"] = true;
        record["detailsCompacted"] = true;
        Statement archive(db, "UPDATE profile_batches SET archived=1,record_json=?2,"
                              "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1");
        archive.Bind(1, id);
        archive.Bind(2, record.dump());
        archive.Row();
        transaction.Commit();
        return record;
    }
    if (record.at("finished").get<bool>())
        throw Error("BATCH_STATE_INVALID", "Finished batch journal is immutable");
    if (operation == "dispatch" || operation == "complete")
    {
        const auto index = arguments.at("index").get<size_t>();
        if (index >= record.at("items").size())
            throw Error("INVALID_ARGUMENTS", "Batch index exceeds journal size");
        auto &item = record["items"][index];
        if (operation == "dispatch")
        {
            if (item.at("state") != "pending")
                throw Error("BATCH_STATE_INVALID", "Only pending items can be dispatched once");
            item["state"] = "dispatching";
        }
        else
        {
            const auto state = arguments.at("state").get<std::string>();
            if (item.at("state") != "dispatching" ||
                (state != "succeeded" && state != "failed" && state != "outcome-unknown"))
                throw Error("BATCH_STATE_INVALID", "Only dispatched items can receive a terminal result");
            item["state"] = state;
            if (arguments.contains("errorCode"))
                item["errorCode"] = arguments.at("errorCode");
        }
    }
    else
    {
        bool unknown = false;
        size_t succeeded = 0, failed = 0, skipped = 0;
        for (auto &item : record.at("items"))
        {
            if (item.at("state") == "dispatching")
                throw Error("BATCH_STATE_INVALID", "Dispatched item still requires reconciliation");
            if (item.at("state") == "pending")
                item["state"] = "not-run";
            succeeded += item.at("state") == "succeeded";
            failed += item.at("state") == "failed" || item.at("state") == "outcome-unknown";
            skipped += item.at("state") == "not-run";
            unknown = unknown || item.at("state") == "outcome-unknown";
        }
        record["state"] = unknown ? "outcome-unknown" : (failed || skipped ? "partial" : "succeeded");
        record["finished"] = true;
        record["succeeded"] = succeeded;
        record["failed"] = failed;
        record["notRun"] = skipped;
    }
    Statement save(db, "UPDATE profile_batches SET record_json=?2,"
                       "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1");
    save.Bind(1, id);
    save.Bind(2, record.dump());
    save.Row();
    transaction.Commit();
    return record;
}
// 用途：将旧JSON索引一次性导入事务，原文件和迁移副本均保留；日期：2026-11-03。
void Catalog::ImportLegacy(const std::wstring &input)
{
    const auto manifest = SafeAbsolute(input);
    if (GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return;
        throw Error("STORAGE_OPEN_FAILED", "Cannot inspect legacy catalog");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    auto *db = impl_->connection->db;
    Statement imported(db, "SELECT value FROM catalog_meta WHERE key='legacy_import'");
    if (imported.Row())
        return;
    const auto bytes = ReadFileBounded(manifest, 1024 * 1024);
    const auto saved = Json::parse(bytes);
    if (!saved.is_object() || saved.value("schemaVersion", 0) != 1 ||
        saved.value("mode", "") != "development-poc" || !saved.at("profiles").is_array())
        throw Error("STORAGE_VERSION_UNSUPPORTED", "Unsupported legacy POC catalog");
    Json profiles = Json::array();
    std::set<std::string> ids;
    for (const auto &value : saved.at("profiles"))
    {
        auto metadata = Metadata(value);
        if (!ids.insert(metadata.at("id")).second)
            throw Error("PROFILE_MANIFEST_INVALID", "Duplicate legacy profile identifier");
        SafeAbsolute(manifest.substr(0, manifest.find_last_of(L'\\') + 1) + Wide(metadata.at("id")));
        profiles.push_back(std::move(metadata));
    }
    const auto copy = DatabasePath(manifest + L".pre-sqlite.bak");
    if (!CopyFileW(manifest.c_str(), copy.c_str(), TRUE))
    {
        if (GetFileAttributesW(copy.c_str()) == INVALID_FILE_ATTRIBUTES ||
            ReadFileBounded(copy, 1024 * 1024) != bytes)
            throw Error("STORAGE_BACKUP_FAILED", "Cannot preserve legacy catalog before import");
    }
    if (ReadFileBounded(copy, 1024 * 1024) != bytes)
        throw Error("STORAGE_BACKUP_FAILED", "Legacy catalog changed while preserving migration copy");
    {
        Handle file(CreateFileW(copy.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        const DWORD attributes = GetFileAttributesW(copy.c_str());
        if (!(attributes & FILE_ATTRIBUTE_READONLY) &&
            (file.get() == INVALID_HANDLE_VALUE || !FlushFileBuffers(file.get()) ||
             !SetFileAttributesW(copy.c_str(), attributes | FILE_ATTRIBUTE_READONLY)))
            throw Error("STORAGE_BACKUP_FAILED", "Cannot durably preserve legacy catalog copy");
    }
    Transaction transaction(db, impl_->healthy);
    if (Integer(db, "SELECT count(*) FROM profiles") != 0)
        throw Error("STORAGE_IMPORT_CONFLICT", "Legacy import requires an empty managed catalog");
    for (const auto &metadata : profiles)
        Insert(db, metadata);
    Sql(db, "INSERT INTO catalog_meta VALUES('legacy_import','json-v1-preserved')");
    transaction.Commit();
}
// 用途：持有串行读写锁生成自包含元数据快照，不复制浏览器会话文件；日期：2026-11-03。
void Catalog::Backup(const std::wstring &destination)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RequireHealthy();
    CopyDatabase(impl_->connection->db, destination);
}
// 用途：只读验证源快照并复制到新目标，升级失败只清理本次创建文件；日期：2026-11-03。
void Catalog::Restore(const std::wstring &backup, const std::wstring &destination)
{
    Connection source(backup, true);
    Verify(source.db);
    ReadProfiles(source.db);
    CopyDatabase(source.db, destination);
    try
    {
        Catalog restored(destination);
        restored.Profiles();
    }
    catch (...)
    {
        RemoveCreated(SafeAbsolute(destination));
        throw;
    }
}
} // namespace awb
