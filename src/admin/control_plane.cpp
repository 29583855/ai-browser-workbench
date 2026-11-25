#include "awb/control_plane.hpp"
#include "awb/backend_extensions.hpp"
#include <algorithm>
#include <bcrypt.h>
#include <chrono>
#include <mutex>
#include <sqlite/sqlite3.h>

namespace awb
{
namespace
{
// 用途：使用UTC秒存储账期，避免夏令时和本地时区改变计费；日期：2026-11-05。
long long Now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
// 用途：验证并读取有界业务字符串；日期：2026-11-05。
std::string Text(const Json &body, const char *key, size_t limit = 128)
{
    if (!body.contains(key) || !body.at(key).is_string())
        throw Error("INVALID_ARGUMENTS", std::string("Expected string: ") + key);
    const auto value = body.at(key).get<std::string>();
    if (value.empty() || value.size() > limit || value.find('\0') != std::string::npos)
        throw Error("INVALID_ARGUMENTS", std::string("Invalid length: ") + key);
    Wide(value);
    return value;
}
// 用途：读取严格有界整数，金额全部采用人民币分；日期：2026-11-05。
long long Number(const Json &body, const char *key, long long minimum, long long maximum)
{
    if (!body.contains(key) || !body.at(key).is_number_integer())
        throw Error("INVALID_ARGUMENTS", std::string("Expected integer: ") + key);
    const auto value = body.at(key).get<long long>();
    if (value < minimum || value > maximum)
        throw Error("INVALID_ARGUMENTS", std::string("Integer out of range: ") + key);
    return value;
}
// 用途：归一化ASCII邮箱作为账号唯一键，拒绝控制字符；日期：2026-11-05。
std::string Email(const Json &body)
{
    auto value = Text(body, "email", 254);
    if (value.find('@') == std::string::npos || value.front() == '@' || value.back() == '@')
        throw Error("INVALID_ARGUMENTS", "Expected an email address");
    for (auto &c : value)
    {
        if (static_cast<unsigned char>(c) <= 32 || static_cast<unsigned char>(c) >= 127)
            throw Error("INVALID_ARGUMENTS", "Email must use ASCII without spaces");
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
    return value;
}
// 用途：将哈希或密钥派生结果编码为十六进制；日期：2026-11-05。
std::string Hex(const std::vector<unsigned char> &bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes)
    {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
// 用途：计算SHA256/HMAC或PBKDF2，密码使用二十万轮独立随机盐；日期：2026-11-05。
std::string Digest(const std::string &value, const std::string &key = "", bool password = false)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    (password || !key.empty()) ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) < 0)
        throw Error("CRYPTO_FAILED", "Cannot open SHA256 provider");
    std::vector<unsigned char> output(32);
    NTSTATUS status = 0;
    if (password)
        status = BCryptDeriveKeyPBKDF2(
            algorithm, reinterpret_cast<PUCHAR>(const_cast<char *>(value.data())),
            static_cast<ULONG>(value.size()), reinterpret_cast<PUCHAR>(const_cast<char *>(key.data())),
            static_cast<ULONG>(key.size()), 200000, output.data(), static_cast<ULONG>(output.size()), 0);
    else
        status = BCryptHash(
            algorithm, key.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<char *>(key.data())),
            static_cast<ULONG>(key.size()), reinterpret_cast<PUCHAR>(const_cast<char *>(value.data())),
            static_cast<ULONG>(value.size()), output.data(), static_cast<ULONG>(output.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
        throw Error("CRYPTO_FAILED", "Cannot derive cryptographic digest");
    return Hex(output);
}
// 用途：固定长度比较签名与密码摘要，避免逐字符提前返回；日期：2026-11-05。
bool Equal(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}
// 用途：验证成员角色，组织所有者仅允许通过成员变更明确授予；日期：2026-11-05。
std::string Role(const Json &body)
{
    const auto role = Text(body, "role");
    if (role != "owner" && role != "admin" && role != "finance" && role != "member")
        throw Error("INVALID_ARGUMENTS", "Role must be owner/admin/finance/member");
    return role;
}
class Statement
{
  public:
    sqlite3_stmt *value = nullptr;
    // 用途：编译固定SQL，所有外部字段只允许参数绑定；日期：2026-11-05。
    Statement(sqlite3 *database, const char *sql)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &value, nullptr) != SQLITE_OK)
            throw Error("ADMIN_DATABASE", "Cannot prepare control-plane statement");
    }
    // 用途：释放预编译SQL，异常路径同样关闭；日期：2026-11-05。
    ~Statement()
    {
        sqlite3_finalize(value);
    }
    // 用途：以瞬时复制形式绑定UTF8字符串；日期：2026-11-05。
    void Bind(int index, const std::string &text)
    {
        if (sqlite3_bind_text(value, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT) !=
            SQLITE_OK)
            throw Error("ADMIN_DATABASE", "Cannot bind statement");
    }
    // 用途：读取一行或检查SQL最终完成，禁止忽略写失败；日期：2026-11-05。
    bool Step()
    {
        const int result = sqlite3_step(value);
        if (result != SQLITE_ROW && result != SQLITE_DONE)
            throw Error("ADMIN_DATABASE", "Control-plane database operation failed");
        return result == SQLITE_ROW;
    }
    // 用途：复制数据库JSON正文，不暴露SQLite内部缓冲区；日期：2026-11-05。
    std::string String(int index)
    {
        const auto value_text = sqlite3_column_text(value, index);
        return value_text ? reinterpret_cast<const char *>(value_text) : "";
    }
};
} // namespace

struct ControlPlane::Impl : BackendStore
{
    std::wstring root;
    Handle owner;
    sqlite3 *database = nullptr;
    std::mutex mutex;
    Json config;
    std::unique_ptr<OfficialPayments> payments;
    std::unique_ptr<OidcIdentity> identity;
    std::unique_ptr<NodeScheduler> scheduler;
    // 用途：独占私有数据根并创建独立版本化数据库与待配置的支付文件；日期：2026-11-05。
    explicit Impl(const std::wstring &path) : root(SafeAbsolute(path))
    {
        PrivateDirectory(root);
        owner.reset(CreateFileW((root + L"\\.admin.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr));
        if (owner.get() == INVALID_HANDLE_VALUE)
            throw Error("ROOT_IN_USE", "Another admin process owns this directory");
        const auto file = SafeAbsolute(root + L"\\control-plane.sqlite3");
        if (sqlite3_open_v2(Utf8(file).c_str(), &database,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK)
        {
            sqlite3_close(database);
            database = nullptr;
            throw Error("ADMIN_DATABASE", "Cannot open control-plane database");
        }
        try
        {
            Sql("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA busy_timeout=5000;");
            Statement version(database, "PRAGMA user_version");
            if (!version.Step() || sqlite3_column_int(version.value, 0) > 1)
                throw Error("ADMIN_DATABASE_VERSION", "Unsupported control-plane migration version");
            Sql("BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS records (kind TEXT NOT NULL, id TEXT NOT NULL, "
                "body TEXT NOT NULL CHECK(json_valid(body)), PRIMARY KEY(kind,id)); "
                "CREATE TRIGGER IF NOT EXISTS immutable_ledger_update BEFORE UPDATE ON records "
                "WHEN OLD.kind IN ('ledger','audit','event','time_ledger') BEGIN SELECT "
                "RAISE(ABORT,'immutable'); END; "
                "CREATE TRIGGER IF NOT EXISTS immutable_ledger_delete BEFORE DELETE ON records "
                "WHEN OLD.kind IN ('ledger','audit','event','time_ledger') BEGIN SELECT "
                "RAISE(ABORT,'immutable'); END; "
                "PRAGMA user_version=1; COMMIT;");
            const auto settings = SafeAbsolute(root + L"\\payment-config.json");
            if (GetFileAttributesW(settings.c_str()) == INVALID_FILE_ATTRIBUTES)
                AtomicWrite(settings, Json{{"enabled", false},
                                           {"checkoutBaseUrl", ""},
                                           {"merchantId", ""},
                                           {"webhookSecret", ""},
                                           {"contract", "awb-signed-checkout-v1"}}
                                              .dump(2) +
                                          "\n");
            config = ParseExternalJson(ReadFileBounded(settings, 16384));
            if (config.value("enabled", false) &&
                (Text(config, "webhookSecret", 512).size() < 32 ||
                 Text(config, "checkoutBaseUrl", 2048).find("https://") != 0 ||
                 Text(config, "merchantId", 128)
                         .find_first_not_of(
                             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                     std::string::npos))
                throw Error("PAYMENT_CONFIG",
                            "Configured gateway requires HTTPS, merchant ID and 32-byte secret");
            payments = std::make_unique<OfficialPayments>(root);
            identity = std::make_unique<OidcIdentity>(root);
            Sql("BEGIN IMMEDIATE");
            scheduler = std::make_unique<NodeScheduler>(*this);
            Sql("COMMIT");
            if (List("user").empty() &&
                GetFileAttributesW((root + L"\\bootstrap.key").c_str()) == INVALID_FILE_ATTRIBUTES)
                AtomicWrite(root + L"\\bootstrap.key", RandomId() + RandomId());
        }
        catch (...)
        {
            sqlite3_close(database);
            database = nullptr;
            throw;
        }
    }
    // 用途：在所有事务和后台工作结束后关闭数据库；日期：2026-11-05。
    ~Impl()
    {
        sqlite3_close(database);
    }
    // 用途：执行内部固定SQL语句，不接收用户SQL；日期：2026-11-05。
    void Sql(const char *sql)
    {
        if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw Error("ADMIN_DATABASE", "Control-plane transaction failed");
    }
    // 用途：读取指定类型与唯一业务键，不存在返回空对象；日期：2026-11-05。
    Json Get(const std::string &kind, const std::string &id)
    {
        Statement query(database, "SELECT body FROM records WHERE kind=? AND id=?");
        query.Bind(1, kind);
        query.Bind(2, id);
        return query.Step() ? ParseExternalJson(query.String(0)) : Json::object();
    }
    // 用途：要求资源存在并屏蔽底层数据库细节；日期：2026-11-05。
    Json Require(const std::string &kind, const std::string &id)
    {
        const auto row = Get(kind, id);
        if (row.empty())
            throw Error("NOT_FOUND", "Requested resource does not exist");
        return row;
    }
    // 用途：参数化写入业务对象，由外围立即事务统一提交；日期：2026-11-05。
    void Put(const std::string &kind, const std::string &id, const Json &body)
    {
        Statement query(database, "INSERT INTO records(kind,id,body) VALUES(?,?,?) "
                                  "ON CONFLICT(kind,id) DO UPDATE SET body=excluded.body");
        query.Bind(1, kind);
        query.Bind(2, id);
        query.Bind(3, body.dump());
        query.Step();
    }
    // 用途：按类型与租户在SQL内筛选记录，超出预算要求先导出归档；日期：2026-11-05。
    Json List(const std::string &kind, const std::string &tenant = "")
    {
        Statement query(database, tenant.empty()
                                      ? "SELECT body FROM records WHERE kind=? ORDER BY rowid LIMIT 10001"
                                      : "SELECT body FROM records WHERE kind=? AND "
                                        "json_extract(body,'$.tenantId')=? ORDER BY rowid LIMIT 10001");
        query.Bind(1, kind);
        if (!tenant.empty())
            query.Bind(2, tenant);
        Json result = Json::array();
        while (query.Step())
            result.push_back(ParseExternalJson(query.String(0)));
        if (result.size() > 10000)
            throw Error(
                "ADMIN_CAPACITY",
                "This single-node edition supports 10000 records per resource; export before continuing");
        return result;
    }
    // 用途：原子追加不含密码、令牌和正文的审计事件；日期：2026-11-05。
    void Audit(const std::string &actor, const std::string &action, const std::string &tenant,
               const std::string &resource)
    {
        const auto id = RandomId();
        Put("audit", id,
            {{"id", id},
             {"actor", actor},
             {"action", action},
             {"tenantId", tenant},
             {"resourceId", resource},
             {"at", Now()}});
    }
    // 用途：根据有效授权计算免费/付费环境、并发和席位，退款授权立即失效；日期：2026-11-05。
    Json Entitlement(const std::string &tenant)
    {
        Json result{{"environmentLimit", 10}, {"concurrentLimit", 10}, {"seatLimit", 1},
                    {"proxyAllowed", true},   {"rpaFree", true},       {"localApiFree", true},
                    {"aiTestingFree", true},  {"paid", false},         {"expiresAt", 0},
                    {"revision", 1}};
        const auto wallet = Get("wallet", tenant);
        result["timeBalanceSeconds"] = wallet.value("seconds", 0LL);
        result["subscriptionPaid"] = false;
        for (const auto &grant : List("grant", tenant))
        {
            if (grant.at("state") != "active" || grant.at("start") > Now() || grant.at("end") <= Now())
                continue;
            const bool prepaid = grant.at("plan").value("billingMode", "subscription") == "prepaid";
            if (prepaid && result.at("timeBalanceSeconds").get<long long>() <= 0)
            {
                bool held = false;
                for (const auto &slot : List("slot", tenant))
                    if (slot.at("state") != "released" && slot.value("billingModel", "") == "prepaid-seconds")
                        held = true;
                if (!held)
                    continue;
            }
            if (!prepaid)
                result["subscriptionPaid"] = true;
            for (const auto *field : {"environmentLimit", "concurrentLimit", "seatLimit"})
                result[field] = (std::max)(result.at(field).get<long long>(),
                                           grant.at("plan").at(field).get<long long>());
            result["paid"] = true;
            result["expiresAt"] =
                (std::max)(result.at("expiresAt").get<long long>(), grant.at("end").get<long long>());
            result["revision"] = grant.at("end");
        }
        return result;
    }
    // 用途：累计已激活成员和未过期邀请，防止并发管理员超售席位；日期：2026-11-05。
    long long OccupiedSeats(const std::string &tenant)
    {
        long long total = 0;
        for (const auto &member : List("member", tenant))
            if (member.at("state") == "active")
                ++total;
        for (const auto &invite : List("invite", tenant))
            if (invite.at("state") == "pending" && invite.at("expiresAt") > Now())
                ++total;
        return total;
    }
    // 用途：建立不可变双分录，人民币分总和必须为零；日期：2026-11-05。
    void Ledger(const Json &order, const std::string &event, bool refund)
    {
        const auto amount = order.at("amountMinor").get<long long>() * (refund ? -1 : 1);
        const Json entries = Json::array({{{"account", "cash"}, {"deltaMinor", amount}},
                                          {{"account", "customer_receipts"}, {"deltaMinor", -amount}}});
        Put("ledger", event,
            {{"id", event},
             {"tenantId", order.at("tenantId")},
             {"orderId", order.at("id")},
             {"currency", "CNY"},
             {"entries", entries},
             {"at", Now()}});
    }
    // 用途：原子改变预付秒余额并追加不可变用量分录，负余额阻止新的付费准入；日期：2026-11-05。
    long long TimeChange(const std::string &tenant, long long delta, const std::string &resource,
                         const std::string &reason)
    {
        auto wallet = Get("wallet", tenant);
        const auto next = wallet.value("seconds", 0LL) + delta;
        if (next > 1000000000000LL || next < -1000000000000LL)
            throw Error("TIME_BALANCE_LIMIT", "Time balance exceeds supported range");
        Put("wallet", tenant, {{"tenantId", tenant}, {"seconds", next}});
        const auto id = RandomId();
        Put("time_ledger", id,
            {{"id", id},
             {"tenantId", tenant},
             {"resourceId", resource},
             {"deltaSeconds", delta},
             {"balanceSeconds", next},
             {"reason", reason},
             {"at", Now()}});
        return next;
    }
    // 用途：官方退款发出前原子预留时长，未知渠道结果期间不重复释放；日期：2026-11-05。
    Json PrepareRefund(Json order) override
    {
        if (order.at("plan").value("billingMode", "subscription") == "prepaid" &&
            !order.contains("refundReservedSeconds"))
        {
            const auto seconds =
                order.at("plan").at("timeSeconds").get<long long>() * order.at("quantity").get<long long>();
            if (Get("wallet", order.at("tenantId")).value("seconds", 0LL) < seconds)
                throw Error("REFUND_TIME_USED",
                            "Consumed or reserved time cannot be refunded through the official channel");
            TimeChange(order.at("tenantId"), -seconds, order.at("id"), "reserve-official-refund");
            order["refundReservedSeconds"] = seconds;
            Put("order", order.at("id"), order);
        }
        return order;
    }
    // 用途：支付与退款在同一事务内改变订单、不可变分录和授权；日期：2026-11-05。
    Json Settle(Json order, const std::string &transaction, bool refund, bool reconciled = false)
    {
        const auto key = Digest(transaction);
        const auto previous = Get("transaction", key);
        if (!previous.empty())
        {
            if (previous.at("orderId") != order.at("id") || previous.at("refund") != refund)
                throw Error("PAYMENT_CONFLICT", "Transaction is already bound to a different payment");
            return order;
        }
        if (reconciled && !refund && order.at("state") == "cancelled")
            order["state"] = "pending";
        if (order.at("state") != (refund ? "refund_pending" : "pending"))
            throw Error("ORDER_STATE", "Order is not eligible for this settlement");
        if (!refund && !reconciled && order.at("expiresAt") < Now())
            throw Error("ORDER_EXPIRED", "Expired order requires reconciliation before acceptance");
        if (refund)
        {
            auto grant = Require("grant", order.at("id"));
            if (order.at("plan").value("billingMode", "subscription") == "prepaid")
            {
                const auto seconds = order.at("plan").at("timeSeconds").get<long long>() *
                                     order.at("quantity").get<long long>();
                if (!order.contains("refundReservedSeconds") && !reconciled &&
                    Get("wallet", order.at("tenantId")).value("seconds", 0LL) < seconds)
                    throw Error("REFUND_TIME_USED",
                                "Time credit is consumed or reserved; full pack refund is unavailable");
                if (!order.contains("refundReservedSeconds"))
                    TimeChange(order.at("tenantId"), -seconds, order.at("id"), "refund-time-pack");
            }
            grant["state"] = "revoked";
            Put("grant", order.at("id"), grant);
            auto invoice = Get("invoice", order.at("id"));
            if (!invoice.empty())
            {
                invoice["state"] = invoice.at("state") == "issued" ? "credit_note_required" : "cancelled";
                Put("invoice", order.at("id"), invoice);
            }
            order["state"] = "refunded";
            order["refundedAt"] = Now();
        }
        else
        {
            long long start = Now();
            const bool prepaid = order.at("plan").value("billingMode", "subscription") == "prepaid";
            if (prepaid)
                TimeChange(order.at("tenantId"),
                           order.at("plan").at("timeSeconds").get<long long>() *
                               order.at("quantity").get<long long>(),
                           order.at("id"), "purchase-time-pack");
            for (const auto &grant : List("grant", order.at("tenantId")))
                if (!prepaid && grant.at("plan").value("billingMode", "subscription") == "subscription" &&
                    grant.at("state") == "active" && grant.at("end") > Now())
                {
                    if (grant.at("plan").at("productId") != order.at("plan").at("productId"))
                        throw Error("PLAN_CONFLICT", "Active subscriptions must renew the same product");
                    start = (std::max)(start, grant.at("end").get<long long>());
                }
            const auto end = start + order.at("plan").at("days").get<long long>() *
                                         order.at("quantity").get<long long>() * 86400;
            Put("grant", order.at("id"),
                {{"id", order.at("id")},
                 {"tenantId", order.at("tenantId")},
                 {"plan", order.at("plan")},
                 {"start", start},
                 {"end", end},
                 {"state", "active"}});
            order["state"] = "paid";
            order["paidAt"] = Now();
        }
        order[refund ? "refundTransaction" : "paymentTransaction"] = transaction;
        Put("order", order.at("id"), order);
        Put("transaction", key, {{"orderId", order.at("id")}, {"refund", refund}});
        Ledger(order, key, refund);
        return order;
    }
    // 用途：创建有盐密码账号、默认组织和唯一所有者，禁止硬编码测试口令；日期：2026-11-05。
    Json CreateUser(const Json &body, bool platform)
    {
        const auto email = Email(body);
        if (!Get("email", email).empty())
            throw Error("ACCOUNT_EXISTS", "Account already exists");
        const auto password = Text(body, "password", 256);
        if (UnicodeLength(password) < 12)
            throw Error("PASSWORD_POLICY", "Password must contain at least 12 characters");
        const auto id = RandomId();
        const auto tenant = RandomId();
        const auto salt = RandomId();
        const Json user{{"id", id},
                        {"email", email},
                        {"name", Text(body, "name")},
                        {"salt", salt},
                        {"passwordHash", Digest(password, salt, true)},
                        {"platform", platform},
                        {"state", "active"},
                        {"defaultTenant", tenant},
                        {"failed", 0},
                        {"lockedUntil", 0}};
        Put("user", id, user);
        Put("email", email, {{"userId", id}});
        Put("tenant", tenant,
            {{"id", tenant}, {"name", user.at("name")}, {"state", "active"}, {"createdAt", Now()}});
        Put("member", tenant + ":" + id,
            {{"id", tenant + ":" + id},
             {"tenantId", tenant},
             {"userId", id},
             {"email", email},
             {"role", "owner"},
             {"state", "active"}});
        Audit(id, "account.create", tenant, id);
        return Session(user);
    }
    // 用途：签发八小时会话，仅在本次响应返回明文随机令牌；日期：2026-11-05。
    Json Session(const Json &user)
    {
        const auto token = RandomId() + RandomId();
        const auto expires = Now() + 8 * 3600;
        Put("session", Digest(token), {{"userId", user.at("id")}, {"expiresAt", expires}});
        return {{"token", token},
                {"expiresAt", expires},
                {"tenantId", user.at("defaultTenant")},
                {"userId", user.at("id")},
                {"platform", user.at("platform")}};
    }
    // 用途：验证会话时重新读取账号，停用账号不能继续使用旧会话；日期：2026-11-05。
    Json Authenticate(const std::string &token)
    {
        if (token.empty() || token.size() != 64)
            throw Error("UNAUTHORIZED", "Login required");
        const auto session = Get("session", Digest(token));
        if (session.empty() || session.at("expiresAt") <= Now())
            throw Error("UNAUTHORIZED", "Session expired or revoked");
        const auto user = Require("user", session.at("userId"));
        if (user.at("state") != "active")
            throw Error("FORBIDDEN", "Account disabled");
        return user;
    }
    // 用途：校验租户资源归属，杜绝通过业务ID跨租户读取订单或成员；日期：2026-11-05。
    Json Owned(const std::string &kind, const std::string &id, const std::string &tenant)
    {
        auto row = Require(kind, id);
        if (row.at("tenantId") != tenant)
            throw Error("FORBIDDEN", "Resource belongs to another organization");
        return row;
    }
    // 用途：限定财务与管理员业务能力，平台管理员不自动冒充租户成员；日期：2026-11-05。
    void Permission(const std::string &role, bool finance = false)
    {
        if (role != "owner" && role != "admin" && !(finance && role == "finance"))
            throw Error("FORBIDDEN", "Role does not permit this operation");
    }
    // 用途：释放确认停止的运行槽并生成秒级用量，未确认的槽保持占用；日期：2026-11-05。
    Json Release(Json slot, const std::string &reason)
    {
        if (slot.at("state") == "released")
            return slot;
        slot["state"] = "released";
        slot["endedAt"] = Now();
        slot["seconds"] = (std::max)(0LL, Now() - slot.at("startedAt").get<long long>());
        slot["releaseReason"] = reason;
        if (slot.value("billingModel", "") == "prepaid-seconds")
        {
            const auto adjustment =
                slot.at("reservedSeconds").get<long long>() - slot.at("seconds").get<long long>();
            TimeChange(slot.at("tenantId"), adjustment, slot.at("id"), "settle-stopped-usage");
            slot["chargedSeconds"] = slot.at("seconds");
        }
        Put("slot", slot.at("id"), slot);
        Put("usage", slot.at("id"), slot);
        return slot;
    }
    // 用途：在SQL保存点中原子准入，失败回滚所有子写入且立即吊销内部会话；日期：2026-11-05。
    Json AdmitAction(const Json &user, const std::string &action, const Json &body) override
    {
        Sql("SAVEPOINT node_admission");
        try
        {
            const auto session = Session(user);
            const auto token = session.at("token").get<std::string>();
            const auto result = Action(action, body, token);
            Put("session", Digest(token), {{"userId", user.at("id")}, {"expiresAt", 0}});
            Sql("RELEASE node_admission");
            return result;
        }
        catch (...)
        {
            Sql("ROLLBACK TO node_admission; RELEASE node_admission");
            throw;
        }
    }
    // 用途：处理单个业务动作，外围事务同时覆盖鉴权、席位、账务和审计；日期：2026-11-05。
    Json Action(const std::string &action, const Json &body, const std::string &token)
    {
        if (!body.is_object() || body.dump().size() > 768 * 1024)
            throw Error("INVALID_ARGUMENTS", "Expected bounded object body");
        if (action.find("oidc.") == 0 || action.find("client.code.") == 0)
            return identity->Action(*this, action, body, token);
        if (action.find("node.") == 0 || action.find("project.") == 0 || action.find("assignment.") == 0 ||
            action.find("run.") == 0 || action == "scheduler.snapshot" || action == "workflow.remote.publish")
            return scheduler->Action(*this, action, body, token);
        if (action == "status")
            return {{"initialized", !List("user").empty()},
                    {"version", 1},
                    {"paymentGatewayConfigured", config.value("enabled", false)},
                    {"manualPayments", true},
                    {"officialPayments", payments->Status()},
                    {"oidc", identity->Status()},
                    {"scheduler", {{"protocolVersion", 1}}}};
        if (action == "bootstrap")
        {
            if (!List("user").empty())
                throw Error("BOOTSTRAP_CLOSED", "Initialization is already complete");
            if (!Equal(Text(body, "key", 64), ReadFileBounded(root + L"\\bootstrap.key", 64)))
                throw Error("FORBIDDEN", "Bootstrap key is invalid");
            return CreateUser(body, true);
        }
        if (action == "register")
        {
            if (List("user").empty())
                throw Error("BOOTSTRAP_REQUIRED", "Initialize platform administrator first");
            return CreateUser(body, false);
        }
        if (action == "login")
        {
            const auto email = Email(body);
            const auto index = Get("email", email);
            auto user = index.empty() ? Json::object() : Require("user", index.at("userId"));
            const auto supplied =
                Digest(Text(body, "password", 256), user.value("salt", "unknown-salt"), true);
            if (user.empty() || user.at("state") != "active" || user.at("lockedUntil") > Now() ||
                !user.value("passwordEnabled", true) || !Equal(supplied, user.value("passwordHash", "")))
            {
                if (!user.empty() && user.at("lockedUntil") <= Now())
                {
                    user["failed"] = user.at("failed").get<int>() + 1;
                    if (user.at("failed") >= 5)
                    {
                        user["lockedUntil"] = Now() + 300;
                        user["failed"] = 0;
                    }
                    Put("user", user.at("id"), user);
                }
                // 业务失败返回值仍提交锁定计数，不允许回滚绕过限流。
                return {{"authenticationFailed", true},
                        {"code", "UNAUTHORIZED"},
                        {"message", "Invalid login or temporarily locked"}};
            }
            user["failed"] = 0;
            user["lockedUntil"] = 0;
            Put("user", user.at("id"), user);
            Audit(user.at("id"), "login", user.at("defaultTenant"), user.at("id"));
            return Session(user);
        }
        const auto user = Authenticate(token);
        const auto actor = user.at("id").get<std::string>();
        const bool platform = user.at("platform");
        if (action == "logout")
        {
            Put("session", Digest(token), {{"userId", actor}, {"expiresAt", 0}});
            return {{"loggedOut", true}};
        }
        if (action == "identity")
        {
            Json tenants = Json::array();
            for (const auto &member : List("member"))
                if (member.at("userId") == actor && member.at("state") == "active")
                {
                    auto tenant = Require("tenant", member.at("tenantId"));
                    tenant["role"] = member.at("role");
                    tenants.push_back(tenant);
                }
            return {
                {"userId", actor}, {"email", user.at("email")}, {"platform", platform}, {"tenants", tenants}};
        }
        if (action == "password.change")
        {
            if (!Equal(Digest(Text(body, "oldPassword", 256), user.at("salt"), true),
                       user.at("passwordHash")))
                throw Error("FORBIDDEN", "Current password is incorrect");
            const auto password = Text(body, "password", 256);
            if (UnicodeLength(password) < 12)
                throw Error("PASSWORD_POLICY", "Password must contain at least 12 characters");
            auto changed = user;
            changed["salt"] = RandomId();
            changed["passwordHash"] = Digest(password, changed.at("salt"), true);
            Put("user", actor, changed);
            // 会话表没有明文令牌；SQL按账号一次吊销所有旧会话。
            Statement revoke(database, "UPDATE records SET body=json_set(body,'$.expiresAt',0) WHERE "
                                       "kind='session' AND json_extract(body,'$.userId')=?");
            revoke.Bind(1, actor);
            revoke.Step();
            Audit(actor, action, "", actor);
            return Session(changed);
        }
        if (action == "platform.snapshot" || action == "plan.save" || action == "plan.retire" ||
            action == "tenant.state" || action == "user.state" || action == "payment.confirm" ||
            action == "refund.confirm" || action == "payment.reconcile" || action == "invoice.issue" ||
            action == "invoice.credit")
        {
            if (!platform)
                throw Error("FORBIDDEN", "Platform operator required");
            if (action == "platform.snapshot")
            {
                Json users = Json::array();
                for (auto row : List("user"))
                {
                    row.erase("passwordHash");
                    row.erase("salt");
                    users.push_back(row);
                }
                long long balance = 0;
                for (const auto &line : List("ledger"))
                {
                    long long sum = 0;
                    for (const auto &entry : line.at("entries"))
                        sum += entry.at("deltaMinor").get<long long>();
                    if (sum != 0)
                        throw Error("LEDGER_UNBALANCED", "Ledger needs reconciliation");
                    balance += line.at("entries").at(0).at("deltaMinor").get<long long>();
                }
                return {{"tenants", List("tenant")},
                        {"users", users},
                        {"plans", List("plan")},
                        {"orders", List("order")},
                        {"invoices", List("invoice")},
                        {"ledger", List("ledger")},
                        {"events", List("event")},
                        {"paymentReviews", List("payment_review")},
                        {"timeLedger", List("time_ledger")},
                        {"audit", List("audit")},
                        {"reconciliation",
                         {{"balanced", true}, {"netReceiptsMinor", balance}, {"currency", "CNY"}}}};
            }
            if (action == "plan.save")
            {
                const auto product = Text(body, "productId", 64);
                Json row{{"id", RandomId()},
                         {"productId", product},
                         {"name", Text(body, "name")},
                         {"amountMinor", Number(body, "amountMinor", 1, 100000000)},
                         {"currency", "CNY"},
                         {"days", Number(body, "days", 1, 3650)},
                         {"environmentLimit", Number(body, "environmentLimit", 10, 100000)},
                         {"concurrentLimit", Number(body, "concurrentLimit", 1, 10000)},
                         {"seatLimit", Number(body, "seatLimit", 1, 10000)},
                         {"published", true},
                         {"createdAt", Now()}};
                const auto mode = body.value("billingMode", "subscription");
                if (mode != "subscription" && mode != "prepaid")
                    throw Error("INVALID_ARGUMENTS", "Billing mode must be subscription or prepaid");
                row["billingMode"] = mode;
                row["timeSeconds"] = mode == "prepaid" ? Number(body, "timeSeconds", 300, 315360000) : 0;
                if (row.at("concurrentLimit") > row.at("environmentLimit"))
                    throw Error("INVALID_ARGUMENTS", "Concurrent limit cannot exceed environment limit");
                for (auto plan : List("plan"))
                    if (plan.at("productId") == product && plan.at("published") == true)
                    {
                        plan["published"] = false;
                        Put("plan", plan.at("id"), plan);
                    }
                Put("plan", row.at("id"), row);
                Audit(actor, action, "", row.at("id"));
                return row;
            }
            if (action == "plan.retire")
            {
                auto row = Require("plan", Text(body, "id"));
                row["published"] = false;
                Put("plan", row.at("id"), row);
                Audit(actor, action, "", row.at("id"));
                return row;
            }
            if (action == "tenant.state" || action == "user.state")
            {
                const auto kind = action == "tenant.state" ? "tenant" : "user";
                auto row = Require(kind, Text(body, "id"));
                const auto state = Text(body, "state");
                if (state != "active" && state != "suspended")
                    throw Error("INVALID_ARGUMENTS", "State must be active or suspended");
                if (action == "user.state" && row.at("platform") == true)
                    throw Error("FORBIDDEN", "Platform account cannot be suspended by this action");
                row["state"] = state;
                Put(kind, row.at("id"), row);
                Audit(actor, action, action == "tenant.state" ? row.at("id").get<std::string>() : "",
                      row.at("id"));
                return {{"id", row.at("id")}, {"state", state}};
            }
            if (action == "payment.confirm" || action == "refund.confirm" || action == "payment.reconcile")
            {
                auto order = Require("order", Text(body, "orderId"));
                if (Number(body, "amountMinor", 1, 1200000000) != order.at("amountMinor"))
                    throw Error("PAYMENT_AMOUNT", "Amount must exactly match order snapshot");
                Text(body, "reason", 1024);
                std::string transaction = "manual:" + Text(body, "receipt", 128);
                if (action == "payment.reconcile")
                {
                    auto review = Require("payment_review", Text(body, "reviewId"));
                    if (review.at("orderId") != order.at("id") || review.at("state") != "pending")
                        throw Error("PAYMENT_CONFLICT", "Review does not match the pending settlement");
                    transaction = review.at("transaction");
                    review["state"] = "resolved";
                    Put("payment_review", review.at("id"), review);
                }
                order = Settle(order, transaction, action == "refund.confirm", action == "payment.reconcile");
                Audit(actor, action, order.at("tenantId"), order.at("id"));
                return order;
            }
            auto row = Require("invoice", Text(body, "id"));
            if (action == "invoice.credit")
            {
                if (row.at("state") != "credit_note_required")
                    throw Error("INVOICE_STATE", "Only refunded issued invoices require a credit note");
                row["state"] = "credited";
                row["creditNoteNumber"] = Text(body, "creditNoteNumber");
                Put("invoice", row.at("id"), row);
                Audit(actor, action, row.at("tenantId"), row.at("id"));
                return row;
            }
            if (row.at("state") != "requested")
                throw Error("INVOICE_STATE", "Invoice has already been issued");
            row["state"] = "issued";
            row["invoiceNumber"] = Text(body, "invoiceNumber");
            Put("invoice", row.at("id"), row);
            Audit(actor, action, row.at("tenantId"), row.at("id"));
            return row;
        }
        if (action == "member.accept")
        {
            auto invite = Require("invite", Digest(Text(body, "invitationToken", 64)));
            if (invite.at("email") != user.at("email") || invite.at("state") != "pending" ||
                invite.at("expiresAt") <= Now())
                throw Error("INVITATION_INVALID",
                            "Invitation expired, cancelled or belongs to another email");
            const auto tenant = invite.at("tenantId").get<std::string>();
            if (Require("tenant", tenant).at("state") != "active" ||
                OccupiedSeats(tenant) > Entitlement(tenant).at("seatLimit").get<long long>())
                throw Error("SEAT_LIMIT", "Organization is suspended or seat entitlement expired");
            auto member = Get("member", tenant + ":" + actor);
            if (!member.empty() && member.at("state") == "active")
                throw Error("MEMBER_EXISTS", "Already an active member");
            member = {{"id", tenant + ":" + actor}, {"tenantId", tenant},        {"userId", actor},
                      {"email", user.at("email")},  {"role", invite.at("role")}, {"state", "active"}};
            Put("member", member.at("id"), member);
            invite["state"] = "accepted";
            Put("invite", invite.at("id"), invite);
            Audit(actor, action, tenant, actor);
            return member;
        }
        const auto tenant = body.value("tenantId", user.at("defaultTenant").get<std::string>());
        const auto member = Get("member", tenant + ":" + actor);
        if (member.empty() || member.at("state") != "active")
            throw Error("FORBIDDEN", "Active organization membership required");
        const auto tenant_row = Require("tenant", tenant);
        const auto role = member.at("role").get<std::string>();
        const auto entitlement = Entitlement(tenant);
        if (action == "snapshot")
        {
            Json plans = Json::array();
            for (const auto &plan : List("plan"))
                if (plan.at("published") == true)
                    plans.push_back(plan);
            Json result{{"tenant", tenant_row},
                        {"role", role},
                        {"entitlement", entitlement},
                        {"occupiedSeats", OccupiedSeats(tenant)},
                        {"plans", plans},
                        {"members", List("member", tenant)},
                        {"devices", List("device", tenant)},
                        {"slots", List("slot", tenant)}};
            if (role == "owner" || role == "admin" || role == "finance")
                for (const auto *kind :
                     {"order", "grant", "ledger", "usage", "invoice", "invite", "time_ledger"})
                    result[kind] = List(kind, tenant);
            return result;
        }
        if (tenant_row.at("state") != "active" && action != "slot.release" && action != "slot.resolve" &&
            action != "slot.lookup")
            throw Error("TENANT_SUSPENDED", "Organization is suspended");
        if (action == "member.invite")
        {
            Permission(role);
            const auto email = Email(body);
            const auto invited_role = Role(body);
            if (invited_role == "owner" && role != "owner")
                throw Error("FORBIDDEN", "Only owner can invite another owner");
            if (OccupiedSeats(tenant) >= entitlement.at("seatLimit").get<long long>())
                throw Error("SEAT_LIMIT", "All licensed seats are assigned or reserved");
            for (const auto &row : List("member", tenant))
                if (row.at("email") == email && row.at("state") == "active")
                    throw Error("MEMBER_EXISTS", "Member already exists");
            for (const auto &row : List("invite", tenant))
                if (row.at("email") == email && row.at("state") == "pending" && row.at("expiresAt") > Now())
                    throw Error("INVITATION_EXISTS", "An active invitation already reserves this email");
            const auto secret = RandomId() + RandomId();
            const auto id = Digest(secret);
            Json row{{"id", id},           {"tenantId", tenant},
                     {"email", email},     {"role", invited_role},
                     {"state", "pending"}, {"expiresAt", Now() + 7 * 86400}};
            Put("invite", id, row);
            Audit(actor, action, tenant, id);
            row["invitationToken"] = secret;
            return row;
        }
        if (action == "invite.cancel")
        {
            Permission(role);
            auto row = Owned("invite", Text(body, "id"), tenant);
            if (row.at("state") != "pending")
                throw Error("INVITATION_INVALID", "Invitation is no longer pending");
            row["state"] = "cancelled";
            Put("invite", row.at("id"), row);
            Audit(actor, action, tenant, row.at("id"));
            return row;
        }
        if (action == "member.update")
        {
            Permission(role);
            auto row = Owned("member", Text(body, "id"), tenant);
            const auto next_role = Role(body);
            const auto state = Text(body, "state");
            if (state != "active" && state != "disabled")
                throw Error("INVALID_ARGUMENTS", "Member state must be active or disabled");
            if ((row.at("role") == "owner" || next_role == "owner") && role != "owner")
                throw Error("FORBIDDEN", "Only owners may change owner membership");
            if (row.at("role") == "owner" && row.at("state") == "active" &&
                (state != "active" || next_role != "owner"))
            {
                size_t owners = 0;
                for (const auto &other : List("member", tenant))
                    if (other.at("role") == "owner" && other.at("state") == "active")
                        ++owners;
                if (owners <= 1)
                    throw Error("LAST_OWNER", "Organization must retain an active owner");
            }
            if (row.at("state") != "active" && state == "active" &&
                OccupiedSeats(tenant) >= entitlement.at("seatLimit").get<long long>())
                throw Error("SEAT_LIMIT", "No licensed seat available");
            row["role"] = next_role;
            row["state"] = state;
            Put("member", row.at("id"), row);
            Audit(actor, action, tenant, row.at("id"));
            return row;
        }
        if (action == "order.create")
        {
            Permission(role, true);
            const auto key = tenant + ":" + Text(body, "idempotencyKey");
            const auto plan_id = Text(body, "planId");
            const auto quantity = Number(body, "quantity", 1, 12);
            const auto prior = Get("order_key", key);
            if (!prior.empty())
            {
                if (prior.at("planId") != plan_id || prior.at("quantity") != quantity)
                    throw Error("IDEMPOTENCY_CONFLICT", "Order key was used with different arguments");
                return Require("order", prior.at("id"));
            }
            const auto plan = Require("plan", plan_id);
            if (plan.at("published") != true)
                throw Error("PLAN_RETIRED", "Product version is no longer available");
            for (const auto &grant : List("grant", tenant))
                if (plan.value("billingMode", "subscription") == "subscription" &&
                    grant.at("plan").value("billingMode", "subscription") == "subscription" &&
                    grant.at("state") == "active" && grant.at("end") > Now() &&
                    grant.at("plan").at("productId") != plan.at("productId"))
                    throw Error("PLAN_CONFLICT", "Renew the active product or wait for expiry");
            const auto id = RandomId();
            const Json order{{"id", id},
                             {"tenantId", tenant},
                             {"plan", plan},
                             {"quantity", quantity},
                             {"amountMinor", plan.at("amountMinor").get<long long>() * quantity},
                             {"currency", "CNY"},
                             {"state", "pending"},
                             {"createdAt", Now()},
                             {"expiresAt", Now() + 86400}};
            Put("order", id, order);
            Put("order_key", key, {{"id", id}, {"planId", plan_id}, {"quantity", quantity}});
            Audit(actor, action, tenant, id);
            return order;
        }
        if (action == "payment.prepare" || action == "order.cancel" || action == "refund.request" ||
            action == "invoice.request")
        {
            Permission(role, true);
            auto order = Owned("order", Text(body, "orderId"), tenant);
            if (action == "payment.prepare")
            {
                if (order.at("state") != "pending" || order.at("expiresAt") <= Now())
                    throw Error("ORDER_STATE", "Order is not payable");
                if (body.value("channel", "manual") == "manual")
                    return {
                        {"channel", "manual"},
                        {"orderId", order.at("id")},
                        {"amountMinor", order.at("amountMinor")},
                        {"instructions",
                         "Operator must verify actual receipt and confirm using a unique bank reference"}};
                if (body.at("channel") != "gateway")
                    throw Error("INVALID_ARGUMENTS", "Channel must be manual or gateway");
                if (!config.value("enabled", false))
                    throw Error("PAYMENT_NOT_CONFIGURED",
                                "Configure merchant and signed-checkout gateway first");
                const auto payload = Json{{"merchantId", config.at("merchantId")},
                                          {"orderId", order.at("id")},
                                          {"amountMinor", order.at("amountMinor")},
                                          {"currency", "CNY"},
                                          {"expiresAt", order.at("expiresAt")}}
                                         .dump();
                const auto base = Text(config, "checkoutBaseUrl", 2048);
                // payload采用十六进制编码，避免URL注入，接入网关按协议验证签名。
                const std::vector<unsigned char> bytes(payload.begin(), payload.end());
                return {{"channel", "gateway"},
                        {"orderId", order.at("id")},
                        {"checkoutUrl", base + (base.find('?') == std::string::npos ? "?" : "&") +
                                            "payload=" + Hex(bytes) +
                                            "&signature=" + Digest(payload, config.at("webhookSecret"))}};
            }
            if (action == "order.cancel")
            {
                if (order.at("state") != "pending")
                    throw Error("ORDER_STATE", "Only unpaid orders can be cancelled");
                order["state"] = "cancelled";
            }
            else if (action == "refund.request")
            {
                if (order.at("state") != "paid")
                    throw Error("ORDER_STATE", "Only paid orders can request a full refund");
                order["state"] = "refund_pending";
                order["refundReason"] = Text(body, "reason", 1024);
            }
            else
            {
                if (order.at("state") != "paid")
                    throw Error("ORDER_STATE", "Only paid orders can request invoices");
                const auto existing = Get("invoice", order.at("id"));
                if (!existing.empty())
                    return existing;
                const Json invoice{{"id", order.at("id")},
                                   {"tenantId", tenant},
                                   {"orderId", order.at("id")},
                                   {"amountMinor", order.at("amountMinor")},
                                   {"title", Text(body, "title", 256)},
                                   {"taxId", Text(body, "taxId", 64)},
                                   {"state", "requested"},
                                   {"createdAt", Now()}};
                Put("invoice", order.at("id"), invoice);
                Audit(actor, action, tenant, order.at("id"));
                return invoice;
            }
            Put("order", order.at("id"), order);
            Audit(actor, action, tenant, order.at("id"));
            return order;
        }
        if (action == "device.register")
        {
            const auto id = Text(body, "deviceId", 64);
            const auto key = tenant + ":" + actor + ":" + id;
            const Json row{{"id", key},
                           {"deviceId", id},
                           {"tenantId", tenant},
                           {"userId", actor},
                           {"name", Text(body, "name")},
                           {"lastSeenAt", Now()},
                           {"state", "active"}};
            const auto old = Get("device", key);
            if (!old.empty() && old.at("state") == "revoked")
                throw Error("DEVICE_REVOKED", "Device was revoked; use a newly authorized device ID");
            Put("device", key, row);
            Audit(actor, action, tenant, key);
            return row;
        }
        if (action == "device.revoke")
        {
            Permission(role);
            auto row = Owned("device", Text(body, "id", 256), tenant);
            row["state"] = "revoked";
            Put("device", row.at("id"), row);
            Audit(actor, action, tenant, row.at("id"));
            return row;
        }
        if (action == "environment.reserve" || action == "environment.release")
        {
            const auto device = Owned("device", Text(body, "deviceId", 256), tenant);
            if (device.at("userId") != actor || device.at("state") != "active")
                throw Error("FORBIDDEN", "Active owned device required");
            const auto profile = Text(body, "profileId", 64);
            const auto id = tenant + ":" + device.at("id").get<std::string>() + ":" + profile;
            auto row = Get("environment", id);
            if (action == "environment.reserve")
            {
                if (row.empty() || row.at("state") != "active")
                {
                    size_t active = 0;
                    for (const auto &item : List("environment", tenant))
                        if (item.at("state") == "active")
                            ++active;
                    if (active >= entitlement.at("environmentLimit").get<size_t>())
                        throw Error("ENVIRONMENT_LIMIT", "Environment entitlement exhausted");
                }
                row = {{"id", id},           {"profileId", profile},
                       {"tenantId", tenant}, {"deviceId", device.at("id")},
                       {"userId", actor},    {"state", "active"}};
            }
            else
            {
                if (row.empty())
                    return {{"released", true}};
                for (const auto &slot : List("slot", tenant))
                    if (slot.at("environmentId") == id && slot.at("state") != "released")
                        throw Error("ENVIRONMENT_RUNNING",
                                    "Release the confirmed stopped browser slot first");
                row["state"] = "released";
            }
            Put("environment", id, row);
            Audit(actor, action, tenant, id);
            return row;
        }
        if (action == "slot.lookup")
        {
            const auto key = tenant + ":" + Text(body, "idempotencyKey");
            const auto index = Get("slot_key", key);
            if (index.empty())
                return {{"found", false}};
            const auto slot = Owned("slot", index.at("id"), tenant);
            if (slot.at("userId") != actor)
                throw Error("FORBIDDEN", "Start intent is owned by another member");
            return {{"found", true}, {"slot", slot}};
        }
        if (action == "slot.acquire")
        {
            const auto environment = Owned("environment", Text(body, "environmentId", 512), tenant);
            const auto device = Require("device", environment.at("deviceId"));
            if (environment.at("state") != "active" || environment.at("userId") != actor ||
                device.at("state") != "active")
                throw Error("FORBIDDEN", "Active owned environment and device required");
            const auto key = tenant + ":" + Text(body, "idempotencyKey");
            const auto old = Get("slot_key", key);
            if (!old.empty())
            {
                auto slot = Require("slot", old.at("id"));
                if (slot.at("environmentId") != environment.at("id"))
                    throw Error("IDEMPOTENCY_CONFLICT", "Slot key belongs to another environment");
                if (slot.at("state") == "released")
                    throw Error("SLOT_RELEASED", "Use a new start key after stopping");
                return slot;
            }
            if (OccupiedSeats(tenant) > entitlement.at("seatLimit").get<long long>() && role != "owner")
                throw Error("SEAT_LIMIT", "Reclaim surplus seats after subscription expiry");
            size_t occupied = 0;
            for (const auto &slot : List("slot", tenant))
                if (slot.at("state") != "released")
                {
                    ++occupied;
                    if (slot.at("environmentId") == environment.at("id"))
                        throw Error("SLOT_BUSY", "Environment already has an active or unconfirmed slot");
                }
            if (occupied >= entitlement.at("concurrentLimit").get<size_t>())
                throw Error("CONCURRENT_LIMIT", "Global concurrent entitlement exhausted");
            const auto id = RandomId();
            Json row{{"id", id},
                     {"tenantId", tenant},
                     {"userId", actor},
                     {"environmentId", environment.at("id")},
                     {"deviceId", environment.at("deviceId")},
                     {"state", "active"},
                     {"startedAt", Now()},
                     {"lastSeenAt", Now()},
                     {"paid", entitlement.at("paid")},
                     {"billingModel", "subscription-included"}};
            if (occupied < 10)
                row["billingModel"] = "free";
            else if (!entitlement.at("subscriptionPaid").get<bool>())
            {
                if (Get("wallet", tenant).value("seconds", 0LL) < 300)
                    throw Error("TIME_BALANCE_LOW",
                                "Paid parallel use requires at least 300 prepaid seconds");
                row["billingModel"] = "prepaid-seconds";
                row["reservedSeconds"] = 300;
                TimeChange(tenant, -300, id, "reserve-runtime");
            }
            Put("slot", id, row);
            Put("slot_key", key, {{"id", id}});
            Audit(actor, action, tenant, id);
            return row;
        }
        if (action == "slot.heartbeat" || action == "slot.release" || action == "slot.resolve")
        {
            auto row = Owned("slot", Text(body, "id"), tenant);
            if (row.at("userId") != actor && role != "owner" && role != "admin")
                throw Error("FORBIDDEN", "Slot is owned by another member");
            if (action == "slot.heartbeat")
            {
                if (row.at("state") == "released")
                    throw Error("SLOT_RELEASED", "Slot has already ended");
                const auto device = Require("device", row.at("deviceId"));
                if (device.at("state") != "active")
                    throw Error("DEVICE_REVOKED", "Device authorization revoked");
                size_t position = 0;
                for (const auto &slot : List("slot", tenant))
                {
                    if (slot.at("id") == row.at("id"))
                        break;
                    if (slot.at("state") != "released")
                        ++position;
                }
                if (position >= entitlement.at("concurrentLimit").get<size_t>())
                    throw Error("ENTITLEMENT_EXPIRED", "Concurrent grant expired or was refunded");
                if (row.value("billingModel", "") == "prepaid-seconds")
                {
                    const auto elapsed = Now() - row.at("startedAt").get<long long>();
                    if (elapsed + 60 >= row.at("reservedSeconds").get<long long>())
                    {
                        if (Get("wallet", tenant).value("seconds", 0LL) < 300)
                            throw Error("TIME_EXHAUSTED",
                                        "Prepaid seconds exhausted; stop and settle this browser");
                        TimeChange(tenant, -300, row.at("id"), "extend-runtime-reservation");
                        row["reservedSeconds"] = row.at("reservedSeconds").get<long long>() + 300;
                    }
                }
                row["lastSeenAt"] = Now();
                Put("slot", row.at("id"), row);
                return row;
            }
            if (action == "slot.resolve")
                Permission(role);
            if (Text(body, "confirmStopped") != row.at("id"))
                throw Error("CONFIRMATION_REQUIRED", "Provide slot ID after verifying browser has stopped");
            row =
                Release(row, action == "slot.resolve" ? Text(body, "reason", 1024) : "client-confirmed-stop");
            Audit(actor, action, tenant, row.at("id"));
            return row;
        }
        throw Error("NOT_FOUND", "Unknown control-plane action");
    }
};

// 用途：构造独立控制面存储，避免改变现有环境数据库迁移；日期：2026-11-05。
ControlPlane::ControlPlane(const std::wstring &root) : impl_(std::make_unique<Impl>(root))
{
}
// 用途：释放控制面实现；日期：2026-11-05。
ControlPlane::~ControlPlane() = default;
// 用途：串行化立即事务，任何异常回滚整个业务动作，提交失败明确为结果未决；日期：2026-11-05。
Json ControlPlane::Execute(const std::string &action, const Json &body, const std::string &token)
{
    // 用途：单个阶段使用立即事务，外部网络阶段不持有数据库锁；日期：2026-11-05。
    const auto transaction = [&](const std::function<Json()> &operation) -> Json
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->Sql("BEGIN IMMEDIATE");
        try
        {
            const auto result = operation();
            impl_->Sql("COMMIT");
            return result;
        }
        catch (...)
        {
            sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    };
    if (!body.is_object() || body.dump().size() > 768 * 1024)
        throw Error("INVALID_ARGUMENTS", "Expected bounded object body");
    if (action == "payment.channel.create" || action == "payment.channel.query" ||
        action == "payment.channel.refund" || action == "payment.channel.refund-query")
    {
        const auto intent = transaction([&] { return impl_->payments->Begin(*impl_, action, body, token); });
        Json result;
        try
        {
            result = impl_->payments->Perform(intent);
        }
        catch (const std::exception &error)
        {
            const auto typed = dynamic_cast<const Error *>(&error);
            result = {{"state", "unknown"},
                      {"error", typed ? typed->code : "PAYMENT_PROTOCOL"},
                      {"message", "Channel result was not confirmed; query before retrying"}};
        }
        return transaction([&] { return impl_->payments->Finish(*impl_, intent, result); });
    }
    if (action == "oidc.end-session")
    {
        auto result =
            transaction([&] { return impl_->identity->Action(*impl_, "oidc.logout", body, token); });
        try
        {
            result["logoutUrl"] = impl_->identity->LogoutUrl();
        }
        catch (...)
        {
            result["logoutUrl"] = "";
            result["providerLogoutUnavailable"] = true;
        }
        return result;
    }
    if (action == "oidc.begin")
    {
        const auto discovery = impl_->identity->Discover();
        return transaction([&] { return impl_->identity->Begin(*impl_, discovery, body, token); });
    }
    if (action == "oidc.callback")
    {
        const auto context = transaction([&] { return impl_->identity->Consume(*impl_, body); });
        const auto claims = impl_->identity->Exchange(context);
        return transaction([&] { return impl_->identity->Finish(*impl_, context, claims); });
    }
    return transaction([&] { return impl_->Action(action, body, token); });
}
// 用途：事务外验签官方渠道原文，事务内原子核验订单并入账；日期：2026-11-05。
Json ControlPlane::OfficialWebhook(const std::string &channel, const std::string &body,
                                   const std::map<std::string, std::string> &headers)
{
    const auto fact = impl_->payments->Verify(channel, body, headers);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Sql("BEGIN IMMEDIATE");
    try
    {
        const auto result = impl_->payments->Apply(*impl_, fact);
        impl_->Sql("COMMIT");
        return result;
    }
    catch (...)
    {
        sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
// 用途：先验原始正文HMAC和五分钟窗口，再将回调去重、金额核验与入账放入同一事务；日期：2026-11-05。
Json ControlPlane::Webhook(const std::string &body, const std::string &timestamp,
                           const std::string &signature)
{
    if (!impl_->config.value("enabled", false))
        throw Error("PAYMENT_NOT_CONFIGURED", "Gateway is not configured");
    if (timestamp.empty() || timestamp.size() > 12 ||
        timestamp.find_first_not_of("0123456789") != std::string::npos)
        throw Error("PAYMENT_SIGNATURE", "Invalid webhook timestamp");
    const auto seconds = std::stoll(timestamp);
    if (seconds < Now() - 300 || seconds > Now() + 300 ||
        !Equal(signature, Digest(timestamp + "." + body, impl_->config.at("webhookSecret"))))
        throw Error("PAYMENT_SIGNATURE", "Invalid or expired webhook signature");
    const auto event = ParseExternalJson(body);
    const auto id = Text(event, "eventId");
    const auto order_id = Text(event, "orderId");
    const auto transaction = "gateway:" + Text(event, "transactionId");
    const auto type = Text(event, "type");
    if ((type != "paid" && type != "refunded") || Text(event, "currency") != "CNY" ||
        Text(event, "merchantId") != impl_->config.at("merchantId"))
        throw Error("PAYMENT_EVENT", "Unsupported event, merchant or currency");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Sql("BEGIN IMMEDIATE");
    try
    {
        const auto old = impl_->Get("event", id);
        if (!old.empty())
        {
            if (old.at("digest") != Digest(body))
                throw Error("PAYMENT_CONFLICT", "Event ID reused with another body");
            impl_->Sql("COMMIT");
            return {{"accepted", true}, {"duplicate", true}};
        }
        auto order = impl_->Require("order", order_id);
        if (Number(event, "amountMinor", 1, 1200000000) != order.at("amountMinor"))
            throw Error("PAYMENT_AMOUNT", "Callback amount differs from immutable order snapshot");
        const bool needs_review =
            type == "paid" && (order.at("state") == "cancelled" ||
                               (order.at("state") == "pending" && order.at("expiresAt") < Now()));
        if (needs_review)
            impl_->Put("payment_review", id,
                       {{"id", id},
                        {"orderId", order_id},
                        {"tenantId", order.at("tenantId")},
                        {"transaction", transaction},
                        {"amountMinor", order.at("amountMinor")},
                        {"state", "pending"},
                        {"reason", "verified-payment-after-order-closed"},
                        {"at", Now()}});
        else
            order = impl_->Settle(order, transaction, type == "refunded");
        impl_->Put("event", id,
                   {{"id", id},
                    {"orderId", order_id},
                    {"tenantId", order.at("tenantId")},
                    {"digest", Digest(body)},
                    {"type", type},
                    {"at", Now()}});
        impl_->Audit("payment-gateway", type, order.at("tenantId"), order_id);
        impl_->Sql("COMMIT");
        return {{"accepted", true},
                {"duplicate", false},
                {"reconciliationRequired", needs_review},
                {"orderState", order.at("state")}};
    }
    catch (...)
    {
        sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
} // namespace awb
