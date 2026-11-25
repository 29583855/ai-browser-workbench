#include "awb/local_api.hpp"
#include "awb/service.hpp"
#include "awb/version.hpp"
#include <iostream>

// 用途：校验JSONL信封与关联ID，标准输出仅保留协议，退出关闭受管进程；日期：2026-11-04。
int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc == 2 && std::wstring(argv[1]) == L"--print-contract")
        {
            std::cout << awb::Json{{"contractVersion", 1},
                                   {"productVersion", awb::Version},
                                   {"tools", awb::Service::Tools()},
                                   {"capabilities", awb::Service::Capabilities()}}
                             .dump(2)
                      << std::endl;
            return 0;
        }
        std::wstring root = awb::DefaultRoot();
        std::wstring executable;
        bool http = false;
        unsigned short port = 0;
        for (int index = 1; index < argc; ++index)
        {
            const std::wstring option(argv[index]);
            if ((option == L"--root" || option == L"--browser") && index + 1 < argc)
            {
                const std::wstring value(argv[++index]);
                if (option == L"--root")
                    root = value;
                else
                    executable = value;
            }
            else if (option == L"--http" && index + 1 < argc)
            {
                const std::wstring text(argv[++index]);
                if (text.empty() || text.size() > 5 ||
                    text.find_first_not_of(L"0123456789") != std::wstring::npos || std::stoul(text) > 65535)
                    throw awb::Error("INVALID_ARGUMENTS", "HTTP port must be 0..65535");
                port = static_cast<unsigned short>(std::stoul(text));
                http = true;
            }
            else
                throw awb::Error("INVALID_ARGUMENTS", "Usage: workbench-cli [--root PATH] [--browser EXE]");
        }
        if (http)
            return awb::RunLocalApi(root, executable, port);
        awb::Service service(root, executable);
        std::string line;
        while (awb::ReadProtocolLine(std::cin, line))
        {
            awb::Json response;
            awb::Json id = nullptr;
            try
            {
                const auto request = awb::ParseExternalJson(line);
                if (!request.is_object())
                    throw awb::Error("INVALID_REQUEST", "Expected an object request");
                const auto candidate_id = request.value("id", awb::Json(nullptr));
                if (!candidate_id.is_null() && !candidate_id.is_string() && !candidate_id.is_number_integer())
                    throw awb::Error("INVALID_REQUEST", "Request id must be a string, integer or null");
                id = candidate_id;
                if (!request.contains("command") || !request.at("command").is_string())
                    throw awb::Error("INVALID_REQUEST", "Expected id, command and arguments");
                response = {{"ok", true},
                            {"result", service.Execute(request.at("command"),
                                                       request.value("arguments", awb::Json::object()))}};
            }
            catch (const std::exception &error)
            {
                response = awb::ErrorResult(error);
            }
            response["id"] = id;
            std::cout << response.dump() << std::endl;
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << awb::ErrorResult(error).dump() << std::endl;
        return 1;
    }
}
