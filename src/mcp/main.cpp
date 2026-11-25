#include "awb/service.hpp"
#include "awb/version.hpp"
#include <iostream>

// 用途：形成JSON-RPC错误回复，不把通知误当请求回复；日期：2026-11-03。
static awb::Json RpcError(const awb::Json &id, int code, const std::string &message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

// 用途：严格验证JSON-RPC信封和命名参数，提供MCP握手及共享工具；日期：2026-11-04。
int wmain(int argc, wchar_t **argv)
{
    try
    {
        std::wstring root = awb::DefaultRoot();
        std::wstring browser;
        for (int index = 1; index < argc; ++index)
        {
            const std::wstring option(argv[index]);
            if ((option == L"--root" || option == L"--browser") && index + 1 < argc)
            {
                const std::wstring value(argv[++index]);
                if (option == L"--root")
                    root = value;
                else
                    browser = value;
            }
            else
                throw awb::Error("INVALID_ARGUMENTS", "Usage: workbench-mcp [--root PATH] [--browser EXE]");
        }
        awb::Service service(root, browser);
        bool initialized = false;
        bool ready = false;
        std::string line;
        while (awb::ReadProtocolLine(std::cin, line))
        {
            awb::Json id = nullptr;
            bool has_id = true;
            awb::Json response;
            try
            {
                const auto request = awb::ParseExternalJson(line);
                if (!request.is_object() || !request.contains("jsonrpc") || request["jsonrpc"] != "2.0" ||
                    !request.contains("method") || !request["method"].is_string() ||
                    (request.contains("id") && !request["id"].is_null() && !request["id"].is_string() &&
                     !request["id"].is_number_integer()))
                    response = RpcError(nullptr, -32600, "Invalid JSON-RPC request");
                else
                {
                    has_id = request.contains("id");
                    if (has_id)
                        id = request["id"];
                    const std::string method = request["method"];
                    const auto params = request.value("params", awb::Json::object());
                    if (!params.is_object())
                        throw awb::Error("INVALID_ARGUMENTS", "MCP expects named object params");
                    awb::Json result = awb::Json::object();
                    if (method == "initialize" && has_id && !initialized)
                    {
                        const auto requested = params.value("protocolVersion", "");
                        const auto protocol = requested == "2024-11-05" ? requested : "2025-03-26";
                        result = {
                            {"protocolVersion", protocol},
                            {"capabilities", {{"tools", {{"listChanged", false}}}}},
                            {"serverInfo", {{"name", "ai-browser-workbench"}, {"version", awb::Version}}},
                            {"instructions", "Local development POC. No production quota or commerce. "
                                             "Experimental unauthenticated HTTP/SOCKS5 proxies. "
                                             "Batches are non-atomic; inspect per-item results and never "
                                             "replay unknown outcomes. "
                                             "Tools share origin and lease checks."}};
                        initialized = true;
                    }
                    else if (method == "notifications/initialized" && !has_id && initialized)
                    {
                        ready = true;
                    }
                    else if (method == "ping" && has_id)
                    {
                        result = awb::Json::object();
                    }
                    else if (!ready)
                        response = RpcError(id, -32002, "Server is not initialized");
                    else if (method == "tools/list" && has_id)
                        result = {{"tools", awb::Service::Tools()}};
                    else if (method == "tools/call" && has_id)
                    {
                        awb::Json content;
                        bool failed = false;
                        try
                        {
                            content = service.Execute(params.at("name"),
                                                      params.value("arguments", awb::Json::object()));
                        }
                        catch (const std::exception &error)
                        {
                            content = awb::ErrorResult(error);
                            failed = true;
                        }
                        result = {
                            {"content", awb::Json::array({{{"type", "text"}, {"text", content.dump()}}})},
                            {"isError", failed}};
                    }
                    else
                        response = RpcError(id, -32601, "Method not supported");
                    if (response.is_null())
                        response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
                }
            }
            catch (const awb::Json::parse_error &)
            {
                response = RpcError(nullptr, -32700, "Parse error");
            }
            catch (const std::exception &)
            {
                response = RpcError(id, -32602, "Invalid params");
            }
            if (has_id)
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
