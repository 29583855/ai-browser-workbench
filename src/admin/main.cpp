#include "awb/control_plane.hpp"
#include <iostream>

// 用途：解析后台独立数据目录和回环监听端口，不包含默认账号密码；日期：2026-11-05。
int wmain(int argc, wchar_t **argv)
{
    try
    {
        std::wstring root = awb::DefaultRoot() + L"-admin";
        unsigned short port = 17861;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring option(argv[i]);
            if (option == L"--root" && i + 1 < argc)
                root = argv[++i];
            else if (option == L"--port" && i + 1 < argc)
            {
                const std::wstring number(argv[++i]);
                if (number.empty() || number.size() > 5 ||
                    number.find_first_not_of(L"0123456789") != std::wstring::npos ||
                    std::stoul(number) > 65535)
                    throw awb::Error("INVALID_ARGUMENTS", "Port must be 0..65535");
                port = static_cast<unsigned short>(std::stoul(number));
            }
            else
                throw awb::Error("INVALID_ARGUMENTS", "workbench-admin [--root PATH] [--port 0..65535]");
        }
        return awb::RunAdmin(root, port);
    }
    catch (const std::exception &error)
    {
        std::cerr << awb::ErrorResult(error).dump() << std::endl;
        return 1;
    }
}
