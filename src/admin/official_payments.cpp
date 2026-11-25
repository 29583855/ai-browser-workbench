#include "awb/backend_extensions.hpp"
#include "awb/browser.hpp"
#include <algorithm>
#include <ctime>

namespace awb
{
namespace
{
// 用途：严格处理人民币元字符串，禁止浮点舍入或指数记法进入账本；日期：2026-11-05。
long long Minor(const std::string &text)
{
    const auto dot = text.find('.');
    const auto whole = text.substr(0, dot);
    const auto fraction = dot == std::string::npos ? "" : text.substr(dot + 1);
    if (whole.empty() || whole.size() > 9 || whole.find_first_not_of("0123456789") != std::string::npos ||
        fraction.size() > 2 || fraction.find_first_not_of("0123456789") != std::string::npos ||
        (dot != std::string::npos && fraction.empty()))
        throw Error("PAYMENT_AMOUNT", "Invalid decimal currency amount");
    return std::stoll(whole) * 100 +
           (fraction.empty() ? 0 : std::stoll(fraction) * (fraction.size() == 1 ? 10 : 1));
}
// 用途：将整数分格式化为支付宝要求的两位小数元；日期：2026-11-05。
std::string Major(long long amount)
{
    return std::to_string(amount / 100) + "." + (amount % 100 < 10 ? "0" : "") + std::to_string(amount % 100);
}
// 用途：读取某个必须存在的签名头；日期：2026-11-05。
std::string Header(const Headers &headers, const std::string &key)
{
    const auto found = headers.find(key);
    if (found == headers.end() || found->second.empty())
        throw Error("PAYMENT_SIGNATURE", "Required channel signature header is absent");
    return found->second;
}
// 用途：校验微信或Stripe时间窗，避免未去重的新旧签名重放；日期：2026-11-05。
void Timestamp(const std::string &text)
{
    if (text.empty() || text.size() > 12 || text.find_first_not_of("0123456789") != std::string::npos)
        throw Error("PAYMENT_SIGNATURE", "Invalid channel signature timestamp");
    const auto value = std::stoll(text);
    if (value < BackendNow() - 300 || value > BackendNow() + 300)
        throw Error("PAYMENT_SIGNATURE", "Channel signature is outside the accepted time window");
}
// 用途：验证微信V3响应和通知的原文RSA签名及平台公钥序列；日期：2026-11-05。
void WechatSignature(const Json &config, const std::string &body, const Headers &headers)
{
    const auto timestamp = Header(headers, "wechatpay-timestamp");
    Timestamp(timestamp);
    const auto serial = Header(headers, "wechatpay-serial");
    if (!config.at("platformKeys").contains(serial) ||
        !RsaVerify(config.at("platformKeys").at(serial),
                   timestamp + "\n" + Header(headers, "wechatpay-nonce") + "\n" + body + "\n",
                   Header(headers, "wechatpay-signature")))
        throw Error("PAYMENT_SIGNATURE", "Wechat platform key or response signature is invalid");
}
// 用途：生成支付宝对非空参数排序后的RSA2签名正文；日期：2026-11-05。
std::string AlipayCanonical(const Headers &fields, bool notify)
{
    std::string output;
    for (const auto &item : fields)
    {
        if (item.first == "sign" || (notify && item.first == "sign_type") || item.second.empty())
            continue;
        if (!output.empty())
            output += '&';
        output += item.first + "=" + item.second;
    }
    return output;
}
// 用途：从顶层JSON中取得指定对象的原始字节，支付宝响应验签不能重新序列化；日期：2026-11-05。
std::string RawObject(const std::string &body, const std::string &key)
{
    ParseExternalJson(body);
    const auto quoted = Json(key).dump();
    bool string = false, escaped = false;
    int depth = 0;
    for (size_t i = 0; i < body.size(); ++i)
    {
        const char c = body[i];
        if (!string && depth == 1 && c == '"' && body.compare(i, quoted.size(), quoted) == 0)
        {
            size_t begin = body.find_first_not_of(" \t\r\n", i + quoted.size());
            if (begin == std::string::npos || body[begin] != ':')
                throw Error("PAYMENT_SIGNATURE", "Malformed signed response object");
            begin = body.find_first_not_of(" \t\r\n", begin + 1);
            if (begin == std::string::npos || body[begin] != '{')
                throw Error("PAYMENT_SIGNATURE", "Signed response must be an object");
            int nested = 0;
            bool inside = false, slash = false;
            for (size_t end = begin; end < body.size(); ++end)
            {
                const char value = body[end];
                if (inside)
                {
                    if (slash)
                        slash = false;
                    else if (value == '\\')
                        slash = true;
                    else if (value == '"')
                        inside = false;
                }
                else if (value == '"')
                    inside = true;
                else if (value == '{' || value == '[')
                    ++nested;
                else if ((value == '}' || value == ']') && --nested == 0)
                    return body.substr(begin, end - begin + 1);
            }
            throw Error("PAYMENT_SIGNATURE", "Signed response object is truncated");
        }
        if (string)
        {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                string = false;
        }
        else if (c == '"')
            string = true;
        else if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']')
            --depth;
    }
    throw Error("PAYMENT_SIGNATURE", "Signed response object is missing");
}
// 用途：生成固定UTC+8的支付宝协议时间，不依赖运行机时区；日期：2026-11-05。
std::string AlipayTime()
{
    const auto seconds = static_cast<time_t>(BackendNow() + 8 * 3600);
    tm calendar{};
    gmtime_s(&calendar, &seconds);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &calendar);
    return buffer;
}
// 用途：验证规范化渠道经济事实的金额、币种和资源标识形状；日期：2026-11-05。
Json Fact(const std::string &channel, const std::string &event, const std::string &order,
          const std::string &transaction, long long amount, const std::string &currency, bool refund)
{
    if (amount <= 0 || amount > 1200000000 || currency != "CNY" || order.empty() || transaction.empty())
        throw Error("PAYMENT_AMOUNT", "Channel currency, amount or reference is invalid");
    return {{"channel", channel},
            {"eventId", channel + ":" + event},
            {"orderId", order},
            {"transaction", channel + ":" + transaction},
            {"amountMinor", amount},
            {"currency", currency},
            {"type", refund ? "refunded" : "paid"},
            {"channelTransactionId", transaction}};
}
// 用途：从官方渠道得到的对象中规范化Stripe成功支付或退款；日期：2026-11-05。
Json StripeFact(const Json &object, const std::string &event, bool refund)
{
    const auto currency = Field(object, "currency");
    if (currency != "cny")
        throw Error("PAYMENT_AMOUNT", "Stripe currency differs from the CNY order");
    const auto order =
        refund ? Field(object.at("metadata"), "orderId") : Field(object, "client_reference_id");
    if (!refund && object.contains("metadata") && object.at("metadata").value("orderId", order) != order)
        throw Error("PAYMENT_EVENT", "Stripe client reference and metadata differ");
    return Fact("stripe", event, order, refund ? Field(object, "id") : Field(object, "payment_intent"),
                object.at(refund ? "amount" : "amount_total").get<long long>(), "CNY", refund);
}
} // namespace

// 用途：生成安全关闭的官方渠道配置模板，开启时检查必要参数；日期：2026-11-05。
OfficialPayments::OfficialPayments(const std::wstring &root)
{
    const auto path = SafeAbsolute(root + L"\\official-payments.json");
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        AtomicWrite(path, Json{{"allowLoopbackTest", false},
                               {"alipay",
                                {{"enabled", false},
                                 {"appId", ""},
                                 {"sellerId", ""},
                                 {"privateKeyPem", ""},
                                 {"publicKeyPem", ""},
                                 {"baseUrl", "https://openapi.alipay.com/gateway.do"},
                                 {"notifyUrl", ""}}},
                               {"wechat",
                                {{"enabled", false},
                                 {"appId", ""},
                                 {"merchantId", ""},
                                 {"merchantSerial", ""},
                                 {"privateKeyPem", ""},
                                 {"apiV3Key", ""},
                                 {"platformKeys", Json::object()},
                                 {"baseUrl", "https://api.mch.weixin.qq.com"},
                                 {"notifyUrl", ""}}},
                               {"stripe",
                                {{"enabled", false},
                                 {"secretKey", ""},
                                 {"webhookSecrets", Json::array()},
                                 {"liveMode", false},
                                 {"apiVersion", "2024-06-20"},
                                 {"baseUrl", "https://api.stripe.com"},
                                 {"successUrl", ""},
                                 {"cancelUrl", ""}}}}
                                  .dump(2) +
                              "\n");
    config_ = ParseExternalJson(ReadFileBounded(path, 256 * 1024));
    for (const auto *name : {"alipay", "wechat", "stripe"})
    {
        const auto &channel = config_.at(name);
        if (!channel.value("enabled", false))
            continue;
        for (const auto *field :
             name == std::string("stripe")
                 ? std::vector<const char *>{"secretKey", "apiVersion", "baseUrl", "successUrl", "cancelUrl"}
             : name == std::string("wechat")
                 ? std::vector<const char *>{"appId", "merchantId", "merchantSerial", "privateKeyPem",
                                             "baseUrl", "notifyUrl", "apiV3Key"}
                 : std::vector<const char *>{"appId", "sellerId", "privateKeyPem", "publicKeyPem", "baseUrl",
                                             "notifyUrl"})
            if (!channel.contains(field) || !channel.at(field).is_string() ||
                channel.at(field).get<std::string>().empty())
                throw Error("PAYMENT_CONFIG",
                            std::string("Missing official channel parameter: ") + name + "." + field);
        const auto base = Field(channel, "baseUrl", 2048);
        const bool fixture =
            config_.value("allowLoopbackTest", false) && Browser::Origin(base).find("http://127.0.0.1:") == 0;
        if (!fixture && ((name == std::string("alipay") && base != "https://openapi.alipay.com/gateway.do" &&
                          base != "https://openapi-sandbox.dl.alipaydev.com/gateway.do") ||
                         (name == std::string("wechat") && base != "https://api.mch.weixin.qq.com" &&
                          base != "https://api2.mch.weixin.qq.com") ||
                         (name == std::string("stripe") && base != "https://api.stripe.com")))
            throw Error("PAYMENT_CONFIG", "Official API endpoint is outside the trusted channel list");
        for (const auto *url_field : name == std::string("stripe")
                                         ? std::vector<const char *>{"successUrl", "cancelUrl"}
                                         : std::vector<const char *>{"notifyUrl"})
        {
            const auto target = Field(channel, url_field, 4096);
            const auto origin = Browser::Origin(target);
            if (origin.find("https://") != 0 && !(fixture && origin.find("http://127.0.0.1:") == 0))
                throw Error("PAYMENT_CONFIG", "Channel callback and return URLs must use HTTPS");
        }
        if (name != std::string("stripe"))
        {
            const auto probe = RsaSign(channel.at("privateKeyPem"), "AWB payment configuration");
            (void)probe;
        }
        if (name == std::string("wechat") &&
            (channel.at("apiV3Key").get<std::string>().size() != 32 || channel.at("platformKeys").empty()))
            throw Error("PAYMENT_CONFIG",
                        "Wechat requires a 32-byte API V3 key and trusted platform public keys");
        if (name == std::string("stripe") && channel.at("webhookSecrets").empty())
            throw Error("PAYMENT_CONFIG", "Stripe requires at least one webhook signing secret");
    }
}
// 用途：只公开已配置状态及渠道标识；日期：2026-11-05。
Json OfficialPayments::Status() const
{
    Json output = Json::array();
    for (const auto *name : {"alipay", "wechat", "stripe"})
        output.push_back({{"channel", name}, {"configured", config_.at(name).value("enabled", false)}});
    return output;
}
// 用途：先原子记录调用意图及渠道绑定，同一订单不能跨渠道重复下单；日期：2026-11-05。
Json OfficialPayments::Begin(BackendStore &store, const std::string &action, const Json &body,
                             const std::string &token)
{
    const auto user = store.Authenticate(token);
    const auto channel = Field(body, "channel", 16);
    if (!config_.contains(channel) || !config_.at(channel).value("enabled", false))
        throw Error("PAYMENT_NOT_CONFIGURED", "Official channel parameters are not configured");
    auto order = store.Require("order", Field(body, "orderId"));
    const bool refund = action == "payment.channel.refund" || action == "payment.channel.refund-query";
    if (!(refund && user.at("platform").get<bool>()))
    {
        const auto member = TenantMember(store, user, order.at("tenantId"));
        if (member.at("role") != "owner" && member.at("role") != "admin" && member.at("role") != "finance")
            throw Error("FORBIDDEN", "Financial organization role required");
    }
    if (action == "payment.channel.refund" && !user.at("platform").get<bool>())
        throw Error("FORBIDDEN", "Platform operator must approve the real channel refund");
    if (refund && order.at("state") != "refund_pending" && order.at("state") != "refunded")
        throw Error("ORDER_STATE", "Request refund before calling the official channel");
    if (action == "payment.channel.create" &&
        (order.at("state") != "pending" || order.at("expiresAt") <= BackendNow()))
        throw Error("ORDER_STATE", "Only a current pending order can create checkout");
    if (order.contains("channel") && order.at("channel") != channel)
        throw Error("PAYMENT_CONFLICT", "Order is already bound to another channel");
    if (!order.contains("channel") && action != "payment.channel.create")
        throw Error("ORDER_STATE", "Create checkout before querying the channel");
    const auto verb = action.substr(std::string("payment.channel.").size());
    const auto key = Field(body, "idempotencyKey", 128);
    const auto id = Sha256(channel + ":" + order.at("id").get<std::string>() + ":" + verb + ":" + key);
    const auto previous = store.Get("channel_operation", id);
    if (!previous.empty())
    {
        if (previous.at("state") == "completed")
            return previous;
        throw Error("PAYMENT_OPERATION_UNKNOWN",
                    "Query the order using a new query key; do not blindly repeat a write", true);
    }
    if (verb == "create")
    {
        const auto existing = store.Get("channel_create", order.at("id"));
        if (!existing.empty())
            throw Error("PAYMENT_OPERATION_EXISTS", "Checkout was already requested; query its result", true);
        order["channel"] = channel;
        order["channelBoundAt"] = BackendNow();
        store.Put("order", order.at("id"), order);
        store.Put("channel_create", order.at("id"), {{"operationId", id}});
    }
    if (verb == "refund-query" && !order.contains("channelRefundAt"))
        throw Error("ORDER_STATE", "An explicit refund attempt must precede refund query");
    if (verb == "refund")
    {
        order = store.PrepareRefund(order);
        if (order.contains("channelRefundAt") &&
            BackendNow() - order.at("channelRefundAt").get<long long>() > 23 * 3600)
            throw Error("PAYMENT_RECONCILIATION_REQUIRED",
                        "Refund idempotency window elapsed; reconcile before another write");
        if (!order.contains("channelRefundAt"))
            order["channelRefundAt"] = BackendNow();
        store.Put("order", order.at("id"), order);
    }
    const Json operation{{"id", id},
                         {"tenantId", order.at("tenantId")},
                         {"orderId", order.at("id")},
                         {"order", order},
                         {"channel", channel},
                         {"verb", verb},
                         {"state", "prepared"},
                         {"createdAt", BackendNow()},
                         {"actor", user.at("id")}};
    store.Put("channel_operation", id, operation);
    store.Audit(user.at("id"), action, order.at("tenantId"), id);
    return operation;
}
// 用途：按官方协议发送下单、查询和退款请求，返回最小已验签结果；日期：2026-11-05。
Json OfficialPayments::Perform(const Json &operation) const
{
    if (operation.at("state") == "completed")
        return operation.at("result");
    const auto channel = operation.at("channel").get<std::string>();
    const auto verb = operation.at("verb").get<std::string>();
    const auto &order = operation.at("order");
    const auto id = order.at("id").get<std::string>();
    const auto amount = order.at("amountMinor").get<long long>();
    const auto &settings = config_.at(channel);
    const bool test = config_.value("allowLoopbackTest", false);
    const bool refund = verb == "refund" || verb == "refund-query";
    Json result{{"state", "pending"}, {"channel", channel}, {"orderId", id}};
    if (channel == "alipay")
    {
        const auto method = verb == "create"   ? "alipay.trade.precreate"
                            : verb == "query"  ? "alipay.trade.query"
                            : verb == "refund" ? "alipay.trade.refund"
                                               : "alipay.trade.fastpay.refund.query";
        Json business{{"out_trade_no", id}};
        if (verb == "create")
        {
            business["total_amount"] = Major(amount);
            business["subject"] = order.at("plan").at("name");
        }
        if (refund)
        {
            business["out_request_no"] = "refund-" + id;
            if (verb == "refund")
                business["refund_amount"] = Major(amount);
        }
        Headers fields{{"app_id", settings.at("appId")},
                       {"method", method},
                       {"format", "JSON"},
                       {"charset", "utf-8"},
                       {"sign_type", "RSA2"},
                       {"timestamp", AlipayTime()},
                       {"version", "1.0"},
                       {"biz_content", business.dump()}};
        if (verb == "create")
            fields["notify_url"] = settings.at("notifyUrl");
        fields["sign"] = RsaSign(settings.at("privateKeyPem"), AlipayCanonical(fields, false));
        const auto response = BackendHttp(
            "POST", settings.at("baseUrl"),
            {{"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"}}, EncodeForm(fields), test);
        const auto parsed = ParseExternalJson(response.body);
        std::string object_key = method;
        std::replace(object_key.begin(), object_key.end(), '.', '_');
        object_key += "_response";
        if (!RsaVerify(settings.at("publicKeyPem"), RawObject(response.body, object_key),
                       Field(parsed, "sign", 2048)))
            throw Error("PAYMENT_SIGNATURE", "Alipay response signature is invalid", true);
        const auto &object = parsed.at(object_key);
        if (response.status != 200 || object.value("code", "") != "10000")
        {
            if (verb == "query" && object.value("sub_code", "") == "ACQ.TRADE_NOT_EXIST")
                return result;
            throw Error("PAYMENT_CHANNEL_REJECTED", "Alipay did not confirm the requested operation",
                        verb == "create" || verb == "refund");
        }
        result["channelReference"] = object.value("trade_no", id);
        if (verb == "create")
            result["checkoutCode"] = Field(object, "qr_code", 4096);
        const bool paid = verb == "query" && (object.value("trade_status", "") == "TRADE_SUCCESS" ||
                                              object.value("trade_status", "") == "TRADE_FINISHED");
        const bool refunded =
            refund && (verb == "refund" || object.value("refund_status", "") == "REFUND_SUCCESS");
        if (paid || refunded)
        {
            if (object.value("out_trade_no", id) != id ||
                (paid && object.value("buyer_pay_amount", object.value("total_amount", "")).empty()))
                throw Error("PAYMENT_EVENT", "Alipay response does not match the order");
            const auto verified_amount = Minor(Field(object, paid               ? "total_amount"
                                                             : verb == "refund" ? "refund_fee"
                                                                                : "refund_amount"));
            result["fact"] =
                Fact(channel, "query-" + operation.at("id").get<std::string>(), id,
                     refunded ? "refund-" + id : Field(object, "trade_no"), verified_amount, "CNY", refunded);
        }
    }
    else if (channel == "wechat")
    {
        std::string path, method = "GET", body;
        if (verb == "create")
        {
            path = "/v3/pay/transactions/native";
            method = "POST";
            body = Json{{"appid", settings.at("appId")},
                        {"mchid", settings.at("merchantId")},
                        {"description", order.at("plan").at("name")},
                        {"out_trade_no", id},
                        {"notify_url", settings.at("notifyUrl")},
                        {"amount", {{"total", amount}, {"currency", "CNY"}}}}
                       .dump();
        }
        else if (verb == "query")
            path = "/v3/pay/transactions/out-trade-no/" + UrlEncode(id) +
                   "?mchid=" + UrlEncode(settings.at("merchantId"));
        else if (verb == "refund")
        {
            method = "POST";
            path = "/v3/refund/domestic/refunds";
            body = Json{{"out_trade_no", id},
                        {"out_refund_no", "refund-" + id},
                        {"notify_url", settings.at("notifyUrl")},
                        {"amount", {{"refund", amount}, {"total", amount}, {"currency", "CNY"}}}}
                       .dump();
        }
        else
            path = "/v3/refund/domestic/refunds/" + UrlEncode("refund-" + id);
        const auto timestamp = std::to_string(BackendNow()), nonce = RandomId();
        const auto signature = RsaSign(settings.at("privateKeyPem"), method + "\n" + path + "\n" + timestamp +
                                                                         "\n" + nonce + "\n" + body + "\n");
        const auto authorization = "WECHATPAY2-SHA256-RSA2048 mchid=\"" + Field(settings, "merchantId") +
                                   "\",nonce_str=\"" + nonce + "\",timestamp=\"" + timestamp +
                                   "\",serial_no=\"" + Field(settings, "merchantSerial") + "\",signature=\"" +
                                   signature + "\"";
        const auto response = BackendHttp(method, settings.at("baseUrl").get<std::string>() + path,
                                          {{"Content-Type", "application/json"},
                                           {"Accept", "application/json"},
                                           {"Authorization", authorization}},
                                          body, test);
        WechatSignature(settings, response.body, response.headers);
        const auto object = ParseExternalJson(response.body);
        if (response.status < 200 || response.status >= 300)
        {
            if (verb == "query" && object.value("code", "") == "ORDER_NOT_EXIST")
                return result;
            throw Error("PAYMENT_CHANNEL_REJECTED", "Wechat did not confirm the requested operation",
                        method == "POST");
        }
        if (verb == "create")
            result["checkoutCode"] = Field(object, "code_url", 4096);
        else if ((!refund && object.value("trade_state", "") == "SUCCESS") ||
                 (refund && object.value("status", "") == "SUCCESS"))
        {
            if (Field(object, "out_trade_no") != id ||
                (object.contains("mchid") && Field(object, "mchid") != settings.at("merchantId")) ||
                (!refund && Field(object, "appid") != settings.at("appId")))
                throw Error("PAYMENT_EVENT", "Wechat merchant, application or order differs");
            if (refund && Field(object, "out_refund_no") != "refund-" + id)
                throw Error("PAYMENT_EVENT", "Wechat refund reference differs");
            result["fact"] = Fact(channel, "query-" + operation.at("id").get<std::string>(), id,
                                  refund ? "refund-" + id : Field(object, "transaction_id"),
                                  object.at("amount").at(refund ? "refund" : "total").get<long long>(),
                                  object.at("amount").value("currency", "CNY"), refund);
        }
        result["channelReference"] = object.value(refund ? "refund_id" : "transaction_id", id);
    }
    else
    {
        std::string path, method = "GET", data;
        Headers fields;
        bool recover_create = verb == "query" && !order.contains("channelReference");
        if (recover_create && BackendNow() - order.at("channelBoundAt").get<long long>() > 23 * 3600)
            throw Error("PAYMENT_RECONCILIATION_REQUIRED",
                        "Stripe idempotency retention window elapsed; reconcile in merchant dashboard");
        if (verb == "create" || recover_create)
        {
            method = "POST";
            path = "/v1/checkout/sessions";
            fields = {{"mode", "payment"},
                      {"client_reference_id", id},
                      {"metadata[orderId]", id},
                      {"payment_intent_data[metadata][orderId]", id},
                      {"success_url", settings.at("successUrl")},
                      {"cancel_url", settings.at("cancelUrl")},
                      {"line_items[0][quantity]", "1"},
                      {"line_items[0][price_data][currency]", "cny"},
                      {"line_items[0][price_data][unit_amount]", std::to_string(amount)},
                      {"line_items[0][price_data][product_data][name]", order.at("plan").at("name")}};
        }
        else if (verb == "query")
            path = "/v1/checkout/sessions/" + UrlEncode(Field(order, "channelReference"));
        else if (verb == "refund")
        {
            method = "POST";
            path = "/v1/refunds";
            fields = {{"payment_intent", Field(order, "channelTransactionId")},
                      {"amount", std::to_string(amount)},
                      {"metadata[orderId]", id}};
        }
        else
        {
            const auto reference = order.value("channelRefundId", "");
            if (reference.empty())
            {
                method = "POST";
                path = "/v1/refunds";
                fields = {{"payment_intent", Field(order, "channelTransactionId")},
                          {"amount", std::to_string(amount)},
                          {"metadata[orderId]", id}};
                if (BackendNow() - order.value("channelRefundAt", BackendNow()) > 23 * 3600)
                    throw Error("PAYMENT_RECONCILIATION_REQUIRED",
                                "Query Stripe refund in merchant dashboard after idempotency expiry");
            }
            else
                path = "/v1/refunds/" + UrlEncode(reference);
        }
        Headers headers{{"Authorization", "Bearer " + Field(settings, "secretKey", 1024)},
                        {"Stripe-Version", Field(settings, "apiVersion")},
                        {"Content-Type", "application/x-www-form-urlencoded"}};
        if (method == "POST")
            headers["Idempotency-Key"] = std::string("awb-") + (refund ? "refund-" : "checkout-") + id;
        data = EncodeForm(fields);
        const auto response =
            BackendHttp(method, settings.at("baseUrl").get<std::string>() + path, headers, data, test);
        if (response.status < 200 || response.status >= 300)
            throw Error("PAYMENT_CHANNEL_REJECTED", "Stripe did not confirm the requested operation",
                        method == "POST");
        const auto object = ParseExternalJson(response.body);
        if (object.value("livemode", false) != settings.value("liveMode", false))
            throw Error("PAYMENT_EVENT", "Stripe live/test mode differs from configured mode");
        if (refund)
        {
            result["channelRefundId"] = Field(object, "id");
            if (object.value("status", "") == "succeeded")
                result["fact"] = StripeFact(object, "query-" + operation.at("id").get<std::string>(), true);
        }
        else
        {
            if (Field(object, "client_reference_id") != id)
                throw Error("PAYMENT_EVENT", "Stripe checkout belongs to another order");
            result["channelReference"] = Field(object, "id");
            if (object.contains("url") && object.at("url").is_string())
                result["checkoutUrl"] = Field(object, "url", 4096);
            if (object.value("payment_status", "") == "paid")
                result["fact"] = StripeFact(object, "query-" + operation.at("id").get<std::string>(), false);
        }
    }
    if (result.contains("fact"))
        result["state"] = "confirmed";
    return result;
}
// 用途：将渠道最小结果保存到持久意图，查单和回调竞争只入账一次；日期：2026-11-05。
Json OfficialPayments::Finish(BackendStore &store, const Json &operation, const Json &result)
{
    auto current = store.Require("channel_operation", operation.at("id"));
    if (current.at("state") == "completed")
        return current.at("result");
    auto order = store.Require("order", operation.at("orderId"));
    Json response = result;
    if (result.contains("fact"))
        response["settlement"] = Apply(store, result.at("fact"));
    order = store.Require("order", operation.at("orderId"));
    for (const auto *field : {"channelReference", "channelRefundId"})
        if (result.contains(field))
            order[field] = result.at(field);
    if (operation.at("verb") == "refund")
        order["channelRefundAt"] = operation.at("createdAt");
    store.Put("order", order.at("id"), order);
    current["state"] = result.contains("error") ? "unknown" : "completed";
    current["result"] = response;
    current["finishedAt"] = BackendNow();
    store.Put("channel_operation", current.at("id"), current);
    return response;
}
// 用途：验签官方回调原文并只产生已确认的支付或退款事实；日期：2026-11-05。
Json OfficialPayments::Verify(const std::string &channel, const std::string &body,
                              const Headers &headers) const
{
    if (!config_.contains(channel) || !config_.at(channel).value("enabled", false))
        throw Error("PAYMENT_NOT_CONFIGURED", "Official channel is not configured");
    const auto &settings = config_.at(channel);
    if (channel == "alipay")
    {
        const auto fields = DecodeForm(body);
        if (!fields.count("sign_type") || fields.at("sign_type") != "RSA2" || !fields.count("sign") ||
            !RsaVerify(settings.at("publicKeyPem"), AlipayCanonical(fields, true), fields.at("sign")))
            throw Error("PAYMENT_SIGNATURE", "Alipay notification signature is invalid");
        Json object(fields);
        if (Field(object, "app_id") != settings.at("appId") ||
            Field(object, "seller_id") != settings.at("sellerId"))
            throw Error("PAYMENT_EVENT", "Alipay application or seller differs");
        if (object.value("trade_status", "") != "TRADE_SUCCESS" &&
            object.value("trade_status", "") != "TRADE_FINISHED")
            return {{"ignored", true}};
        return Fact(channel, Field(object, "notify_id"), Field(object, "out_trade_no"),
                    Field(object, "trade_no"), Minor(Field(object, "total_amount")), "CNY", false);
    }
    if (channel == "wechat")
    {
        WechatSignature(settings, body, headers);
        const auto envelope = ParseExternalJson(body);
        const auto type = Field(envelope, "event_type");
        if (type != "TRANSACTION.SUCCESS" && type != "REFUND.SUCCESS")
            return {{"ignored", true}};
        const auto &resource = envelope.at("resource");
        if (resource.value("algorithm", "") != "AEAD_AES_256_GCM")
            throw Error("PAYMENT_SIGNATURE", "Unsupported Wechat resource algorithm");
        const auto plaintext = AesGcmDecrypt(settings.at("apiV3Key"), Field(resource, "nonce"),
                                             resource.value("associated_data", ""),
                                             Base64(Field(resource, "ciphertext", 65536), false, true));
        const auto object = ParseExternalJson(plaintext);
        const bool refund = type == "REFUND.SUCCESS";
        if (Field(object, "mchid") != settings.at("merchantId") ||
            (!refund && Field(object, "appid") != settings.at("appId")) ||
            Field(object, refund ? "refund_status" : "trade_state") != "SUCCESS")
            throw Error("PAYMENT_EVENT", "Wechat merchant, application or status differs");
        const auto id = Field(object, "out_trade_no");
        if (refund && Field(object, "out_refund_no") != "refund-" + id)
            throw Error("PAYMENT_EVENT", "Wechat refund reference differs");
        return Fact(channel, Field(envelope, "id"), id,
                    refund ? "refund-" + id : Field(object, "transaction_id"),
                    object.at("amount").at(refund ? "refund" : "total").get<long long>(),
                    object.at("amount").value("currency", "CNY"), refund);
    }
    const auto signature = Header(headers, "stripe-signature");
    std::string timestamp;
    std::vector<std::string> signatures;
    size_t begin = 0;
    while (begin < signature.size())
    {
        const auto end = signature.find(',', begin);
        const auto item = signature.substr(begin, end == std::string::npos ? end : end - begin);
        if (item.find("t=") == 0)
        {
            if (!timestamp.empty())
                throw Error("PAYMENT_SIGNATURE", "Duplicate Stripe timestamp");
            timestamp = item.substr(2);
        }
        if (item.find("v1=") == 0)
            signatures.push_back(item.substr(3));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    Timestamp(timestamp);
    bool verified = false;
    for (const auto &secret : settings.at("webhookSecrets"))
        for (const auto &candidate : signatures)
            verified |= SecureEqual(Sha256(timestamp + "." + body, secret), candidate);
    if (!verified)
        throw Error("PAYMENT_SIGNATURE", "Stripe webhook signature is invalid");
    const auto event = ParseExternalJson(body);
    if (event.value("livemode", false) != settings.value("liveMode", false))
        throw Error("PAYMENT_EVENT", "Stripe notification mode differs");
    const auto type = Field(event, "type");
    const auto &object = event.at("data").at("object");
    if ((type == "checkout.session.completed" || type == "checkout.session.async_payment_succeeded") &&
        object.value("payment_status", "") == "paid" && object.value("mode", "") == "payment")
        return StripeFact(object, Field(event, "id"), false);
    if ((type == "refund.created" || type == "refund.updated") && object.value("status", "") == "succeeded")
        return StripeFact(object, Field(event, "id"), true);
    return {{"ignored", true}};
}
// 用途：将官方经济事实幂等入账，金额或渠道不一致必须拒绝；日期：2026-11-05。
Json OfficialPayments::Apply(BackendStore &store, const Json &event)
{
    if (event.value("ignored", false))
        return {{"accepted", true}, {"ignored", true}};
    const auto id = Field(event, "eventId");
    const auto digest = Sha256(event.dump());
    const auto old = store.Get("event", id);
    if (!old.empty())
    {
        if (old.at("digest") != digest)
            throw Error("PAYMENT_CONFLICT", "Official event ID has conflicting contents");
        return {{"accepted", true}, {"duplicate", true}};
    }
    auto order = store.Require("order", Field(event, "orderId"));
    if (order.value("channel", "") != Field(event, "channel") ||
        order.at("amountMinor") != event.at("amountMinor") || Field(event, "currency") != "CNY")
        throw Error("PAYMENT_AMOUNT", "Official channel or amount differs from immutable order");
    const bool refund = event.at("type") == "refunded";
    const bool review = !refund && (order.at("state") == "cancelled" ||
                                    (order.at("state") == "pending" && order.at("expiresAt") < BackendNow()));
    const auto transaction = Field(event, "transaction");
    if (review)
        store.Put("payment_review", id,
                  {{"id", id},
                   {"orderId", order.at("id")},
                   {"tenantId", order.at("tenantId")},
                   {"transaction", transaction},
                   {"amountMinor", order.at("amountMinor")},
                   {"state", "pending"},
                   {"reason", "verified-official-payment-after-close"},
                   {"at", BackendNow()}});
    else if (!((!refund && order.at("state") == "paid") || (refund && order.at("state") == "refunded")))
        order = store.Settle(order, transaction, refund, refund);
    else if (order.value(refund ? "refundTransaction" : "paymentTransaction", "") != transaction)
        throw Error("PAYMENT_CONFLICT", "Different official transaction already settled this order");
    if (!refund)
        order["channelTransactionId"] = event.at("channelTransactionId");
    store.Put("order", order.at("id"), order);
    store.Put("event", id,
              {{"id", id},
               {"tenantId", order.at("tenantId")},
               {"orderId", order.at("id")},
               {"digest", digest},
               {"type", event.at("type")},
               {"channel", event.at("channel")},
               {"at", BackendNow()}});
    store.Audit("official-channel", event.at("type"), order.at("tenantId"), order.at("id"));
    return {{"accepted", true},
            {"duplicate", false},
            {"reconciliationRequired", review},
            {"orderState", order.at("state")}};
}
} // namespace awb
