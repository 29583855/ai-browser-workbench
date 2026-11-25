#include "awb/control_plane.hpp"
#include <wincrypt.h>
#include <winhttp.h>

namespace awb
{
namespace
{
class Internet
{
  public:
    HINTERNET value;
    // 用途：接管WinHTTP句柄并立即拒绝创建失败；日期：2026-11-05。
    explicit Internet(HINTERNET handle) : value(handle)
    {
        if (!value)
            throw Error("CONTROL_UNAVAILABLE", "Cannot create control-plane HTTP request");
    }
    // 用途：按WinHTTP规则关闭句柄；日期：2026-11-05。
    ~Internet()
    {
        WinHttpCloseHandle(value);
    }
};
} // namespace
// 用途：有界请求控制面，禁用重定向避免会话令牌被转发，失败不自动重放业务动作；日期：2026-11-05。
Json ControlRequest(const std::string &url, const std::string &action, const Json &body,
                    const std::string &token, const std::string &client_certificate)
{
    const auto target = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(target.c_str(), static_cast<DWORD>(target.size()), 0, &parts))
        throw Error("INVALID_ARGUMENTS", "Invalid control-plane URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if ((!secure && (parts.nScheme != INTERNET_SCHEME_HTTP || host != L"127.0.0.1")) ||
        (!path.empty() && path != L"/") || parts.dwExtraInfoLength || parts.dwUserNameLength ||
        parts.dwPasswordLength)
        throw Error("INVALID_ARGUMENTS", "Use HTTPS origin or loopback HTTP origin without credentials/path");
    if (action.empty() || action.find_first_not_of("abcdefghijklmnopqrstuvwxyz.-") != std::string::npos ||
        token.find_first_of("\r\n") != std::string::npos)
        throw Error("INVALID_ARGUMENTS", "Invalid action or token");
    Internet session(WinHttpOpen(L"AWB-Control/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpSetTimeouts(session.value, 3000, 3000, 5000, 10000);
    Internet connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    Internet request(WinHttpOpenRequest(connection.value, L"POST", Wide("/api/" + action).c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        secure ? WINHTTP_FLAG_SECURE : 0));
    DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy)))
        throw Error("CONTROL_UNAVAILABLE", "Cannot disable automatic Windows authentication");
    if (!client_certificate.empty())
    {
        if (!secure || client_certificate.size() != 40 ||
            client_certificate.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw Error("NODE_CERTIFICATE", "mTLS requires HTTPS and a 40-digit SHA1 certificate thumbprint");
        std::vector<BYTE> hash;
        for (size_t offset = 0; offset < 40; offset += 2)
            hash.push_back(static_cast<BYTE>(std::stoul(client_certificate.substr(offset, 2), nullptr, 16)));
        const auto store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                         CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG, L"MY");
        if (!store)
            throw Error("NODE_CERTIFICATE", "Cannot open current-user certificate store");
        CRYPT_HASH_BLOB blob{static_cast<DWORD>(hash.size()), hash.data()};
        const auto certificate = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                            CERT_FIND_SHA1_HASH, &blob, nullptr);
        const BOOL configured =
            certificate && WinHttpSetOption(request.value, WINHTTP_OPTION_CLIENT_CERT_CONTEXT,
                                            const_cast<CERT_CONTEXT *>(certificate), sizeof(CERT_CONTEXT));
        if (certificate)
            CertFreeCertificateContext(certificate);
        CertCloseStore(store, 0);
        if (!configured)
            throw Error("NODE_CERTIFICATE", "mTLS certificate is missing or cannot be selected");
    }
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)))
        throw Error("CONTROL_UNAVAILABLE", "Cannot disable HTTP redirects");
    const auto headers = Wide("Content-Type: application/json\r\nAuthorization: Bearer " + token + "\r\n");
    const auto data = body.dump();
    if (data.size() > 768 * 1024)
        throw Error("INVALID_ARGUMENTS", "Control-plane body exceeds budget");
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
                            const_cast<char *>(data.data()), static_cast<DWORD>(data.size()),
                            static_cast<DWORD>(data.size()), 0) ||
        !WinHttpReceiveResponse(request.value, nullptr))
        throw Error("CONTROL_UNAVAILABLE", "Control-plane request failed; reconcile state before retrying",
                    true);
    std::string response;
    for (;;)
    {
        char buffer[8192];
        DWORD count = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &count))
            throw Error("CONTROL_UNAVAILABLE", "Control-plane response was interrupted", true);
        if (!count)
            break;
        response.append(buffer, count);
        if (response.size() > 8 * 1024 * 1024)
            throw Error("CONTROL_UNAVAILABLE", "Control-plane response exceeds budget", true);
    }
    const auto result = ParseExternalJson(response);
    if (!result.value("ok", false))
        throw Error(result.at("error").value("code", "CONTROL_FAILED"),
                    result.at("error").value("message", "Control-plane request rejected"),
                    result.at("error").value("outcomeUnknown", false));
    return result.at("result");
}
} // namespace awb
