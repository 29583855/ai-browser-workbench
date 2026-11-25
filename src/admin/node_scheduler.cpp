#include "awb/backend_extensions.hpp"
#include "awb/browser.hpp"
#include "awb/service.hpp"
#include "awb/version.hpp"
#include "awb/workflow.hpp"
#include <algorithm>
#include <set>

namespace awb
{
namespace
{
// 用途：识别已经结束或必须人工核对的运行状态；日期：2026-11-05。
bool ActiveRun(const Json &run)
{
    const auto state = run.value("state", "");
    return state == "dispatched" || state == "accepted" || state == "running" || state == "cancel_requested";
}
// 用途：按不可伪造的中央身份调用既有商业配额门，短期内部会话立即吊销；日期：2026-11-05。
Json Admit(BackendStore &store, const Json &user, const std::string &action, const Json &body)
{
    return store.AdmitAction(user, action, body);
}
// 用途：限制项目资源可见性，组织管理者与显式项目授权成员可访问；日期：2026-11-05。
Json Project(BackendStore &store, const Json &user, const std::string &id, bool manage = false)
{
    const auto project = store.Require("project", id);
    const auto member = TenantMember(store, user, project.at("tenantId"), manage);
    if (project.at("state") != "active")
        throw Error("PROJECT_DISABLED", "Project is disabled");
    const auto &users = project.at("authorizedUsers");
    if (member.at("role") != "owner" && member.at("role") != "admin" &&
        std::find(users.begin(), users.end(), user.at("id")) == users.end())
        throw Error("FORBIDDEN", "User is not assigned to this project");
    return project;
}
// 用途：项目缩小来源范围后，旧分配在心跳和排队领取时立即失去派发资格；日期：2026-11-05。
void CurrentScope(const Json &project, const Json &assignment)
{
    std::set<std::string> scope;
    for (const auto &origin : project.at("allowedOrigins"))
        scope.insert(Browser::Origin(origin));
    for (const auto &origin : assignment.at("configuration").at("allowedOrigins"))
        if (!scope.count(Browser::Origin(origin)))
            throw Error("FORBIDDEN", "Assignment origin was removed from the project");
}
// 用途：检查节点是否被批准用于指定项目；日期：2026-11-05。
bool NodeProject(const Json &node, const std::string &project)
{
    const auto &projects = node.at("projectIds");
    return std::find(projects.begin(), projects.end(), project) != projects.end();
}
// 用途：读取严格有界的节点及调度整数参数；日期：2026-11-05。
long long Integer(const Json &body, const char *key, long long minimum, long long maximum)
{
    if (!body.contains(key) || !body.at(key).is_number_integer())
        throw Error("INVALID_ARGUMENTS", std::string("Expected integer: ") + key);
    const auto value = body.at(key).get<long long>();
    if (value < minimum || value > maximum)
        throw Error("INVALID_ARGUMENTS", std::string("Integer outside range: ") + key);
    return value;
}
// 用途：隐藏节点认证摘要和请求回执缓存，门户不获得节点凭据；日期：2026-11-05。
Json PublicNode(Json node)
{
    node.erase("credentialHash");
    node.erase("lastReply");
    node.erase("lastRequestHash");
    node["online"] = node.value("lastSeenAt", 0LL) >= BackendNow() - 15;
    return node;
}
// 用途：在浏览器启动前为分配预留中央容量，并绑定组织、节点与配置；日期：2026-11-05。
Json MakeAssignment(BackendStore &store, const Json &user, const Json &project, const Json &body)
{
    auto node_id = body.value("nodeId", "");
    const auto project_id = project.at("id").get<std::string>();
    if (node_id.empty())
    {
        size_t minimum = static_cast<size_t>(-1);
        for (const auto &node : store.List("node", project.at("tenantId")))
        {
            if (node.at("state") != "active" || !NodeProject(node, project_id) ||
                node.value("lastSeenAt", 0LL) < BackendNow() - 15)
                continue;
            size_t assigned = 0;
            for (const auto &assignment : store.List("assignment", project.at("tenantId")))
                if (assignment.at("nodeId") == node.at("id") && assignment.at("state") == "active")
                    ++assigned;
            if (assigned < minimum)
            {
                minimum = assigned;
                node_id = node.at("id");
            }
        }
    }
    auto node = store.Require("node", node_id);
    if (node.at("tenantId") != project.at("tenantId") || node.at("state") != "active" ||
        !NodeProject(node, project_id))
        throw Error("NODE_UNAVAILABLE", "No approved node is available for this project");
    auto config = body.at("configuration");
    if (!config.is_object() || config.dump().size() > 32768)
        throw Error("INVALID_ARGUMENTS", "Expected bounded profile configuration");
    for (const auto &item : config.items())
        if (item.key() != "name" && item.key() != "allowedOrigins" && item.key() != "proxyServer" &&
            item.key() != "runtimeSettings")
            throw Error("INVALID_ARGUMENTS", "Central profile configuration contains unsupported fields");
    Field(config, "name", 128);
    if (!config.at("allowedOrigins").is_array() || config.at("allowedOrigins").empty() ||
        config.at("allowedOrigins").size() > 32)
        throw Error("INVALID_ARGUMENTS", "Profile must specify a bounded origin scope");
    std::set<std::string> scope;
    for (const auto &url : project.at("allowedOrigins"))
        scope.insert(Browser::Origin(url));
    for (auto &url : config["allowedOrigins"])
    {
        url = Browser::Origin(url);
        if (!scope.count(url))
            throw Error("FORBIDDEN", "Profile origin is outside the project scope");
    }
    if (config.contains("proxyServer") && !config.at("proxyServer").get<std::string>().empty())
        Browser::ValidateProxyServer(config.at("proxyServer"));
    const auto id = RandomId();
    const auto tenant = project.at("tenantId").get<std::string>();
    const auto device = Admit(store, user, "device.register",
                              {{"tenantId", tenant}, {"deviceId", node_id}, {"name", node.at("name")}});
    const auto environment = Admit(store, user, "environment.reserve",
                                   {{"tenantId", tenant}, {"deviceId", device.at("id")}, {"profileId", id}});
    const Json assignment{{"id", id},
                          {"tenantId", tenant},
                          {"projectId", project_id},
                          {"nodeId", node_id},
                          {"ownerUserId", user.at("id")},
                          {"environmentId", environment.at("id")},
                          {"configuration", config},
                          {"configurationHash", Sha256(config.dump())},
                          {"generation", 1},
                          {"state", "active"},
                          {"createdAt", BackendNow()}};
    store.Put("assignment", id, assignment);
    return assignment;
}
} // namespace

// 用途：提升协调代数并冻结旧进程未确认完成的任务，不复活旧Outbox；日期：2026-11-05。
NodeScheduler::NodeScheduler(BackendStore &store)
{
    const auto deployment = store.Get("scheduler", "deployment");
    epoch_ = deployment.value("epoch", 0LL) + 1;
    store.Put("scheduler", "deployment",
              {{"epoch", epoch_}, {"startedAt", BackendNow()}, {"protocolVersion", 1}});
    for (auto run : store.List("remote_run"))
        if (ActiveRun(run))
        {
            run["state"] = "unknown";
            run["reason"] = "coordinator-restarted-reconciliation-required";
            store.Put("remote_run", run.at("id"), run);
        }
}
// 用途：按项目权限与幂等键管理节点及固定版本任务，外部网络不在事务内调用；日期：2026-11-05。
Json NodeScheduler::Action(BackendStore &store, const std::string &action, const Json &body,
                           const std::string &token)
{
    if (action == "node.enroll")
    {
        const auto enrollment = Field(body, "enrollmentToken", 128);
        auto invitation = store.Get("node_enrollment", Sha256(enrollment));
        if (invitation.empty() || invitation.at("expiresAt") <= BackendNow())
            throw Error("NODE_ENROLLMENT", "Node enrollment token is expired or already used");
        const auto name = Field(body, "name", 128);
        const auto public_key = body.at("publicKey");
        if (!public_key.is_object() || public_key.contains("d") || public_key.contains("p") ||
            public_key.contains("q") ||
            !RsaVerify(public_key, "AWB-NODE-ENROLL\n" + enrollment + "\n" + public_key.dump() + "\n" + name,
                       Field(body, "signature", 2048)))
            throw Error("NODE_SIGNATURE", "Node enrollment public key proof is invalid");
        if (invitation.at("used") == true)
        {
            if (invitation.value("requestHash", "") != Sha256(body.dump()))
                throw Error("NODE_ENROLLMENT", "Enrollment token was used by another identity");
            return ParseExternalJson(
                ProtectBytes(Base64(invitation.at("replyProtected"), false, true), true));
        }
        const auto id = RandomId(), credential = RandomId() + RandomId();
        const Json node{{"id", id},
                        {"tenantId", invitation.at("tenantId")},
                        {"projectIds", invitation.at("projectIds")},
                        {"name", name},
                        {"publicKey", public_key},
                        {"credentialHash", Sha256(credential)},
                        {"state", "pending"},
                        {"capacity", invitation.at("capacity")},
                        {"lastSequence", 0},
                        {"lastSeenAt", 0},
                        {"bootId", ""},
                        {"createdAt", BackendNow()}};
        store.Put("node", id, node);
        const Json reply{{"nodeId", id},
                         {"credential", credential},
                         {"state", "pending"},
                         {"protocolVersion", 1},
                         {"capacity", invitation.at("capacity")}};
        invitation["used"] = true;
        invitation["requestHash"] = Sha256(body.dump());
        invitation["replyProtected"] = Base64(ProtectBytes(reply.dump()));
        store.Put("node_enrollment", invitation.at("id"), invitation);
        store.Audit("node-enrollment", action, invitation.at("tenantId"), id);
        return reply;
    }
    if (action == "node.open" || action == "node.poll" || action == "node.ack" || action == "node.complete" ||
        action == "node.event")
        return NodeAction(store, action, body, token);
    const auto user = store.Authenticate(token);
    const auto actor = user.at("id").get<std::string>();
    const auto tenant = body.value("tenantId", user.at("defaultTenant").get<std::string>());
    const auto member = TenantMember(store, user, tenant);
    if (action == "scheduler.snapshot")
    {
        for (auto run : store.List("remote_run", tenant))
        {
            if (ActiveRun(run) &&
                (run.value("leaseUntil", 0LL) <= BackendNow() || run.at("deadline") <= BackendNow()))
            {
                run["state"] = "unknown";
                run["reason"] = "node-offline-or-deadline";
                store.Put("remote_run", run.at("id"), run);
            }
            else if (run.at("state") == "queued" && run.at("deadline") <= BackendNow())
            {
                run["state"] = "expired";
                store.Put("remote_run", run.at("id"), run);
            }
        }
        Json projects = Json::array(), nodes = Json::array(), assignments = Json::array(),
             runs = Json::array(), versions = Json::array();
        std::set<std::string> allowed;
        for (const auto &project : store.List("project", tenant))
            try
            {
                if (project.at("state") == "disabled")
                    TenantMember(store, user, tenant, true);
                else
                    Project(store, user, project.at("id"));
                projects.push_back(project);
                allowed.insert(project.at("id"));
            }
            catch (const Error &)
            {
            }
        for (const auto &node : store.List("node", tenant))
        {
            bool visible = false;
            for (const auto &id : allowed)
                visible |= NodeProject(node, id);
            if (visible)
                nodes.push_back(PublicNode(node));
        }
        for (const auto &assignment : store.List("assignment", tenant))
            if (allowed.count(assignment.at("projectId")))
                assignments.push_back(assignment);
        for (auto run : store.List("remote_run", tenant))
            if (allowed.count(run.at("projectId")))
            {
                run.erase("inputProtected");
                runs.push_back(run);
            }
        for (auto version : store.List("remote_version", tenant))
            if (allowed.count(version.at("projectId")))
            {
                version.erase("definition");
                versions.push_back(version);
            }
        return {{"epoch", epoch_},      {"projects", projects},
                {"nodes", nodes},       {"assignments", assignments},
                {"versions", versions}, {"runs", runs},
                {"protocolVersion", 1}};
    }
    if (action == "project.state")
    {
        TenantMember(store, user, tenant, true);
        auto project = store.Require("project", Field(body, "id"));
        if (project.at("tenantId") != tenant)
            throw Error("FORBIDDEN", "Project belongs to another organization");
        const auto state = Field(body, "state");
        if (state != "active" && state != "disabled")
            throw Error("INVALID_ARGUMENTS", "Expected active/disabled project state");
        project["state"] = state;
        store.Put("project", project.at("id"), project);
        store.Audit(actor, action, tenant, project.at("id"));
        return project;
    }
    if (action == "project.save")
    {
        TenantMember(store, user, tenant, true);
        const auto id = body.value("id", RandomId());
        auto previous = store.Get("project", id);
        if (!previous.empty() && previous.at("tenantId") != tenant)
            throw Error("FORBIDDEN", "Project belongs to another organization");
        const auto origins = body.at("allowedOrigins");
        if (!origins.is_array() || origins.empty() || origins.size() > 32)
            throw Error("INVALID_ARGUMENTS", "Project requires bounded origins");
        Json normalized = Json::array();
        for (const auto &origin : origins)
            normalized.push_back(Browser::Origin(origin));
        const auto users = body.value("authorizedUsers", Json::array({actor}));
        if (!users.is_array() || users.size() > 1000)
            throw Error("INVALID_ARGUMENTS", "Invalid project member list");
        for (const auto &id_value : users)
            TenantMember(store, store.Require("user", id_value), tenant);
        const Json project{{"id", id},
                           {"tenantId", tenant},
                           {"name", Field(body, "name", 128)},
                           {"allowedOrigins", normalized},
                           {"authorizedUsers", users},
                           {"state", "active"}};
        store.Put("project", id, project);
        store.Audit(actor, action, tenant, id);
        return project;
    }
    if (action == "node.enrollment.create")
    {
        TenantMember(store, user, tenant, true);
        const auto projects = body.at("projectIds");
        if (!projects.is_array() || projects.empty() || projects.size() > 32)
            throw Error("INVALID_ARGUMENTS", "Enrollment needs approved project IDs");
        for (const auto &id : projects)
            if (Project(store, user, id, true).at("tenantId") != tenant)
                throw Error("FORBIDDEN", "Enrollment project belongs to another organization");
        const auto enrollment = RandomId() + RandomId(), id = Sha256(enrollment);
        const Json record{{"id", id},
                          {"tenantId", tenant},
                          {"projectIds", projects},
                          {"capacity", Integer(body, "capacity", 1, 64)},
                          {"expiresAt", BackendNow() + 300},
                          {"used", false}};
        store.Put("node_enrollment", id, record);
        store.Audit(actor, action, tenant, id);
        return {{"enrollmentToken", enrollment}, {"expiresAt", record.at("expiresAt")}};
    }
    if (action == "node.state")
    {
        TenantMember(store, user, tenant, true);
        auto node = store.Require("node", Field(body, "id"));
        if (node.at("tenantId") != tenant)
            throw Error("FORBIDDEN", "Node belongs to another organization");
        const auto state = Field(body, "state");
        if ((state != "active" && state != "maintenance" && state != "revoked") ||
            node.at("state") == "revoked")
            throw Error("NODE_STATE",
                        "Node state transition is invalid; revoked identities must enroll again");
        node["state"] = state;
        store.Put("node", node.at("id"), node);
        store.Audit(actor, action, tenant, node.at("id"));
        return PublicNode(node);
    }
    if (action == "assignment.create")
    {
        const auto project = Project(store, user, Field(body, "projectId"), true);
        if (project.at("tenantId") != tenant)
            throw Error("FORBIDDEN", "Project organization differs");
        const auto assignment = MakeAssignment(store, user, project, body);
        store.Audit(actor, action, tenant, assignment.at("id"));
        return assignment;
    }
    if (action == "assignment.release")
    {
        auto assignment = store.Require("assignment", Field(body, "id"));
        Project(store, user, assignment.at("projectId"), true);
        for (const auto &run : store.List("remote_run", assignment.at("tenantId")))
            if (run.at("assignmentId") == assignment.at("id") &&
                (ActiveRun(run) || run.at("state") == "queued" ||
                 (run.at("state") == "unknown" && !run.value("stoppedConfirmed", false))))
                throw Error("ASSIGNMENT_BUSY",
                            "Cancel and reconcile every outstanding run before releasing capacity");
        const auto environment = store.Require("environment", assignment.at("environmentId"));
        for (const auto &slot : store.List("slot", assignment.at("tenantId")))
            if (slot.at("environmentId") == environment.at("id") && slot.at("state") != "released")
                throw Error("ASSIGNMENT_BUSY", "An unresolved browser slot still owns this assignment");
        auto released = environment;
        released["state"] = "released";
        store.Put("environment", environment.at("id"), released);
        assignment["state"] = "released";
        assignment["generation"] = assignment.at("generation").get<long long>() + 1;
        store.Put("assignment", assignment.at("id"), assignment);
        store.Audit(actor, action, assignment.at("tenantId"), assignment.at("id"));
        return assignment;
    }
    if (action == "workflow.remote.publish")
    {
        const auto project = Project(store, user, Field(body, "projectId"), true);
        const auto definition = body.at("definition");
        const auto validation = ValidateWorkflow(definition);
        if (!validation.at("valid").get<bool>())
            throw Error("WORKFLOW_INVALID", "Remote workflow failed the shared native DSL validator");
        std::set<std::string> scope;
        for (const auto &origin : project.at("allowedOrigins"))
            scope.insert(Browser::Origin(origin));
        for (const auto &origin : definition.at("policy").at("allowedOrigins"))
            if (!scope.count(Browser::Origin(origin)))
                throw Error("FORBIDDEN", "Workflow origin is outside the project scope");
        const auto digest = Sha256(definition.dump());
        const auto id = Sha256(project.at("id").get<std::string>() + ":" + digest);
        const Json version{{"id", id},
                           {"tenantId", project.at("tenantId")},
                           {"projectId", project.at("id")},
                           {"definition", definition},
                           {"definitionHash", digest},
                           {"createdAt", BackendNow()}};
        if (store.Get("remote_version", id).empty())
            store.Put("remote_version", id, version);
        store.Audit(actor, action, project.at("tenantId"), id);
        return {{"id", id}, {"definitionHash", digest}, {"immutable", true}};
    }
    if (action == "run.submit")
    {
        const auto assignment = store.Require("assignment", Field(body, "assignmentId"));
        const auto project = Project(store, user, assignment.at("projectId"));
        if (project.at("tenantId") != tenant || assignment.at("state") != "active")
            throw Error("FORBIDDEN", "Assignment is not active in this organization");
        if (assignment.at("ownerUserId") != actor)
            throw Error("FORBIDDEN", "Use an environment assigned to your account");
        const auto version = store.Require("remote_version", Field(body, "versionId"));
        if (version.at("projectId") != project.at("id"))
            throw Error("FORBIDDEN", "Workflow version belongs to a different project");
        const auto input = body.value("input", Json::object());
        if (!ValidateWorkflow(version.at("definition"), &input).at("valid").get<bool>())
            throw Error("WORKFLOW_INVALID", "Run input does not satisfy the fixed workflow schema");
        if (assignment.at("configuration").at("allowedOrigins") !=
            version.at("definition").at("policy").at("allowedOrigins"))
            throw Error("WORKFLOW_SCOPE_MISMATCH",
                        "Assignment and workflow must have identical origin scopes");
        const auto key = Field(body, "idempotencyKey", 128);
        const auto id = Sha256(tenant + ":" + actor + ":" + key);
        const auto digest = Sha256(body.dump());
        auto previous = store.Get("remote_run", id);
        if (!previous.empty())
        {
            if (previous.at("requestHash") != digest)
                throw Error("IDEMPOTENCY_CONFLICT", "Remote run key has different parameters");
            previous.erase("inputProtected");
            return previous;
        }
        Json run{{"id", id},
                 {"tenantId", tenant},
                 {"projectId", project.at("id")},
                 {"actor", actor},
                 {"assignmentId", assignment.at("id")},
                 {"nodeId", assignment.at("nodeId")},
                 {"assignmentGeneration", assignment.at("generation")},
                 {"versionId", version.at("id")},
                 {"requestHash", digest},
                 {"inputProtected", Base64(ProtectBytes(input.dump()))},
                 {"state", "queued"},
                 {"createdAt", BackendNow()},
                 {"deadline", BackendNow() + Integer(body, "timeoutSeconds", 5, 86400)},
                 {"epoch", epoch_},
                 {"stoppedConfirmed", false}};
        auto counter = store.Get("scheduler", "queue-order");
        const auto ordinal = counter.value("value", 0LL) + 1;
        store.Put("scheduler", "queue-order", {{"value", ordinal}});
        run["queuedOrder"] = ordinal;
        store.Put("remote_run", id, run);
        store.Audit(actor, action, tenant, id);
        run.erase("inputProtected");
        return run;
    }
    if (action == "run.cancel" || action == "run.resolve")
    {
        auto run = store.Require("remote_run", Field(body, "id"));
        const auto project = Project(store, user, run.at("projectId"), action == "run.resolve");
        if (run.at("actor") != actor && member.at("role") != "owner" && member.at("role") != "admin")
            throw Error("FORBIDDEN", "Only the run owner or project manager may cancel");
        if (action == "run.resolve")
        {
            if (run.at("state") != "unknown" || !run.value("stoppedConfirmed", false) ||
                Field(body, "confirmRunId") != run.at("id"))
                throw Error(
                    "RECONCILIATION_REQUIRED",
                    "Node must confirm stopped; then explicitly confirm the run ID and business outcome");
            run["state"] = "resolved";
            run["resolution"] = Field(body, "reason", 1024);
        }
        else if (run.at("state") == "queued")
            run["state"] = "cancelled";
        else if (ActiveRun(run))
            run["state"] = "cancel_requested";
        store.Put("remote_run", run.at("id"), run);
        store.Audit(actor, action, project.at("tenantId"), run.at("id"));
        run.erase("inputProtected");
        return run;
    }
    throw Error("NOT_FOUND", "Unknown scheduler action");
}
// 用途：按真实节点容量和中央配额领取任务，未确认停止的分配永不自动复用；日期：2026-11-05。
Json NodeScheduler::Dispatch(BackendStore &store, Json node)
{
    Json commands = Json::array(), grants = Json::array(), stops = Json::array();
    const auto tenant = node.at("tenantId").get<std::string>();
    size_t active = 0;
    std::set<std::string> occupied;
    auto runs = store.List("remote_run", tenant);
    // 用途：按持久入队时间选择任务，避免随机ID决定队列先后；日期：2026-11-05。
    std::sort(runs.begin(), runs.end(),
              [](const Json &left, const Json &right)
              {
                  return left.value("queuedOrder", 0LL) == right.value("queuedOrder", 0LL)
                             ? left.at("id") < right.at("id")
                             : left.value("queuedOrder", 0LL) < right.value("queuedOrder", 0LL);
              });
    for (auto run : runs)
    {
        if (run.at("nodeId") != node.at("id"))
            continue;
        if (ActiveRun(run) &&
            (run.value("leaseUntil", 0LL) <= BackendNow() || run.at("deadline") <= BackendNow() ||
             run.at("epoch") != epoch_ || node.at("state") == "revoked"))
        {
            run["state"] = "unknown";
            run["reason"] = "lease-expired-or-node-revoked";
            store.Put("remote_run", run.at("id"), run);
        }
        if (ActiveRun(run) || (run.at("state") == "unknown" && !run.value("stoppedConfirmed", false)))
        {
            ++active;
            occupied.insert(run.at("assignmentId"));
        }
        if (run.at("state") == "unknown" || run.at("state") == "cancel_requested")
            stops.push_back({{"runId", run.at("id")},
                             {"commandId", run.value("commandId", "")},
                             {"reason", run.at("state")}});
        if (!ActiveRun(run) || run.at("state") == "cancel_requested")
            continue;
        const auto owner = store.Require("user", run.at("actor"));
        try
        {
            CurrentScope(Project(store, owner, run.at("projectId")),
                         store.Require("assignment", run.at("assignmentId")));
            if (owner.at("state") != "active")
                throw Error("FORBIDDEN", "Run owner was disabled");
            Admit(store, owner, "slot.heartbeat", {{"tenantId", tenant}, {"id", run.at("slotId")}});
        }
        catch (const Error &)
        {
            run["state"] = "cancel_requested";
            run["reason"] = "authorization-or-entitlement-revoked";
            store.Put("remote_run", run.at("id"), run);
            stops.push_back(
                {{"runId", run.at("id")}, {"commandId", run.at("commandId")}, {"reason", run.at("reason")}});
            continue;
        }
        run["leaseUntil"] = BackendNow() + 20;
        store.Put("remote_run", run.at("id"), run);
        grants.push_back({{"runId", run.at("id")},
                          {"epoch", epoch_},
                          {"generation", run.at("assignmentGeneration")},
                          {"seconds", 20}});
        auto command = store.Require("node_outbox", run.at("commandId"));
        if (!command.at("acknowledged").get<bool>())
        {
            auto output = command;
            output["payload"] =
                ParseExternalJson(ProtectBytes(Base64(output.at("payloadProtected"), false, true), true));
            output.erase("payloadProtected");
            commands.push_back(output);
        }
    }
    if (node.at("state") == "active")
        for (auto run : runs)
        {
            if (run.at("nodeId") != node.at("id") || run.at("state") != "queued")
                continue;
            if (run.at("deadline") <= BackendNow())
            {
                run["state"] = "expired";
                store.Put("remote_run", run.at("id"), run);
                continue;
            }
            if (active >= node.at("capacity").get<size_t>() || occupied.count(run.at("assignmentId")))
                continue;
            const auto assignment = store.Require("assignment", run.at("assignmentId"));
            const auto owner = store.Require("user", run.at("actor"));
            try
            {
                CurrentScope(Project(store, owner, run.at("projectId")),
                             store.Require("assignment", run.at("assignmentId")));
                if (owner.at("state") != "active" || assignment.at("state") != "active" ||
                    assignment.at("generation") != run.at("assignmentGeneration") ||
                    !NodeProject(node, run.at("projectId")))
                    throw Error("FORBIDDEN", "Assignment or run authorization is no longer valid");
                const auto slot = Admit(store, owner, "slot.acquire",
                                        {{"tenantId", tenant},
                                         {"environmentId", assignment.at("environmentId")},
                                         {"idempotencyKey", "run-" + run.at("id").get<std::string>()}});
                run["slotId"] = slot.at("id");
            }
            catch (const Error &error)
            {
                run["queueReason"] = error.code;
                store.Put("remote_run", run.at("id"), run);
                continue;
            }
            const auto version = store.Require("remote_version", run.at("versionId"));
            const auto command_id = RandomId();
            const Json payload{{"runId", run.at("id")},
                               {"assignmentId", assignment.at("id")},
                               {"configuration", assignment.at("configuration")},
                               {"configurationHash", assignment.at("configurationHash")},
                               {"definition", version.at("definition")},
                               {"definitionHash", version.at("definitionHash")},
                               {"input", ParseExternalJson(ProtectBytes(
                                             Base64(run.at("inputProtected"), false, true), true))},
                               {"epoch", epoch_},
                               {"generation", assignment.at("generation")},
                               {"deadline", run.at("deadline")}};
            const Json command{{"id", command_id},
                               {"tenantId", tenant},
                               {"nodeId", node.at("id")},
                               {"bootId", node.at("bootId")},
                               {"protocolVersion", 1},
                               {"payloadProtected", Base64(ProtectBytes(payload.dump()))},
                               {"payloadHash", Sha256(payload.dump())},
                               {"acknowledged", false}};
            store.Put("node_outbox", command_id, command);
            run["commandId"] = command_id;
            run["state"] = "dispatched";
            run["bootId"] = node.at("bootId");
            run["epoch"] = epoch_;
            run["leaseUntil"] = BackendNow() + 20;
            run["dispatchedAt"] = BackendNow();
            store.Put("remote_run", run.at("id"), run);
            {
                auto output = command;
                output["payload"] =
                    ParseExternalJson(ProtectBytes(Base64(output.at("payloadProtected"), false, true), true));
                output.erase("payloadProtected");
                commands.push_back(output);
            }
            grants.push_back({{"runId", run.at("id")},
                              {"epoch", epoch_},
                              {"generation", assignment.at("generation")},
                              {"seconds", 20}});
            occupied.insert(assignment.at("id"));
            ++active;
        }
    return {{"state", node.at("state")},
            {"epoch", epoch_},
            {"commands", commands},
            {"grants", grants},
            {"stops", stops}};
}
// 用途：以节点独立RSA身份核验每次出站请求，序号和正文摘要阻止重放或冲突；日期：2026-11-05。
Json NodeScheduler::NodeAction(BackendStore &store, const std::string &action, const Json &body,
                               const std::string &token)
{
    const auto id = Field(body, "nodeId");
    auto node = store.Require("node", id);
    const auto sequence = Integer(body, "sequence", 1, 9007199254740991LL);
    const auto timestamp = Integer(body, "timestamp", 1, 9999999999LL);
    const auto &payload = body.at("payload");
    if (!payload.is_object() || payload.dump().size() > 512 * 1024 || timestamp < BackendNow() - 300 ||
        timestamp > BackendNow() + 300 || !SecureEqual(Sha256(token), node.at("credentialHash")) ||
        !RsaVerify(node.at("publicKey"),
                   "AWB-NODE/1\n" + action + "\n" + id + "\n" + std::to_string(sequence) + "\n" +
                       std::to_string(timestamp) + "\n" + payload.dump() + "\n",
                   Field(body, "signature", 2048)))
        throw Error("NODE_SIGNATURE", "Node credential, signature or timestamp is invalid");
    const auto digest = Sha256(action + "\n" + payload.dump());
    const auto previous = node.at("lastSequence").get<long long>();
    if (sequence <= previous)
    {
        if (sequence == previous && node.value("lastRequestHash", "") == digest)
            return ParseExternalJson(ProtectBytes(Base64(node.at("lastReply"), false, true), true));
        throw Error("NODE_REPLAY", "Node sequence is stale or has conflicting contents");
    }
    Json reply;
    if (action == "node.open")
    {
        if (node.at("state") == "revoked")
            throw Error("NODE_REVOKED", "Node identity is revoked");
        if (payload.value("protocolVersion", 0) != 1 || payload.value("agentVersion", "") != Version ||
            payload.value("contractHash", "") != Sha256(Service::Tools().dump()))
            throw Error("NODE_VERSION", "Node protocol or tool contract differs from the coordinator");
        const auto boot = Field(payload, "bootId", 128);
        if (node.at("bootId") != boot)
            for (auto run : store.List("remote_run", node.at("tenantId")))
                if (run.at("nodeId") == id && ActiveRun(run))
                {
                    run["state"] = "unknown";
                    run["reason"] = "execution-node-restarted";
                    store.Put("remote_run", run.at("id"), run);
                }
        node["bootId"] = boot;
        node["version"] = Version;
        node["capabilities"] = payload.value("capabilities", Json::object());
        node["interactiveSession"] = payload.value("interactiveSession", false);
        node["lastSeenAt"] = BackendNow();
        store.Put("node", id, node);
        reply = {{"state", node.at("state")},
                 {"epoch", epoch_},
                 {"protocolVersion", 1},
                 {"capacity", node.at("capacity")}};
    }
    else
    {
        if (Field(payload, "bootId", 128) != node.at("bootId"))
            throw Error("NODE_BOOT", "Node boot identity is stale");
        node["lastSeenAt"] = BackendNow();
        store.Put("node", id, node);
        if (action == "node.poll")
            reply = Dispatch(store, node);
        else
        {
            auto run = store.Require("remote_run", Field(payload, "runId"));
            if (run.at("nodeId") != id || Field(payload, "commandId") != run.at("commandId"))
                throw Error("FORBIDDEN", "Node does not own this command");
            if (action == "node.ack")
            {
                if (node.at("state") == "revoked" || run.at("epoch") != epoch_ ||
                    run.at("bootId") != node.at("bootId") || !ActiveRun(run) ||
                    run.at("state") == "cancel_requested" || run.at("leaseUntil") <= BackendNow())
                    throw Error("NODE_FENCED", "Command was revoked or its control lease expired");
                auto command = store.Require("node_outbox", run.at("commandId"));
                if (Field(payload, "payloadHash") != command.at("payloadHash"))
                    throw Error("NODE_COMMAND_CONFLICT", "Node acknowledged a different command body");
                command["acknowledged"] = true;
                store.Put("node_outbox", command.at("id"), command);
                if (run.at("state") == "dispatched")
                    run["state"] = "accepted";
                store.Put("remote_run", run.at("id"), run);
                reply = {{"accepted", true}, {"epoch", epoch_}, {"leaseSeconds", 20}};
            }
            else if (action == "node.complete")
            {
                if (!payload.value("stoppedConfirmed", false))
                    throw Error("CONFIRMATION_REQUIRED",
                                "Node must confirm browser process stopped before releasing its slot");
                const auto result = payload.at("result");
                if (!result.is_object() || result.dump().size() > 32768)
                    throw Error("INVALID_ARGUMENTS", "Node completion result exceeds budget");
                const auto hash = Sha256(result.dump());
                if (run.value("stoppedConfirmed", false))
                {
                    if (run.value("resultHash", "") != hash)
                        throw Error("NODE_COMMAND_CONFLICT", "Command completion has conflicting results");
                    reply = {{"accepted", true}, {"duplicate", true}};
                }
                else
                {
                    if (run.contains("slotId"))
                        store.Release(store.Require("slot", run.at("slotId")), "signed-node-confirmed-stop");
                    const auto state = Field(result, "state");
                    if (state != "succeeded" && state != "failed" && state != "cancelled" &&
                        state != "unknown")
                        throw Error("INVALID_ARGUMENTS", "Unsupported execution outcome");
                    if (run.at("state") == "unknown" || run.at("epoch") != epoch_ ||
                        run.at("bootId") != node.at("bootId"))
                        run["state"] = "unknown";
                    else
                        run["state"] = run.at("state") == "cancel_requested" ? "cancelled" : state;
                    run["result"] = result;
                    run["resultHash"] = hash;
                    run["stoppedConfirmed"] = true;
                    run["finishedAt"] = BackendNow();
                    store.Put("remote_run", run.at("id"), run);
                    store.Audit(id, "node.complete", node.at("tenantId"), run.at("id"));
                    reply = {{"accepted", true}, {"state", run.at("state")}};
                }
            }
            else
            {
                const auto event_id = Field(payload, "eventId", 128);
                const auto key = Sha256(id + ":" + event_id);
                const auto event = payload.at("event");
                if (!event.is_object() || event.dump().size() > 16384)
                    throw Error("INVALID_ARGUMENTS", "Node event exceeds budget");
                const auto old = store.Get("node_event", key);
                if (!old.empty() && old.at("hash") != Sha256(event.dump()))
                    throw Error("NODE_COMMAND_CONFLICT", "Node event identifier has conflicting contents");
                if (old.empty())
                    store.Put("node_event", key,
                              {{"id", key},
                               {"tenantId", node.at("tenantId")},
                               {"runId", run.at("id")},
                               {"nodeId", id},
                               {"event", event},
                               {"hash", Sha256(event.dump())},
                               {"at", BackendNow()}});
                if (event.value("type", "") == "started" && run.at("state") == "accepted" &&
                    run.at("leaseUntil") > BackendNow())
                {
                    run["state"] = "running";
                    store.Put("remote_run", run.at("id"), run);
                }
                reply = {{"accepted", true}, {"duplicate", !old.empty()}};
            }
        }
    }
    node = store.Require("node", id);
    node["lastSequence"] = sequence;
    node["lastRequestHash"] = digest;
    node["lastReply"] = Base64(ProtectBytes(reply.dump()));
    store.Put("node", id, node);
    return reply;
}
} // namespace awb
