#pragma once
#include "awb/platform.hpp"
#include <functional>
#include <map>

namespace awb
{
using Headers = std::map<std::string, std::string>;
struct HttpResult
{
    int status = 0;
    std::string body;
    Headers headers;
};
// 用途：在预算内访问HTTPS或显式启用的回环测试地址，不跟随重定向或重试；日期：2026-11-05。
HttpResult BackendHttp(const std::string &method, const std::string &url, const Headers &headers,
                       const std::string &body, bool loopback_test = false);
// 用途：编码RFC3986参数，支付和OIDC表单共用同一严格实现；日期：2026-11-05。
// 用途：序列化标准HTTP Origin，省略80/443默认端口以匹配浏览器标头；日期：2026-11-05。
std::string ProtocolOrigin(const std::string &url);
std::string UrlEncode(const std::string &text);
// 用途：严格解码查询或表单并拒绝重复键、非法UTF8及截断百分号；日期：2026-11-05。
Headers DecodeForm(const std::string &text);
// 用途：按字段名有序编码application/x-www-form-urlencoded正文；日期：2026-11-05。
std::string EncodeForm(const Headers &fields);
// 用途：读取UTC秒供协议时限和持久化事件使用；日期：2026-11-05。
long long BackendNow();
// 用途：计算标准SHA256或HMAC-SHA256，输出小写十六进制；日期：2026-11-05。
std::string Sha256(const std::string &text, const std::string &key = "");
// 用途：编码或严格解码标准Base64和Base64url字节；日期：2026-11-05。
std::string Base64(const std::string &bytes, bool url = false, bool decode = false);
// 用途：使用Windows原生PKCS8 RSA私钥产生RS256签名；日期：2026-11-05。
std::string RsaSign(const std::string &pem, const std::string &bytes);
// 用途：使用SPKI公钥或RSA JWK验证RS256，不接受算法降级；日期：2026-11-05。
bool RsaVerify(const Json &key, const std::string &bytes, const std::string &signature);
// 用途：生成节点独立的RSA密钥对，私钥由调用方以DPAPI保存；日期：2026-11-05。
Json RsaGenerate();
// 用途：通过Windows AES-256-GCM校验并解密微信资源，认证失败不返回明文；日期：2026-11-05。
std::string AesGcmDecrypt(const std::string &key, const std::string &nonce, const std::string &associated,
                          const std::string &ciphertext);
// 用途：固定长度比较秘密摘要；日期：2026-11-05。
bool SecureEqual(const std::string &left, const std::string &right);
// 用途：由Windows当前用户DPAPI保护短期验证器和节点私钥；日期：2026-11-05。
std::string ProtectBytes(const std::string &bytes, bool decrypt = false);
// 用途：验证有界业务字符串，拒绝控制字节；日期：2026-11-05。
std::string Field(const Json &body, const char *key, size_t maximum = 256);

class BackendStore
{
  public:
    // 用途：允许派生存储按接口析构；日期：2026-11-05。
    virtual ~BackendStore() = default;
    // 用途：从当前事务读取指定资源；日期：2026-11-05。
    virtual Json Get(const std::string &kind, const std::string &id) = 0;
    // 用途：要求指定资源已经存在；日期：2026-11-05。
    virtual Json Require(const std::string &kind, const std::string &id) = 0;
    // 用途：在当前事务持久保存资源；日期：2026-11-05。
    virtual void Put(const std::string &kind, const std::string &id, const Json &body) = 0;
    // 用途：列出当前租户可见的某类资源；日期：2026-11-05。
    virtual Json List(const std::string &kind, const std::string &tenant = "") = 0;
    // 用途：重新核验用户会话和账号状态；日期：2026-11-05。
    virtual Json Authenticate(const std::string &token) = 0;
    // 用途：为已经核实的用户产生本地会话；日期：2026-11-05。
    virtual Json Session(const Json &user) = 0;
    // 用途：在业务事务中追加脱敏审计；日期：2026-11-05。
    virtual void Audit(const std::string &actor, const std::string &action, const std::string &tenant,
                       const std::string &resource) = 0;
    // 用途：将已核实渠道事实与订单、权益、账本原子提交；日期：2026-11-05。
    virtual Json Settle(Json order, const std::string &transaction, bool refund, bool reconciled = false) = 0;
    // 用途：读取组织当前免费及付费权益；日期：2026-11-05。
    // 用途：官方退款前锁定可退时长，避免渠道调用期间继续消费；日期：2026-11-05。
    virtual Json PrepareRefund(Json order) = 0;
    virtual Json Entitlement(const std::string &tenant) = 0;
    // 用途：节点确认本机进程停止后，原子结算已绑定的中央槽；日期：2026-11-05。
    virtual Json Release(Json slot, const std::string &reason) = 0;
    // 用途：统一调用现有浏览器商业准入动作；日期：2026-11-05。
    // 用途：以子事务执行节点内部准入，失败不遗留会话或钱包写入；日期：2026-11-05。
    virtual Json AdmitAction(const Json &user, const std::string &action, const Json &body) = 0;
    virtual Json Action(const std::string &action, const Json &body, const std::string &token) = 0;
};
// 用途：核验组织成员并返回成员记录，管理操作另检查角色；日期：2026-11-05。
Json TenantMember(BackendStore &store, const Json &user, const std::string &tenant, bool manage = false);

class OfficialPayments
{
  public:
    // 用途：加载三个官方渠道配置，默认全部关闭并保留空参数；日期：2026-11-05。
    explicit OfficialPayments(const std::wstring &root);
    // 用途：返回已配置渠道摘要，不泄露商户密钥；日期：2026-11-05。
    Json Status() const;
    // 用途：在事务中校验权限并创建持久渠道操作意图；日期：2026-11-05。
    Json Begin(BackendStore &store, const std::string &action, const Json &body, const std::string &token);
    // 用途：在数据库事务外调用官方接口，超时不自动重放写入；日期：2026-11-05。
    Json Perform(const Json &operation) const;
    // 用途：在新事务提交渠道响应和已证实的支付或退款；日期：2026-11-05。
    Json Finish(BackendStore &store, const Json &operation, const Json &result);
    // 用途：验证渠道原始回调并规范化经济事实；日期：2026-11-05。
    Json Verify(const std::string &channel, const std::string &body, const Headers &headers) const;
    // 用途：原子处理已验签渠道事实、金额、事件去重和迟到来款；日期：2026-11-05。
    Json Apply(BackendStore &store, const Json &event);

  private:
    Json config_;
};

class OidcIdentity
{
  public:
    // 用途：加载受信OIDC发行方和客户端配置，未配置默认关闭；日期：2026-11-05。
    explicit OidcIdentity(const std::wstring &root);
    // 用途：提供不包含秘密的SSO配置状态；日期：2026-11-05。
    Json Status() const;
    // 用途：在事务外获取发现文档并验证受信发行方及端点；日期：2026-11-05。
    Json Discover() const;
    // 用途：按受信发现端点生成RP发起注销地址；日期：2026-11-05。
    std::string LogoutUrl() const;
    // 用途：保存短期state、nonce、PKCE及浏览器绑定，返回授权地址；日期：2026-11-05。
    Json Begin(BackendStore &store, const Json &discovery, const Json &body, const std::string &token);
    // 用途：一次消费state并验证同浏览器绑定后取得令牌交换上下文；日期：2026-11-05。
    Json Consume(BackendStore &store, const Json &body);
    // 用途：交换授权码并对ID Token的JWKS/RS256和全部必要声明核验；日期：2026-11-05。
    Json Exchange(const Json &context) const;
    // 用途：按issuer/sub绑定账号，禁止通过邮箱自动接管已有账号；日期：2026-11-05。
    Json Finish(BackendStore &store, const Json &context, const Json &claims);
    // 用途：处理显式身份绑定、一次性桌面连接码和本地注销；日期：2026-11-05。
    Json Action(BackendStore &store, const std::string &action, const Json &body, const std::string &token);

  private:
    Json config_;
};

class NodeScheduler
{
  public:
    // 用途：加载调度代数并在协调器重启后冻结未决任务，禁止自动重放；日期：2026-11-05。
    explicit NodeScheduler(BackendStore &store);
    // 用途：处理项目、节点、流程、分配、任务和经过签名的节点出站协议；日期：2026-11-05。
    Json Action(BackendStore &store, const std::string &action, const Json &body, const std::string &token);

  private:
    long long epoch_ = 0;
    // 用途：在同一事务核验节点签名/序号、去重并推进其出站请求；日期：2026-11-05。
    Json NodeAction(BackendStore &store, const std::string &action, const Json &body,
                    const std::string &token);
    // 用途：按节点容量、项目、固定资源版本和未决租约事务领取任务；日期：2026-11-05。
    Json Dispatch(BackendStore &store, Json node);
};
// 用途：运行仅接收已批准固定任务的出站执行节点，记录本地去重回执；日期：2026-11-05。
int RunNode(const std::wstring &root, const std::wstring &browser, const Json &options);
} // namespace awb
