#include "awb/browser.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace awb
{
// 用途：严格核对对象字段，未实现的Schema或动作字段不能静默忽略；日期：2026-11-04。
static void Keys(const Json &object, const std::set<std::string> &allowed,
                 const std::set<std::string> &required)
{
    if (!object.is_object())
        throw Error("INVALID_ARGUMENTS", "Expected a bounded definition object");
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.count(it.key()))
            throw Error("INVALID_ARGUMENTS", "Unsupported definition field");
    for (const auto &key : required)
        if (!object.contains(key))
            throw Error("INVALID_ARGUMENTS", "Required definition field is missing");
}
// 用途：按Unicode码点限制字符串，拒绝空选择器及嵌入零字符；日期：2026-11-04。
static void BoundedString(const Json &value, size_t minimum, size_t maximum)
{
    if (!value.is_string() || value.get_ref<const std::string &>().find('\0') != std::string::npos ||
        UnicodeLength(value.get_ref<const std::string &>()) < minimum ||
        UnicodeLength(value.get_ref<const std::string &>()) > maximum)
        throw Error("INVALID_ARGUMENTS", "Definition string is outside its bounds");
}
// 用途：只接受声明的标量类型，布尔值不能冒充整数；日期：2026-11-04。
static bool ScalarType(const Json &value, const std::string &type)
{
    return (type == "string" && value.is_string()) || (type == "boolean" && value.is_boolean()) ||
           (type == "integer" && value.is_number_integer()) || (type == "number" && value.is_number());
}
// 用途：声明标量Schema校验器，供数组Schema复用；日期：2026-11-05。
static void ScalarSchema(const Json &schema);
// 用途：校验数组提取字段的受限元素类型和数量边界；日期：2026-11-05。
static void ArraySchema(const Json &schema)
{
    Keys(schema, {"type", "items", "minItems", "maxItems"}, {"type", "items"});
    if (schema.at("type") != "array" || !schema.at("items").is_object() ||
        schema.at("items").value("type", "") != "string")
        throw Error("INVALID_ARGUMENTS", "Only string arrays are supported by extraction");
    ScalarSchema(schema.at("items"));
    const Json minimumValue = schema.value("minItems", Json(0));
    const Json maximumValue = schema.value("maxItems", Json(100));
    if (!minimumValue.is_number_integer() || !maximumValue.is_number_integer() || minimumValue < 0 ||
        minimumValue > 100 || maximumValue < 0 || maximumValue > 100)
        throw Error("INVALID_ARGUMENTS", "Array item bounds must be ordered integers in 0..100");
    const auto minimum = minimumValue.get<int>();
    const auto maximum = maximumValue.get<int>();
    if (minimum < 0 || maximum > 100 || minimum > maximum)
        throw Error("INVALID_ARGUMENTS", "Array item bounds must be ordered integers in 0..100");
}
// 用途：限定标量Schema方言并检查互相矛盾的边界，不加载引用或执行正则；日期：2026-11-04。
static void ScalarSchema(const Json &schema)
{
    Keys(schema, {"type", "enum", "minLength", "maxLength", "minimum", "maximum"}, {"type"});
    if (!schema.at("type").is_string())
        throw Error("INVALID_ARGUMENTS", "Scalar type must be a string");
    const auto type = schema.at("type").get<std::string>();
    if (type != "string" && type != "number" && type != "integer" && type != "boolean")
        throw Error("INVALID_ARGUMENTS", "Extraction supports only declared scalar types");
    for (const auto &key : {"minLength", "maxLength"})
        if (schema.contains(key) && (type != "string" || !schema.at(key).is_number_integer() ||
                                     schema.at(key) < 0 || schema.at(key) > 4096))
            throw Error("INVALID_ARGUMENTS", "String bounds must be integers in 0..4096");
    if (schema.value("minLength", 0) > schema.value("maxLength", 4096))
        throw Error("INVALID_ARGUMENTS", "String bounds are contradictory");
    for (const auto &key : {"minimum", "maximum"})
        if (schema.contains(key) && ((type != "number" && type != "integer") || !schema.at(key).is_number() ||
                                     !std::isfinite(schema.at(key).get<double>())))
            throw Error("INVALID_ARGUMENTS", "Numeric bounds require finite numbers");
    if (schema.contains("minimum") && schema.contains("maximum") &&
        schema.at("minimum") > schema.at("maximum"))
        throw Error("INVALID_ARGUMENTS", "Numeric bounds are contradictory");
    if (schema.contains("enum"))
    {
        const auto &choices = schema.at("enum");
        if (!choices.is_array() || choices.empty() || choices.size() > 32)
            throw Error("INVALID_ARGUMENTS", "Enum must contain 1..32 scalar choices");
        Json seen = Json::array();
        for (const auto &choice : choices)
        {
            if (!ScalarType(choice, type) || std::find(seen.begin(), seen.end(), choice) != seen.end())
                throw Error("INVALID_ARGUMENTS", "Enum choices must match the declared type and be unique");
            seen.push_back(choice);
            if (type == "string")
                BoundedString(choice, 0, 4096);
        }
    }
}
// 用途：校验提取Schema、字段绑定及资源预算，敏感控件值不提供读取源；日期：2026-11-04。
static void ExtractionDefinition(const Json &schema, const Json &bindings)
{
    Keys(schema, {"$schema", "type", "properties", "required", "additionalProperties"},
         {"type", "properties", "required", "additionalProperties"});
    if (schema.at("type") != "object" || schema.at("additionalProperties") != false ||
        (schema.contains("$schema") && schema.at("$schema") != "http://json-schema.org/draft-07/schema#") ||
        !schema.at("properties").is_object() || schema.at("properties").empty() ||
        schema.at("properties").size() > 16 || !schema.at("required").is_array() || !bindings.is_object() ||
        bindings.size() != schema.at("properties").size())
        throw Error("INVALID_ARGUMENTS", "Invalid bounded extraction object schema or bindings");
    std::set<std::string> required;
    for (const auto &name : schema.at("required"))
        if (!name.is_string() || !schema.at("properties").contains(name.get<std::string>()) ||
            !required.insert(name.get<std::string>()).second)
            throw Error("INVALID_ARGUMENTS", "Required fields must be unique declared names");
    for (auto it = schema.at("properties").begin(); it != schema.at("properties").end(); ++it)
    {
        const auto &name = it.key();
        if (name.empty() || name.size() > 64 || name == "__proto__" || name == "constructor" ||
            name == "prototype" ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
                std::string::npos ||
            !bindings.contains(name))
            throw Error("INVALID_ARGUMENTS", "Extraction field name or binding is invalid");
        if (it.value().value("type", "") == "array")
            ArraySchema(it.value());
        else
            ScalarSchema(it.value());
        const auto &binding = bindings.at(name);
        Keys(binding, {"selector", "source", "attribute"}, {"selector", "source"});
        BoundedString(binding.at("selector"), 1, 2048);
        if (!binding.at("source").is_string())
            throw Error("INVALID_ARGUMENTS", "Extraction source must be a string");
        const auto source = binding.at("source").get<std::string>();
        if (source != "text" && source != "texts" && source != "attribute" && source != "count" &&
            source != "checked")
            throw Error("INVALID_ARGUMENTS", "Extraction source is unsupported");
        if (source == "attribute")
        {
            if (!binding.contains("attribute") ||
                (binding.at("attribute") != "title" && binding.at("attribute") != "aria-label" &&
                 binding.at("attribute") != "data-testid"))
                throw Error("INVALID_ARGUMENTS",
                            "Only title, aria-label and data-testid attributes are readable");
        }
        else if (binding.contains("attribute"))
            throw Error("INVALID_ARGUMENTS", "Attribute name requires an attribute source");
        if ((source == "texts" && it.value().at("type") != "array") ||
            (source == "count" && it.value().at("type") != "integer") ||
            (source == "checked" && it.value().at("type") != "boolean") ||
            (source == "attribute" && it.value().at("type") != "string") ||
            (source != "texts" && it.value().at("type") == "array"))
            throw Error("INVALID_ARGUMENTS", "Extraction source and declared type are incompatible");
    }
}
// 用途：只读提取主文档公开标量及有界字符串数组，转换失败不返回原始值；日期：2026-11-05。
Json Browser::Extract(const Json &schema, const Json &bindings)
{
    ExtractionDefinition(schema, bindings);
    const auto document = Document();
    const auto world = Call("Page.createIsolatedWorld",
                            {{"frameId", document.at("id")}, {"worldName", "awb-public-scalar-reader-v1"}});
    const std::string script = R"JS((function(bindings){
const result=Object.create(null);
for(const name of Object.keys(bindings)){
 const binding=bindings[name]; let nodes;
 try { nodes=document.querySelectorAll(binding.selector); }
 catch(_error){result[name]={error:'INVALID_SELECTOR'};continue;}
 if(binding.source==='count'){result[name]={value:nodes.length};continue;}
 if(binding.source==='texts'){
  if(nodes.length>100){result[name]={error:'EXTRACTION_VALUE_TOO_LARGE'};continue;}
  const values=[]; for(const node of nodes){
   if(node.matches('input,textarea,select')||node.closest('[contenteditable]:not([contenteditable="false"])')||
      node.querySelector('input,textarea,select,[contenteditable]')){
    result[name]={error:'EXTRACTION_SOURCE_DENIED'}; values.length=0; break;
   }
   const value=(node.innerText||node.textContent||'');
   if(value.length>8192||Array.from(value).length>4096){result[name]={error:'EXTRACTION_VALUE_TOO_LARGE'}; values.length=0; break;}
   values.push(value);
  }
  if(!result[name]) result[name]={value:values}; continue;
 }
 if(nodes.length!==1){result[name]={error:nodes.length?'LOCATOR_AMBIGUOUS':'ELEMENT_NOT_FOUND'};continue;}
 const node=nodes[0];
 if(binding.source==='checked'){
  result[name]=node instanceof HTMLInputElement&&['checkbox','radio'].includes(node.type)?
   {value:node.checked}:{error:'EXTRACTION_SOURCE_DENIED'};continue;
 }
 if(node.matches('input,textarea,select')||node.closest('[contenteditable]:not([contenteditable="false"])')||
    (binding.source==='text'&&node.querySelector('input,textarea,select,[contenteditable]'))){
  result[name]={error:'EXTRACTION_SOURCE_DENIED'};continue;
 }
 const value=binding.source==='attribute'?node.getAttribute(binding.attribute):node.innerText;
 if(typeof value!=='string'){result[name]={error:'EXTRACTION_VALUE_MISSING'};continue;}
 if(value.length>8192||Array.from(value).length>4096){result[name]={error:'EXTRACTION_VALUE_TOO_LARGE'};continue;}
 result[name]={value:value};
}
return result;
})JS";
    const auto response = Call("Runtime.evaluate", {{"expression", script + ")(" + bindings.dump() + ")"},
                                                    {"contextId", world.at("executionContextId")},
                                                    {"returnByValue", true}});
    if (response.contains("exceptionDetails") || !response.at("result").contains("value"))
        throw Error("EXTRACTION_FAILED", "Fixed extraction reader did not return structured values");
    const auto after = Document();
    if (after.value("loaderId", "") != document.value("loaderId", "") || after.at("id") != document.at("id"))
        throw Error("EXTRACTION_DOCUMENT_CHANGED", "Document changed during extraction; no data returned");
    const auto raw = response.at("result").at("value");
    Json data = Json::object(), errors = Json::array(), missing = Json::array();
    bool valid = true;
    for (auto it = schema.at("properties").begin(); it != schema.at("properties").end(); ++it)
    {
        const auto &field = it.key();
        const bool required = std::find(schema.at("required").begin(), schema.at("required").end(), field) !=
                              schema.at("required").end();
        const auto observed = raw.value(field, Json{{"error", "EXTRACTION_VALUE_MISSING"}});
        auto error = observed.value("error", "");
        Json value;
        const auto type = it.value().at("type").get<std::string>();
        if (error.empty())
        {
            value = observed.at("value");
            if (type == "array")
            {
                if (!value.is_array())
                    error = "EXTRACTION_TYPE_MISMATCH";
                else
                {
                    const auto minimum = it.value().value("minItems", Json(0)).get<size_t>();
                    const auto maximum = it.value().value("maxItems", Json(100)).get<size_t>();
                    if (value.size() < minimum || value.size() > maximum)
                        error = "EXTRACTION_CONSTRAINT_FAILED";
                    const auto &itemSchema = it.value().at("items");
                    for (const auto &item : value)
                    {
                        if (!item.is_string() || UnicodeLength(item.get<std::string>()) > 4096)
                            error = "EXTRACTION_TYPE_MISMATCH";
                        else if (UnicodeLength(item.get<std::string>()) <
                                     itemSchema.value("minLength", size_t{0}) ||
                                 UnicodeLength(item.get<std::string>()) >
                                     itemSchema.value("maxLength", size_t{4096}) ||
                                 (itemSchema.contains("enum") &&
                                  std::find(itemSchema.at("enum").begin(), itemSchema.at("enum").end(),
                                            item) == itemSchema.at("enum").end()))
                            error = "EXTRACTION_CONSTRAINT_FAILED";
                    }
                }
            }
            else if (type != "string" && value.is_string())
            {
                value = Json::parse(value.get<std::string>(), nullptr, false);
                if (value.is_discarded())
                    error = "EXTRACTION_TYPE_MISMATCH";
            }
            if (error.empty() && type != "array" && !ScalarType(value, type))
                error = "EXTRACTION_TYPE_MISMATCH";
            if (error.empty() && type == "string" &&
                (UnicodeLength(value.get<std::string>()) < it.value().value("minLength", size_t{0}) ||
                 UnicodeLength(value.get<std::string>()) > it.value().value("maxLength", size_t{4096})))
                error = "EXTRACTION_CONSTRAINT_FAILED";
            if (error.empty() && value.is_number() &&
                ((it.value().contains("minimum") && value < it.value().at("minimum")) ||
                 (it.value().contains("maximum") && value > it.value().at("maximum"))))
                error = "EXTRACTION_CONSTRAINT_FAILED";
            if (error.empty() && it.value().contains("enum") &&
                std::find(it.value().at("enum").begin(), it.value().at("enum").end(), value) ==
                    it.value().at("enum").end())
                error = "EXTRACTION_CONSTRAINT_FAILED";
        }
        if (error.empty())
            data[field] = value;
        else
        {
            const bool absent = error == "ELEMENT_NOT_FOUND" || error == "EXTRACTION_VALUE_MISSING";
            if (absent)
                missing.push_back(field);
            errors.push_back({{"field", field}, {"code", error}, {"required", required}});
            if (required || !absent)
                valid = false;
        }
    }
    return {{"valid", valid},
            {"data", data},
            {"errors", errors},
            {"missing", missing},
            {"partial", !errors.empty()},
            {"frameScope", "main-frame-only"},
            {"loaderId", document.value("loaderId", "")},
            {"dialect", "awb-scalar-extraction-v1-array"},
            {"inputValuesReadable", false},
            {"readerWorld", "isolated"},
            {"atomicSnapshot", false},
            {"liveDom", true}};
}
// 用途：验证一条动作的全部参数，点击必须有明确业务后置条件；日期：2026-11-04。
static void BatchAction(const Json &action)
{
    if (!action.is_object() || !action.contains("type") || !action.at("type").is_string())
        throw Error("INVALID_ARGUMENTS", "Batch action requires a type");
    const auto type = action.at("type").get<std::string>();
    std::set<std::string> keys{"type", "selector"};
    if (type == "navigate")
        keys = {"type", "url"};
    else if (type == "fill")
        keys.insert("value");
    else if (type == "select")
        keys.insert("values");
    else if (type == "click")
        keys.insert("postcondition");
    else if (type == "assertText")
    {
        keys.insert("expected");
        keys.insert("timeoutMs");
    }
    else
        throw Error("INVALID_ARGUMENTS", "Batch action type is unsupported");
    Keys(action, keys, keys);
    if (type == "navigate")
    {
        BoundedString(action.at("url"), 1, 8192);
        return;
    }
    BoundedString(action.at("selector"), 1, 2048);
    if (type == "fill")
        BoundedString(action.at("value"), 0, 65536);
    if (type == "select")
    {
        if (!action.at("values").is_array() || action.at("values").size() > 32)
            throw Error("INVALID_ARGUMENTS", "Batch selection requires at most 32 values");
        std::set<std::string> unique;
        for (const auto &value : action.at("values"))
        {
            BoundedString(value, 0, 4096);
            if (!unique.insert(value.get<std::string>()).second)
                throw Error("INVALID_ARGUMENTS", "Batch selection values must be unique");
        }
    }
    if (type == "click" || type == "assertText")
    {
        const auto &assertion = type == "click" ? action.at("postcondition") : action;
        if (type == "click")
            Keys(assertion, {"selector", "expected", "timeoutMs"}, {"selector", "expected", "timeoutMs"});
        BoundedString(assertion.at("selector"), 1, 2048);
        BoundedString(assertion.at("expected"), 0, 16384);
        if (!assertion.at("timeoutMs").is_number_integer() || assertion.at("timeoutMs") < 1 ||
            assertion.at("timeoutMs") > 3000)
            throw Error("INVALID_ARGUMENTS", "Batch assertion timeout must be in 1..3000 ms");
    }
}
// 用途：先校验整批结构与导航来源，再持有同一Profile排他权执行有界序列；日期：2026-11-04。
Json Browser::Batch(const Json &actions, bool stop_on_error)
{
    if (!actions.is_array() || actions.empty() || actions.size() > 16)
        throw Error("INVALID_ARGUMENTS", "Batch must contain 1..16 actions");
    for (const auto &action : actions)
    {
        BatchAction(action);
        if (action.at("type") == "navigate")
            CheckOrigin(action.at("url"));
    }
    const auto batch_id = RandomId();
    const auto started = GetTickCount64();
    Json items = Json::array();
    bool stopped = false, unknown = false;
    size_t succeeded = 0, failed = 0;
    for (size_t index = 0; index < actions.size(); ++index)
    {
        const auto &action = actions.at(index);
        Json item{{"index", index}, {"type", action.at("type")}, {"state", "not-run"}};
        if (!stopped)
        {
            bool clicked = false, mutated = false;
            try
            {
                if (GetTickCount64() - started >= 30000)
                    throw Error("BATCH_TIME_BUDGET", "Batch exceeded its step-boundary time budget");
                Diagnose({{"batchId", batch_id}, {"ordinal", index + 1}, {"command", "page_batch"}},
                         [&]() -> Json
                         {
                             const auto type = action.at("type").get<std::string>();
                             if (type == "navigate")
                             {
                                 const auto result = Navigate(action.at("url"));
                                 mutated = true;
                                 return result;
                             }
                             if (type == "assertText")
                                 return WaitText(action.at("selector"), action.at("expected"),
                                                 action.at("timeoutMs"));
                             const auto ref = Locate(action.at("selector")).at("ref").get<std::string>();
                             if (type == "fill")
                             {
                                 const auto result = Fill(ref, action.at("value"));
                                 mutated = true;
                                 return result;
                             }
                             if (type == "select")
                             {
                                 const auto result =
                                     Select(ref, action.at("values").get<std::vector<std::string>>());
                                 mutated = true;
                                 return result;
                             }
                             Click(ref);
                             clicked = true;
                             const auto &assertion = action.at("postcondition");
                             return WaitText(assertion.at("selector"), assertion.at("expected"),
                                             assertion.at("timeoutMs"));
                         });
                item["state"] = "succeeded";
                ++succeeded;
            }
            catch (const Error &error)
            {
                const bool uncertain = clicked || mutated || error.outcome_unknown;
                const std::set<std::string> continuable{
                    "ELEMENT_NOT_FOUND",      "LOCATOR_AMBIGUOUS",    "INVALID_SELECTOR",
                    "ELEMENT_NOT_ACTIONABLE", "ELEMENT_NOT_EDITABLE", "OPTION_NOT_FOUND",
                    "OPTION_AMBIGUOUS",       "OPTION_DISABLED",      "ASSERTION_TIMEOUT"};
                stopped = stop_on_error || uncertain || !continuable.count(error.code);
                unknown = unknown || uncertain;
                item["state"] = uncertain ? "outcome-unknown" : "failed";
                item["error"] = {{"code", clicked ? "BATCH_POSTCONDITION_UNCONFIRMED" : error.code},
                                 {"outcomeUnknown", uncertain},
                                 {"automaticRetryAllowed", false}};
                ++failed;
            }
            catch (const std::exception &)
            {
                stopped = true;
                unknown = true;
                item["state"] = "outcome-unknown";
                item["error"] = {{"code", "BATCH_INTERNAL_ERROR"},
                                 {"outcomeUnknown", true},
                                 {"automaticRetryAllowed", false}};
                ++failed;
            }
        }
        items.push_back(item);
    }
    return {{"batchId", batch_id},
            {"state", unknown  ? "outcome-unknown"
                      : failed ? "partial"
                               : "succeeded"},
            {"items", items},
            {"succeeded", succeeded},
            {"failed", failed},
            {"notRun", actions.size() - succeeded - failed},
            {"stopped", stopped},
            {"outcomeUnknown", unknown},
            {"automaticRetryAllowed", false},
            {"atomic", false},
            {"rollbackPerformed", false},
            {"elapsedMs", GetTickCount64() - started},
            {"timeBudgetMs", 30000},
            {"budgetBoundary", "between-actions"}};
}
} // namespace awb
