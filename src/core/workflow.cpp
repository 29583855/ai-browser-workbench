#include "awb/workflow.hpp"
#include "awb/browser.hpp"
#include <algorithm>
#include <bcrypt.h>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace awb
{
namespace
{
constexpr size_t MaximumDefinitionBytes = 256 * 1024;
constexpr size_t MaximumRecords = 256;
struct Invalid : Error
{
    std::string path;
    // 用途：携带错误位置与固定说明，不夹带非法输入内容；日期：2026-11-04。
    Invalid(const std::string &code, const std::string &where, const std::string &message)
        : Error(code, message), path(where)
    {
    }
};
// 用途：按固定错误码中断校验，调用者可准确定位错误字段；日期：2026-11-04。
void Check(bool accepted, const std::string &path, const std::string &message,
           const std::string &code = "WORKFLOW_SCHEMA_INVALID")
{
    if (!accepted)
        throw Invalid(code, path, message);
}
// 用途：预检JSON深度、节点数和文本预算，避免递归及导入资源失控；日期：2026-11-04。
void Budget(const Json &value, const std::string &path, size_t depth, size_t &nodes, size_t &bytes)
{
    Check(depth <= 32 && ++nodes <= 16384, path, "JSON structural budget exceeded", "WORKFLOW_BUDGET");
    if (value.is_string())
    {
        const auto &text = value.get_ref<const std::string &>();
        UnicodeLength(text);
        Check(text.find('\0') == std::string::npos, path, "NUL is not allowed");
        bytes += text.size();
    }
    if (value.is_structured())
        for (auto it = value.begin(); it != value.end(); ++it)
        {
            if (value.is_object())
                bytes += it.key().size();
            Budget(it.value(), path, depth + 1, nodes, bytes);
        }
    Check(bytes <= MaximumDefinitionBytes, path, "JSON text budget exceeded", "WORKFLOW_BUDGET");
}
// 用途：限制对象字段并检查必填，未知字段不被静默忽略；日期：2026-11-04。
void Fields(const Json &value, const std::string &path, const std::set<std::string> &allowed,
            const std::set<std::string> &required = {})
{
    Check(value.is_object(), path, "Expected an object");
    for (const auto &key : required)
        Check(value.contains(key), path + "/" + key, "Required field is missing");
    for (auto it = value.begin(); it != value.end(); ++it)
        Check(allowed.count(it.key()) != 0, path, "Unknown field is not supported");
}
// 用途：校验有界Unicode文本，不把错误业务文本写进诊断；日期：2026-11-04。
std::string Text(const Json &value, const std::string &path, size_t maximum, bool empty = false)
{
    Check(value.is_string(), path, "Expected a string");
    const auto text = value.get<std::string>();
    Check((empty || !text.empty()) && UnicodeLength(text) <= maximum, path, "String length is invalid");
    return text;
}
// 用途：严格读取有限整数，拒绝浮点、无符号溢出和非法范围；日期：2026-11-04。
long long Number(const Json &value, const std::string &path, long long low, long long high)
{
    Check(value.is_number_integer() && value >= low && value <= high, path, "Integer is outside bounds");
    return value.get<long long>();
}
// 用途：将流程节点/子流程标识限制为稳定ASCII名称，不接受路径；日期：2026-11-04。
std::string Name(const Json &value, const std::string &path)
{
    const auto name = Text(value, path, 64);
    Check(name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
              std::string::npos,
          path, "Identifier contains unsupported characters");
    return name;
}
// 用途：对规范JSON字节计算SHA-256，发布版本读取时再次比对；日期：2026-11-04。
std::string Digest(const Json &value)
{
    const auto bytes = value.dump();
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw Error("WORKFLOW_HASH_FAILED", "Cannot initialize version hashing");
    unsigned char hash[32]{};
    const auto result =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(bytes.data())),
                   static_cast<ULONG>(bytes.size()), hash, sizeof(hash));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0)
        throw Error("WORKFLOW_HASH_FAILED", "Cannot hash workflow version");
    std::ostringstream output;
    for (const auto byte : hash)
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return output.str();
}
// 用途：允许Schema枚举在完整结构检查后共用实际值验证器；日期：2026-11-05。
void Input(const Json &schema, const Json &value, const std::string &path, bool choices);
// 用途：核对受限draft-07输入方言，拒绝远程引用、脚本、未知关键字及无界数据；日期：2026-11-04。
void Schema(const Json &schema, const std::string &path, size_t depth = 0)
{
    Check(depth <= 8, path, "Input schema nesting exceeds eight", "WORKFLOW_BUDGET");
    Fields(schema, path,
           {"$schema", "type", "properties", "required", "additionalProperties", "items", "minItems",
            "maxItems", "minLength", "maxLength", "minimum", "maximum", "enum"},
           {"type"});
    if (schema.contains("$schema"))
        Check(depth == 0 && schema.at("$schema") == "http://json-schema.org/draft-07/schema#", path,
              "Only the declared draft-07 input subset is accepted");
    const auto type = Text(schema.at("type"), path + "/type", 16);
    std::set<std::string> keys{"$schema", "type", "enum"};
    if (type == "object")
    {
        keys.insert({"properties", "required", "additionalProperties"});
        Check(schema.contains("properties") && schema.at("properties").is_object() &&
                  schema.at("properties").size() <= 32 && schema.contains("required") &&
                  schema.at("required").is_array() && schema.contains("additionalProperties") &&
                  schema.at("additionalProperties") == false,
              path, "Objects require closed properties and an explicit required list");
        std::set<std::string> required;
        for (const auto &field : schema.at("required"))
        {
            const auto key = Name(field, path + "/required");
            Check(schema.at("properties").contains(key) && required.insert(key).second, path,
                  "Required property is missing or duplicated");
        }
        for (auto it = schema.at("properties").begin(); it != schema.at("properties").end(); ++it)
        {
            Name(it.key(), path + "/properties");
            Schema(it.value(), path + "/properties/" + it.key(), depth + 1);
        }
    }
    else if (type == "string")
    {
        keys.insert({"minLength", "maxLength"});
        Check(schema.contains("maxLength"), path, "Strings require maxLength");
        const auto maximum = Number(schema.at("maxLength"), path, 0, 65536);
        if (schema.contains("minLength"))
            Number(schema.at("minLength"), path, 0, maximum);
    }
    else if (type == "integer")
    {
        keys.insert({"minimum", "maximum"});
        Check(schema.contains("minimum") && schema.contains("maximum"), path,
              "Integers require minimum and maximum");
        const auto low = Number(schema.at("minimum"), path, -1000000000, 1000000000);
        Number(schema.at("maximum"), path, low, 1000000000);
    }
    else if (type == "array")
    {
        keys.insert({"items", "minItems", "maxItems"});
        Check(schema.contains("items") && schema.contains("maxItems"), path,
              "Arrays require items and maxItems");
        const auto maximum = Number(schema.at("maxItems"), path, 0, 32);
        if (schema.contains("minItems"))
            Number(schema.at("minItems"), path, 0, maximum);
        Schema(schema.at("items"), path + "/items", depth + 1);
    }
    else
        Check(type == "boolean", path, "Unsupported input type");
    for (auto it = schema.begin(); it != schema.end(); ++it)
        Check(keys.count(it.key()) != 0, path, "Keyword does not apply to this input type");
    if (schema.contains("enum"))
    {
        Check(type != "object" && type != "array" && schema.at("enum").is_array() &&
                  !schema.at("enum").empty() && schema.at("enum").size() <= 32,
              path, "Enums must contain one to 32 scalar choices");
        std::set<std::string> unique;
        for (const auto &value : schema.at("enum"))
        {
            Check(unique.insert(value.dump()).second, path, "Enum contains duplicate choices");
            Input(schema, value, path + "/enum", false);
        }
    }
}
// 用途：按已校验的输入Schema核对数据，错误仅给字段位置及类型；日期：2026-11-04。
void Input(const Json &schema, const Json &value, const std::string &path, bool choices = true)
{
    const auto type = schema.at("type").get<std::string>();
    if (type == "object")
    {
        Check(value.is_object(), path, "Expected an object input");
        for (const auto &key : schema.at("required"))
            Check(value.contains(key.get<std::string>()), path, "Required input is missing");
        for (auto it = value.begin(); it != value.end(); ++it)
        {
            Check(schema.at("properties").contains(it.key()), path, "Unrecognized input property");
            Input(schema.at("properties").at(it.key()), it.value(), path + "/" + it.key());
        }
    }
    else if (type == "string")
    {
        const auto text = Text(value, path, schema.at("maxLength"), true);
        Check(UnicodeLength(text) >= schema.value("minLength", size_t{0}), path, "Input string is too short");
    }
    else if (type == "integer")
        Number(value, path, schema.at("minimum"), schema.at("maximum"));
    else if (type == "boolean")
        Check(value.is_boolean(), path, "Expected a boolean input");
    else if (type == "array")
    {
        Check(value.is_array() && value.size() >= schema.value("minItems", size_t{0}) &&
                  value.size() <= schema.at("maxItems").get<size_t>(),
              path, "Input array is outside bounds");
        for (const auto &item : value)
            Input(schema.at("items"), item, path + "/items");
    }
    if (choices && schema.contains("enum"))
        Check(std::find(schema.at("enum").begin(), schema.at("enum").end(), value) != schema.at("enum").end(),
              path, "Input is outside declared choices");
}
// 用途：取得必填输入字段的静态类型，禁止任意路径、可选字段及数组动态索引；日期：2026-11-04。
Json Reference(const Json &schema, const Json &reference, const std::string &path)
{
    const auto pointer = Text(reference, path, 512);
    Check(pointer.size() > 1 && pointer.front() == '/', path, "Input reference must be an absolute pointer");
    Json current = schema;
    size_t start = 1;
    while (start < pointer.size())
    {
        const auto end = pointer.find('/', start);
        const auto key = pointer.substr(start, end == std::string::npos ? end : end - start);
        Name(key, path);
        Check(current.at("type") == "object" && current.at("properties").contains(key) &&
                  std::find(current.at("required").begin(), current.at("required").end(), key) !=
                      current.at("required").end(),
              path, "References require a declared mandatory input", "WORKFLOW_TYPE_INVALID");
        current = Json(current.at("properties").at(key));
        if (end == std::string::npos)
            return current;
        start = end + 1;
    }
    throw Invalid("WORKFLOW_TYPE_INVALID", path, "Input reference ends with a slash");
}
// 用途：核对表达式静态类型能否完整装入目标Schema，含边界、枚举和嵌套；日期：2026-11-05。
void Fits(const Json &source, const Json &target, const std::string &path)
{
    Check(source.at("type") == target.at("type"), path, "Variable type does not match",
          "WORKFLOW_TYPE_INVALID");
    const auto type = source.at("type").get<std::string>();
    if (type == "string")
        Check(source.value("minLength", 0) >= target.value("minLength", 0) &&
                  source.at("maxLength") <= target.at("maxLength"),
              path, "String bounds do not fit target", "WORKFLOW_TYPE_INVALID");
    if (type == "integer")
        Check(source.at("minimum") >= target.at("minimum") && source.at("maximum") <= target.at("maximum"),
              path, "Integer bounds do not fit target", "WORKFLOW_TYPE_INVALID");
    if (type == "array")
    {
        Check(source.value("minItems", 0) >= target.value("minItems", 0) &&
                  source.at("maxItems") <= target.at("maxItems"),
              path, "List bounds do not fit target", "WORKFLOW_TYPE_INVALID");
        Fits(source.at("items"), target.at("items"), path + "/items");
    }
    if (type == "object")
    {
        Check(source.at("properties").size() == target.at("properties").size(), path,
              "Object property sets must match", "WORKFLOW_TYPE_INVALID");
        for (const auto &key : target.at("required"))
            Check(std::find(source.at("required").begin(), source.at("required").end(), key) !=
                      source.at("required").end(),
                  path, "Target mandatory property is not guaranteed", "WORKFLOW_TYPE_INVALID");
        for (auto it = source.at("properties").begin(); it != source.at("properties").end(); ++it)
        {
            Check(target.at("properties").contains(it.key()), path, "Object properties differ",
                  "WORKFLOW_TYPE_INVALID");
            Fits(it.value(), target.at("properties").at(it.key()), path + "/" + it.key());
        }
    }
    if (target.contains("enum"))
    {
        Check(source.contains("enum"), path, "Expression must guarantee the declared choices",
              "WORKFLOW_TYPE_INVALID");
        for (const auto &choice : source.at("enum"))
            Check(std::find(target.at("enum").begin(), target.at("enum").end(), choice) !=
                      target.at("enum").end(),
                  path, "Expression choice is outside target choices", "WORKFLOW_TYPE_INVALID");
    }
}
// 用途：校验有界AST、变量和显式数据字面量，绝不执行宿主脚本；日期：2026-11-05。
Json Expression(const Json &value, const Json &input, const std::string &path,
                const Json &variables = Json::object(), size_t depth = 0)
{
    Check(depth <= 8, path, "Expression nesting exceeds eight", "WORKFLOW_BUDGET");
    if (value.is_object())
    {
        if (value.contains("input"))
        {
            Fields(value, path, {"input"}, {"input"});
            return Reference(input, value.at("input"), path + "/input");
        }
        if (value.contains("var"))
        {
            Fields(value, path, {"var"}, {"var"});
            const auto name = Name(value.at("var"), path + "/var");
            Check(variables.contains(name), path, "Variable is not declared", "WORKFLOW_TYPE_INVALID");
            return variables.at(name);
        }
        if (value.contains("literal"))
        {
            Fields(value, path, {"literal", "schema"}, {"literal", "schema"});
            Schema(value.at("schema"), path + "/schema");
            Input(value.at("schema"), value.at("literal"), path + "/literal");
            return value.at("schema");
        }
        Fields(value, path, {"op", "args"}, {"op", "args"});
        const auto op = Text(value.at("op"), path + "/op", 16);
        const auto &args = value.at("args");
        Check(args.is_array() && !args.empty() && args.size() <= 8, path,
              "Expression requires bounded arguments");
        std::vector<Json> schemas;
        for (const auto &arg : args)
            schemas.push_back(Expression(arg, input, path + "/args", variables, depth + 1));
        if (op == "add" || op == "sub")
        {
            Check(schemas.size() == 2 && schemas[0].at("type") == "integer" &&
                      schemas[1].at("type") == "integer",
                  path, "Arithmetic requires two integer operands", "WORKFLOW_TYPE_INVALID");
            const auto low = schemas[0].at("minimum").get<long long>() +
                             (op == "add" ? schemas[1].at("minimum").get<long long>()
                                          : -schemas[1].at("maximum").get<long long>());
            const auto high = schemas[0].at("maximum").get<long long>() +
                              (op == "add" ? schemas[1].at("maximum").get<long long>()
                                           : -schemas[1].at("minimum").get<long long>());
            Check(low >= -1000000000 && high <= 1000000000, path, "Arithmetic may exceed integer budget",
                  "WORKFLOW_TYPE_INVALID");
            return {{"type", "integer"}, {"minimum", low}, {"maximum", high}};
        }
        if (op == "concat")
        {
            size_t maximum = 0;
            for (const auto &schema : schemas)
            {
                Check(schema.at("type") == "string", path, "Concat requires strings",
                      "WORKFLOW_TYPE_INVALID");
                maximum += schema.at("maxLength").get<size_t>();
            }
            Check(maximum <= 65536, path, "Concat may exceed text budget", "WORKFLOW_TYPE_INVALID");
            return {{"type", "string"}, {"maxLength", maximum}};
        }
        Check(schemas.size() == 1, path, "Unary expression requires one operand");
        const auto &schema = schemas[0];
        if (op == "length")
        {
            Check(schema.at("type") == "string" || schema.at("type") == "array", path,
                  "Length requires string or list", "WORKFLOW_TYPE_INVALID");
            return {{"type", "integer"},
                    {"minimum", 0},
                    {"maximum", schema.at(schema.at("type") == "string" ? "maxLength" : "maxItems")}};
        }
        Check(op == "trim" && schema.at("type") == "string", path,
              "Unsupported expression operator or operand", "WORKFLOW_TYPE_INVALID");
        return {{"type", "string"}, {"maxLength", schema.at("maxLength")}};
    }
    if (value.is_string())
    {
        Text(value, path, 65536, true);
        const auto length = UnicodeLength(value.get<std::string>());
        return {
            {"type", "string"}, {"minLength", length}, {"maxLength", length}, {"enum", Json::array({value})}};
    }
    if (value.is_number_integer())
    {
        const auto integer = Number(value, path, -1000000000, 1000000000);
        return {
            {"type", "integer"}, {"minimum", integer}, {"maximum", integer}, {"enum", Json::array({value})}};
    }
    Check(value.is_boolean(), path, "Expressions require scalar literals or a supported AST");
    return {{"type", "boolean"}, {"enum", Json::array({value})}};
}
// 用途：核对表达式与页面动作类型，变量/AST仅由新版流程传入；日期：2026-11-05。
void Typed(const Json &value, const Json &input, const std::string &path, const std::string &type,
           size_t maximum = 65536, const Json &variables = Json::object())
{
    const auto schema = Expression(value, input, path, variables);
    Check(schema.at("type") == type && (type != "string" || schema.at("maxLength").get<size_t>() <= maximum),
          path, "Expression does not fit the required type and bounds", "WORKFLOW_TYPE_INVALID");
}
// 用途：校验有限谓词AST及比较类型，禁止递归过深和脚本求值；日期：2026-11-04。
void Predicate(const Json &value, const Json &input, const std::string &path, size_t depth = 0,
               const Json &variables = Json::object())
{
    Check(depth <= 8, path, "Predicate depth exceeded", "WORKFLOW_BUDGET");
    Check(value.is_object() && value.contains("op"), path, "Predicate requires an operator");
    const auto op = Text(value.at("op"), path + "/op", 16);
    if (op == "and" || op == "or")
    {
        Fields(value, path, {"op", "args"}, {"op", "args"});
        Check(value.at("args").is_array() && !value.at("args").empty() && value.at("args").size() <= 8, path,
              "Boolean predicates require one to eight arguments");
        for (const auto &arg : value.at("args"))
            Predicate(arg, input, path + "/args", depth + 1, variables);
    }
    else if (op == "not")
    {
        Fields(value, path, {"op", "arg"}, {"op", "arg"});
        Predicate(value.at("arg"), input, path + "/arg", depth + 1, variables);
    }
    else
    {
        Fields(value, path, {"op", "left", "right"}, {"op", "left", "right"});
        Check(op == "eq" || op == "ne" || op == "lt" || op == "le" || op == "gt" || op == "ge", path,
              "Unsupported predicate operator");
        const auto left = Expression(value.at("left"), input, path + "/left", variables);
        const auto right = Expression(value.at("right"), input, path + "/right", variables);
        Check(left.at("type") == right.at("type") &&
                  ((op == "eq" || op == "ne") || left.at("type") == "integer"),
              path, "Predicate operand types do not match", "WORKFLOW_TYPE_INVALID");
    }
}
// 用途：解释有界纯数据AST，变量只在运行内存中读取，不访问文件/环境或宿主；日期：2026-11-05。
Json Evaluate(const Json &expression, const Json &input, const Json &variables = Json::object())
{
    if (!expression.is_object())
        return expression;
    if (expression.contains("input"))
        return input.at(Json::json_pointer(expression.at("input").get<std::string>()));
    if (expression.contains("var"))
        return variables.at(expression.at("var").get<std::string>());
    if (expression.contains("literal"))
        return expression.at("literal");
    const auto op = expression.at("op").get<std::string>();
    const auto &args = expression.at("args");
    const auto first = Evaluate(args[0], input, variables);
    if (op == "add" || op == "sub")
    {
        const auto left = first.get<long long>();
        const auto right = Evaluate(args[1], input, variables).get<long long>();
        return op == "add" ? left + right : left - right;
    }
    if (op == "length")
        return first.is_string() ? UnicodeLength(first.get<std::string>()) : first.size();
    if (op == "trim")
    {
        const auto text = first.get<std::string>();
        const auto begin = text.find_first_not_of(" \t\r\n");
        return begin == std::string::npos ? ""
                                          : text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
    }
    std::string result;
    for (const auto &arg : args)
        result += Evaluate(arg, input, variables).get<std::string>();
    return result;
}
// 用途：解释受限布尔AST，短路组合且仅比较已声明类型；日期：2026-11-04。
bool Test(const Json &predicate, const Json &input, const Json &variables = Json::object())
{
    const auto op = predicate.at("op").get<std::string>();
    if (op == "not")
        return !Test(predicate.at("arg"), input, variables);
    if (op == "and" || op == "or")
    {
        for (const auto &arg : predicate.at("args"))
            if (Test(arg, input, variables) == (op == "or"))
                return op == "or";
        return op == "and";
    }
    const auto left = Evaluate(predicate.at("left"), input, variables);
    const auto right = Evaluate(predicate.at("right"), input, variables);
    if (op == "eq")
        return left == right;
    if (op == "ne")
        return left != right;
    if (op == "lt")
        return left < right;
    if (op == "le")
        return left <= right;
    if (op == "gt")
        return left > right;
    return left >= right;
}

struct Validator
{
    const Json &definition;
    const Json &input;
    std::map<std::string, Json> flows;
    std::map<std::string, size_t> costs;
    std::map<std::string, size_t> heights;
    std::set<std::string> visiting;
    std::set<std::string> capabilities;
    std::set<std::string> origins;
    size_t total_nodes = 0;
    Json variables = Json::object();
    bool extended = false;
    // 用途：保存顶层定义及全局输入Schema，不复制任意运行数据；日期：2026-11-04。
    explicit Validator(const Json &value) : definition(value), input(value.at("inputSchema"))
    {
    }
    // 用途：逐节点验证类型、文件动作、只读输出、静态导航范围和显式循环上限；日期：2026-11-05。
    void Step(const Json &step, const std::string &path)
    {
        Check(step.is_object() && step.contains("id") && step.contains("type"), path,
              "Step requires id and type");
        Name(step.at("id"), path + "/id");
        const auto type = Text(step.at("type"), path + "/type", 32);
        std::set<std::string> fields{"id", "type"};
        std::set<std::string> required{"id", "type"};
        if (type != "end" && type != "condition")
        {
            fields.insert("next");
            required.insert("next");
        }
        if (type == "navigate")
        {
            fields.insert("url");
            required.insert("url");
        }
        else if (type == "fill" || type == "click" || type == "select" || type == "assertText")
        {
            fields.insert("selector");
            required.insert("selector");
            if (type == "fill")
            {
                fields.insert("value");
                required.insert("value");
            }
            if (type == "select")
            {
                fields.insert("values");
                required.insert("values");
            }
            if (type == "click")
            {
                fields.insert("postcondition");
                required.insert("postcondition");
            }
            if (type == "assertText")
            {
                fields.insert({"expected", "timeoutMs"});
                required.insert({"expected", "timeoutMs"});
            }
        }
        else if (type == "condition")
        {
            fields.insert({"when", "then", "else"});
            required.insert({"when", "then", "else"});
        }
        else if (type == "foreach" && extended)
        {
            fields.insert({"items", "item", "maxIterations", "flow"});
            required.insert({"items", "item", "maxIterations", "flow"});
        }
        else if ((type == "set" || type == "readText") && extended)
        {
            fields.insert({"variable", type == "set" ? "value" : "selector"});
            required.insert({"variable", type == "set" ? "value" : "selector"});
        }
        else if (type == "readTexts" && extended)
        {
            fields.insert({"variable", "selector", "maxItems"});
            required.insert({"variable", "selector", "maxItems"});
        }
        else if (type == "upload" && extended)
        {
            fields.insert({"selector", "path"});
            required.insert({"selector", "path"});
        }
        else if (type == "downloadConfig" && extended)
        {
            fields.insert("directory");
            required.insert("directory");
        }
        else if (type == "downloadWait" && extended)
        {
            fields.insert({"fileName", "timeoutMs"});
            required.insert({"fileName", "timeoutMs"});
        }
        else if (type == "assert" && extended)
        {
            fields.insert("assertion");
            required.insert("assertion");
        }
        else if (type == "loop")
        {
            fields.insert({"count", "maxIterations", "flow"});
            required.insert({"count", "maxIterations", "flow"});
        }
        else if (type == "call")
        {
            fields.insert("flow");
            required.insert("flow");
        }
        else
            Check(type == "end", path + "/type", "Step capability is not implemented",
                  "WORKFLOW_CAPABILITY_UNSUPPORTED");
        if (extended && (type == "set" || type == "readText" || type == "assertText" || type == "assert" ||
                         type == "readTexts"))
            fields.insert("onError");
        Fields(step, path, fields, required);
        if (step.contains("onError"))
        {
            const auto &route = step.at("onError");
            Fields(route, path + "/onError", {"codes", "next"}, {"codes", "next"});
            Name(route.at("next"), path + "/onError/next");
            Check(route.at("codes").is_array() && !route.at("codes").empty() && route.at("codes").size() <= 4,
                  path, "Error route requires bounded explicit error codes");
            const std::set<std::string> allowed{"ELEMENT_NOT_FOUND",
                                                "LOCATOR_AMBIGUOUS",
                                                "ASSERTION_TIMEOUT",
                                                "WORKFLOW_SCHEMA_INVALID",
                                                "ASSERTION_FAILED",
                                                "INVALID_SELECTOR",
                                                "EXTRACTION_CONSTRAINT_FAILED",
                                                "EXTRACTION_SOURCE_DENIED",
                                                "EXTRACTION_VALUE_TOO_LARGE"};
            std::set<std::string> unique;
            for (const auto &code : route.at("codes"))
                Check(code.is_string() && allowed.count(code.get<std::string>()) &&
                          unique.insert(code.get<std::string>()).second,
                      path, "Only explicit deterministic observation/type failures can be routed");
        }
        for (const auto &edge : {"next", "then", "else", "flow"})
            if (step.contains(edge))
                Name(step.at(edge), path + "/" + edge);
        if (step.contains("selector"))
            Text(step.at("selector"), path + "/selector", 2048);
        if (type == "navigate")
        {
            const auto url = Text(step.at("url"), path + "/url", 8192);
            Check(origins.count(Browser::Origin(url)) != 0, path + "/url",
                  "Navigation is outside workflow scope", "WORKFLOW_ORIGIN_DENIED");
        }
        if (type == "fill")
        {
            Check(step.at("value").is_object(), path + "/value", "Fill values must use input references");
            if (extended && step.at("value").contains("secretRef"))
            {
                Fields(step.at("value"), path + "/value", {"secretRef"}, {"secretRef"});
                const auto id = Text(step.at("value").at("secretRef"), path + "/value/secretRef", 32);
                Check(id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos, path,
                      "SecretRef requires a generated credential ID");
            }
            else
                Typed(step.at("value"), input, path + "/value", "string", 65536, variables);
        }
        if (type == "select")
        {
            Check(step.at("values").is_array() && step.at("values").size() <= 32, path + "/values",
                  "Select values require a bounded expression array");
            for (const auto &value : step.at("values"))
                Typed(value, input, path + "/values", "string", 4096, variables);
        }
        if (type == "assertText" || type == "click")
        {
            const auto &assertion = type == "click" ? step.at("postcondition") : step;
            if (type == "click")
                Fields(assertion, path + "/postcondition", {"selector", "expected", "timeoutMs"},
                       {"selector", "expected", "timeoutMs"});
            Text(assertion.at("selector"), path + "/selector", 2048);
            Typed(assertion.at("expected"), input, path + "/expected", "string", 16384, variables);
            Number(assertion.at("timeoutMs"), path + "/timeoutMs", 1, 10000);
        }
        if (type == "condition")
            Predicate(step.at("when"), input, path + "/when", 0, variables);
        if (type == "downloadWait")
        {
            const auto name = Text(step.at("fileName"), path + "/fileName", 255);
            Check(name != "." && name != ".." && name.find_first_of("\\/:*?\"<>|") == std::string::npos,
                  path + "/fileName", "Download wait requires a file name without path components");
            Number(step.at("timeoutMs"), path + "/timeoutMs", 1, 10000);
        }
        if (type == "upload" || type == "downloadConfig")
        {
            const auto key = type == "upload" ? "path" : "directory";
            Check(step.at(key).is_object(), path, "Local paths must use typed input or variable references");
            Typed(step.at(key), input, path + "/" + key, "string", 32768, variables);
        }
        if (type == "assert")
        {
            try
            {
                Browser::ValidateAssertion(step.at("assertion"));
            }
            catch (const Error &)
            {
                Check(false, path + "/assertion", "Invalid structured assertion");
            }
        }
        if (type == "loop")
        {
            const auto maximum = Number(step.at("maxIterations"), path, 0, 100);
            const auto count = Expression(step.at("count"), input, path + "/count", variables);
            Check(count.at("type") == "integer" && count.at("minimum") >= 0 && count.at("maximum") <= maximum,
                  path + "/count", "Loop count must have a finite declared bound", "WORKFLOW_TYPE_INVALID");
        }
        if (type == "set" || type == "readText" || type == "readTexts")
        {
            const auto variable = Name(step.at("variable"), path + "/variable");
            Check(variables.contains(variable), path, "Output variable is not declared",
                  "WORKFLOW_TYPE_INVALID");
            const auto source =
                type == "set"         ? Expression(step.at("value"), input, path + "/value", variables)
                : type == "readTexts" ? Json{{"type", "array"},
                                             {"maxItems", Number(step.at("maxItems"), path, 0, 32)},
                                             {"items", {{"type", "string"}, {"maxLength", 4096}}}}
                                      : Json{{"type", "string"}, {"maxLength", 16384}};
            Check(source.at("type") == variables.at(variable).at("type"), path + "/variable",
                  "Assignment type differs from the declared variable", "WORKFLOW_TYPE_INVALID");
            if (type == "readText" || type == "readTexts")
                Fits(source, variables.at(variable), path + "/variable");
        }
        if (type == "foreach")
        {
            const auto maximum = Number(step.at("maxIterations"), path, 0, 32);
            const auto items = Expression(step.at("items"), input, path + "/items", variables);
            Check(items.at("type") == "array" && items.at("maxItems") <= maximum, path,
                  "Foreach requires a list within the declared iteration bound", "WORKFLOW_TYPE_INVALID");
            const auto item = Name(step.at("item"), path + "/item");
            Check(variables.contains(item), path, "Foreach item variable is not declared",
                  "WORKFLOW_TYPE_INVALID");
            Fits(items.at("items"), variables.at(item), path + "/item");
        }
        if (type == "navigate" || type == "fill" || type == "click" || type == "select" ||
            type == "assertText" || type == "readText" || type == "set" || type == "foreach")
            capabilities.insert(type);
        if (type == "upload" || type == "downloadConfig" || type == "downloadWait" || type == "assert" ||
            type == "readTexts")
            capabilities.insert(type);
    }
    // 用途：验证无隐式环的流程图、不可达节点及子流程递归，并计算最坏派发预算；日期：2026-11-04。
    size_t Flow(const std::string &name, size_t depth)
    {
        Check(depth <= 8, "/subflows", "Subflow call depth exceeded", "WORKFLOW_BUDGET");
        Check(flows.count(name) != 0, "/subflows", "Referenced subflow does not exist",
              "WORKFLOW_GRAPH_INVALID");
        Check(visiting.count(name) == 0, "/subflows", "Recursive subflow calls are forbidden",
              "WORKFLOW_GRAPH_INVALID");
        if (costs.count(name))
        {
            Check(depth + heights.at(name) <= 8, "/subflows", "Subflow call depth exceeded",
                  "WORKFLOW_BUDGET");
            return costs.at(name);
        }
        Check(visiting.insert(name).second, "/subflows", "Recursive subflow calls are forbidden",
              "WORKFLOW_GRAPH_INVALID");
        const auto &flow = flows.at(name);
        Fields(flow, "/flows/" + name, {"entry", "steps"}, {"entry", "steps"});
        const auto entry = Name(flow.at("entry"), "/flows/" + name + "/entry");
        Check(flow.at("steps").is_array() && !flow.at("steps").empty() && flow.at("steps").size() <= 128,
              "/flows/" + name, "Flows require one to 128 steps");
        std::map<std::string, Json> steps;
        for (const auto &step : flow.at("steps"))
        {
            const auto path = "/flows/" + name + "/steps/" + std::to_string(steps.size());
            Step(step, path);
            Check(steps.emplace(step.at("id").get<std::string>(), step).second, path, "Duplicate step ID",
                  "WORKFLOW_GRAPH_INVALID");
        }
        if (costs.count(name) == 0)
        {
            total_nodes += steps.size();
            Check(total_nodes <= 256, "/steps", "Total step definition budget exceeded", "WORKFLOW_BUDGET");
        }
        std::map<std::string, size_t> memo;
        std::set<std::string> active;
        size_t height = 0;
        // 用途：遍历每条有向边，分支取最大路径且显式循环按上限计算；日期：2026-11-04。
        std::function<size_t(const std::string &)> visit = [&](const std::string &id)
        {
            Check(steps.count(id) != 0, "/flows/" + name, "Step edge points to a missing node",
                  "WORKFLOW_GRAPH_INVALID");
            Check(active.insert(id).second, "/flows/" + name, "Implicit graph cycles are forbidden",
                  "WORKFLOW_GRAPH_INVALID");
            if (memo.count(id))
            {
                active.erase(id);
                return memo.at(id);
            }
            const auto &step = steps.at(id);
            const auto type = step.at("type").get<std::string>();
            size_t cost = 1;
            if (type == "condition")
                cost += (std::max)(visit(step.at("then")), visit(step.at("else")));
            else if (type != "end")
            {
                cost += visit(step.at("next"));
                if (type == "loop" || type == "call" || type == "foreach")
                {
                    cost += Flow(step.at("flow"), depth + 1) * (type == "loop" || type == "foreach"
                                                                    ? step.at("maxIterations").get<size_t>()
                                                                    : size_t{1});
                    height = (std::max)(height, heights.at(step.at("flow").get<std::string>()) + 1);
                }
            }
            if (step.contains("onError"))
                cost = (std::max)(cost, size_t{1} + visit(step.at("onError").at("next")));
            Check(cost <= 1024, "/flows/" + name, "Worst-case step budget exceeded", "WORKFLOW_BUDGET");
            active.erase(id);
            memo[id] = cost;
            return cost;
        };
        const auto cost = visit(entry);
        Check(memo.size() == steps.size(), "/flows/" + name, "Unreachable step definitions are forbidden",
              "WORKFLOW_GRAPH_INVALID");
        visiting.erase(name);
        costs[name] = cost;
        heights[name] = height;
        return cost;
    }
    // 用途：统一验证根版本、输入方言、作用域、流程图和能力预算；日期：2026-11-04。
    Json Run()
    {
        Check(definition.at("schemaVersion") == "1.0" || definition.at("schemaVersion") == "1.1",
              "/schemaVersion", "Unsupported workflow DSL version", "WORKFLOW_VERSION_UNSUPPORTED");
        extended = definition.at("schemaVersion") == "1.1";
        Check(extended || !definition.contains("variables"), "/variables", "Variables require DSL 1.1");
        Text(definition.at("name"), "/name", 128);
        Number(definition.at("version"), "/version", 1, 1000000);
        Schema(input, "/inputSchema");
        Check(input.at("type") == "object", "/inputSchema", "Workflow input must be a closed object");
        // 用途：枚举值同样必须满足声明的类型与大小界限；日期：2026-11-04。
        std::function<void(const Json &)> enums = [&](const Json &schema)
        {
            if (schema.contains("enum"))
                for (const auto &value : schema.at("enum"))
                    Input(schema, value, "/inputSchema/enum", false);
            if (schema.at("type") == "object")
                for (const auto &child : schema.at("properties"))
                    enums(child);
            if (schema.at("type") == "array")
                enums(schema.at("items"));
        };
        enums(input);
        if (definition.contains("variables"))
        {
            const auto &declarations = definition.at("variables");
            Check(declarations.is_object() && declarations.size() <= 32, "/variables",
                  "Variable budget exceeded");
            for (auto it = declarations.begin(); it != declarations.end(); ++it)
            {
                Name(it.key(), "/variables");
                const auto path = "/variables/" + it.key();
                Fields(it.value(), path, {"schema", "initial", "sensitive"},
                       {"schema", "initial", "sensitive"});
                Check(it.value().at("sensitive").is_boolean(), path, "Sensitive marker must be boolean");
                Schema(it.value().at("schema"), path + "/schema");
                enums(it.value().at("schema"));
                const auto source = Expression(it.value().at("initial"), input, path + "/initial");
                Fits(source, it.value().at("schema"), path + "/initial");
                variables[it.key()] = it.value().at("schema");
            }
        }
        const auto &policy = definition.at("policy");
        Fields(policy, "/policy", {"allowedOrigins", "maxSteps", "maxDurationMs"},
               {"allowedOrigins", "maxSteps", "maxDurationMs"});
        Check(policy.at("allowedOrigins").is_array() && !policy.at("allowedOrigins").empty() &&
                  policy.at("allowedOrigins").size() <= 32,
              "/policy/allowedOrigins", "Workflow requires one to 32 origins");
        for (const auto &origin : policy.at("allowedOrigins"))
            Check(origins.insert(Browser::Origin(Text(origin, "/policy/allowedOrigins", 8192))).second,
                  "/policy/allowedOrigins", "Duplicate normalized origins");
        const auto maximum = Number(policy.at("maxSteps"), "/policy/maxSteps", 1, 1024);
        Number(policy.at("maxDurationMs"), "/policy/maxDurationMs", 1, 300000);
        flows["main"] = {{"entry", definition.at("entry")}, {"steps", definition.at("steps")}};
        if (definition.contains("subflows"))
        {
            Check(definition.at("subflows").is_object() && definition.at("subflows").size() <= 32,
                  "/subflows", "Subflow definition budget exceeded");
            for (auto it = definition.at("subflows").begin(); it != definition.at("subflows").end(); ++it)
            {
                Name(it.key(), "/subflows");
                Check(it.key() != "main", "/subflows", "Main is a reserved flow identifier");
                flows[it.key()] = it.value();
            }
        }
        if (!extended)
        {
            // 用途：旧方言拒绝新增表达式，避免同一版本在升级后改变原有准入行为；日期：2026-11-05。
            std::function<void(const Json &)> legacy = [&](const Json &value)
            {
                if (value.is_object())
                    Check(!value.contains("var") && !value.contains("literal") &&
                              (!value.contains("op") || !value.contains("args") || value.at("op") == "and" ||
                               value.at("op") == "or"),
                          "/steps", "New expressions require DSL 1.1");
                if (value.is_structured())
                    for (const auto &child : value)
                        legacy(child);
            };
            for (const auto &flow : flows)
                legacy(flow.second.at("steps"));
        }
        const auto cost = Flow("main", 0);
        Check(costs.size() == flows.size(), "/subflows", "Unused subflow definitions are forbidden",
              "WORKFLOW_GRAPH_INVALID");
        Check(cost <= static_cast<size_t>(maximum), "/policy/maxSteps",
              "Declared maxSteps is below the worst-case execution cost", "WORKFLOW_BUDGET");
        return {{"valid", true},
                {"schemaVersion", definition.at("schemaVersion")},
                {"inputDialect", "awb-bounded-draft07-v1"},
                {"sha256", Digest(definition)},
                {"maximumExecutedSteps", cost},
                {"requiredCapabilities", capabilities},
                {"errors", Json::array()}};
    }
};
// 用途：在读取或写入文件前限制标识为生成的32位十六进制，禁止路径穿越；日期：2026-11-04。
void RecordId(const std::string &id)
{
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw Error("WORKFLOW_ID_INVALID", "Expected an owned record ID");
}
// 用途：在受管目录中有界枚举记录，重解析点、硬链接及未知文件均拒绝；日期：2026-11-04。
std::vector<std::string> Records(const std::wstring &directory)
{
    std::vector<std::string> records;
    WIN32_FIND_DATAW data{};
    const auto search = FindFirstFileW((L"\\\\?\\" + SafeAbsolute(directory) + L"\\*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE)
        throw Error("WORKFLOW_STORAGE_ERROR", "Cannot enumerate workflow records");
    try
    {
        size_t inspected = 0;
        do
        {
            const std::wstring name(data.cFileName);
            if (name == L"." || name == L"..")
                continue;
            if (++inspected > MaximumRecords * 3)
                throw Error("WORKFLOW_STORAGE_ERROR", "Workflow directory enumeration budget exceeded");
            // 原子写中断留下的临时文件不可当作发布或运行记录。
            if (name.size() == 74 && name.substr(32, 6) == L".json." && name.substr(70) == L".tmp")
                continue;
            if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                name.size() != 37 || name.substr(32) != L".json")
                throw Error("WORKFLOW_STORAGE_ERROR", "Unrecognized workflow record");
            const auto id = Utf8(name.substr(0, 32));
            RecordId(id);
            records.push_back(id);
            if (records.size() > MaximumRecords)
                throw Error("WORKFLOW_STORAGE_ERROR", "Workflow record count exceeds budget");
        } while (FindNextFileW(search, &data));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            throw Error("WORKFLOW_STORAGE_ERROR", "Workflow enumeration was interrupted");
        FindClose(search);
    }
    catch (...)
    {
        FindClose(search);
        throw;
    }
    return records;
}
// 用途：区分可安全查看的终态与可继续派发的活动状态；日期：2026-11-04。
bool Terminal(const std::string &state)
{
    return state == "succeeded" || state == "failed" || state == "cancelled" || state == "outcome-unknown" ||
           state == "interrupted";
}
} // namespace

// 用途：校验受限方言与输入，固定错误结构且不回显无效业务值；日期：2026-11-04。
Json ValidateWorkflow(const Json &definition, const Json *input)
{
    try
    {
        size_t nodes = 0, bytes = 0;
        Budget(definition, "/", 0, nodes, bytes);
        Check(definition.dump().size() <= MaximumDefinitionBytes, "/", "Serialized definition exceeds budget",
              "WORKFLOW_BUDGET");
        Fields(definition, "/",
               {"schemaVersion", "name", "version", "inputSchema", "policy", "entry", "steps", "subflows",
                "variables"},
               {"schemaVersion", "name", "version", "inputSchema", "policy", "entry", "steps"});
        auto result = Validator(definition).Run();
        if (input)
        {
            nodes = bytes = 0;
            Budget(*input, "/input", 0, nodes, bytes);
            Check(input->dump().size() <= MaximumDefinitionBytes, "/input", "Serialized input exceeds budget",
                  "WORKFLOW_BUDGET");
            Input(definition.at("inputSchema"), *input, "/input");
        }
        result["inputChecked"] = input != nullptr;
        return result;
    }
    catch (const Invalid &error)
    {
        return {{"valid", false},
                {"errors",
                 Json::array({{{"code", error.code}, {"path", error.path}, {"message", error.what()}}})}};
    }
    catch (const Error &error)
    {
        return {{"valid", false},
                {"errors", Json::array({{{"code", error.code},
                                         {"path", "/"},
                                         {"message", "Workflow contains an invalid value"}}})}};
    }
    catch (const Json::exception &)
    {
        return {{"valid", false},
                {"errors", Json::array({{{"code", "WORKFLOW_SCHEMA_INVALID"},
                                         {"path", "/"},
                                         {"message", "Malformed workflow field"}}})}};
    }
}

namespace
{
// 用途：读取独占不可变快照，拒绝重解析点、硬链接及过深JSON；日期：2026-11-04。
Json ReadRecord(const std::wstring &input)
{
    const auto path = L"\\\\?\\" + SafeAbsolute(input);
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER length{};
    if (file.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(), &info) ||
        info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file.get(), &length) || length.QuadPart < 1 || length.QuadPart > 1024 * 1024)
        throw Error("WORKFLOW_STORAGE_ERROR", "Cannot read an ordinary bounded workflow record");
    std::string bytes(static_cast<size_t>(length.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.get(), &bytes[0], static_cast<DWORD>(bytes.size()), &read, nullptr) ||
        read != bytes.size())
        throw Error("WORKFLOW_STORAGE_ERROR", "Cannot read complete workflow record");
    try
    {
        // 用途：在解析阶段限制嵌套，避免损坏文件先耗尽调用栈；日期：2026-11-04。
        auto envelope =
            Json::parse(bytes,
                        [](int depth, Json::parse_event_t, Json &)
                        {
                            if (depth > 40)
                                throw Error("WORKFLOW_STORAGE_ERROR", "Record nesting exceeds budget");
                            return true;
                        });
        Fields(envelope, "/", {"format", "record", "sha256"}, {"format", "record", "sha256"});
        if (envelope.at("format") != 1 || envelope.at("sha256") != Digest(envelope.at("record")))
            throw Error("WORKFLOW_STORAGE_ERROR", "Workflow record checksum mismatch");
        return envelope.at("record");
    }
    catch (const Json::exception &)
    {
        throw Error("WORKFLOW_STORAGE_ERROR", "Workflow record contains invalid JSON");
    }
}
// 用途：先封装完整内容校验和再原子提交，失败不覆盖前一检查点；日期：2026-11-04。
void WriteRecord(const std::wstring &path, const Json &value)
{
    const auto bytes = Json{{"format", 1}, {"record", value}, {"sha256", Digest(value)}}.dump() + "\n";
    for (size_t attempt = 0;; ++attempt)
    {
        try
        {
            AtomicWrite(path, bytes);
            return;
        }
        catch (const Error &error)
        {
            const std::string message = error.what();
            const bool contention = error.code == "WINDOWS_ERROR" && (message == "MoveFileEx failed: 5" ||
                                                                      message == "MoveFileEx failed: 32" ||
                                                                      message == "MoveFileEx failed: 33");
            if (!contention || attempt >= 3)
                throw;
            // 仅重写同一检查点字节，最多等待60毫秒；这里绝不重发网页动作。
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}
// 用途：区分网页写入、下载配置和只读输出，文件动作同样禁止自动重放；日期：2026-11-05。
std::string ReplayClass(const std::string &type)
{
    if (type == "navigate" || type == "fill" || type == "click" || type == "select" || type == "upload")
        return "page-effect-no-replay";
    if (type == "downloadConfig")
        return "local-effect-no-replay";
    return type == "assertText" || type == "readText" || type == "readTexts" || type == "assert" ||
                   type == "downloadWait"
               ? "read-only"
               : "control-only";
}
// 用途：验证断点确实属于发布版本，禁止未知节点或未公开调试选项；日期：2026-11-05。
std::set<std::string> DebugOptions(const Json &definition, const Json &options, bool starting)
{
    try
    {
        Fields(options, "/debug",
               starting ? std::set<std::string>{"breakpoints", "pauseOnStart"}
                        : std::set<std::string>{"breakpoints"});
        if (options.contains("pauseOnStart"))
            Check(options.at("pauseOnStart").is_boolean(), "/debug", "pauseOnStart must be boolean");
        std::set<std::string> result;
        if (!options.contains("breakpoints"))
            return result;
        const auto &points = options.at("breakpoints");
        Check(points.is_array() && points.size() <= 64, "/debug", "Breakpoint budget exceeded");
        for (const auto &point : points)
        {
            Fields(point, "/debug/breakpoints", {"flow", "stepId"}, {"flow", "stepId"});
            const auto flow = Name(point.at("flow"), "/debug/flow");
            const auto step = Name(point.at("stepId"), "/debug/stepId");
            Check(flow == "main" ||
                      (definition.contains("subflows") && definition.at("subflows").contains(flow)),
                  "/debug", "Breakpoint flow is not declared");
            const auto &steps =
                flow == "main" ? definition.at("steps") : definition.at("subflows").at(flow).at("steps");
            bool found = false;
            for (const auto &node : steps)
                found = found || node.at("id") == step;
            Check(found && result.insert(flow + "/" + step).second, "/debug",
                  "Breakpoint is unknown or duplicated");
        }
        return result;
    }
    catch (const std::exception &)
    {
        throw Error("WORKFLOW_DEBUG_INVALID", "Debug options must reference exact published flow/step IDs");
    }
}
struct Cancelled
{
};
} // namespace

struct WorkflowEngine::Impl
{
    struct Run
    {
        Json record;
        Json definition;
        Json input;
        Json values = Json::object();
        std::set<std::string> sensitive;
        std::set<std::string> breakpoints;
        std::string step_mode;
        size_t step_depth = 0;
        Dispatch dispatch;
        std::thread worker;
        bool active = false;
        bool cancel = false;
        bool pause = false;
        bool storage_failed = false;
        std::chrono::steady_clock::time_point deadline;
    };
    std::wstring versions_path;
    std::wstring runs_path;
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, std::shared_ptr<Run>> runs;
    std::map<std::string, Json> versions;
    bool closing = false;

    // 用途：传播输入及网页输出的敏感性，复合表达式取所有依赖的并集；日期：2026-11-05。
    bool Tainted(const Run &run, const Json &expression)
    {
        if (!expression.is_object())
            return false;
        if (expression.contains("input") || expression.contains("secretRef"))
            return true;
        if (expression.contains("var"))
            return run.sensitive.count(expression.at("var").get<std::string>()) != 0;
        if (expression.contains("args"))
            for (const auto &arg : expression.at("args"))
                if (Tainted(run, arg))
                    return true;
        return false;
    }
    // 用途：运行期再次核对赋值并单调传播敏感标记，秘密不写入检查点；日期：2026-11-05。
    void Assign(Run &run, const std::string &name, const Json &value, bool sensitive)
    {
        Input(run.definition.at("variables").at(name).at("schema"), value, "/runtime/variable");
        std::lock_guard<std::mutex> lock(mutex);
        run.values[name] = value;
        if (sensitive)
            run.sensitive.insert(name);
    }
    // 用途：持有引擎锁时保存检查点，写失败即封闭后续派发；日期：2026-11-04。
    void Save(Run &run)
    {
        try
        {
            WriteRecord(runs_path + L"\\" + Wide(run.record.at("runId")) + L".json", run.record);
        }
        catch (const Error &error)
        {
            run.storage_failed = true;
            run.record["checkpointErrorCode"] = error.code;
            // 平台错误只包含固定API名和系统错误号，不包含路径或业务值。
            if (error.code == "WINDOWS_ERROR")
                run.record["checkpointSystemError"] = error.what();
            throw;
        }
        catch (...)
        {
            run.storage_failed = true;
            throw;
        }
    }
    // 用途：在每个步骤开始前处理暂停/取消/期限，暂停不承诺撤回已发业务；日期：2026-11-04。
    void Boundary(Run &run)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (run.pause && !run.cancel && !closing)
        {
            run.record["state"] = "paused";
            Save(run);
            changed.wait_until(lock, run.deadline, [&] { return !run.pause || run.cancel || closing; });
        }
        if (run.cancel || closing)
            throw Cancelled();
        if (run.storage_failed)
            throw Error("WORKFLOW_STORAGE_ERROR", "Dispatch blocked after checkpoint failure", true);
        if (std::chrono::steady_clock::now() >= run.deadline)
            throw Error("WORKFLOW_DURATION_EXCEEDED", "Workflow duration budget exhausted");
        run.record["state"] = "running";
    }
    // 用途：记录写前意图再允许派发，历史只包含不敏感标识与状态；日期：2026-11-04。
    size_t Begin(Run &run, const Json &step, const std::string &flow, size_t depth)
    {
        bool debug_examined = false;
        for (;;)
        {
            if (!debug_examined)
            {
                std::lock_guard<std::mutex> lock(mutex);
                run.record["nextStep"] = {{"flow", flow},
                                          {"stepId", step.at("id")},
                                          {"depth", depth},
                                          {"ordinal", run.record.at("steps").size()}};
                const bool breakpoint =
                    run.breakpoints.count(flow + "/" + step.at("id").get<std::string>()) != 0;
                const bool stepping = run.step_mode == "step" ||
                                      (run.step_mode == "stepOver" && depth <= run.step_depth) ||
                                      (run.step_mode == "stepOut" && depth < run.step_depth);
                if (breakpoint || stepping)
                {
                    run.pause = true;
                    run.record["pauseReason"] = breakpoint ? "breakpoint" : run.step_mode;
                    run.step_mode.clear();
                }
                else if (run.pause)
                    run.record["pauseReason"] = "requested";
                debug_examined = true;
            }
            Boundary(run);
            run.dispatch("$check", Json::object());
            std::unique_lock<std::mutex> lock(mutex);
            // 校验租约期间收到暂停时，重新进入边界等待，避免暂停返回后领取新步骤。
            if (run.pause && !run.cancel && !closing)
            {
                continue;
            }
            if (run.cancel || closing)
                throw Cancelled();
            auto &history = run.record["steps"];
            if (history.size() >= run.definition.at("policy").at("maxSteps").get<size_t>())
                throw Error("WORKFLOW_STEP_BUDGET_EXCEEDED", "Runtime step budget exhausted");
            const auto ordinal = history.size();
            history.push_back({{"ordinal", ordinal},
                               {"flow", flow},
                               {"stepId", step.at("id")},
                               {"type", step.at("type")},
                               {"depth", depth},
                               {"state", "executing"},
                               {"replayClass", ReplayClass(step.at("type"))}});
            Save(run);
            return ordinal;
        }
    }
    // 用途：业务步骤确认完成后保存状态，不记录DOM、填写值及原生引用；日期：2026-11-04。
    void Complete(Run &run, size_t ordinal)
    {
        std::lock_guard<std::mutex> lock(mutex);
        run.record["steps"][ordinal]["state"] =
            run.record["steps"][ordinal].value("handled", false) ? "handled-error" : "succeeded";
        Save(run);
    }
    // 用途：解释已校验图及有界文件/数组节点，输出带敏感标记且写入不重试；日期：2026-11-05。
    void Flow(Run &run, const std::string &name, size_t depth)
    {
        const auto flow = name == "main" ? Json{{"entry", run.definition.at("entry")},
                                                {"steps", run.definition.at("steps")}}
                                         : run.definition.at("subflows").at(name);
        std::map<std::string, Json> steps;
        for (const auto &step : flow.at("steps"))
            steps[step.at("id").get<std::string>()] = step;
        std::string current = flow.at("entry");
        while (true)
        {
            const auto &step = steps.at(current);
            const auto type = step.at("type").get<std::string>();
            const auto ordinal = Begin(run, step, name, depth);
            try
            {
                if (type == "condition")
                    current = step.at(Test(step.at("when"), run.input, run.values) ? "then" : "else");
                else if (type == "set")
                {
                    Assign(run, step.at("variable"), Evaluate(step.at("value"), run.input, run.values),
                           Tainted(run, step.at("value")));
                    current = step.at("next");
                }
                else if (type == "call" || type == "loop" || type == "foreach")
                {
                    const auto items =
                        type == "foreach" ? Evaluate(step.at("items"), run.input, run.values) : Json::array();
                    const auto count = type == "foreach" ? items.size()
                                       : type == "loop"
                                           ? Evaluate(step.at("count"), run.input, run.values).get<size_t>()
                                           : 1;
                    const auto item_name = type == "foreach" ? step.at("item").get<std::string>() : "";
                    const auto saved = type == "foreach" ? run.values.at(item_name) : Json();
                    const bool item_sensitive = type == "foreach" && Tainted(run, step.at("items"));
                    for (size_t iteration = 0; iteration < count; ++iteration)
                    {
                        if (type == "foreach")
                            Assign(run, item_name, items[iteration], item_sensitive);
                        Flow(run, step.at("flow"), depth + 1);
                    }
                    if (type == "foreach")
                        Assign(run, item_name, saved, item_sensitive);
                    current = step.at("next");
                }
                else if (type != "end")
                {
                    Json arguments;
                    if (type == "navigate")
                        arguments = {{"url", step.at("url")}};
                    else if (type == "downloadConfig")
                        arguments = {{"directory", Evaluate(step.at("directory"), run.input, run.values)}};
                    else if (type == "downloadWait")
                        arguments = {{"fileName", step.at("fileName")}, {"timeoutMs", step.at("timeoutMs")}};
                    else if (type == "assert")
                        arguments = {{"assertion", step.at("assertion")}};
                    else
                    {
                        arguments = {{"selector", step.at("selector")}};
                        if (type == "upload")
                            arguments["path"] = Evaluate(step.at("path"), run.input, run.values);
                        if (type == "readTexts")
                            arguments["maxItems"] = step.at("maxItems");
                        if (type == "fill")
                            arguments["value"] = step.at("value").contains("secretRef")
                                                     ? step.at("value")
                                                     : Evaluate(step.at("value"), run.input, run.values);
                        if (type == "select")
                        {
                            arguments["values"] = Json::array();
                            for (const auto &value : step.at("values"))
                                arguments["values"].push_back(Evaluate(value, run.input, run.values));
                        }
                        if (type == "assertText")
                        {
                            arguments["expected"] = Evaluate(step.at("expected"), run.input, run.values);
                            arguments["timeoutMs"] = step.at("timeoutMs");
                        }
                        if (type == "click")
                        {
                            arguments["postcondition"] = step.at("postcondition");
                            arguments["postcondition"]["expected"] =
                                Evaluate(step.at("postcondition").at("expected"), run.input, run.values);
                        }
                    }
                    arguments["$diagnosticContext"] = {{"runId", run.record.at("runId")},
                                                       {"flow", name},
                                                       {"stepId", current},
                                                       {"ordinal", ordinal}};
                    const auto result = run.dispatch(type, arguments);
                    if (type == "readText")
                        Assign(run, step.at("variable"), result.at("text"), true);
                    if (type == "readTexts")
                        Assign(run, step.at("variable"), result.at("texts"), true);
                    current = step.at("next");
                }
            }
            catch (const Error &error)
            {
                if (error.outcome_unknown || !step.contains("onError") ||
                    std::find(step.at("onError").at("codes").begin(), step.at("onError").at("codes").end(),
                              error.code) == step.at("onError").at("codes").end())
                    throw;
                current = step.at("onError").at("next");
                std::lock_guard<std::mutex> lock(mutex);
                run.record["steps"][ordinal]["handled"] = true;
                run.record["steps"][ordinal]["errorCode"] = error.code;
            }
            Complete(run, ordinal);
            if (type == "end")
                return;
        }
    }
    // 用途：运行任务并保存终态，任意失败均不自动重试网页动作；日期：2026-11-04。
    void Execute(const std::shared_ptr<Run> &run)
    {
        std::string state = "succeeded", code;
        bool unknown = false;
        try
        {
            Flow(*run, "main", 0);
        }
        catch (const Cancelled &)
        {
            state = "cancelled";
        }
        catch (const Error &error)
        {
            unknown = error.outcome_unknown;
            state = unknown ? "outcome-unknown" : "failed";
            code = error.code;
        }
        catch (...)
        {
            state = "outcome-unknown";
            unknown = true;
            code = "WORKFLOW_INTERNAL_ERROR";
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (run->storage_failed)
        {
            state = "outcome-unknown";
            unknown = true;
            code = "WORKFLOW_STORAGE_ERROR";
        }
        for (auto &step : run->record["steps"])
            if (step.at("state") == "executing")
                step["state"] = unknown ? "outcome-unknown" : state == "cancelled" ? "cancelled" : "failed";
        run->record["state"] = state;
        run->record["outcomeUnknown"] = unknown;
        run->record["errorCode"] = code;
        try
        {
            Save(*run);
        }
        catch (...)
        {
            run->record["state"] = "outcome-unknown";
            run->record["outcomeUnknown"] = true;
            run->record["errorCode"] = "WORKFLOW_STORAGE_ERROR";
        }
        run->record["checkpointDurable"] = !run->storage_failed;
        run->active = false;
        run->input = nullptr;
        run->values = Json::object();
        run->sensitive.clear();
        run->record.erase("nextStep");
        run->dispatch = {};
        changed.notify_all();
    }
};

// 用途：载入发布定义并冻结中断任务，输入/租约不持久化且从不自动重放；日期：2026-11-04。
WorkflowEngine::WorkflowEngine(const std::wstring &root) : impl_(std::make_unique<Impl>())
{
    const auto directory = SafeAbsolute(root + L"\\workflows");
    PrivateDirectory(directory);
    impl_->versions_path = directory + L"\\versions";
    impl_->runs_path = directory + L"\\runs";
    PrivateDirectory(impl_->versions_path);
    PrivateDirectory(impl_->runs_path);
    for (const auto &id : Records(impl_->versions_path))
    {
        const auto record = ReadRecord(impl_->versions_path + L"\\" + Wide(id) + L".json");
        Fields(record, "/", {"workflowId", "sha256", "definition"}, {"workflowId", "sha256", "definition"});
        const auto report = ValidateWorkflow(record.at("definition"));
        if (record.at("workflowId") != id || !report.at("valid").get<bool>() ||
            record.at("sha256") != report.at("sha256"))
            throw Error("WORKFLOW_STORAGE_ERROR", "Published workflow failed integrity verification");
        impl_->versions[id] = record;
    }
    for (const auto &id : Records(impl_->runs_path))
    {
        auto run = std::make_shared<Impl::Run>();
        run->record = ReadRecord(impl_->runs_path + L"\\" + Wide(id) + L".json");
        Fields(run->record, "/",
               {"runId", "workflowId", "profileId", "sha256", "state", "steps", "errorCode", "outcomeUnknown",
                "automaticRetryAllowed", "checkpointDurable", "recovered", "checkpointErrorCode",
                "checkpointSystemError", "nextStep", "pauseReason"},
               {"runId", "workflowId", "profileId", "sha256", "state", "steps", "outcomeUnknown", "errorCode",
                "automaticRetryAllowed", "checkpointDurable"});
        RecordId(run->record.at("profileId"));
        const auto workflow = run->record.at("workflowId").get<std::string>();
        if (run->record.at("runId") != id || !impl_->versions.count(workflow) ||
            run->record.at("sha256") != impl_->versions.at(workflow).at("sha256") ||
            !run->record.at("steps").is_array() || run->record.at("steps").size() > 1024)
            throw Error("WORKFLOW_STORAGE_ERROR", "Run metadata is not a valid owned checkpoint");
        const auto state = run->record.at("state").get<std::string>();
        if (!Terminal(state))
        {
            bool unknown = false;
            for (auto &step : run->record["steps"])
                if (step.at("state") == "executing")
                {
                    const bool write = step.at("replayClass") == "page-effect-no-replay";
                    unknown = unknown || write;
                    step["state"] = write ? "outcome-unknown" : "interrupted";
                }
            run->record["state"] = unknown ? "outcome-unknown" : "interrupted";
            run->record["outcomeUnknown"] = unknown;
            run->record["errorCode"] = "WORKFLOW_SERVICE_INTERRUPTED";
            run->record["recovered"] = true;
            impl_->Save(*run);
        }
        impl_->runs[id] = run;
    }
}
// 用途：请求所有任务停止后续派发，再在锁外回收线程以免析构死锁；日期：2026-11-04。
WorkflowEngine::~WorkflowEngine()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->closing = true;
        impl_->changed.notify_all();
    }
    for (const auto &item : impl_->runs)
        if (item.second->worker.joinable())
            item.second->worker.join();
}
// 用途：按内容摘要去重发布，版本文件永不覆盖，已发布运行不读取可变草稿；日期：2026-11-04。
Json WorkflowEngine::Publish(const Json &definition)
{
    const auto report = ValidateWorkflow(definition);
    if (!report.at("valid").get<bool>())
        throw Error("WORKFLOW_INVALID", "Validate and correct workflow before publishing");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto &item : impl_->versions)
        if (item.second.at("sha256") == report.at("sha256"))
            return {{"workflowId", item.first}, {"sha256", report.at("sha256")}, {"existing", true}};
    if (impl_->versions.size() >= MaximumRecords)
        throw Error("WORKFLOW_RECORD_BUDGET", "Published version capacity reached");
    const auto id = RandomId();
    const Json record{{"workflowId", id}, {"sha256", report.at("sha256")}, {"definition", definition}};
    WriteRecord(impl_->versions_path + L"\\" + Wide(id) + L".json", record);
    impl_->versions[id] = record;
    return {{"workflowId", id}, {"sha256", report.at("sha256")}, {"existing", false}};
}
// 用途：列出有界发布版本摘要，完整定义必须显式读取；日期：2026-11-04。
Json WorkflowEngine::List()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Json list = Json::array();
    for (const auto &item : impl_->versions)
        list.push_back({{"workflowId", item.first},
                        {"sha256", item.second.at("sha256")},
                        {"name", item.second.at("definition").at("name")},
                        {"version", item.second.at("definition").at("version")}});
    return {{"workflows", list}, {"maximumRecords", MaximumRecords}};
}
// 用途：取得当前进程校验过的不可变定义，未拥有的版本拒绝；日期：2026-11-04。
Json WorkflowEngine::Definition(const std::string &id)
{
    RecordId(id);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->versions.count(id))
        throw Error("WORKFLOW_NOT_FOUND", "Published version is not owned by this service");
    return impl_->versions.at(id);
}
// 用途：创建持久queued记录后启动工作线程，同Profile仅允许一个活动流程；日期：2026-11-04。
Json WorkflowEngine::Start(const std::string &id, const Json &input, const std::string &profile,
                           Dispatch dispatch, const Json &debug)
{
    const auto published = Definition(id);
    const auto report = ValidateWorkflow(published.at("definition"), &input);
    if (!report.at("valid").get<bool>())
        throw Error("WORKFLOW_INPUT_INVALID", "Input does not satisfy the published workflow contract");
    const auto points = DebugOptions(published.at("definition"), debug, true);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->closing || impl_->runs.size() >= MaximumRecords)
        throw Error("WORKFLOW_RECORD_BUDGET", "Run capacity reached or service is closing");
    for (const auto &item : impl_->runs)
        if (item.second->active && item.second->record.at("profileId") == profile)
            throw Error("WORKFLOW_PROFILE_BUSY", "Another workflow owns this profile");
    size_t active = 0;
    for (const auto &item : impl_->runs)
        active += item.second->active ? 1 : 0;
    if (active >= 5)
        throw Error("WORKFLOW_CONCURRENCY_LIMIT", "Five local workflow workers are already active");
    auto run = std::make_shared<Impl::Run>();
    const auto run_id = RandomId();
    run->definition = published.at("definition");
    run->input = input;
    run->breakpoints = points;
    run->pause = debug.value("pauseOnStart", false);
    if (run->definition.contains("variables"))
        for (auto it = run->definition.at("variables").begin(); it != run->definition.at("variables").end();
             ++it)
        {
            run->values[it.key()] = Evaluate(it.value().at("initial"), input);
            if (it.value().at("sensitive").get<bool>() || impl_->Tainted(*run, it.value().at("initial")))
                run->sensitive.insert(it.key());
        }
    run->dispatch = std::move(dispatch);
    run->active = true;
    run->deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(run->definition.at("policy").at("maxDurationMs").get<int>());
    run->record = {{"runId", run_id},         {"workflowId", id},
                   {"profileId", profile},    {"sha256", published.at("sha256")},
                   {"state", "queued"},       {"steps", Json::array()},
                   {"outcomeUnknown", false}, {"automaticRetryAllowed", false},
                   {"errorCode", ""},         {"checkpointDurable", true}};
    impl_->Save(*run);
    impl_->runs[run_id] = run;
    try
    {
        // 用途：后台执行固定发布版本，不依赖客户端连接存活；日期：2026-11-04。
        run->worker = std::thread([this, run] { impl_->Execute(run); });
    }
    catch (...)
    {
        run->active = false;
        run->record["state"] = "failed";
        run->record["errorCode"] = "WORKFLOW_THREAD_FAILED";
        impl_->Save(*run);
        throw Error("WORKFLOW_THREAD_FAILED", "Cannot start workflow worker");
    }
    return run->record;
}
// 用途：读取实际步骤状态，存储故障会明确标记非持久状态；日期：2026-11-04。
Json WorkflowEngine::Status(const std::string &id)
{
    RecordId(id);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->runs.count(id))
        throw Error("WORKFLOW_RUN_NOT_FOUND", "Run is not owned by this service");
    return impl_->runs.at(id)->record;
}
// 用途：返回最多256个持久运行摘要，断线后通过固定版本和环境找回任务；日期：2026-11-04。
Json WorkflowEngine::Runs()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Json list = Json::array();
    for (const auto &item : impl_->runs)
    {
        auto summary = item.second->record;
        summary["recordedSteps"] = summary.at("steps").size();
        summary.erase("steps");
        list.push_back(summary);
    }
    return {{"runs", list}, {"maximumRecords", MaximumRecords}};
}
// 用途：暂停只阻止下一步骤，取消不回滚业务，未知/中断/终态无法恢复；日期：2026-11-04。
Json WorkflowEngine::Control(const std::string &id, const std::string &operation)
{
    RecordId(id);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->runs.count(id))
        throw Error("WORKFLOW_RUN_NOT_FOUND", "Run is not owned by this service");
    auto &run = *impl_->runs.at(id);
    const auto state = run.record.at("state").get<std::string>();
    if (!run.active || Terminal(state) || run.storage_failed)
        throw Error("WORKFLOW_CONTROL_INVALID", "Terminal, interrupted or uncertain runs cannot resume");
    if (operation == "pause")
    {
        if (run.cancel)
            throw Error("WORKFLOW_CONTROL_INVALID", "Cancellation is already pending");
        run.pause = true;
        if (state != "paused")
            run.record["state"] = "pausing";
    }
    else if (operation == "resume")
    {
        if (run.cancel || state != "paused")
            throw Error("WORKFLOW_CONTROL_INVALID", "Only a fully paused run can resume");
        run.pause = false;
        run.step_mode.clear();
        run.record["state"] = "running";
    }
    else if (operation == "step" || operation == "stepOver" || operation == "stepOut")
    {
        if (run.cancel || state != "paused" || !run.record.contains("nextStep"))
            throw Error("WORKFLOW_CONTROL_INVALID", "Only a paused step boundary can be stepped");
        run.step_mode = operation;
        run.step_depth = run.record.at("nextStep").at("depth").get<size_t>();
        run.pause = false;
        run.record["state"] = "running";
    }
    else if (operation == "cancel")
    {
        run.cancel = true;
        run.record["state"] = "cancelling";
    }
    else
        throw Error("WORKFLOW_CONTROL_INVALID", "Unsupported workflow control operation");
    impl_->Save(run);
    impl_->changed.notify_all();
    return run.record;
}
// 用途：调试配置与变量快照共用引擎锁，仅暂停边界允许读取且敏感性单调传播；日期：2026-11-05。
Json WorkflowEngine::Debug(const std::string &id, const Json &options)
{
    RecordId(id);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->runs.count(id))
        throw Error("WORKFLOW_RUN_NOT_FOUND", "Run is not owned by this service");
    auto &run = *impl_->runs.at(id);
    if (!run.active || run.storage_failed)
        throw Error("WORKFLOW_CONTROL_INVALID", "Debugging requires an active non-uncertain run");
    const auto points = DebugOptions(run.definition, options, false);
    if (options.contains("breakpoints"))
        run.breakpoints = points;
    Json variables = Json::object(), breakpoints = Json::array();
    const bool paused = run.record.at("state") == "paused";
    if (paused)
        for (auto it = run.values.begin(); it != run.values.end(); ++it)
        {
            const bool sensitive = run.sensitive.count(it.key()) != 0;
            variables[it.key()] = {
                {"type", run.definition.at("variables").at(it.key()).at("schema").at("type")},
                {"sensitive", sensitive},
                {"value", sensitive ? Json("[REDACTED]") : it.value()}};
        }
    for (const auto &point : run.breakpoints)
    {
        const auto separator = point.find('/');
        breakpoints.push_back(
            {{"flow", point.substr(0, separator)}, {"stepId", point.substr(separator + 1)}});
    }
    return {{"runId", id},
            {"state", run.record.at("state")},
            {"variablesAvailable", paused},
            {"variables", variables},
            {"breakpoints", breakpoints},
            {"nextStep", run.record.value("nextStep", Json())}};
}
// 用途：在运行期排除非所属线程的页面入口，控制撤销与停止由服务单独允许；日期：2026-11-04。
bool WorkflowEngine::OwnsProfile(const std::string &id)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto &item : impl_->runs)
        if (item.second->active && item.second->record.at("profileId") == id &&
            item.second->worker.get_id() != std::this_thread::get_id())
            return true;
    return false;
}
} // namespace awb
