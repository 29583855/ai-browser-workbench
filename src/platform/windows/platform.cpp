#include "awb/platform.hpp"
#include <algorithm>
#include <bcrypt.h>
#include <iomanip>
#include <limits>
#include <sddl.h>
#include <set>
#include <shlobj.h>
#include <sstream>
#include <winioctl.h>

namespace awb
{
// 用途：将系统失败转成结构化错误，不回显秘密；日期：2026-11-03。
static void Check(bool success, const char *operation)
{
    if (!success)
        throw Error("WINDOWS_ERROR", std::string(operation) + " failed: " + std::to_string(GetLastError()));
}

// 用途：保存错误码和说明；日期：2026-11-03。
Error::Error(std::string value, const std::string &message, bool unknown)
    : std::runtime_error(message), code(std::move(value)), outcome_unknown(unknown)
{
}
// 用途：接管Windows句柄；日期：2026-11-03。
Handle::Handle(HANDLE value) : value_(value)
{
}
// 用途：释放句柄；日期：2026-11-03。
Handle::~Handle()
{
    reset();
}
// 用途：转移句柄；日期：2026-11-03。
Handle::Handle(Handle &&other) noexcept : value_(other.value_)
{
    other.value_ = nullptr;
}
// 用途：替换句柄所有权；日期：2026-11-03。
Handle &Handle::operator=(Handle &&other) noexcept
{
    if (this != &other)
    {
        reset(other.value_);
        other.value_ = nullptr;
    }
    return *this;
}
// 用途：借用句柄；日期：2026-11-03。
HANDLE Handle::get() const
{
    return value_;
}
// 用途：释放旧句柄；日期：2026-11-03。
void Handle::reset(HANDLE value)
{
    if (value_ && value_ != INVALID_HANDLE_VALUE)
        CloseHandle(value_);
    value_ = value;
}
// 用途：严格转换UTF-8；日期：2026-11-03。
std::wstring Wide(const std::string &text)
{
    if (text.empty())
        return {};
    if (text.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        throw Error("INPUT_TOO_LARGE", "UTF-8 input exceeds limit");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (!count)
        throw Error("INVALID_UTF8", "Invalid UTF-8 input");
    std::wstring result(static_cast<size_t>(count), L'\0');
    Check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                              &result[0], count) == count,
          "UTF-8 conversion");
    return result;
}
// 用途：严格转换UTF-16；日期：2026-11-03。
std::string Utf8(const std::wstring &text)
{
    if (text.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    Check(count > 0, "UTF-16 conversion");
    std::string result(static_cast<size_t>(count), '\0');
    Check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                              &result[0], count, nullptr, nullptr) == count,
          "UTF-16 conversion");
    return result;
}
// 用途：使用BCrypt生成不可预测标识；日期：2026-11-03。
std::string RandomId()
{
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw Error("RANDOM_FAILED", "System random generator failed");
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : bytes)
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    return stream.str();
}
// 用途：引用单参数并保留尾反斜杠，不经shell；日期：2026-11-03。
std::wstring QuoteArgument(const std::wstring &argument)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto character : argument)
    {
        if (character == L'\\')
            ++slashes;
        else
        {
            result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
            if (character == L'"')
                result += L'\\';
            result += character;
            slashes = 0;
        }
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
// 用途：仅允许本机绝对路径，按扩展长度路径逐段拒绝重解析点；日期：2026-11-04。
std::wstring SafeAbsolute(const std::wstring &path)
{
    if (path.empty() || path.find(L'\0') != std::wstring::npos)
        throw Error("PATH_INVALID", "Empty or invalid path");
    std::vector<wchar_t> buffer(32768);
    const DWORD length =
        GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length || length >= buffer.size())
        throw Error("PATH_INVALID", "Cannot resolve local path");
    std::wstring result(buffer.data(), length);
    if (result.size() < 3 || result[1] != L':' || result[2] != L'\\' ||
        result.find(L':', 2) != std::wstring::npos)
        throw Error("PATH_INVALID", "Only local drive paths are allowed");
    for (size_t end = 3; end <= result.size(); ++end)
    {
        if (end != result.size() && result[end] != L'\\')
            continue;
        const auto part = result.substr(0, end);
        const DWORD attributes = GetFileAttributesW((L"\\\\?\\" + part).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw Error("PATH_REPARSE_POINT", "Managed paths cannot contain reparse points");
    }
    return result;
}

namespace
{
struct MountPointRecord
{
    DWORD tag;
    WORD data_length;
    WORD reserved;
    WORD substitute_offset;
    WORD substitute_length;
    WORD print_offset;
    WORD print_length;
    wchar_t names[1];
};

// 用途：以完整规范路径摘要生成稳定私有别名，碰撞仍须通过目标校验；日期：2026-11-04。
std::wstring BrowserAliasKey(const std::wstring &path)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw Error("BROWSER_STORAGE_ALIAS_FAILED", "Cannot initialize storage alias identity");
    unsigned char digest[32]{};
    const auto result =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(path.data())),
                   static_cast<ULONG>(path.size() * sizeof(wchar_t)), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0)
        throw Error("BROWSER_STORAGE_ALIAS_FAILED", "Cannot calculate storage alias identity");
    std::wostringstream text;
    text << std::hex << std::setfill(L'0');
    for (size_t index = 0; index < 16; ++index)
        text << std::setw(2) << static_cast<unsigned int>(digest[index]);
    return text.str();
}

// 用途：核验目录连接仅指向预期本机规范目录，拒绝其他重解析类型和越界数据；日期：2026-11-04。
void ValidateBrowserAlias(HANDLE handle, const std::wstring &target)
{
    std::vector<unsigned char> data(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD size = 0;
    if (!DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, data.data(),
                         static_cast<DWORD>(data.size()), &size, nullptr) ||
        size < offsetof(MountPointRecord, names))
        throw Error("BROWSER_STORAGE_ALIAS_CONFLICT", "Storage alias is not a verified directory junction");
    const auto *record = reinterpret_cast<const MountPointRecord *>(data.data());
    const size_t header = offsetof(MountPointRecord, names);
    if (record->tag != IO_REPARSE_TAG_MOUNT_POINT || record->data_length + 8U > size ||
        record->substitute_offset % sizeof(wchar_t) || record->substitute_length % sizeof(wchar_t) ||
        header + record->substitute_offset + record->substitute_length > record->data_length + 8U)
        throw Error("BROWSER_STORAGE_ALIAS_CONFLICT", "Storage alias record is invalid");
    const auto *begin = reinterpret_cast<const wchar_t *>(data.data() + header + record->substitute_offset);
    const std::wstring actual(begin, record->substitute_length / sizeof(wchar_t));
    const std::wstring expected = L"\\??\\" + target;
    if (CompareStringOrdinal(actual.c_str(), static_cast<int>(actual.size()), expected.c_str(),
                             static_cast<int>(expected.size()), TRUE) != CSTR_EQUAL)
        throw Error("BROWSER_STORAGE_ALIAS_CONFLICT", "Storage alias points to a different profile");
}
} // namespace

// 用途：仅为长目录建立私有本机连接，持有禁止替换的句柄并验证目标；日期：2026-11-04。
BrowserDataPath::BrowserDataPath(const std::wstring &canonical) : path_(SafeAbsolute(canonical))
{
    if (path_.size() <= 100)
        return;
    const auto target = path_;
    target_handle_.reset(CreateFileW((L"\\\\?\\" + target).c_str(), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION target_info{};
    if (!target_handle_.get() || target_handle_.get() == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(target_handle_.get(), &target_info) ||
        !(target_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (target_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw Error("BROWSER_STORAGE_ALIAS_FAILED", "Cannot hold the canonical profile directory");
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)))
        throw Error("BROWSER_STORAGE_ALIAS_FAILED", "LocalAppData is unavailable for a short storage alias");
    std::wstring parent(local);
    CoTaskMemFree(local);
    parent = SafeAbsolute(parent + L"\\AWB-Paths");
    path_ = parent + L"\\" + BrowserAliasKey(target);
    if (path_.size() > 100)
        throw Error(
            "BROWSER_STORAGE_PATH_TOO_LONG",
            "Use a shorter local data root; this user directory cannot provide a safe browser storage alias");
    PrivateDirectory(parent);
    const bool created = CreateDirectoryW(path_.c_str(), nullptr) != FALSE;
    if (!created && GetLastError() != ERROR_ALREADY_EXISTS)
        throw Error("BROWSER_STORAGE_ALIAS_FAILED", "Cannot create a private browser storage alias");
    try
    {
        alias_handle_.reset(CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!alias_handle_.get() || alias_handle_.get() == INVALID_HANDLE_VALUE)
            throw Error("BROWSER_STORAGE_ALIAS_FAILED", "Cannot hold the browser storage alias");
        if (created)
        {
            const auto substitute = L"\\??\\" + target;
            const size_t names_bytes = (substitute.size() + 1 + target.size() + 1) * sizeof(wchar_t);
            const size_t size = offsetof(MountPointRecord, names) + names_bytes;
            if (size > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
                throw Error("BROWSER_STORAGE_PATH_TOO_LONG",
                            "Profile path exceeds the browser alias record limit");
            std::vector<unsigned char> buffer(size, 0);
            auto *record = reinterpret_cast<MountPointRecord *>(buffer.data());
            record->tag = IO_REPARSE_TAG_MOUNT_POINT;
            record->data_length = static_cast<WORD>(size - 8);
            record->substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
            record->print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
            record->print_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
            memcpy(record->names, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
            memcpy(reinterpret_cast<unsigned char *>(record->names) + record->print_offset, target.c_str(),
                   (target.size() + 1) * sizeof(wchar_t));
            DWORD returned = 0;
            if (!DeviceIoControl(alias_handle_.get(), FSCTL_SET_REPARSE_POINT, buffer.data(),
                                 static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr))
                throw Error("BROWSER_STORAGE_ALIAS_FAILED",
                            "Cannot initialize the private browser storage junction");
        }
        ValidateBrowserAlias(alias_handle_.get(), target);
        alias_ = true;
    }
    catch (...)
    {
        alias_handle_.reset();
        if (created)
            RemoveDirectoryW(path_.c_str());
        throw;
    }
}

// 用途：删除本次已验证连接本身，失败保留供下次校验复用，绝不递归；日期：2026-11-04。
BrowserDataPath::~BrowserDataPath()
{
    alias_handle_.reset();
    if (alias_)
        RemoveDirectoryW(path_.c_str());
}

// 用途：返回实际派发给浏览器的短路径，元数据继续使用规范目录；日期：2026-11-04。
const std::wstring &BrowserDataPath::get() const
{
    return path_;
}

// 用途：创建支持长路径的私有目录并设置可继承用户/SYSTEM访问规则；日期：2026-11-04。
void PrivateDirectory(const std::wstring &input)
{
    const auto path = L"\\\\?\\" + SafeAbsolute(input);
    HANDLE token = nullptr;
    Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "OpenProcessToken");
    Handle owner(token);
    DWORD size = 0;
    GetTokenInformation(owner.get(), TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> storage(size);
    Check(GetTokenInformation(owner.get(), TokenUser, storage.data(), size, &size) != FALSE,
          "GetTokenInformation");
    wchar_t *sid = nullptr;
    Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(storage.data())->User.Sid, &sid) != FALSE,
          "ConvertSidToStringSid");
    const std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                               nullptr) != FALSE,
          "Security descriptor");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    const DWORD creation_error = GetLastError();
    if (!created && creation_error != ERROR_ALREADY_EXISTS)
    {
        LocalFree(descriptor);
        throw Error("DIRECTORY_CREATE_FAILED", "Parent directory must exist and be writable");
    }
    const BOOL secured = SetFileSecurityW(
        path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    Check(secured != FALSE, "SetFileSecurity");
    const DWORD flags = GetFileAttributesW(path.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY))
        throw Error("PATH_INVALID", "Managed root is not a directory");
}
namespace
{
// 用途：打开且固定一个节点，禁止在核验和使用之间被改名替换；日期：2026-11-04。
Handle PinNode(const std::wstring &path, bool remove, BY_HANDLE_FILE_INFORMATION &info)
{
    Handle handle(CreateFileW((L"\\\\?\\" + path).c_str(), FILE_READ_ATTRIBUTES | (remove ? DELETE : 0),
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.get(), &info))
        throw Error("PROFILE_DIRECTORY_BUSY", "Cannot lock the profile node for cleanup");
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw Error("PATH_REPARSE_POINT", "Cleanup refuses links and junctions");
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && info.nNumberOfLinks != 1)
        throw Error("PATH_INVALID", "Cleanup refuses files with multiple hard links");
    return handle;
}
// 用途：有界遍历并按已固定句柄删除，不沿目录链接进入外部路径；日期：2026-11-04。
void VisitProfile(const std::wstring &path, bool remove, size_t depth, size_t &count)
{
    if (depth > 128 || ++count > 1000000 || path.size() > 32000)
        throw Error("PROFILE_DIRECTORY_BUDGET", "Cleanup tree exceeds its bounded traversal budget");
    BY_HANDLE_FILE_INFORMATION info{};
    auto node = PinNode(path, remove, info);
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        WIN32_FIND_DATAW data{};
        const auto search = FindFirstFileW((L"\\\\?\\" + path + L"\\*").c_str(), &data);
        if (search == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() != ERROR_FILE_NOT_FOUND)
                throw Error("PROFILE_DIRECTORY_BUSY", "Cannot enumerate the profile directory");
        }
        else
        {
            try
            {
                do
                {
                    const std::wstring name(data.cFileName);
                    if (name != L"." && name != L"..")
                        VisitProfile(path + L"\\" + name, remove, depth + 1, count);
                } while (FindNextFileW(search, &data));
                if (GetLastError() != ERROR_NO_MORE_FILES)
                    throw Error("PROFILE_DIRECTORY_BUSY", "Profile enumeration did not finish");
            }
            catch (...)
            {
                FindClose(search);
                throw;
            }
            FindClose(search);
        }
    }
    if (remove)
    {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(node.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
            throw Error("PROFILE_DIRECTORY_BUSY",
                        "Profile cleanup is incomplete; retry after closing file owners");
    }
}
} // namespace
// 用途：严格限定根目录和环境ID，先全树核验，再删除，失败保留清理状态；日期：2026-11-04。
void RemoveProfileDirectory(const std::wstring &input, const std::string &id)
{
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw Error("PATH_INVALID", "Cleanup requires an opaque profile identifier");
    const auto root = SafeAbsolute(input);
    if (root.size() <= 3 || root.back() == L'\\')
        throw Error("PATH_INVALID", "Cleanup root must be an application data directory");
    std::vector<Handle> ancestors;
    for (size_t end = 3; end <= root.size(); ++end)
    {
        if (end != root.size() && root[end] != L'\\')
            continue;
        BY_HANDLE_FILE_INFORMATION info{};
        ancestors.push_back(PinNode(root.substr(0, end), false, info));
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            throw Error("PATH_INVALID", "Cleanup root ancestor is not a directory");
    }
    const auto directory = root + L"\\" + Wide(id);
    const auto attributes = GetFileAttributesW((L"\\\\?\\" + directory).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return;
        throw Error("PROFILE_DIRECTORY_BUSY", "Cannot inspect profile directory");
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        throw Error("PATH_INVALID", "Profile directory was replaced by a file");
    size_t count = 0;
    VisitProfile(directory, false, 0, count);
    count = 0;
    VisitProfile(directory, true, 0, count);
}
// 用途：使用扩展长度临时路径持久写入，完整刷盘后原子替换；日期：2026-11-04。
void AtomicWrite(const std::wstring &input, const std::string &data)
{
    const auto path = L"\\\\?\\" + SafeAbsolute(input);
    const auto temporary = path + L"." + Wide(RandomId()) + L".tmp";
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                            nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
        throw Error("FILE_WRITE_FAILED", "Cannot create managed temporary file");
    try
    {
        DWORD written = 0;
        Check(data.size() <= MAXDWORD &&
                  WriteFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                  written == data.size(),
              "WriteFile");
        Check(FlushFileBuffers(file.get()) != FALSE, "FlushFileBuffers");
        file.reset();
        Check(MoveFileExW(temporary.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
              "MoveFileEx");
    }
    catch (...)
    {
        file.reset();
        DeleteFileW(temporary.c_str());
        throw;
    }
}
// 用途：按扩展长度路径有界读取，不允许共享写入内容扩展内存；日期：2026-11-04。
std::string ReadFileBounded(const std::wstring &input, size_t limit)
{
    const auto path = L"\\\\?\\" + SafeAbsolute(input);
    Handle file(CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
        throw Error("FILE_READ_FAILED", "Cannot read managed file");
    LARGE_INTEGER length{};
    Check(GetFileSizeEx(file.get(), &length) != FALSE, "GetFileSizeEx");
    if (length.QuadPart < 0 || static_cast<unsigned long long>(length.QuadPart) > limit)
        throw Error("FILE_TOO_LARGE", "Managed file exceeds size budget");
    std::string result(static_cast<size_t>(length.QuadPart), '\0');
    DWORD read = 0;
    Check(result.empty() ||
              (ReadFile(file.get(), &result[0], static_cast<DWORD>(result.size()), &read, nullptr) &&
               read == result.size()),
          "ReadFile");
    return result;
}
// 用途：取得当前模块目录，拒绝截断和受管路径重解析；日期：2026-11-04。
std::wstring ExecutableDirectory()
{
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        throw Error("PATH_INVALID", "Cannot resolve executable location");
    const auto absolute = SafeAbsolute(std::wstring(path.data(), length));
    return absolute.substr(0, absolute.find_last_of(L'\\'));
}
// 用途：优先使用包内测试浏览器，再发现本机安装，不下载或修改系统浏览器；日期：2026-11-04。
std::wstring DiscoverBrowser()
{
    const auto bundled = ExecutableDirectory() + L"\\browser\\chrome-win64\\chrome.exe";
    if (GetFileAttributesW(bundled.c_str()) != INVALID_FILE_ATTRIBUTES)
        return SafeAbsolute(bundled);
    const std::vector<std::wstring> candidates = {
        L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"};
    for (const auto &candidate : candidates)
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return SafeAbsolute(candidate);
    throw Error("BROWSER_NOT_FOUND", "Install Edge/Chrome or provide an explicit browser executable");
}
// 用途：便携模式保存至程序目录data，普通构建继续使用LocalAppData；日期：2026-11-04。
std::wstring DefaultRoot()
{
    const auto executable_directory = ExecutableDirectory();
    const auto marker = executable_directory + L"\\portable.mode";
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if (ReadFileBounded(marker, 128) != "AI_BROWSER_WORKBENCH_PORTABLE_V1\n")
            throw Error("PORTABLE_CONFIG_INVALID", "Portable marker is invalid");
        PrivateDirectory(executable_directory + L"\\data");
        return executable_directory + L"\\data\\desktop";
    }
    PWSTR directory = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &directory)))
        throw Error("DIRECTORY_NOT_FOUND", "LocalAppData is unavailable");
    std::wstring result(directory);
    CoTaskMemFree(directory);
    return result + L"\\AIBrowserWorkbench-POC";
}
// 用途：按验证过的UTF-8首字节计Unicode码点，避免中文被当作三字符；日期：2026-11-03。
size_t UnicodeLength(const std::string &text)
{
    Wide(text);
    return static_cast<size_t>(std::count_if(text.begin(), text.end(),
                                             // 用途：每个UTF-8码点只计算一个非续字节；日期：2026-11-03。
                                             [](unsigned char byte) { return (byte & 0xc0) != 0x80; }));
}
// 用途：转换异常为可机器解析的失败结果；日期：2026-11-03。
Json ErrorResult(const std::exception &error)
{
    const auto *typed = dynamic_cast<const Error *>(&error);
    if (dynamic_cast<const Json::exception *>(&error))
        return {{"ok", false},
                {"error", {{"code", "INVALID_JSON"}, {"message", "Invalid JSON or argument shape"}}}};
    return {{"ok", false},
            {"error",
             {{"code", typed ? typed->code : "INTERNAL_ERROR"},
              {"message", typed ? error.what() : "Internal operation failed; inspect diagnostics"},
              {"outcomeUnknown", typed && typed->outcome_unknown},
              {"automaticRetryAllowed", false}}}};
}
// 用途：协议解析阶段限制深度和重复键，避免校验前耗尽栈或产生歧义输入；日期：2026-11-04。
Json ParseExternalJson(const std::string &text, int maximum_depth)
{
    if (text.size() > 1024 * 1024 || maximum_depth < 1 || maximum_depth > 64)
        throw Error("INPUT_TOO_LARGE", "External JSON exceeds parsing budget");
    std::vector<std::set<std::string>> keys;
    // 用途：每个容器独立核对字段唯一性，结束后立即释放该层记录；日期：2026-11-04。
    return Json::parse(
        text,
        [&](int depth, Json::parse_event_t event, Json &value)
        {
            if (depth > maximum_depth)
                throw Error("INPUT_TOO_DEEP", "External JSON nesting exceeds parsing budget");
            if (event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start)
                keys.emplace_back();
            else if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end)
                keys.pop_back();
            else if (event == Json::parse_event_t::key &&
                     !keys.back().insert(value.get<std::string>()).second)
                throw Error("INVALID_JSON", "Duplicate object fields are not allowed");
            return true;
        });
}
} // namespace awb
