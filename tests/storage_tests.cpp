#include "awb/catalog.hpp"
#include "awb/service.hpp"
#include <atomic>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <process.h>
#include <sqlite/sqlite3.h>
#include <sstream>
#include <thread>

namespace
{
// 用途：记录真实本地执行日期，避免将用例创建日期误报为测试日期；日期：2026-11-04。
std::string ExecutionDate()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::ostringstream date;
    date << std::setfill('0') << std::setw(4) << now.wYear << '-' << std::setw(2) << now.wMonth << '-'
         << std::setw(2) << now.wDay;
    return date.str();
}
// 用途：测试断言失败时给出具体诊断；日期：2026-11-03。
void Require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
// 用途：验证指定错误码，禁止把其他异常误算为拒绝通过；日期：2026-11-03。
template <typename Function> void Expect(const std::string &code, Function operation)
{
    try
    {
        operation();
    }
    catch (const awb::Error &error)
    {
        Require(error.code == code, ("Wrong error: " + error.code + "; expected " + code).c_str());
        return;
    }
    throw std::runtime_error("Expected rejection: " + code);
}

struct Raw
{
    sqlite3 *db = nullptr;
    // 用途：建立独立SQLite连接，构造旧版本或异常磁盘状态；日期：2026-11-03。
    explicit Raw(const std::wstring &path, bool readonly = false)
    {
        Require(sqlite3_open_v2(awb::Utf8(path).c_str(), &db,
                                readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                nullptr) == SQLITE_OK,
                "Raw database open failed");
    }
    // 用途：关闭测试原始连接，避免锁干扰后续断言；日期：2026-11-03。
    ~Raw()
    {
        sqlite3_close(db);
    }
    // 用途：仅执行测试中写死的DDL/故障夹具SQL；日期：2026-11-03。
    void Run(const char *sql)
    {
        Require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "Raw fixture SQL failed");
    }
    // 用途：独立观察落盘版本及行数，不通过被测读取函数自证；日期：2026-11-03。
    int Value(const char *sql)
    {
        sqlite3_stmt *query = nullptr;
        Require(sqlite3_prepare_v2(db, sql, -1, &query, nullptr) == SQLITE_OK, "Raw query prepare failed");
        const int result = sqlite3_step(query);
        const int value = result == SQLITE_ROW ? sqlite3_column_int(query, 0) : -1;
        sqlite3_finalize(query);
        Require(result == SQLITE_ROW, "Raw query returned no row");
        return value;
    }
};

struct FileMethods
{
    sqlite3_io_methods patched{};
    const sqlite3_io_methods *original = nullptr;
    bool target = false;
};
std::mutex files_mutex;
std::map<sqlite3_file *, std::unique_ptr<FileMethods>> files;
sqlite3_vfs *original_vfs = nullptr;
sqlite3_vfs fault_vfs{};
std::atomic<int> fault{0};
std::string fault_path;

// 用途：获取原始文件方法，测试包装保持原VFS文件布局；日期：2026-11-03。
FileMethods Methods(sqlite3_file *file)
{
    std::lock_guard<std::mutex> lock(files_mutex);
    return *files.at(file);
}
// 用途：注入一次真实SQLite磁盘满返回码，不修改生产代码；日期：2026-11-03。
int FaultWrite(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset)
{
    const auto methods = Methods(file);
    int expected = 1;
    if (methods.target && fault.compare_exchange_strong(expected, 0))
        return SQLITE_FULL;
    return methods.original->xWrite(file, buffer, amount, offset);
}
// 用途：模拟刷盘已发生但回执失败，验证提交结果不明的封闭处理；日期：2026-11-03。
int FaultSync(sqlite3_file *file, int flags)
{
    const auto methods = Methods(file);
    const int result = methods.original->xSync(file, flags);
    int expected = 2;
    if (result == SQLITE_OK && methods.target && fault.compare_exchange_strong(expected, 0))
        return SQLITE_IOERR_FSYNC;
    return result;
}
// 用途：关闭原文件并回收测试方法表，避免悬空方法指针；日期：2026-11-03。
int FaultClose(sqlite3_file *file)
{
    const auto methods = Methods(file);
    file->pMethods = methods.original;
    const int result = methods.original->xClose(file);
    std::lock_guard<std::mutex> lock(files_mutex);
    files.erase(file);
    return result;
}
// 用途：包装本次受测数据库的文件I/O，其他文件保持原方法；日期：2026-11-03。
int FaultOpen(sqlite3_vfs *, const char *name, sqlite3_file *file, int flags, int *output_flags)
{
    const int result = original_vfs->xOpen(original_vfs, name, file, flags, output_flags);
    if (result != SQLITE_OK || !file->pMethods)
        return result;
    const auto *original = file->pMethods;
    try
    {
        auto methods = std::make_unique<FileMethods>();
        methods->original = file->pMethods;
        methods->patched = *file->pMethods;
        methods->patched.xWrite = FaultWrite;
        methods->patched.xSync = FaultSync;
        methods->patched.xClose = FaultClose;
        const std::string path = name ? name : "";
        methods->target =
            path == fault_path || path == fault_path + "-wal" || path == fault_path + "-journal";
        std::lock_guard<std::mutex> lock(files_mutex);
        const auto inserted = files.emplace(file, std::move(methods));
        file->pMethods = &inserted.first->second->patched;
        return SQLITE_OK;
    }
    catch (...)
    {
        file->pMethods = original;
        original->xClose(file);
        file->pMethods = nullptr;
        return SQLITE_NOMEM;
    }
}
// 用途：登记仅测试使用的VFS，生产目标不包含故障注入代码；日期：2026-11-03。
void InstallFaultVfs(const std::wstring &path)
{
    original_vfs = sqlite3_vfs_find(nullptr);
    Require(original_vfs != nullptr, "Missing native SQLite VFS");
    fault_vfs = *original_vfs;
    fault_vfs.zName = "awb-storage-fault-test";
    fault_vfs.pNext = nullptr;
    fault_vfs.xOpen = FaultOpen;
    fault_path = awb::Utf8(path);
    Require(sqlite3_vfs_register(&fault_vfs, 1) == SQLITE_OK, "Cannot register test VFS");
}
// 用途：生成包含中文、SQL特殊字符和多来源的真实环境配置；日期：2026-11-03。
awb::Json Profile(const std::string &id = awb::RandomId())
{
    return {{"id", id},
            {"name", "中文环境 '); DROP TABLE profiles; -- 😀"},
            {"allowedOrigins", awb::Json::array({"http://127.0.0.1:3000", "https://example.com"})}};
}
// 用途：独立构造已发布v1关系表夹具，验证升级和升级前快照；日期：2026-11-03。
void VersionOne(const std::wstring &path)
{
    Raw raw(path);
    raw.Run("PRAGMA application_id=0x41574250; PRAGMA user_version=1;"
            "CREATE TABLE catalog_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL) STRICT;"
            "INSERT INTO catalog_meta VALUES('mode','development-poc');"
            "CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY,sha256 TEXT NOT NULL) STRICT;"
            "INSERT INTO schema_migrations "
            "VALUES(1,'43526be70fad99767f0545df5408142bd9ba072af3d522cc5052f37a657bd2ee');"
            "CREATE TABLE profiles(id TEXT PRIMARY KEY CHECK(length(id)=32 AND id NOT GLOB '*[^0-9a-f]*'),"
            "name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 128),proxy_server TEXT NOT NULL DEFAULT '') "
            "STRICT;"
            "CREATE TABLE profile_origins(profile_id TEXT NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,"
            "ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 31),origin TEXT NOT NULL,"
            "PRIMARY KEY(profile_id,ordinal),UNIQUE(profile_id,origin)) STRICT;"
            "INSERT INTO profiles VALUES('11111111111111111111111111111111','旧环境','');"
            "INSERT INTO profile_origins "
            "VALUES('11111111111111111111111111111111',0,'https://example.com:443');");
}
// 用途：查找自动生成的升级前快照，确认其仍为旧版并含旧数据；日期：2026-11-03。
void CheckMigrationBackup(const std::wstring &path, int version = 1)
{
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW((path + L".before-v7.*.sqlite3").c_str(), &entry);
    Require(search != INVALID_HANDLE_VALUE, "Migration did not preserve a pre-upgrade backup");
    const std::wstring filename(entry.cFileName);
    FindClose(search);
    Raw raw(path.substr(0, path.find_last_of(L'\\') + 1) + filename, true);
    Require(raw.Value("PRAGMA user_version") == version && raw.Value("SELECT count(*) FROM profiles") == 1,
            "Pre-upgrade backup lost original schema or profiles");
}
// 用途：独立构造旧v2/v3/v4文件，验证直接升级保留版本、状态、引用和运行配置；日期：2026-11-05。
void LegacyVersion(const std::wstring &path, int version)
{
    VersionOne(path);
    Raw raw(path);
    raw.Run("ALTER TABLE profiles ADD COLUMN revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>=1);"
            "ALTER TABLE profiles ADD COLUMN created_at TEXT NOT NULL DEFAULT 'unknown-legacy';"
            "ALTER TABLE profiles ADD COLUMN updated_at TEXT NOT NULL DEFAULT 'unknown-legacy';"
            "INSERT INTO schema_migrations "
            "VALUES(2,'3ea255f26cbb16394ea8e7a418d34adcca1bc8d8f14a468d665b143f3436c5a0');"
            "UPDATE profiles SET revision=7; PRAGMA user_version=2;");
    if (version >= 3)
        raw.Run("ALTER TABLE profiles ADD COLUMN state TEXT NOT NULL DEFAULT 'active' "
                "CHECK(state IN ('active','archived','trashed','purging'));"
                "CREATE TABLE profile_references(profile_id TEXT NOT NULL REFERENCES profiles(id) "
                "ON DELETE RESTRICT,owner_id TEXT NOT NULL CHECK(length(owner_id) BETWEEN 1 AND 128),"
                "PRIMARY KEY(profile_id,owner_id)) STRICT;"
                "INSERT INTO schema_migrations "
                "VALUES(3,'51c7eeddfc1e39269f99caf5e9f516216cca653b3ad9595dc926263bf257988f');"
                "INSERT INTO profile_references VALUES('11111111111111111111111111111111','legacy-flow');"
                "PRAGMA user_version=3;");
    if (version == 4)
        raw.Run("ALTER TABLE profiles ADD COLUMN runtime_json TEXT NOT NULL DEFAULT '{}';"
                "INSERT INTO schema_migrations "
                "VALUES(4,'344e9ddbfc91c502c5f5f116df42f6f9d00e7e0341c34b0307d9e8358fb2cb20');"
                "PRAGMA user_version=4;");
}
// 用途：在子进程中分别模拟未提交WAL及已提交后立刻崩溃；日期：2026-11-03。
int CrashWorker(const std::wstring &path, const std::wstring &phase)
{
    awb::Catalog catalog(path);
    if (phase == L"committed")
    {
        catalog.InsertProfile(Profile("22222222222222222222222222222222"));
        _exit(73);
    }
    Raw raw(path);
    raw.Run("PRAGMA cache_size=8; BEGIN IMMEDIATE;"
            "WITH RECURSIVE n(x) AS(SELECT 100 UNION ALL SELECT x+1 FROM n WHERE x<5100) "
            "INSERT INTO profiles(id,name,proxy_server) SELECT printf('%032x',x),'uncommitted','' FROM n;");
    Require(GetFileAttributesW((path + L"-wal").c_str()) != INVALID_FILE_ATTRIBUTES,
            "Crash fixture failed to generate real WAL");
    _exit(72);
}
// 用途：启动本测试程序的崩溃子进程，等待精确进程句柄退出；日期：2026-11-03。
void CrashProcess(const std::wstring &path, const std::wstring &phase, DWORD expected)
{
    wchar_t executable[32768]{};
    Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Missing test executable path");
    std::wstring command = awb::QuoteArgument(executable) + L" --crash-worker " + awb::QuoteArgument(path) +
                           L" " + awb::QuoteArgument(phase);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &startup, &process) != FALSE,
            "Cannot launch crash worker");
    awb::Handle owner(process.hProcess), thread(process.hThread);
    Require(WaitForSingleObject(owner.get(), 20000) == WAIT_OBJECT_0, "Crash worker exceeded deadline");
    DWORD result = 0;
    Require(GetExitCodeProcess(owner.get(), &result) && result == expected, "Wrong crash worker exit");
}
// 用途：仅删除已核验临时根的一层普通测试文件，绝不递归遍历；日期：2026-11-03。
void Cleanup(const std::wstring &input)
{
    const auto root = awb::SafeAbsolute(input);
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW((root + L"\\*").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name(entry.cFileName);
            if (name == L"." || name == L"..")
                continue;
            Require(!(entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)),
                    "Unexpected directory in storage test cleanup");
            const auto file = awb::SafeAbsolute(root + L"\\" + name);
            SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
            Require(DeleteFileW(file.c_str()) != FALSE, "Cannot remove storage test file");
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    Require(RemoveDirectoryW(root.c_str()) != FALSE, "Cannot remove storage test root");
}
// 用途：验证v6升级、批次分页、保留摘要去重、预算、并发及磁盘失败的真实事务边界；日期：2026-11-05。
void BatchRetention(const std::wstring &root)
{
    const auto path = root + L"\\batch-retention.sqlite3";
    const auto profile = Profile();
    const awb::Json operations =
        awb::Json::array({{{"command", "browser_stop"}, {"arguments", {{"profileId", profile.at("id")}}}}});
    // 用途：生成固定宽度合法请求ID，使SQL夹具和分页预期共享可核对身份；日期：2026-11-05。
    const auto identity = [](int value)
    {
        std::ostringstream id;
        id << std::hex << std::setfill('0') << std::setw(32) << value;
        return id.str();
    };
    // 用途：创建真实已确认终态回执，不绕过生产状态机；日期：2026-11-05。
    const auto terminal = [&](awb::Catalog &catalog, int value)
    {
        const auto id = identity(value);
        catalog.Batches("begin", {{"id", id}, {"operations", operations}, {"onError", "stop"}});
        catalog.Batches("dispatch", {{"id", id}, {"index", 0}});
        catalog.Batches("complete", {{"id", id}, {"index", 0}, {"state", "succeeded"}});
        catalog.Batches("finish", {{"id", id}});
    };
    {
        awb::Catalog catalog(path);
        catalog.InsertProfile(profile);
        terminal(catalog, 1);
    }
    {
        Raw raw(path);
        raw.Run("DROP INDEX profile_batches_retention; ALTER TABLE profile_batches DROP COLUMN archived;"
                "DELETE FROM schema_migrations WHERE version=7; PRAGMA user_version=6;");
    }
    {
        awb::Catalog catalog(path);
        Require(catalog.Profiles().size() == 1 &&
                    catalog.Batches("get", {{"id", identity(1)}}).at("succeeded") == 1,
                "v6 upgrade lost profiles or terminal batch receipts");
        for (int index = 2; index <= 7; ++index)
            terminal(catalog, index);
        auto page = catalog.Batches("list", {{"limit", 2}});
        Require(page.at("items").size() == 2 && page.at("hasMore") == true &&
                    page.at("items")[0].at("batchId") == identity(7),
                "Batch page is not newest first");
        // 用途：归档上一页游标后继续分页仍不得丢失、重复或更换排序身份；日期：2026-11-05。
        catalog.Batches("archive", {{"id", identity(6)}, {"confirmBatchId", identity(6)}});
        std::set<std::string> seen{identity(7), identity(6)};
        while (page.at("hasMore").get<bool>())
        {
            page = catalog.Batches("list", {{"limit", 2}, {"before", page.at("nextCursor")}});
            for (const auto &item : page.at("items"))
                Require(seen.insert(item.at("batchId")).second, "Pagination returned a duplicate");
        }
        Require(seen.size() == 7 && page.at("nextCursor").is_null(), "Pagination omitted a receipt");
        Expect("BATCH_CURSOR_INVALID", [&] { catalog.Batches("list", {{"before", identity(999)}}); });
        Expect("INVALID_ARGUMENTS", [&] { catalog.Batches("list", {{"limit", 101}}); });
        Expect("INVALID_ARGUMENTS", [&] { catalog.Batches("list", {{"scope", "deleted"}}); });
        Expect("BATCH_CONFIRMATION_REQUIRED", [&] { catalog.Batches("archive", {{"id", identity(1)}}); });
        const auto archived =
            catalog.Batches("archive", {{"id", identity(1)}, {"confirmBatchId", identity(1)}});
        Require(archived.at("items").empty() && archived.at("itemCount") == 1 &&
                    archived.at("succeeded") == 1 && archived.at("detailsCompacted") == true,
                "Archive lost retained summary");
        Require(catalog.Batches("archive", {{"id", identity(1)}, {"confirmBatchId", identity(1)}}) ==
                    archived,
                "Repeated archive is not idempotent");
        auto receipt =
            catalog.Batches("begin", {{"id", identity(1)}, {"operations", operations}, {"onError", "stop"}});
        Require(receipt.at("created") == false && receipt.at("batch").at("archived") == true,
                "Archived request identity was replayable");
        Expect("BATCH_ID_CONFLICT",
               [&] {
                   catalog.Batches(
                       "begin", {{"id", identity(1)}, {"operations", operations}, {"onError", "continue"}});
               });
        catalog.Batches("begin", {{"id", identity(8)}, {"operations", operations}, {"onError", "stop"}});
        Expect("BATCH_STATE_INVALID",
               [&] {
                   catalog.Batches("archive", {{"id", identity(8)}, {"confirmBatchId", identity(8)}});
               });
        catalog.Batches("dispatch", {{"id", identity(8)}, {"index", 0}});
        catalog.Batches("recover");
        Expect("BATCH_RECONCILIATION_REQUIRED",
               [&] {
                   catalog.Batches("archive", {{"id", identity(8)}, {"confirmBatchId", identity(8)}});
               });
        Require(catalog.Batches("list", {{"scope", "archived"}}).at("items").size() == 2,
                "Archived scope returned active entries");
    }
    CheckMigrationBackup(path, 6);
    {
        awb::Catalog catalog(path);
        Require(
            catalog.Batches("begin", {{"id", identity(1)}, {"operations", operations}, {"onError", "stop"}})
                    .at("created") == false,
            "Restart forgot archived request identity");
    }
    // 用途：用SQL批量构造容量边界，再由生产begin/archive事务验证预算；日期：2026-11-05。
    const auto budget = root + L"\\batch-budget.sqlite3";
    {
        awb::Catalog catalog(budget);
        terminal(catalog, 1);
    }
    {
        Raw raw(budget);
        raw.Run("WITH RECURSIVE n(x) AS (VALUES(2) UNION ALL SELECT x+1 FROM n WHERE x<1000) "
                "INSERT INTO profile_batches(id,request_hash,record_json) "
                "SELECT printf('%032x',x),request_hash,json_set(record_json,'$.batchId',printf('%032x',x)) "
                "FROM n,profile_batches WHERE id='00000000000000000000000000000001';");
    }
    {
        awb::Catalog catalog(budget);
        Expect("STORAGE_BUDGET_EXCEEDED", [&] { terminal(catalog, 1001); });
    }
    // 用途：两个独立连接并发压缩同一回执必须幂等，且仅释放一份活跃额度；日期：2026-11-05。
    std::atomic<int> successes{0};
    std::vector<std::thread> workers;
    for (int index = 0; index < 2; ++index)
        workers.emplace_back(
            [&]
            {
                try
                {
                    awb::Catalog catalog(budget);
                    catalog.Batches("archive", {{"id", identity(1)}, {"confirmBatchId", identity(1)}});
                    ++successes;
                }
                catch (...)
                {
                }
            });
    for (auto &worker : workers)
        worker.join();
    Require(successes == 2, "Concurrent archive was not idempotent");
    {
        awb::Catalog catalog(budget);
        terminal(catalog, 1001);
        Expect("STORAGE_BUDGET_EXCEEDED", [&] { terminal(catalog, 1002); });
    }
    {
        Raw raw(budget);
        raw.Run("UPDATE profile_batches SET archived=1;"
                "WITH RECURSIVE n(x) AS (VALUES(1002) UNION ALL SELECT x+1 FROM n WHERE x<100000) "
                "INSERT INTO profile_batches(id,request_hash,record_json,archived) "
                "SELECT printf('%032x',x),request_hash,json_set(record_json,'$.batchId',printf('%032x',x)),1 "
                "FROM n,profile_batches WHERE id='00000000000000000000000000000001';");
    }
    {
        awb::Catalog catalog(budget);
        Expect("STORAGE_BUDGET_EXCEEDED", [&] { terminal(catalog, 100001); });
        Require(
            catalog.Batches("begin", {{"id", identity(1)}, {"operations", operations}, {"onError", "stop"}})
                    .at("created") == false,
            "Lifetime budget prevented lookup of existing identity");
    }
    const auto failure_path = root + L"\\batch-archive-failure.sqlite3";
    fault_path = awb::Utf8(failure_path);
    {
        awb::Catalog catalog(failure_path);
        terminal(catalog, 1);
        fault = 1;
        Expect("STORAGE_FULL",
               [&] {
                   catalog.Batches("archive", {{"id", identity(1)}, {"confirmBatchId", identity(1)}});
               });
        const auto receipt = catalog.Batches("get", {{"id", identity(1)}});
        Require(fault == 0 && receipt.at("archived") == false && receipt.at("items").size() == 1,
                "Failed archive lost details or falsely released budget");
        fault = 2;
        bool unknown = false;
        try
        {
            catalog.Batches("archive", {{"id", identity(1)}, {"confirmBatchId", identity(1)}});
        }
        catch (const awb::Error &error)
        {
            unknown = error.outcome_unknown;
        }
        Require(fault == 0 && unknown, "Archive commit uncertainty was reported as a confirmed result");
        Expect("STORAGE_REOPEN_REQUIRED", [&] { catalog.Batches("get", {{"id", identity(1)}}); });
    }
    {
        awb::Catalog catalog(failure_path);
        const auto receipt = catalog.Batches("get", {{"id", identity(1)}});
        Require(receipt.at("items").size() == (receipt.at("archived").get<bool>() ? 0 : 1) &&
                    catalog.Batches("begin",
                                    {{"id", identity(1)}, {"operations", operations}, {"onError", "stop"}})
                            .at("created") == false,
                "Reopen after uncertain archive lost identity or mixed column/detail state");
    }
    const auto concurrent = root + L"\\batch-concurrent-begin.sqlite3";
    {
        awb::Catalog catalog(concurrent);
    }
    successes = 0;
    std::atomic<int> created{0};
    workers.clear();
    for (int index = 0; index < 2; ++index)
        workers.emplace_back(
            [&]
            {
                try
                {
                    awb::Catalog catalog(concurrent);
                    if (catalog
                            .Batches("begin",
                                     {{"id", identity(1)}, {"operations", operations}, {"onError", "stop"}})
                            .at("created")
                            .get<bool>())
                        ++created;
                    ++successes;
                }
                catch (...)
                {
                }
            });
    for (auto &worker : workers)
        worker.join();
    Require(successes == 2 && created == 1, "Concurrent identical requests created multiple dispatch rights");
}
// 用途：验证模板CAS竞争、历史与配置同事务回滚、分组原子更新及版本容量边界；日期：2026-11-05。
void LibraryTransactions(const std::wstring &root)
{
    const auto path = root + L"\\library-transactions.sqlite3";
    const auto profile = Profile();
    const awb::Json configuration{{"name", "事务模板环境"},
                                  {"allowedOrigins", awb::Json::array({"https://example.com"})}};
    std::string template_id, group_id;
    {
        awb::Catalog catalog(path);
        catalog.InsertProfile(profile);
        template_id =
            catalog.Library("template", "create", {{"name", "模板初版"}, {"configuration", configuration}})
                .at("id");
        group_id = catalog
                       .Library("group", "create",
                                {{"name", "原子分组"}, {"profileIds", awb::Json::array({profile.at("id")})}})
                       .at("id");
    }
    std::atomic<int> success{0}, conflicts{0};
    std::vector<std::thread> workers;
    for (int index = 0; index < 2; ++index)
        workers.emplace_back(
            [&, index]
            {
                try
                {
                    awb::Catalog catalog(path);
                    catalog.Library("template", "update",
                                    {{"id", template_id},
                                     {"expectedRevision", 1},
                                     {"name", "并发版本" + std::to_string(index)},
                                     {"configuration", configuration}});
                    ++success;
                }
                catch (const awb::Error &error)
                {
                    if (error.code == "REVISION_CONFLICT")
                        ++conflicts;
                }
                catch (...)
                {
                }
            });
    for (auto &worker : workers)
        worker.join();
    Require(success == 1 && conflicts == 1, "Concurrent template CAS had more than one winner");
    fault_path = awb::Utf8(path);
    awb::Catalog catalog(path);
    const auto original = catalog.Library("template", "list", awb::Json::object());
    {
        Raw raw(path);
        raw.Run("CREATE TRIGGER reject_template_history BEFORE INSERT ON profile_template_versions "
                "WHEN NEW.revision=3 BEGIN SELECT RAISE(ABORT,'history-fixture'); END;");
    }
    Expect("STORAGE_CONSTRAINT",
           [&]
           {
               catalog.Library("template", "update",
                               {{"id", template_id},
                                {"expectedRevision", 2},
                                {"name", "不得部分保存"},
                                {"configuration", configuration}});
           });
    Require(catalog.Library("template", "list", awb::Json::object()) == original &&
                catalog.Library("template", "history", {{"id", template_id}}).at("items").size() == 2,
            "History failure committed current configuration without its history");
    {
        Raw raw(path);
        raw.Run("DROP TRIGGER reject_template_history");
    }
    fault = 1;
    Expect("STORAGE_FULL",
           [&]
           {
               catalog.Library("template", "rollback",
                               {{"id", template_id}, {"expectedRevision", 2}, {"sourceRevision", 1}});
           });
    Require(fault == 0 && catalog.Library("template", "list", awb::Json::object()) == original,
            "Disk-full rollback advanced current template revision");
    const auto group = catalog.Library("group", "list", awb::Json::object());
    fault = 1;
    Expect("STORAGE_FULL",
           [&]
           {
               catalog.Library("group", "update",
                               {{"id", group_id},
                                {"expectedRevision", 1},
                                {"name", "失败分组"},
                                {"profileIds", awb::Json::array()}});
           });
    Require(fault == 0 && catalog.Library("group", "list", awb::Json::object()) == group,
            "Group failure committed name or member changes");
    for (int revision = 2; revision < 100; ++revision)
        catalog.Library("template", "update",
                        {{"id", template_id},
                         {"expectedRevision", revision},
                         {"name", "容量版本"},
                         {"configuration", configuration}});
    Expect("STORAGE_BUDGET_EXCEEDED",
           [&]
           {
               catalog.Library("template", "rollback",
                               {{"id", template_id}, {"expectedRevision", 100}, {"sourceRevision", 1}});
           });
    Require(catalog.Library("template", "history", {{"id", template_id}}).at("items").size() == 100,
            "Rejected history growth changed retained version count");
    catalog.Library("template", "delete", {{"id", template_id}, {"expectedRevision", 100}});
    catalog.Library("group", "delete", {{"id", group_id}, {"expectedRevision", 1}});
    Raw raw(path);
    Require(raw.Value("SELECT count(*) FROM profile_template_versions") == 0 &&
                catalog.Profiles().size() == 1,
            "Resource delete retained orphan history or deleted the actual profile");
}
} // namespace

// 用途：验证SQLite创建/导入/迁移/回滚/故障/备份恢复及真实进程崩溃；日期：2026-11-03。
int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc == 4 && std::wstring(argv[1]) == L"--crash-worker")
            return CrashWorker(argv[2], argv[3]);
        Require(argc == 1 || (argc == 3 && std::wstring(argv[1]) == L"--report"), "Invalid test arguments");
        wchar_t temporary[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temporary);
        const auto root = std::wstring(temporary) + L"awb-storage-中文-" + awb::Wide(awb::RandomId());
        awb::PrivateDirectory(root);
        const auto path = root + L"\\catalog.sqlite3";
        InstallFaultVfs(path);
        const auto original = Profile();
        {
            awb::Catalog catalog(path);
            catalog.InsertProfile(original);
            // 用途：数据库主键冲突不能吞错或覆盖已有环境；日期：2026-11-03。
            Expect("STORAGE_CONSTRAINT", [&] { catalog.InsertProfile(original); });
            // 用途：第二条环境主键冲突时，第一条已经写入的环境也必须回滚；日期：2026-11-05。
            Expect("STORAGE_CONSTRAINT",
                   [&] {
                       catalog.InsertProfiles(awb::Json::array({Profile(), original}));
                   });
            Require(catalog.Profiles().size() == 1, "Atomic profile import retained its first item");
            Raw raw(path);
            raw.Run("CREATE TRIGGER reject_second_origin BEFORE INSERT ON profile_origins "
                    "WHEN NEW.ordinal=1 BEGIN SELECT RAISE(ABORT,'fixture refusal'); END;");
            // 用途：在Profile及首来源已写后失败，整事务必须回滚；日期：2026-11-03。
            Expect("STORAGE_CONSTRAINT", [&] { catalog.InsertProfile(Profile()); });
            Require(raw.Value("SELECT count(*) FROM profiles") == 1 &&
                        raw.Value("SELECT count(*) FROM profile_origins") == 2,
                    "Partial profile transaction survived rollback");
            raw.Run("DROP TRIGGER reject_second_origin");
            fault = 1;
            // 用途：写入期间真实VFS返回磁盘满，不得产生成功答复；日期：2026-11-03。
            Expect("STORAGE_FULL", [&] { catalog.InsertProfile(Profile()); });
            Require(fault == 0 && catalog.Profiles().size() == 1,
                    "Disk-full transaction was not rolled back");
            catalog.Backup(root + L"\\snapshot.sqlite3");
            // 用途：备份目标已存在时禁止覆盖；日期：2026-11-03。
            Expect("STORAGE_DESTINATION_EXISTS", [&] { catalog.Backup(root + L"\\snapshot.sqlite3"); });
            std::vector<std::thread> writers;
            std::atomic<int> errors{0};
            for (int index = 0; index < 4; ++index)
            {
                // 用途：多线程调用同一事务层不得交错半条记录；日期：2026-11-03。
                writers.emplace_back(
                    [&]
                    {
                        try
                        {
                            for (int item = 0; item < 8; ++item)
                                catalog.InsertProfile(Profile());
                        }
                        catch (...)
                        {
                            ++errors;
                        }
                    });
            }
            for (auto &writer : writers)
                writer.join();
            Require(errors == 0 && catalog.Profiles().size() == 33, "Concurrent catalog writes lost data");
            raw.Run("BEGIN IMMEDIATE");
            // 用途：另一连接持有写锁时有界失败，解锁后能恢复写入；日期：2026-11-03。
            Expect("STORAGE_BUSY", [&] { catalog.InsertProfile(Profile()); });
            raw.Run("ROLLBACK");
            fault = 2;
            bool unknown = false;
            try
            {
                catalog.InsertProfile(Profile());
            }
            catch (const awb::Error &error)
            {
                unknown = error.code == "STORAGE_IO_ERROR" && error.outcome_unknown;
            }
            Require(fault == 0 && unknown, "Commit I/O failure was not marked outcome unknown");
            // 用途：结果不明后禁止继续使用可能过时的状态；日期：2026-11-03。
            Expect("STORAGE_REOPEN_REQUIRED", [&] { catalog.Profiles(); });
        }
        {
            awb::Catalog reopened(path);
            Require(reopened.Profiles().size() >= 33 && reopened.Profiles().size() <= 34,
                    "Commit reconciliation lost previously acknowledged profiles");
            Raw raw(path);
            Require(raw.Value("SELECT count(*) FROM profile_origins") ==
                        raw.Value("SELECT count(*) FROM profiles") * 2,
                    "I/O failure left partial profile origins");
        }
        awb::Catalog::Restore(root + L"\\snapshot.sqlite3", root + L"\\restored.sqlite3");
        {
            awb::Catalog restored(root + L"\\restored.sqlite3");
            const auto profiles = restored.Profiles();
            Require(profiles.size() == 1 && profiles[0].at("id") == original.at("id") &&
                        profiles[0].at("name") == original.at("name"),
                    "Snapshot did not restore independent prior state");
        }
        // 用途：恢复不能覆盖仍在使用的原数据库；日期：2026-11-03。
        Expect("STORAGE_DESTINATION_EXISTS",
               [&] { awb::Catalog::Restore(root + L"\\snapshot.sqlite3", path); });
        const auto readonly = root + L"\\readonly.sqlite3";
        Require(CopyFileW((root + L"\\snapshot.sqlite3").c_str(), readonly.c_str(), TRUE) != FALSE,
                "Cannot create readonly fixture");
        const auto readonly_bytes = awb::ReadFileBounded(readonly, 1024 * 1024);
        Require(SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
                "Cannot mark database readonly");
        // 用途：实际只读文件不能被当作可写目录继续使用；日期：2026-11-03。
        Expect("STORAGE_READ_ONLY", [&] { awb::Catalog denied(readonly); });
        Require(awb::ReadFileBounded(readonly, 1024 * 1024) == readonly_bytes,
                "Readonly rejection modified source bytes");
        const auto alias = root + L"\\hardlink.sqlite3";
        Require(CreateHardLinkW(alias.c_str(), readonly.c_str(), nullptr) != FALSE,
                "Cannot create hardlink fixture");
        // 用途：硬链接不能绕过受管数据库文件所有权边界；日期：2026-11-03。
        Expect("PATH_INVALID", [&] { awb::Catalog linked(alias); });
        Require(SetFileAttributesW(alias.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE,
                "Cannot reset shared readonly hardlink attribute");
        Require(DeleteFileW(alias.c_str()) != FALSE, "Cannot remove hardlink fixture");
        const auto v1 = root + L"\\v1.sqlite3";
        VersionOne(v1);
        {
            awb::Catalog migrated(v1);
            Require(migrated.Profiles()[0].at("id") == "11111111111111111111111111111111",
                    "Migration changed profile identity");
        }
        CheckMigrationBackup(v1);
        for (const int version : {2, 3, 4})
        {
            const auto legacy = root + L"\\v" + std::to_wstring(version) + L".sqlite3";
            LegacyVersion(legacy, version);
            {
                awb::Catalog migrated(legacy);
                const auto profile = migrated.Profile("11111111111111111111111111111111");
                Require(profile.at("revision") == 7 && profile.at("state") == "active" &&
                            profile.value("runtimeSettings", awb::Json::object()).empty(),
                        "Direct legacy upgrade lost lifecycle metadata or introduced runtime overrides");
                if (version >= 3)
                {
                    // 用途：旧版真实持久引用不能在升级后被回收操作绕过；日期：2026-11-04。
                    Expect("PROFILE_REFERENCED", [&]
                           { migrated.ChangeProfile(profile.at("id"), 7, awb::Json::object(), "trashed"); });
                }
            }
            CheckMigrationBackup(legacy, version);
        }
        {
            Raw raw(v1);
            Require(raw.Value("PRAGMA user_version") == 7 &&
                        raw.Value("SELECT count(*) FROM schema_migrations") == 7,
                    "Migration did not atomically update version and history");
            raw.Run("PRAGMA user_version=99");
        }
        // 用途：未来版本不可降级或重建；日期：2026-11-03。
        Expect("STORAGE_VERSION_UNSUPPORTED", [&] { awb::Catalog future(v1); });
        {
            Raw raw(v1);
            Require(raw.Value("PRAGMA user_version") == 99, "Future schema was modified");
            raw.Run("PRAGMA user_version=2; UPDATE schema_migrations SET sha256='tampered' WHERE version=2;");
        }
        // 用途：迁移记录不匹配时拒绝打开；日期：2026-11-03。
        Expect("STORAGE_MIGRATION_INVALID", [&] { awb::Catalog changed(v1); });
        const auto failed_migration = root + L"\\failed-migration.sqlite3";
        VersionOne(failed_migration);
        fault_path = awb::Utf8(failed_migration);
        fault = 1;
        // 用途：升级事务I/O失败保留旧版本及旧字段；日期：2026-11-03。
        Expect("STORAGE_FULL", [&] { awb::Catalog failed(failed_migration); });
        {
            Raw raw(failed_migration);
            Require(fault == 0 && raw.Value("PRAGMA user_version") == 1 &&
                        raw.Value("SELECT count(*) FROM schema_migrations") == 1 &&
                        raw.Value("SELECT count(*) FROM pragma_table_info('profiles')") == 3,
                    "Failed migration advanced schema or lost old data");
        }
        fault_path.clear();
        {
            awb::Catalog retry(failed_migration);
            Require(retry.Profiles().size() == 1, "Migration could not resume after failure");
        }
        const auto manifest = root + L"\\workbench-poc.json";
        auto duplicate = awb::Json{{"schemaVersion", 1},
                                   {"mode", "development-poc"},
                                   {"profiles", awb::Json::array({original, original})}};
        awb::AtomicWrite(manifest, duplicate.dump());
        {
            awb::Catalog imported(root + L"\\import.sqlite3");
            // 用途：批量导入重复ID时不能留下第一条记录；日期：2026-11-03。
            Expect("PROFILE_MANIFEST_INVALID", [&] { imported.ImportLegacy(manifest); });
            Require(imported.Profiles().empty(), "Rejected import left partial catalog");
            duplicate["profiles"] = awb::Json::array({original});
            const auto preserved = duplicate.dump();
            awb::AtomicWrite(manifest, preserved);
            imported.ImportLegacy(manifest);
            imported.ImportLegacy(manifest);
            Require(imported.Profiles().size() == 1 &&
                        awb::ReadFileBounded(manifest, 1024 * 1024) == preserved &&
                        awb::ReadFileBounded(manifest + L".pre-sqlite.bak", 1024 * 1024) == preserved,
                    "Legacy import was not idempotent or altered original bytes");
        }
        {
            const auto batch = root + L"\\batch.json";
            const auto batch_path = root + L"\\batch.sqlite3";
            duplicate["profiles"] = awb::Json::array({original, Profile("33333333333333333333333333333333")});
            awb::AtomicWrite(batch, duplicate.dump());
            awb::Catalog imported(batch_path);
            Raw raw(batch_path);
            raw.Run("CREATE TRIGGER reject_import BEFORE INSERT ON profiles "
                    "WHEN NEW.id='33333333333333333333333333333333' "
                    "BEGIN SELECT RAISE(ABORT,'fixture refusal'); END;");
            // 用途：第二条导入写入失败必须回滚第一条及导入标志；日期：2026-11-03。
            Expect("STORAGE_CONSTRAINT", [&] { imported.ImportLegacy(batch); });
            Require(imported.Profiles().empty() &&
                        raw.Value("SELECT count(*) FROM catalog_meta WHERE key='legacy_import'") == 0,
                    "Batch import left partial profiles or completion marker");
            raw.Run("DROP TRIGGER reject_import");
            imported.ImportLegacy(batch);
            Require(imported.Profiles().size() == 2, "Rolled-back import could not be retried");
        }
        awb::AtomicWrite(root + L"\\corrupt.sqlite3", "not a database");
        {
            awb::Catalog imported(root + L"\\import.sqlite3");
            awb::Handle hidden(CreateFileW((root + L"\\inspection-denied.json").c_str(),
                                           GENERIC_READ | GENERIC_WRITE | DELETE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            Require(hidden.get() != INVALID_HANDLE_VALUE, "Cannot create inaccessible legacy fixture");
            FILE_DISPOSITION_INFO disposition{TRUE};
            Require(SetFileInformationByHandle(hidden.get(), FileDispositionInfo, &disposition,
                                               sizeof(disposition)) != FALSE,
                    "Cannot mark legacy fixture delete-pending");
            // 用途：真实删除挂起路径检查失败不能被当作清单不存在；日期：2026-11-03。
            Expect("STORAGE_OPEN_FAILED", [&] { imported.ImportLegacy(root + L"\\inspection-denied.json"); });
        }
        // 用途：损坏快照不得被当作空库，原数据库不能被修改；日期：2026-11-03。
        Expect("STORAGE_CORRUPT", [&] { awb::Catalog damaged(root + L"\\corrupt.sqlite3"); });
        // 用途：恢复验证先于目标创建，损坏源不产生空目标；日期：2026-11-03。
        Expect("STORAGE_CORRUPT",
               [&] { awb::Catalog::Restore(root + L"\\corrupt.sqlite3", root + L"\\bad.sqlite3"); });
        Require(GetFileAttributesW((root + L"\\bad.sqlite3").c_str()) == INVALID_FILE_ATTRIBUTES,
                "Bad restore left a destination file");
        const auto crash = root + L"\\crash.sqlite3";
        {
            awb::Catalog catalog(crash);
            catalog.InsertProfile(original);
        }
        CrashProcess(crash, L"uncommitted", 72);
        {
            awb::Catalog recovered(crash);
            Require(recovered.Profiles().size() == 1, "Crash exposed uncommitted WAL rows");
        }
        CrashProcess(crash, L"committed", 73);
        {
            awb::Catalog recovered(crash);
            Require(recovered.Profiles().size() == 2, "Crash lost acknowledged committed profile");
        }
        const auto lifecycle = root + L"\\lifecycle.sqlite3";
        {
            awb::Catalog catalog(lifecycle);
            catalog.InsertProfile(original);
            const auto id = original.at("id").get<std::string>();
            catalog.SetProfileReference(id, "saved-workflow", true);
            // 用途：持久流程引用阻止归档和回收，事务失败不得增加版本；日期：2026-11-04。
            Expect("PROFILE_REFERENCED",
                   [&] { catalog.ChangeProfile(id, 1, awb::Json::object(), "trashed"); });
            Expect("PROFILE_REFERENCED",
                   [&] { catalog.ChangeProfile(id, 1, awb::Json::object(), "archived"); });
            Require(catalog.Profile(id).at("revision") == 1,
                    "Rejected reference transition changed revision");
        }
        {
            awb::Catalog catalog(lifecycle);
            const auto id = original.at("id").get<std::string>();
            // 用途：关闭重开后引用仍然保护环境，而非仅内存标志；日期：2026-11-04。
            Expect("PROFILE_REFERENCED",
                   [&] { catalog.ChangeProfile(id, 1, awb::Json::object(), "trashed"); });
            catalog.SetProfileReference(id, "saved-workflow", false);
            const auto changed = catalog.ChangeProfile(id, 1, {{"name", "改名后环境"}}, "");
            Require(changed.at("revision") == 2 && changed.at("name") == "改名后环境",
                    "Profile update did not commit");
            // 用途：旧版本和非法状态不得绕过乐观锁或外键；日期：2026-11-04。
            Expect("REVISION_CONFLICT", [&] { catalog.ChangeProfile(id, 1, {{"name", "过时"}}, ""); });
            catalog.ChangeProfile(id, 2, awb::Json::object(), "trashed");
            Expect("PROFILE_STATE_INVALID", [&] { catalog.SetProfileReference(id, "new-owner", true); });
            Expect("REVISION_CONFLICT", [&] { catalog.FinishPurge(id, 3); });
            catalog.ChangeProfile(id, 3, awb::Json::object(), "purging");
        }
        {
            awb::Catalog catalog(lifecycle);
            const auto id = original.at("id").get<std::string>();
            Require(catalog.Profile(id).at("state") == "purging", "Pending purge did not survive restart");
            catalog.FinishPurge(id, 4);
            Require(catalog.Profiles().empty(), "Purge did not remove profile and origins");
        }
        // 用途：按旧版v5形状构造仅保留当前模板的库，升级只回填真实存在版本；日期：2026-11-05。
        const auto history_path = root + L"\\history-v5.sqlite3";
        std::string template_id;
        const awb::Json template_configuration{{"name", "模板配置"},
                                               {"allowedOrigins", awb::Json::array({"https://example.com"})}};
        {
            awb::Catalog catalog(history_path);
            catalog.InsertProfile(original);
            auto resource = catalog.Library("template", "create",
                                            {{"name", "模板一"}, {"configuration", template_configuration}});
            template_id = resource.at("id");
            catalog.Library("template", "update",
                            {{"id", template_id},
                             {"expectedRevision", 1},
                             {"name", "模板二"},
                             {"configuration", template_configuration}});
        }
        {
            Raw raw(history_path);
            raw.Run("DROP TABLE profile_template_versions; DROP TABLE profile_batches;"
                    "DELETE FROM schema_migrations WHERE version>=6; PRAGMA user_version=5;");
        }
        {
            awb::Catalog catalog(history_path);
            const auto history = catalog.Library("template", "history", {{"id", template_id}});
            Require(history.at("items").size() == 1 && history.at("items")[0].at("revision") == 2,
                    "v5 migration invented unavailable old template history");
            Expect("TEMPLATE_VERSION_NOT_FOUND",
                   [&]
                   {
                       catalog.Library("template", "rollback",
                                       {{"id", template_id}, {"expectedRevision", 2}, {"sourceRevision", 1}});
                   });
            Require(catalog.Library("template", "list", awb::Json::object()).at("items")[0].at("revision") ==
                        2,
                    "Rejected rollback changed current template revision");
        }
        CheckMigrationBackup(history_path, 5);
        // 用途：分别重启恢复尚未派发、已派发无回执、已确认未收尾的批次，禁止自动重放；日期：2026-11-05。
        const auto batch_path = root + L"\\batch-journal.sqlite3";
        const awb::Json operations = awb::Json::array(
            {{{"command", "browser_stop"}, {"arguments", {{"profileId", original.at("id")}}}}});
        const std::string pending_id(32, 'a'), dispatched_id(32, 'b'), completed_id(32, 'c');
        {
            awb::Catalog catalog(batch_path);
            for (const auto &id : {pending_id, dispatched_id, completed_id})
                catalog.Batches("begin", {{"id", id}, {"operations", operations}, {"onError", "stop"}});
            catalog.Batches("dispatch", {{"id", dispatched_id}, {"index", 0}});
            catalog.Batches("dispatch", {{"id", completed_id}, {"index", 0}});
            catalog.Batches("complete", {{"id", completed_id}, {"index", 0}, {"state", "succeeded"}});
        }
        {
            awb::Catalog catalog(batch_path);
            catalog.Batches("recover");
            const auto pending = catalog.Batches("get", {{"id", pending_id}});
            const auto dispatched = catalog.Batches("get", {{"id", dispatched_id}});
            const auto completed = catalog.Batches("get", {{"id", completed_id}});
            Require(pending.at("state") == "interrupted" && pending.at("items")[0].at("state") == "not-run" &&
                        dispatched.at("state") == "outcome-unknown" &&
                        dispatched.at("items")[0].at("state") == "outcome-unknown" &&
                        completed.at("state") == "interrupted" &&
                        completed.at("items")[0].at("state") == "succeeded",
                    "Restart batch reconciliation lost durable dispatch boundaries");
            Require(!catalog
                         .Batches("begin",
                                  {{"id", dispatched_id}, {"operations", operations}, {"onError", "stop"}})
                         .at("created")
                         .get<bool>(),
                    "Same durable batch ID was issued twice");
            Expect("BATCH_STATE_INVALID",
                   [&] {
                       catalog.Batches("dispatch", {{"id", dispatched_id}, {"index", 0}});
                   });
            Expect("BATCH_ID_CONFLICT",
                   [&] {
                       catalog.Batches(
                           "begin",
                           {{"id", dispatched_id}, {"operations", operations}, {"onError", "continue"}});
                   });
            catalog.Batches("recover");
            Require(catalog.Batches("get", {{"id", dispatched_id}}) == dispatched,
                    "Repeated restart reconciliation rewrote terminal receipt");
        }
        BatchRetention(root);
        LibraryTransactions(root);
        sqlite3_vfs_register(original_vfs, 1);
        sqlite3_vfs_unregister(&fault_vfs);
        Require(files.empty(), "Test leaked SQLite file handles");
        Cleanup(root);
        const awb::Json report{
            {"date", ExecutionDate()},
            {"result", "passed"},
            {"sqliteVersion", sqlite3_libversion()},
            {"sqliteSourceId", sqlite3_sourceid()},
            {"schemaVersion", 7},
            {"language", "C++14"},
            {"checks", awb::Json::array({"unicode-and-bound-sql-values",
                                         "duplicate-id-refusal",
                                         "profile-origin-transaction-rollback",
                                         "four-concurrent-writers",
                                         "two-second-busy-deadline",
                                         "vfs-disk-full-no-false-success",
                                         "commit-fsync-uncertainty-and-reopen",
                                         "online-backup-snapshot",
                                         "restore-new-file-no-overwrite",
                                         "readonly-file-byte-preservation",
                                         "hardlink-refusal",
                                         "v1-v7-upgrade-and-preupgrade-backup",
                                         "direct-v2-v7-upgrade-and-backup",
                                         "direct-v3-v7-upgrade-reference-and-backup",
                                         "direct-v4-v7-upgrade-reference-and-backup",
                                         "v5-v7-real-template-history-backfill-and-backup",
                                         "durable-batch-dispatch-restart-and-idempotency",
                                         "v6-v7-upgrade-receipt-preservation-and-backup",
                                         "stable-pagination-across-archive-and-scope-validation",
                                         "archive-summary-restart-deduplication-and-unknown-protection",
                                         "active-and-lifetime-batch-budget-boundaries",
                                         "concurrent-idempotent-archive",
                                         "archive-disk-full-transaction-rollback",
                                         "archive-commit-uncertainty-reopen-and-deduplication",
                                         "concurrent-batch-begin-single-dispatch-right",
                                         "concurrent-template-cas-single-history-winner",
                                         "template-history-and-group-write-failure-atomicity",
                                         "template-hundred-version-boundary-and-delete-cascade",
                                         "failed-upgrade-rollback-and-retry",
                                         "future-version-refusal",
                                         "altered-migration-refusal",
                                         "legacy-import-byte-preservation",
                                         "legacy-duplicate-id-atomic-refusal",
                                         "legacy-idempotent-import",
                                         "multi-profile-import-rollback-and-retry",
                                         "corrupt-file-refusal",
                                         "corrupt-restore-no-destination",
                                         "legacy-inspection-failure-not-missing",
                                         "real-uncommitted-wal-process-crash",
                                         "real-committed-wal-process-crash",
                                         "persistent-resource-reference-guard",
                                         "profile-revision-and-state-transitions",
                                         "pending-purge-restart-and-finalize"})},
            {"faultMethod", "Test-only native SQLite VFS; no physical disk exhaustion or power cut"},
            {"scope", "Local metadata catalog; browser session backup, credentials and commerce excluded"}};
        if (argc == 3)
            awb::AtomicWrite(argv[2], report.dump(2) + "\n");
        std::cout << "PASS: SQLite " << sqlite3_libversion()
                  << "; migrations, JSON import, whole-profile rollback, concurrent writer, busy deadline, "
                     "VFS disk-full/commit-I/O, backup/new-file restore, future/corrupt refusal, WAL process "
                     "crash\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
