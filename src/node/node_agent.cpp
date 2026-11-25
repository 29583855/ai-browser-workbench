#include "awb/backend_extensions.hpp"
#include "awb/control_plane.hpp"
#include "awb/service.hpp"
#include "awb/version.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <sqlite/sqlite3.h>
#include <thread>

namespace awb
{
namespace
{
std::atomic<bool> stopping{false};
// 用途：控制台退出只请求停止，让工作线程先关闭真实浏览器；日期：2026-11-05。
BOOL WINAPI StopNode(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT)
    {
        stopping = true;
        return TRUE;
    }
    return FALSE;
}
class Journal
{
    sqlite3 *db_ = nullptr;

  public:
    // 用途：打开节点独立WAL日志并建立持久Inbox，输入正文以DPAPI保护；日期：2026-11-05。
    explicit Journal(const std::wstring &root)
    {
        if (sqlite3_open_v2(Utf8(root + L"\\node-inbox.sqlite3").c_str(), &db_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
            throw Error("NODE_JOURNAL", "Cannot open node Inbox");
        if (sqlite3_exec(db_,
                         "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS "
                         "inbox(id TEXT PRIMARY KEY, body BLOB NOT NULL);",
                         nullptr, nullptr, nullptr) != SQLITE_OK)
            throw Error("NODE_JOURNAL", "Cannot initialize node Inbox");
    }
    // 用途：退出前关闭日志句柄；日期：2026-11-05。
    ~Journal()
    {
        sqlite3_close(db_);
    }
    // 用途：在ACK之前持久保存命令或回执，失败拒绝执行；日期：2026-11-05。
    void Put(const Json &record)
    {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO inbox(id,body) VALUES(?,?)", -1, &statement,
                               nullptr) != SQLITE_OK)
            throw Error("NODE_JOURNAL", "Cannot prepare Inbox write");
        const auto id = record.at("id").get<std::string>(), bytes = ProtectBytes(record.dump());
        sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(statement, 2, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
        const auto status = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (status != SQLITE_DONE)
            throw Error("NODE_JOURNAL", "Inbox write failed; task was not acknowledged");
    }
    // 用途：恢复全部持久命令，重启后只核对停止并补发回执，不执行旧任务；日期：2026-11-05。
    Json All()
    {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT body FROM inbox ORDER BY rowid", -1, &statement, nullptr) !=
            SQLITE_OK)
            throw Error("NODE_JOURNAL", "Cannot read Inbox");
        Json rows = Json::array();
        int status = SQLITE_ROW;
        try
        {
            while ((status = sqlite3_step(statement)) == SQLITE_ROW)
            {
                const auto data = static_cast<const char *>(sqlite3_column_blob(statement, 0));
                const auto size = sqlite3_column_bytes(statement, 0);
                rows.push_back(ParseExternalJson(ProtectBytes(std::string(data, size), true)));
            }
            if (status != SQLITE_DONE)
                throw Error("NODE_JOURNAL", "Inbox read failed");
        }
        catch (...)
        {
            sqlite3_finalize(statement);
            throw;
        }
        sqlite3_finalize(statement);
        return rows;
    }
};
struct Work
{
    std::mutex mutex;
    bool allowed = true;
    bool done = false;
    bool started = false;
    bool stopped = false;
    std::string stop_reason;
    Json result;
    Json command;
    std::chrono::steady_clock::time_point expires;
    std::thread worker;
    // 用途：派发与撤销在同一锁中，过期后不发送任何新网页消息；日期：2026-11-05。
    void Dispatch(const std::function<void()> &send)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!allowed || std::chrono::steady_clock::now() >= expires ||
            BackendNow() >= command.at("payload").at("deadline"))
            throw Error("NODE_FENCED", "Central run lease was revoked or expired");
        send();
    }
    // 用途：撤销此命令，线程随后停止浏览器并保留回执；日期：2026-11-05。
    void Fence(const std::string &reason)
    {
        std::lock_guard<std::mutex> lock(mutex);
        allowed = false;
        stop_reason = reason;
    }
    // 用途：由当前epoch和generation匹配的心跳延长本地单调租约；日期：2026-11-05。
    void Grant(const Json &grant, std::chrono::steady_clock::time_point sent)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto &payload = command.at("payload");
        if (grant.at("epoch") != payload.at("epoch") || grant.at("generation") != payload.at("generation"))
        {
            allowed = false;
            stop_reason = "generation-changed";
            return;
        }
        if (!allowed || std::chrono::steady_clock::now() >= expires)
            return;
        expires = sent + std::chrono::seconds(grant.at("seconds").get<int>() - 2);
    }
};
// 用途：每个固定环境单写执行既有DSL，确认真实进程停止后才产生可结算回执；日期：2026-11-05。
void ExecuteWork(const std::shared_ptr<Work> &work, const std::wstring &root, const std::wstring &browser,
                 bool headless)
{
    Json outcome{{"state", "failed"}};
    bool stopped = false;
    std::string profile;
    std::unique_ptr<Service> service;
    try
    {
        const auto &payload = work->command.at("payload");
        const auto directory = root + L"\\assignments\\" + Wide(Field(payload, "assignmentId", 32));
        service = std::make_unique<Service>(directory, browser);
        service->SetRemoteDispatchGate([work](const std::function<void()> &send) { work->Dispatch(send); });
        auto list = service->Execute("profile_list", Json::object()).at("profiles");
        if (list.empty())
        {
            auto configuration = payload.at("configuration");
            if (configuration.value("proxyServer", "").empty())
                configuration.erase("proxyServer");
            profile = service->Execute("profile_create", configuration).at("id");
        }
        else
        {
            if (list.size() != 1)
                throw Error("NODE_PROFILE_CONFLICT", "Assignment has unexpected local environments");
            const auto &existing = list.at(0);
            const auto &configuration = payload.at("configuration");
            if (existing.at("allowedOrigins") != configuration.at("allowedOrigins") ||
                existing.value("proxyServer", "") != configuration.value("proxyServer", "") ||
                existing.value("runtimeSettings", Json::object()) !=
                    configuration.value("runtimeSettings", Json::object()))
                throw Error("NODE_PROFILE_CONFLICT",
                            "Local environment differs from the immutable assignment");
            profile = existing.at("id");
        }
        work->Dispatch([] {});
        service->Execute("browser_start", {{"profileId", profile}, {"headless", headless}});
        {
            std::lock_guard<std::mutex> lock(work->mutex);
            work->started = true;
        }
        const auto lease = service->Execute(
            "lease_acquire", {{"profileId", profile}, {"owner", "central-node"}, {"ttlMs", 300000}});
        const auto published =
            service->Execute("workflow_publish", {{"definition", payload.at("definition")}});
        const auto run = service->Execute("workflow_run", {{"profileId", profile},
                                                           {"lease", lease},
                                                           {"workflowId", published.at("workflowId")},
                                                           {"input", payload.at("input")}});
        const auto run_id = run.at("runId").get<std::string>();
        auto renewal = std::chrono::steady_clock::now();
        for (;;)
        {
            bool fenced = stopping.load();
            {
                std::lock_guard<std::mutex> lock(work->mutex);
                fenced |= !work->allowed || std::chrono::steady_clock::now() >= work->expires ||
                          BackendNow() >= payload.at("deadline");
            }
            if (fenced)
            {
                work->Fence("lease-expired-or-cancelled");
                service->Execute("workflow_control", {{"runId", run_id}, {"operation", "cancel"}});
                outcome = {{"state", "unknown"}, {"reason", "central-lease-ended"}};
                break;
            }
            const auto status = service->Execute("workflow_status", {{"runId", run_id}});
            const auto state = status.at("state").get<std::string>();
            if (state == "succeeded" || state == "failed" || state == "cancelled" ||
                state == "outcome-unknown" || state == "interrupted")
            {
                outcome = {
                    {"state", state == "outcome-unknown" || state == "interrupted" ? "unknown" : state},
                    {"localRunId", run_id}};
                break;
            }
            if (std::chrono::steady_clock::now() - renewal > std::chrono::seconds(30))
            {
                service->RenewRemoteLease(profile, lease);
                renewal = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    catch (const std::exception &error)
    {
        const auto typed = dynamic_cast<const Error *>(&error);
        outcome = {{"state", typed && typed->outcome_unknown ? "unknown" : "failed"},
                   {"error", typed ? typed->code : "NODE_EXECUTION"}};
    }
    work->Fence("finished");
    try
    {
        if (service && !profile.empty())
        {
            service->Execute("browser_stop", {{"profileId", profile}});
            const auto status = service->Execute("browser_status", {{"profileId", profile}});
            stopped = !status.at("running").get<bool>() && !status.at("recovery").value("blocked", true);
        }
        else if (service)
            stopped = true;
    }
    catch (...)
    {
        outcome = {{"state", "unknown"}, {"error", "STOP_OUTCOME_UNKNOWN"}};
    }
    service.reset();
    std::lock_guard<std::mutex> lock(work->mutex);
    work->result = outcome;
    work->stopped = stopped;
    work->done = true;
}
} // namespace

// 用途：运行独立出站节点，持久身份/序号/Inbox/回执，重启和断网不重放业务；日期：2026-11-05。
int RunNode(const std::wstring &root, const std::wstring &browser, const Json &options)
{
    const auto directory = SafeAbsolute(root);
    const auto event_name = L"Local\\AWB.Node.Stop." + Wide(Sha256(Utf8(directory)));
    if (options.value("stop", false))
    {
        Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str()));
        if (!event.get() || !SetEvent(event.get()))
            throw Error("NODE_NOT_RUNNING", "Node stop event is unavailable");
        return 0;
    }
    PrivateDirectory(directory);
    Handle owner(CreateFileW((directory + L"\\.node.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr));
    if (owner.get() == INVALID_HANDLE_VALUE)
        throw Error("ROOT_IN_USE", "Another execution node owns this root");
    Handle stop_event(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    if (!stop_event.get())
        throw Error("NODE_STOP_EVENT", "Cannot create node stop event");
    const auto path = directory + L"\\node-state.dpapi";
    Json state;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        state = ParseExternalJson(ProtectBytes(ReadFileBounded(path, 1024 * 1024), true));
    else
    {
        state = {{"url", Field(options, "url", 2048)},
                 {"name", Field(options, "name", 128)},
                 {"enrollmentToken", Field(options, "enrollmentToken", 128)},
                 {"keys", RsaGenerate()},
                 {"sequence", 0}};
        AtomicWrite(path, ProtectBytes(state.dump()));
    }
    PrivateDirectory(directory + L"\\assignments");
    Journal journal(directory);
    const auto save = [&] { AtomicWrite(path, ProtectBytes(state.dump())); };
    if (!state.contains("nodeId"))
    {
        const auto enrollment = state.at("enrollmentToken").get<std::string>();
        const auto key = state.at("keys").at("publicKey");
        const auto reply =
            ControlRequest(state.at("url"), "node.enroll",
                           {{"enrollmentToken", enrollment},
                            {"name", state.at("name")},
                            {"publicKey", key},
                            {"signature", RsaSign(state.at("keys").at("privateKey"),
                                                  "AWB-NODE-ENROLL\n" + enrollment + "\n" + key.dump() +
                                                      "\n" + state.at("name").get<std::string>())}},
                           "", options.value("clientCertificateThumbprint", ""));
        state["nodeId"] = reply.at("nodeId");
        state["credential"] = reply.at("credential");
        state["capacity"] = reply.at("capacity");
        state.erase("enrollmentToken");
        save();
    }
    const auto boot = RandomId();
    // 进程重启丢弃旧网络请求；递增序号跳过未确认请求，旧任务仅补停止回执。
    state.erase("pending");
    save();
    std::map<std::string, Json> inbox;
    for (auto row : journal.All())
    {
        if (row.value("state", "") != "reported")
        {
            const auto assignment = Field(row.at("command").at("payload"), "assignmentId", 32);
            bool confirmed = true;
            const auto local = directory + L"\\assignments\\" + Wide(assignment);
            try
            {
                Service recovery(local, browser);
                const auto recovered = recovery.Execute("profile_list", Json::object()).at("profiles");
                for (const auto &profile : recovered)
                {
                    const auto status = recovery.Execute("browser_status", {{"profileId", profile.at("id")}});
                    confirmed &=
                        !status.at("running").get<bool>() && !status.at("recovery").value("blocked", true);
                }
            }
            catch (...)
            {
                confirmed = false;
            }
            if (row.value("state", "") != "complete")
                row["result"] = {{"state", "unknown"}, {"reason", "node-restarted-no-replay"}};
            row["state"] = "complete";
            row["stoppedConfirmed"] = confirmed;
            journal.Put(row);
        }
        inbox[row.at("id")] = row;
    }
    std::map<std::string, std::shared_ptr<Work>> works;
    DWORD windows_session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &windows_session);
    bool opened = false;
    auto next_poll = std::chrono::steady_clock::now();
    stopping = false;
    SetConsoleCtrlHandler(StopNode, TRUE);
    std::cout << Json{{"ready", true}, {"nodeId", state.at("nodeId")}, {"bootId", boot}}.dump() << std::endl;
    try
    {
        while (!stopping.load() && WaitForSingleObject(stop_event.get(), 0) != WAIT_OBJECT_0)
        {
            for (auto &entry : works)
            {
                const auto work = entry.second;
                std::lock_guard<std::mutex> lock(work->mutex);
                if (work->done && inbox.at(entry.first).at("state") == "running")
                {
                    auto &row = inbox.at(entry.first);
                    row["state"] = "complete";
                    row["result"] = work->result;
                    row["stoppedConfirmed"] = work->stopped;
                    journal.Put(row);
                }
            }
            if (!state.contains("pending"))
            {
                std::string action;
                Json payload{{"bootId", boot}};
                if (!opened)
                {
                    action = "node.open";
                    payload.update({{"protocolVersion", 1},
                                    {"agentVersion", Version},
                                    {"contractHash", Sha256(Service::Tools().dump())},
                                    {"interactiveSession", windows_session != 0},
                                    {"capabilities",
                                     {{"browser", true}, {"headless", options.value("headless", false)}}}});
                }
                else if (std::chrono::steady_clock::now() >= next_poll)
                    action = "node.poll";
                else
                    for (auto &entry : inbox)
                    {
                        auto &row = entry.second;
                        if (row.at("state") == "running" && !row.value("startedReported", false) &&
                            works.count(entry.first))
                        {
                            bool started = false;
                            {
                                std::lock_guard<std::mutex> lock(works.at(entry.first)->mutex);
                                started = works.at(entry.first)->started;
                            }
                            if (started)
                            {
                                action = "node.event";
                                payload.update({{"runId", row.at("command").at("payload").at("runId")},
                                                {"commandId", entry.first},
                                                {"eventId", "started-" + entry.first},
                                                {"event", {{"type", "started"}}}});
                                break;
                            }
                        }
                        if (row.at("state") == "received" ||
                            (row.at("state") == "complete" && row.value("stoppedConfirmed", false)))
                        {
                            const bool complete = row.at("state") == "complete";
                            action = complete ? "node.complete" : "node.ack";
                            payload.update({{"runId", row.at("command").at("payload").at("runId")},
                                            {"commandId", entry.first}});
                            if (complete)
                                payload.update({{"stoppedConfirmed", true}, {"result", row.at("result")}});
                            else
                                payload["payloadHash"] = row.at("command").at("payloadHash");
                            break;
                        }
                    }
                if (action.empty())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                state["sequence"] = state.at("sequence").get<long long>() + 1;
                state["pending"] = {
                    {"action", action}, {"payload", payload}, {"sequence", state.at("sequence")}};
                save();
            }
            const auto pending = state.at("pending");
            const auto action = pending.at("action").get<std::string>();
            const auto payload = pending.at("payload");
            const auto timestamp = BackendNow();
            const auto sequence = pending.at("sequence").get<long long>();
            const auto sent = std::chrono::steady_clock::now();
            Json reply;
            try
            {
                reply = ControlRequest(
                    state.at("url"), action,
                    {{"nodeId", state.at("nodeId")},
                     {"sequence", sequence},
                     {"timestamp", timestamp},
                     {"payload", payload},
                     {"signature",
                      RsaSign(state.at("keys").at("privateKey"),
                              "AWB-NODE/1\n" + action + "\n" + state.at("nodeId").get<std::string>() + "\n" +
                                  std::to_string(sequence) + "\n" + std::to_string(timestamp) + "\n" +
                                  payload.dump() + "\n")}},
                    state.at("credential"), options.value("clientCertificateThumbprint", ""));
            }
            catch (const Error &error)
            {
                for (auto &entry : works)
                    entry.second->Fence("connection-or-authorization-lost");
                std::cerr << Json{{"code", error.code}, {"action", action}}.dump() << std::endl;
                if (error.code == "CONTROL_UNAVAILABLE")
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                if (action == "node.ack")
                {
                    auto &row = inbox.at(payload.at("commandId"));
                    row["state"] = "complete";
                    row["stoppedConfirmed"] = true;
                    row["result"] = {{"state", "unknown"}, {"reason", "ack-rejected-no-execution"}};
                    journal.Put(row);
                }
                else if (error.code == "NODE_REVOKED")
                {
                    stopping = true;
                }
                else
                    throw;
                state.erase("pending");
                save();
                continue;
            }
            if (action == "node.open")
            {
                state["capacity"] = reply.at("capacity");
                opened = true;
            }
            else if (action == "node.poll")
            {
                next_poll = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                for (auto &entry : works)
                {
                    const auto &command = entry.second->command;
                    if (command.at("payload").at("epoch") != reply.at("epoch"))
                        entry.second->Fence("coordinator-epoch-changed");
                }
                for (const auto &stop : reply.at("stops"))
                    if (works.count(stop.at("commandId")))
                        works.at(stop.at("commandId"))->Fence("central-cancel");
                for (const auto &grant : reply.at("grants"))
                    for (auto &entry : works)
                        if (entry.second->command.at("payload").at("runId") == grant.at("runId"))
                            entry.second->Grant(grant, sent);
                for (const auto &command : reply.at("commands"))
                {
                    const auto id = Field(command, "id", 32);
                    if (command.at("bootId") != boot || command.at("nodeId") != state.at("nodeId") ||
                        command.at("protocolVersion") != 1 ||
                        Sha256(command.at("payload").dump()) != command.at("payloadHash") ||
                        command.at("payload").at("epoch") != reply.at("epoch") ||
                        Sha256(command.at("payload").at("configuration").dump()) !=
                            command.at("payload").at("configurationHash") ||
                        Sha256(command.at("payload").at("definition").dump()) !=
                            command.at("payload").at("definitionHash"))
                        throw Error("NODE_COMMAND_CONFLICT", "Command scope or immutable digest differs");
                    if (inbox.count(id))
                    {
                        if (inbox.at(id).at("command").at("payloadHash") != command.at("payloadHash"))
                            throw Error("NODE_COMMAND_CONFLICT", "Inbox command changed");
                        continue;
                    }
                    Json row{{"id", id},
                             {"command", command},
                             {"state", "received"},
                             {"receivedAt", BackendNow()}};
                    journal.Put(row);
                    inbox[id] = row;
                }
            }
            else if (action == "node.ack")
            {
                const auto id = payload.at("commandId").get<std::string>();
                auto &row = inbox.at(id);
                row["state"] = "running";
                journal.Put(row);
                if (works.size() >= state.at("capacity").get<size_t>())
                    throw Error("NODE_CAPACITY", "Coordinator exceeded the local approved capacity");
                auto work = std::make_shared<Work>();
                work->command = row.at("command");
                work->expires = sent + std::chrono::seconds(reply.at("leaseSeconds").get<int>() - 2);
                works[id] = work;
                work->worker =
                    std::thread(ExecuteWork, work, directory, browser, options.value("headless", false));
            }
            else if (action == "node.event")
            {
                auto &row = inbox.at(payload.at("commandId"));
                row["startedReported"] = true;
                journal.Put(row);
            }
            else if (action == "node.complete")
            {
                auto &row = inbox.at(payload.at("commandId"));
                row["state"] = "reported";
                journal.Put(row);
                const auto id = payload.at("commandId").get<std::string>();
                if (works.count(id))
                {
                    works.at(id)->worker.join();
                    works.erase(id);
                }
            }
            state.erase("pending");
            save();
        }
    }
    catch (...)
    {
        for (auto &entry : works)
            entry.second->Fence("node-loop-failed");
        for (auto &entry : works)
            if (entry.second->worker.joinable())
                entry.second->worker.join();
        SetConsoleCtrlHandler(StopNode, FALSE);
        throw;
    }
    stopping = true;
    for (auto &entry : works)
        entry.second->Fence("node-stopping");
    for (auto &entry : works)
        if (entry.second->worker.joinable())
            entry.second->worker.join();
    for (auto &entry : works)
    {
        auto &row = inbox.at(entry.first);
        row["state"] = "complete";
        row["result"] = entry.second->result;
        row["stoppedConfirmed"] = entry.second->stopped;
        journal.Put(row);
    }
    SetConsoleCtrlHandler(StopNode, FALSE);
    return 0;
}
} // namespace awb
