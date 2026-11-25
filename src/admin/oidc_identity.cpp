#include "awb/backend_extensions.hpp"
#include "awb/browser.hpp"
#include <algorithm>

namespace awb
{
namespace
{
// 用途：计算OIDC规定的S256或哈希左半部分的Base64url值；日期：2026-11-05。
std::string S256(const std::string &text, bool half = false)
{
    const auto hex = Sha256(text);
    std::string bytes;
    for (size_t i = 0; i < (half ? 32u : 64u); i += 2)
        bytes += static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16));
    return Base64(bytes, true);
}
// 用途：仅允许配置的OIDC发行方及额外受信端点来源；日期：2026-11-05。
void Endpoint(const Json &config, const std::string &url)
{
    const auto origin = Browser::Origin(url);
    const auto issuer = Browser::Origin(Field(config, "issuer", 2048));
    bool trusted = origin == issuer;
    for (const auto &allowed : config.value("allowedEndpointOrigins", Json::array()))
        trusted |= origin == Browser::Origin(allowed);
    if (!trusted || (origin.find("https://") != 0 &&
                     !(config.value("allowLoopbackTest", false) && origin.find("http://127.0.0.1:") == 0)))
        throw Error("OIDC_ENDPOINT", "OIDC endpoint origin is outside configured trust");
}
// 用途：读取并归一化已验证的ASCII邮件标识；日期：2026-11-05。
std::string VerifiedEmail(const Json &claims)
{
    if (!claims.value("email_verified", false))
        throw Error("OIDC_EMAIL", "Automatic provisioning requires a verified email claim");
    auto email = Field(claims, "email", 254);
    if (email.find('@') == std::string::npos || email.front() == '@' || email.back() == '@')
        throw Error("OIDC_EMAIL", "OIDC email claim is invalid");
    for (auto &c : email)
    {
        if (static_cast<unsigned char>(c) <= 32 || static_cast<unsigned char>(c) >= 127)
            throw Error("OIDC_EMAIL", "OIDC email must use ASCII");
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
    return email;
}
} // namespace

// 用途：创建默认关闭的OIDC参数文件，并对开启配置进行静态验证；日期：2026-11-05。
OidcIdentity::OidcIdentity(const std::wstring &root)
{
    const auto path = SafeAbsolute(root + L"\\oidc-config.json");
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        AtomicWrite(path, Json{{"enabled", false},
                               {"issuer", ""},
                               {"clientId", ""},
                               {"clientSecret", ""},
                               {"redirectUri", ""},
                               {"tokenAuthMethod", "client_secret_basic"},
                               {"allowAutoProvision", true},
                               {"allowLoopbackTest", false},
                               {"allowedEndpointOrigins", Json::array()}}
                                  .dump(2) +
                              "\n");
    config_ = ParseExternalJson(ReadFileBounded(path, 65536));
    if (config_.value("enabled", false))
    {
        Field(config_, "clientId", 1024);
        Endpoint(config_, Field(config_, "issuer", 2048));
        const auto callback = Field(config_, "redirectUri", 2048);
        const auto origin = ProtocolOrigin(callback);
        if ((callback != origin + "/auth/callback" &&
             callback != Browser::Origin(callback) + "/auth/callback") ||
            (origin.find("https://") != 0 &&
             !(config_.value("allowLoopbackTest", false) && origin.find("http://127.0.0.1:") == 0)))
            throw Error("OIDC_CONFIG",
                        "OIDC redirectUri must be the exact trusted HTTPS /auth/callback endpoint");
        const auto method = config_.value("tokenAuthMethod", "client_secret_basic");
        if (method != "client_secret_basic" && method != "client_secret_post" && method != "none")
            throw Error("OIDC_CONFIG", "Unsupported OIDC token authentication method");
        if (method != "none")
            Field(config_, "clientSecret", 4096);
    }
}
// 用途：提供门户SSO可用状态，不提供客户端秘密；日期：2026-11-05。
Json OidcIdentity::Status() const
{
    return {{"enabled", config_.value("enabled", false)},
            {"issuer", config_.value("issuer", "")},
            {"redirectUri", config_.value("redirectUri", "")}};
}
// 用途：按OIDC发现规范读取受信配置并防止端点注入；日期：2026-11-05。
Json OidcIdentity::Discover() const
{
    if (!config_.value("enabled", false))
        throw Error("OIDC_NOT_CONFIGURED", "OIDC issuer and client parameters are not configured");
    auto issuer = Field(config_, "issuer", 2048);
    const auto response =
        BackendHttp("GET", issuer + (issuer.back() == '/' ? "" : "/") + ".well-known/openid-configuration",
                    {{"Accept", "application/json"}}, "", config_.value("allowLoopbackTest", false));
    if (response.status != 200)
        throw Error("OIDC_DISCOVERY", "OIDC discovery endpoint did not succeed");
    const auto document = ParseExternalJson(response.body);
    if (document.value("issuer", "") != issuer)
        throw Error("OIDC_ISSUER", "Discovery issuer differs from configured issuer");
    for (const auto *field : {"authorization_endpoint", "token_endpoint", "jwks_uri"})
        Endpoint(config_, Field(document, field, 2048));
    if (document.contains("end_session_endpoint"))
        Endpoint(config_, Field(document, "end_session_endpoint", 2048));
    if (document.contains("code_challenge_methods_supported") &&
        std::find(document.at("code_challenge_methods_supported").begin(),
                  document.at("code_challenge_methods_supported").end(),
                  "S256") == document.at("code_challenge_methods_supported").end())
        throw Error("OIDC_PKCE", "OIDC issuer does not support S256 PKCE");
    return document;
}
// 用途：为每次登录产生独立state、nonce及浏览器绑定，验证器用DPAPI保护；日期：2026-11-05。
// 用途：以已注册客户端和固定回跳地址发起身份服务注销，不把本地会话放入URL；日期：2026-11-05。
std::string OidcIdentity::LogoutUrl() const
{
    const auto discovery = Discover();
    if (!discovery.contains("end_session_endpoint"))
        return "";
    const auto endpoint = Field(discovery, "end_session_endpoint", 2048);
    const auto redirect = ProtocolOrigin(config_.at("redirectUri")) + "/";
    return endpoint + (endpoint.find('?') == std::string::npos ? "?" : "&") +
           EncodeForm({{"client_id", config_.at("clientId")}, {"post_logout_redirect_uri", redirect}});
}
Json OidcIdentity::Begin(BackendStore &store, const Json &discovery, const Json &body,
                         const std::string &token)
{
    const auto state = RandomId() + RandomId(), nonce = RandomId() + RandomId();
    const auto verifier = RandomId() + RandomId(), binding = RandomId() + RandomId();
    std::string link_user;
    if (body.value("link", false))
        link_user = store.Authenticate(token).at("id");
    Json record{{"id", Sha256(state)},
                {"issuer", config_.at("issuer")},
                {"nonce", nonce},
                {"verifierProtected", Base64(ProtectBytes(verifier))},
                {"browserHash", Sha256(binding)},
                {"createdAt", BackendNow()},
                {"expiresAt", BackendNow() + 300},
                {"used", false},
                {"linkUserId", link_user},
                {"discovery", discovery}};
    store.Put("oidc_state", record.at("id"), record);
    const Headers query{{"client_id", config_.at("clientId")},
                        {"response_type", "code"},
                        {"redirect_uri", config_.at("redirectUri")},
                        {"scope", "openid profile email"},
                        {"state", state},
                        {"nonce", nonce},
                        {"code_challenge", S256(verifier)},
                        {"code_challenge_method", "S256"}};
    const auto endpoint = discovery.at("authorization_endpoint").get<std::string>();
    return {{"authorizationUrl",
             endpoint + (endpoint.find('?') == std::string::npos ? "?" : "&") + EncodeForm(query)},
            {"state", state},
            {"browserBinding", binding},
            {"expiresAt", record.at("expiresAt")}};
}
// 用途：在交换令牌之前消费一次性状态，即使后续失败也不允许重用授权码；日期：2026-11-05。
Json OidcIdentity::Consume(BackendStore &store, const Json &body)
{
    auto record = store.Get("oidc_state", Sha256(Field(body, "state", 128)));
    if (record.empty() || record.at("used") == true || record.at("expiresAt") <= BackendNow() ||
        !SecureEqual(record.at("browserHash"), Sha256(Field(body, "browserBinding", 128))))
        throw Error("OIDC_STATE", "OIDC state is expired, consumed or belongs to a different browser");
    record["used"] = true;
    store.Put("oidc_state", record.at("id"), record);
    record["code"] = Field(body, "code", 8192);
    return record;
}
// 用途：严格完成授权码交换和ID Token验证，外部令牌不进入会话响应或日志；日期：2026-11-05。
Json OidcIdentity::Exchange(const Json &context) const
{
    const auto &discovery = context.at("discovery");
    const auto verifier = ProtectBytes(Base64(context.at("verifierProtected"), false, true), true);
    Headers form{{"grant_type", "authorization_code"},
                 {"code", context.at("code")},
                 {"redirect_uri", config_.at("redirectUri")},
                 {"code_verifier", verifier}};
    Headers headers{{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    const auto method = config_.value("tokenAuthMethod", "client_secret_basic");
    if (method == "client_secret_basic")
        headers["Authorization"] = "Basic " + Base64(UrlEncode(config_.at("clientId")) + ":" +
                                                     UrlEncode(config_.at("clientSecret")));
    else
    {
        form["client_id"] = config_.at("clientId");
        if (method == "client_secret_post")
            form["client_secret"] = config_.at("clientSecret");
    }
    const auto response = BackendHttp("POST", discovery.at("token_endpoint"), headers, EncodeForm(form),
                                      config_.value("allowLoopbackTest", false));
    if (response.status != 200)
        throw Error("OIDC_TOKEN", "OIDC token endpoint rejected the authorization code");
    const auto tokens = ParseExternalJson(response.body);
    const auto jwt = Field(tokens, "id_token", 65536);
    const auto first = jwt.find('.'), last = jwt.rfind('.');
    if (first == std::string::npos || first == last || jwt.find('.', first + 1) != last)
        throw Error("OIDC_TOKEN", "Malformed ID Token");
    const auto header = ParseExternalJson(Base64(jwt.substr(0, first), true, true));
    if (header.value("alg", "") != "RS256" || header.contains("jku") || header.contains("x5u") ||
        header.contains("crit"))
        throw Error("OIDC_ALGORITHM", "Only configured RS256 JWKS signatures are accepted");
    const auto keys_response = BackendHttp("GET", discovery.at("jwks_uri"), {{"Accept", "application/json"}},
                                           "", config_.value("allowLoopbackTest", false));
    if (keys_response.status != 200)
        throw Error("OIDC_JWKS", "Cannot read the trusted JWKS endpoint");
    const auto keys = ParseExternalJson(keys_response.body).at("keys");
    if (!keys.is_array() || keys.size() > 64)
        throw Error("OIDC_JWKS", "JWKS exceeds key budget");
    const auto kid = Field(header, "kid", 128);
    Json selected;
    for (const auto &key : keys)
        if (key.value("kid", "") == kid)
        {
            if (!selected.is_null())
                throw Error("OIDC_JWKS", "Duplicate JWKS key identifiers");
            selected = key;
        }
    if (selected.is_null() ||
        !RsaVerify(selected, jwt.substr(0, last), Base64(Base64(jwt.substr(last + 1), true, true))))
        throw Error("OIDC_SIGNATURE", "ID Token signature or key identifier is invalid");
    const auto claims = ParseExternalJson(Base64(jwt.substr(first + 1, last - first - 1), true, true));
    const auto client = config_.at("clientId").get<std::string>();
    bool audience = false, multiple = false;
    if (claims.at("aud").is_string())
        audience = claims.at("aud") == client;
    else if (claims.at("aud").is_array())
    {
        multiple = claims.at("aud").size() > 1;
        for (const auto &value : claims.at("aud"))
            audience |= value.is_string() && value == client;
    }
    if (claims.value("iss", "") != config_.at("issuer") || !audience ||
        ((multiple || claims.contains("azp")) && claims.value("azp", "") != client) ||
        !claims.at("exp").is_number_integer() || !claims.at("iat").is_number_integer() ||
        claims.at("exp") <= BackendNow() || claims.at("iat") > BackendNow() + 60 ||
        claims.at("iat").get<long long>() < context.at("createdAt").get<long long>() - 60 ||
        claims.value("nbf", 0LL) > BackendNow() + 60 ||
        !SecureEqual(Field(claims, "nonce", 128), context.at("nonce")))
        throw Error("OIDC_CLAIMS", "ID Token issuer, audience, nonce or lifetime is invalid");
    Field(claims, "sub", 255);
    if (claims.contains("at_hash") &&
        (!tokens.contains("access_token") ||
         !SecureEqual(claims.at("at_hash"), S256(Field(tokens, "access_token", 16384), true))))
        throw Error("OIDC_CLAIMS", "ID Token access token hash differs");
    if (claims.contains("c_hash") && !SecureEqual(claims.at("c_hash"), S256(context.at("code"), true)))
        throw Error("OIDC_CLAIMS", "ID Token code hash differs");
    return claims;
}
// 用途：按发行方与主体唯一映射本地身份，已有邮箱必须显式登录后绑定；日期：2026-11-05。
Json OidcIdentity::Finish(BackendStore &store, const Json &context, const Json &claims)
{
    const auto id = Sha256(claims.at("iss").get<std::string>() + "\n" + claims.at("sub").get<std::string>());
    auto binding = store.Get("oidc_binding", id);
    auto link = context.at("linkUserId").get<std::string>();
    if (!binding.empty() && !link.empty() && binding.at("userId") != link)
        throw Error("OIDC_BINDING", "OIDC subject is already bound to a different user");
    if (binding.empty() && link.empty())
    {
        if (!config_.value("allowAutoProvision", false))
            throw Error("OIDC_PROVISIONING", "An existing user must explicitly bind this OIDC subject");
        const auto email = VerifiedEmail(claims);
        if (!store.Get("email", email).empty())
            throw Error("OIDC_ACCOUNT_LINK_REQUIRED",
                        "Sign in to the existing account and bind SSO explicitly");
        link = RandomId();
        const auto tenant = RandomId();
        const auto name = claims.value("name", email);
        Json user{{"id", link},
                  {"email", email},
                  {"name", name.size() <= 128 ? name : email},
                  {"platform", false},
                  {"state", "active"},
                  {"defaultTenant", tenant},
                  {"salt", RandomId()},
                  {"passwordHash", Sha256(RandomId() + RandomId())},
                  {"passwordEnabled", false},
                  {"failed", 0},
                  {"lockedUntil", 0}};
        store.Put("user", link, user);
        store.Put("email", email, {{"userId", link}});
        store.Put(
            "tenant", tenant,
            {{"id", tenant}, {"name", user.at("name")}, {"state", "active"}, {"createdAt", BackendNow()}});
        store.Put("member", tenant + ":" + link,
                  {{"id", tenant + ":" + link},
                   {"tenantId", tenant},
                   {"userId", link},
                   {"email", email},
                   {"role", "owner"},
                   {"state", "active"}});
    }
    if (binding.empty())
    {
        binding = {{"id", id},
                   {"issuer", claims.at("iss")},
                   {"subject", claims.at("sub")},
                   {"userId", link},
                   {"createdAt", BackendNow()}};
        store.Put("oidc_binding", id, binding);
    }
    const auto user = store.Require("user", binding.at("userId"));
    if (user.at("state") != "active")
        throw Error("FORBIDDEN", "OIDC account has been disabled locally");
    auto session = store.Session(user);
    auto record = store.Get("session", Sha256(session.at("token")));
    record["oidcIssuer"] = claims.at("iss");
    record["oidcSubject"] = claims.at("sub");
    store.Put("session", Sha256(session.at("token")), record);
    store.Audit(user.at("id"), context.at("linkUserId") == "" ? "oidc.login" : "oidc.bind",
                user.at("defaultTenant"), id);
    return session;
}
// 用途：管理当前用户的SSO绑定和一次性桌面连接码，令牌只在交换响应中返回；日期：2026-11-05。
Json OidcIdentity::Action(BackendStore &store, const std::string &action, const Json &body,
                          const std::string &token)
{
    if (action == "client.code.exchange")
    {
        const auto id = Sha256(Field(body, "code", 128));
        auto record = store.Get("client_code", id);
        if (record.empty() || record.at("used") == true || record.at("expiresAt") <= BackendNow())
            throw Error("UNAUTHORIZED", "Desktop connection code is expired or already used");
        const auto user = store.Require("user", record.at("userId"));
        if (user.at("state") != "active")
            throw Error("FORBIDDEN", "Account disabled");
        TenantMember(store, user, record.at("tenantId"));
        record["used"] = true;
        store.Put("client_code", id, record);
        auto session = store.Session(user);
        session["tenantId"] = record.at("tenantId");
        return session;
    }
    const auto user = store.Authenticate(token);
    const auto actor = user.at("id").get<std::string>();
    if (action == "client.code.create")
    {
        const auto tenant = body.value("tenantId", user.at("defaultTenant").get<std::string>());
        TenantMember(store, user, tenant);
        const auto code = RandomId() + RandomId();
        store.Put("client_code", Sha256(code),
                  {{"id", Sha256(code)},
                   {"userId", actor},
                   {"tenantId", tenant},
                   {"expiresAt", BackendNow() + 120},
                   {"used", false}});
        store.Audit(actor, action, tenant, "single-use-code");
        return {{"code", code}, {"expiresAt", BackendNow() + 120}};
    }
    if (action == "oidc.bindings")
    {
        Json bindings = Json::array();
        for (const auto &binding : store.List("oidc_binding"))
            if (binding.at("userId") == actor)
                bindings.push_back(binding);
        return bindings;
    }
    if (action == "oidc.logout")
    {
        auto record = store.Get("session", Sha256(token));
        record["expiresAt"] = 0;
        store.Put("session", Sha256(token), record);
        return {{"loggedOut", true}};
    }
    throw Error("NOT_FOUND", "Unknown OIDC identity action");
}
} // namespace awb
