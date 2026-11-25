#include "awb/backend_extensions.hpp"
#include <iostream>
// 用途：从节点私有配置启动出站执行服务，密钥和注册令牌不经过命令行；日期：2026-11-05。
int wmain(int argc, wchar_t **argv)
{
    try
    {
        const bool stop = argc == 4 && std::wstring(argv[1]) == L"--stop";
        const int offset = stop ? 2 : 1;
        if ((!stop && argc != 3) || (stop && argc != 4) || std::wstring(argv[offset]) != L"--config")
            throw awb::Error(
                "INVALID_ARGUMENTS",
                "Usage: workbench-node --config PATH (root, browser, url, name, enrollmentToken, headless)");
        auto config =
            awb::ParseExternalJson(awb::ReadFileBounded(awb::SafeAbsolute(argv[offset + 1]), 16384));
        config["stop"] = stop;
        return awb::RunNode(awb::Wide(awb::Field(config, "root", 2048)),
                            awb::Wide(awb::Field(config, "browser", 2048)), config);
    }
    catch (const std::exception &error)
    {
        std::cerr << awb::ErrorResult(error).dump() << std::endl;
        return 1;
    }
}
