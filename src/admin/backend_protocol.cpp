#include "awb/backend_extensions.hpp"
#include "awb/browser.hpp"
#include <algorithm>
#include <bcrypt.h>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ncrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

namespace awb
{
namespace
{
// 用途：将CNG失败转换成不含密钥或协议正文的错误；日期：2026-11-05。
void CryptoCheck(bool success)
{
    if (!success)
        throw Error("PROTOCOL_CRYPTO", "Windows cryptographic operation failed");
}
// 用途：计算原始SHA256或HMAC字节，所有密码算法由CNG完成；日期：2026-11-05。
std::string HashBytes(const std::string &text, const std::string &key)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    CryptoCheck(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                            key.empty() ? 0 : BCRYPT_ALG_HANDLE_HMAC_FLAG) >= 0);
    std::string output(32, '\0');
    const auto status = BCryptHash(
        algorithm, key.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<char *>(key.data())),
        static_cast<ULONG>(key.size()), reinterpret_cast<PUCHAR>(const_cast<char *>(text.data())),
        static_cast<ULONG>(text.size()), reinterpret_cast<PUCHAR>(&output[0]), 32);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    CryptoCheck(status >= 0);
    return output;
}
// 用途：从PEM中读取标准DER，拒绝超预算或非法编码；日期：2026-11-05。
std::vector<BYTE> Pem(const std::string &text)
{
    if (text.size() > 32768)
        throw Error("PROTOCOL_KEY", "PEM key exceeds budget");
    DWORD count = 0;
    CryptoCheck(CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64HEADER,
                                     nullptr, &count, nullptr, nullptr) != FALSE);
    std::vector<BYTE> output(count);
    CryptoCheck(CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64HEADER,
                                     output.data(), &count, nullptr, nullptr) != FALSE);
    return output;
}
class PrivateKey
{
  public:
    NCRYPT_PROV_HANDLE provider = 0;
    NCRYPT_KEY_HANDLE key = 0;
    // 用途：导入临时PKCS8私钥，不向系统密钥库永久写入商户密钥；日期：2026-11-05。
    explicit PrivateKey(const std::string &pem)
    {
        auto bytes = Pem(pem);
        const auto opened = NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0);
        const auto imported =
            opened == ERROR_SUCCESS
                ? NCryptImportKey(provider, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, &key, bytes.data(),
                                  static_cast<DWORD>(bytes.size()), NCRYPT_SILENT_FLAG)
                : opened;
        SecureZeroMemory(bytes.data(), bytes.size());
        if (imported != ERROR_SUCCESS)
        {
            if (provider)
                NCryptFreeObject(provider);
            provider = 0;
            throw Error("PROTOCOL_KEY", "Expected an unencrypted PKCS8 RSA private key");
        }
    }
    // 用途：及时释放导入的私钥和CNG提供者；日期：2026-11-05。
    ~PrivateKey()
    {
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
    }
};
class ProtocolInternetHandle
{
  public:
    HINTERNET value;
    // 用途：接管WinHTTP句柄并禁止空句柄继续请求；日期：2026-11-05。
    explicit ProtocolInternetHandle(HINTERNET handle) : value(handle)
    {
        if (!value)
            throw Error("CHANNEL_TRANSPORT", "Cannot create bounded HTTP request");
    }
    // 用途：关闭本请求的WinHTTP句柄；日期：2026-11-05。
    ~ProtocolInternetHandle()
    {
        WinHttpCloseHandle(value);
    }
};
// 用途：清理HTTP头空白并统一ASCII头名称；日期：2026-11-05。
std::string Trim(std::string text, bool lower = false)
{
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return "";
    text = text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
    if (lower)
        for (auto &byte : text)
            byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
    return text;
}
} // namespace

// 用途：读取UTC秒供签名时限与调度事件共用；日期：2026-11-05。
long long BackendNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
// 用途：使用常量时间比较相同长度的认证摘要；日期：2026-11-05。
bool SecureEqual(const std::string &left, const std::string &right)
{
    if (left.size() != right.size())
        return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);
    return difference == 0;
}
// 用途：使用系统DPAPI保存秘密，异机复制不能直接复用节点身份；日期：2026-11-05。
std::string ProtectBytes(const std::string &bytes, bool decrypt)
{
    DATA_BLOB input{static_cast<DWORD>(bytes.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(bytes.data()))};
    DATA_BLOB output{};
    const BOOL ok = decrypt ? CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                                                 CRYPTPROTECT_UI_FORBIDDEN, &output)
                            : CryptProtectData(&input, L"AWB backend protocol secret", nullptr, nullptr,
                                               nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok)
        throw Error("PROTOCOL_SECRET", "Cannot protect secret for the current Windows user");
    std::string result(reinterpret_cast<const char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
// 用途：把CNG SHA256或HMAC字节表示成协议使用的小写十六进制；日期：2026-11-05。
std::string Sha256(const std::string &text, const std::string &key)
{
    const auto bytes = HashBytes(text, key);
    static const char hex[] = "0123456789abcdef";
    std::string output;
    for (const auto byte : bytes)
    {
        const auto value = static_cast<unsigned char>(byte);
        output += hex[value >> 4];
        output += hex[value & 15];
    }
    return output;
}
// 用途：严格读业务字段，控制字节不能进入URL、日志或身份标识；日期：2026-11-05。
std::string Field(const Json &body, const char *key, size_t maximum)
{
    if (!body.contains(key) || !body.at(key).is_string())
        throw Error("INVALID_ARGUMENTS", std::string("Expected string: ") + key);
    const auto value = body.at(key).get<std::string>();
    if (value.empty() || value.size() > maximum ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw Error("INVALID_ARGUMENTS", std::string("Invalid field: ") + key);
    Wide(value);
    return value;
}
// 用途：将UTF8字节进行RFC3986百分号编码；日期：2026-11-05。
// 用途：保留内部端口显式模型，对外按Web标准省略默认端口；日期：2026-11-05。
std::string ProtocolOrigin(const std::string &url)
{
    auto origin = Browser::Origin(url);
    const auto suffix = origin.find("https://") == 0 ? std::string(":443") : std::string(":80");
    if (origin.size() > suffix.size() &&
        origin.compare(origin.size() - suffix.size(), suffix.size(), suffix) == 0)
        origin.resize(origin.size() - suffix.size());
    return origin;
}
std::string UrlEncode(const std::string &text)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string output;
    for (const auto c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
            byte == '-' || byte == '_' || byte == '.' || byte == '~')
            output += c;
        else
        {
            output += '%';
            output += hex[byte >> 4];
            output += hex[byte & 15];
        }
    }
    return output;
}
// 用途：严格解码表单，拒绝重复参数避免签名与业务解释不同；日期：2026-11-05。
Headers DecodeForm(const std::string &text)
{
    if (text.size() > 1024 * 1024)
        throw Error("INVALID_REQUEST", "Form exceeds budget");
    Headers output;
    // 用途：解码一个百分号参数，保留UTF8并拒绝NUL；日期：2026-11-05。
    const auto decode = [](const std::string &value)
    {
        std::string result;
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '+')
                result += ' ';
            else if (value[i] == '%')
            {
                if (i + 2 >= value.size())
                    throw Error("INVALID_REQUEST", "Truncated percent encoding");
                const auto part = value.substr(i + 1, 2);
                if (part.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    throw Error("INVALID_REQUEST", "Invalid percent encoding");
                result += static_cast<char>(std::stoul(part, nullptr, 16));
                i += 2;
            }
            else
                result += value[i];
        }
        if (result.find('\0') != std::string::npos)
            throw Error("INVALID_REQUEST", "NUL is forbidden in form");
        Wide(result);
        return result;
    };
    size_t begin = 0;
    while (begin < text.size())
    {
        const auto end = text.find('&', begin);
        const auto item = text.substr(begin, end == std::string::npos ? end : end - begin);
        const auto separator = item.find('=');
        const auto key = decode(item.substr(0, separator));
        if (key.empty() || output.count(key))
            throw Error("INVALID_REQUEST", "Duplicate or empty form field");
        output[key] = separator == std::string::npos ? "" : decode(item.substr(separator + 1));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return output;
}
// 用途：编码稳定排序的表单参数；日期：2026-11-05。
std::string EncodeForm(const Headers &fields)
{
    std::string output;
    for (const auto &item : fields)
    {
        if (!output.empty())
            output += '&';
        output += UrlEncode(item.first) + "=" + UrlEncode(item.second);
    }
    return output;
}
// 用途：用系统Base64实现标准及URL安全编码，不接受宽松畸形输入；日期：2026-11-05。
std::string Base64(const std::string &bytes, bool url, bool decode)
{
    if (!decode)
    {
        DWORD size = 0;
        CryptoCheck(CryptBinaryToStringA(reinterpret_cast<const BYTE *>(bytes.data()),
                                         static_cast<DWORD>(bytes.size()),
                                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size) != FALSE);
        std::string output(size, '\0');
        CryptoCheck(CryptBinaryToStringA(
                        reinterpret_cast<const BYTE *>(bytes.data()), static_cast<DWORD>(bytes.size()),
                        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &output[0], &size) != FALSE);
        if (!output.empty() && output.back() == '\0')
            output.pop_back();
        if (url)
        {
            std::replace(output.begin(), output.end(), '+', '-');
            std::replace(output.begin(), output.end(), '/', '_');
            while (!output.empty() && output.back() == '=')
                output.pop_back();
        }
        return output;
    }
    std::string input = bytes;
    const auto alphabet = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_="
                              : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    if (input.empty() || input.size() > 4 * 1024 * 1024 ||
        input.find_first_not_of(alphabet) != std::string::npos || input.size() % 4 == 1)
        throw Error("PROTOCOL_ENCODING", "Invalid Base64 encoding");
    const auto padding = input.find('=');
    if (padding != std::string::npos &&
        (input.find_first_not_of('=', padding) != std::string::npos || input.size() - padding > 2))
        throw Error("PROTOCOL_ENCODING", "Invalid Base64 padding");
    if (url)
    {
        std::replace(input.begin(), input.end(), '-', '+');
        std::replace(input.begin(), input.end(), '_', '/');
        while (input.size() % 4)
            input += '=';
    }
    else if (input.size() % 4)
        throw Error("PROTOCOL_ENCODING", "Standard Base64 needs padding");
    DWORD size = 0;
    CryptoCheck(CryptStringToBinaryA(input.data(), static_cast<DWORD>(input.size()), CRYPT_STRING_BASE64,
                                     nullptr, &size, nullptr, nullptr) != FALSE);
    std::string output(size, '\0');
    CryptoCheck(CryptStringToBinaryA(input.data(), static_cast<DWORD>(input.size()), CRYPT_STRING_BASE64,
                                     reinterpret_cast<BYTE *>(&output[0]), &size, nullptr, nullptr) != FALSE);
    return output;
}
// 用途：使用PKCS1v1.5/SHA256生成支付宝、微信和节点的标准RSA签名；日期：2026-11-05。
std::string RsaSign(const std::string &pem, const std::string &bytes)
{
    PrivateKey key(pem);
    const auto digest = HashBytes(bytes, "");
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    DWORD count = 0;
    CryptoCheck(NCryptSignHash(key.key, &padding, reinterpret_cast<PBYTE>(const_cast<char *>(digest.data())),
                               static_cast<DWORD>(digest.size()), nullptr, 0, &count,
                               NCRYPT_PAD_PKCS1_FLAG) == ERROR_SUCCESS);
    std::string signature(count, '\0');
    CryptoCheck(NCryptSignHash(key.key, &padding, reinterpret_cast<PBYTE>(const_cast<char *>(digest.data())),
                               static_cast<DWORD>(digest.size()), reinterpret_cast<PBYTE>(&signature[0]),
                               count, &count, NCRYPT_PAD_PKCS1_FLAG) == ERROR_SUCCESS);
    return Base64(signature);
}
// 用途：验证RSA SPKI或JWK的RS256签名并限制密钥尺寸及用途；日期：2026-11-05。
bool RsaVerify(const Json &key, const std::string &bytes, const std::string &signature)
{
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (key.is_string())
    {
        const auto der = Pem(key.get<std::string>());
        CERT_PUBLIC_KEY_INFO *info = nullptr;
        DWORD size = 0;
        CryptoCheck(CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, der.data(),
                                        static_cast<DWORD>(der.size()), CRYPT_DECODE_ALLOC_FLAG, nullptr,
                                        &info, &size) != FALSE);
        const auto ok = CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, info, 0, nullptr, &handle);
        LocalFree(info);
        CryptoCheck(ok != FALSE);
    }
    else
    {
        if (key.value("kty", "") != "RSA" || key.value("alg", "RS256") != "RS256" ||
            key.value("use", "sig") != "sig")
            throw Error("PROTOCOL_KEY", "Expected an RSA signature JWK");
        const auto modulus = Base64(Field(key, "n", 1024), true, true);
        const auto exponent = Base64(Field(key, "e", 16), true, true);
        if (modulus.size() < 256 || modulus.size() > 512 || exponent.size() > 8 || modulus.front() == 0)
            throw Error("PROTOCOL_KEY", "RSA key must have 2048 through 4096 bits");
        BCRYPT_RSAKEY_BLOB header{BCRYPT_RSAPUBLIC_MAGIC,
                                  static_cast<ULONG>(modulus.size() * 8),
                                  static_cast<ULONG>(exponent.size()),
                                  static_cast<ULONG>(modulus.size()),
                                  0,
                                  0};
        std::string blob(reinterpret_cast<const char *>(&header), sizeof(header));
        blob += exponent + modulus;
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        CryptoCheck(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0);
        const auto status =
            BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &handle,
                                reinterpret_cast<PUCHAR>(&blob[0]), static_cast<ULONG>(blob.size()), 0);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        CryptoCheck(status >= 0);
    }
    try
    {
        ULONG bits = 0, copied = 0;
        CryptoCheck(BCryptGetProperty(handle, BCRYPT_KEY_LENGTH, reinterpret_cast<PUCHAR>(&bits),
                                      sizeof(bits), &copied, 0) >= 0);
        if (bits < 2048 || bits > 4096)
            throw Error("PROTOCOL_KEY", "RSA key size is outside the accepted range");
        const auto signed_bytes = Base64(signature, false, true);
        const auto digest = HashBytes(bytes, "");
        BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
        const auto status = BCryptVerifySignature(
            handle, &padding, reinterpret_cast<PUCHAR>(const_cast<char *>(digest.data())),
            static_cast<ULONG>(digest.size()),
            reinterpret_cast<PUCHAR>(const_cast<char *>(signed_bytes.data())),
            static_cast<ULONG>(signed_bytes.size()), BCRYPT_PAD_PKCS1);
        BCryptDestroyKey(handle);
        return status >= 0;
    }
    catch (...)
    {
        BCryptDestroyKey(handle);
        throw;
    }
}
// 用途：生成可导出的临时RSA密钥并返回PKCS8私钥与标准RSA JWK公钥；日期：2026-11-05。
Json RsaGenerate()
{
    NCRYPT_PROV_HANDLE provider = 0;
    NCRYPT_KEY_HANDLE key = 0;
    CryptoCheck(NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) == ERROR_SUCCESS);
    try
    {
        CryptoCheck(NCryptCreatePersistedKey(provider, &key, NCRYPT_RSA_ALGORITHM, nullptr, 0, 0) ==
                    ERROR_SUCCESS);
        DWORD bits = 2048, policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
        CryptoCheck(NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits),
                                      sizeof(bits), 0) == ERROR_SUCCESS);
        CryptoCheck(NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy),
                                      sizeof(policy), 0) == ERROR_SUCCESS);
        CryptoCheck(NCryptFinalizeKey(key, 0) == ERROR_SUCCESS);
        // 用途：从临时密钥导出标准二进制格式，调用方负责保护私钥；日期：2026-11-05。
        const auto export_key = [key](const wchar_t *format)
        {
            DWORD count = 0;
            CryptoCheck(NCryptExportKey(key, 0, format, nullptr, nullptr, 0, &count, 0) == ERROR_SUCCESS);
            std::string bytes(count, '\0');
            CryptoCheck(NCryptExportKey(key, 0, format, nullptr, reinterpret_cast<PBYTE>(&bytes[0]), count,
                                        &count, 0) == ERROR_SUCCESS);
            return bytes;
        };
        auto secret = export_key(NCRYPT_PKCS8_PRIVATE_KEY_BLOB);
        const auto public_blob = export_key(BCRYPT_RSAPUBLIC_BLOB);
        BCRYPT_RSAKEY_BLOB header{};
        if (public_blob.size() < sizeof(header))
            throw Error("PROTOCOL_KEY", "Invalid RSA public key export");
        std::memcpy(&header, public_blob.data(), sizeof(header));
        const auto exponent = public_blob.substr(sizeof(header), header.cbPublicExp);
        const auto modulus = public_blob.substr(sizeof(header) + header.cbPublicExp, header.cbModulus);
        Json result{{"privateKey",
                     "-----BEGIN PRIVATE KEY-----\n" + Base64(secret) + "\n-----END PRIVATE KEY-----\n"},
                    {"publicKey",
                     {{"kty", "RSA"},
                      {"alg", "RS256"},
                      {"use", "sig"},
                      {"n", Base64(modulus, true)},
                      {"e", Base64(exponent, true)}}}};
        SecureZeroMemory(&secret[0], secret.size());
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return result;
    }
    catch (...)
    {
        if (key)
            NCryptFreeObject(key);
        NCryptFreeObject(provider);
        throw;
    }
}
// 用途：按微信API V3的AES-256-GCM资源格式解密并验证16字节标签；日期：2026-11-05。
std::string AesGcmDecrypt(const std::string &key, const std::string &nonce, const std::string &associated,
                          const std::string &ciphertext)
{
    if (key.size() != 32 || nonce.size() != 12 || ciphertext.size() < 16)
        throw Error("PAYMENT_SIGNATURE", "Invalid AES-GCM resource parameters");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    CryptoCheck(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0);
    try
    {
        CryptoCheck(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                                      reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                                      sizeof(BCRYPT_CHAIN_MODE_GCM), 0) >= 0);
        CryptoCheck(BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0,
                                               reinterpret_cast<PUCHAR>(const_cast<char *>(key.data())), 32,
                                               0) >= 0);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char *>(nonce.data()));
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char *>(associated.data()));
        info.cbAuthData = static_cast<ULONG>(associated.size());
        info.pbTag = reinterpret_cast<PUCHAR>(const_cast<char *>(ciphertext.data() + ciphertext.size() - 16));
        info.cbTag = 16;
        std::string output(ciphertext.size() - 16, '\0');
        ULONG copied = 0;
        const auto status = BCryptDecrypt(
            handle, reinterpret_cast<PUCHAR>(const_cast<char *>(ciphertext.data())),
            static_cast<ULONG>(ciphertext.size() - 16), &info, nullptr, 0,
            reinterpret_cast<PUCHAR>(&output[0]), static_cast<ULONG>(output.size()), &copied, 0);
        BCryptDestroyKey(handle);
        handle = nullptr;
        BCryptCloseAlgorithmProvider(algorithm, 0);
        algorithm = nullptr;
        if (status < 0)
        {
            SecureZeroMemory(&output[0], output.size());
            throw Error("PAYMENT_SIGNATURE", "AES-GCM authentication failed");
        }
        output.resize(copied);
        return output;
    }
    catch (...)
    {
        if (handle)
            BCryptDestroyKey(handle);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}
// 用途：发送有界HTTP请求，强制TLS验证、禁止重定向和自动认证；日期：2026-11-05。
HttpResult BackendHttp(const std::string &method, const std::string &url, const Headers &headers,
                       const std::string &body, bool loopback_test)
{
    if ((method != "GET" && method != "POST") || body.size() > 1024 * 1024 || url.size() > 8192)
        throw Error("INVALID_ARGUMENTS", "HTTP method or request budget is invalid");
    const auto target = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwUserNameLength =
        parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(target.c_str(), static_cast<DWORD>(target.size()), 0, &parts))
        throw Error("INVALID_ARGUMENTS", "Invalid integration URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if ((!secure && !(loopback_test && parts.nScheme == INTERNET_SCHEME_HTTP && host == L"127.0.0.1")) ||
        parts.dwUserNameLength || parts.dwPasswordLength || url.find('#') != std::string::npos)
        throw Error("PROTOCOL_URL", "Integration endpoints require HTTPS; test HTTP is loopback only");
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty())
        path = L"/";
    ProtocolInternetHandle session(WinHttpOpen(L"AWB-Integrations/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpSetTimeouts(session.value, 3000, 3000, 8000, 12000);
    ProtocolInternetHandle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    ProtocolInternetHandle request(
        WinHttpOpenRequest(connection.value, Wide(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy)))
        throw Error("CHANNEL_TRANSPORT", "Cannot enforce integration transport policy");
    std::string encoded;
    for (const auto &header : headers)
    {
        if (header.first.empty() ||
            header.first.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
            header.second.find_first_of("\r\n") != std::string::npos)
            throw Error("INVALID_ARGUMENTS", "Invalid outbound HTTP header");
        encoded += header.first + ": " + header.second + "\r\n";
    }
    const auto wide_headers = Wide(encoded);
    if (!WinHttpSendRequest(request.value, wide_headers.c_str(), static_cast<DWORD>(wide_headers.size()),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.value, nullptr))
        throw Error("CHANNEL_TRANSPORT", "Integration request failed; query before retrying a write",
                    method == "POST");
    HttpResult result;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        throw Error("CHANNEL_TRANSPORT", "Cannot read integration HTTP status", method == "POST");
    result.status = static_cast<int>(status);
    size = 0;
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (size > 65536)
        throw Error("CHANNEL_TRANSPORT", "Integration headers exceed budget", method == "POST");
    std::vector<wchar_t> raw(size / sizeof(wchar_t) + 1);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                             raw.data(), &size, WINHTTP_NO_HEADER_INDEX))
        throw Error("CHANNEL_TRANSPORT", "Cannot read integration response headers", method == "POST");
    const auto lines = Utf8(raw.data());
    size_t begin = lines.find("\r\n");
    while (begin != std::string::npos)
    {
        begin += 2;
        const auto end = lines.find("\r\n", begin);
        if (end == std::string::npos || end == begin)
            break;
        const auto line = lines.substr(begin, end - begin);
        const auto colon = line.find(':');
        if (colon != std::string::npos)
        {
            const auto name = Trim(line.substr(0, colon), true);
            if (result.headers.count(name) && (name.find("wechatpay-") == 0 || name == "stripe-signature"))
                throw Error("CHANNEL_TRANSPORT", "Duplicate integration signature header", true);
            result.headers[name] = Trim(line.substr(colon + 1));
        }
        begin = end;
    }
    for (;;)
    {
        char buffer[8192];
        DWORD copied = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &copied))
            throw Error("CHANNEL_TRANSPORT", "Integration response interrupted", method == "POST");
        if (!copied)
            break;
        result.body.append(buffer, copied);
        if (result.body.size() > 4 * 1024 * 1024)
            throw Error("CHANNEL_TRANSPORT", "Integration response exceeds budget", method == "POST");
    }
    return result;
}
// 用途：重新核验组织与成员状态，管理员范围不能由请求正文提升；日期：2026-11-05。
Json TenantMember(BackendStore &store, const Json &user, const std::string &tenant, bool manage)
{
    const auto member = store.Get("member", tenant + ":" + user.at("id").get<std::string>());
    const auto organization = store.Require("tenant", tenant);
    if (member.empty() || member.at("state") != "active" || organization.at("state") != "active" ||
        (manage && member.at("role") != "owner" && member.at("role") != "admin"))
        throw Error("FORBIDDEN", "Active authorized organization membership required");
    return member;
}
} // namespace awb
