#include "awb/browser.hpp"

namespace awb
{
// 用途：在隔离世界读取可见功能元数据，有界生成候选并核对文档未变；日期：2026-11-05。
Json Browser::Discover(const std::string &query, const std::string &role, size_t limit)
{
    if (UnicodeLength(query) > 128 || UnicodeLength(role) > 64 || limit < 1 || limit > 20)
        throw Error("INVALID_ARGUMENTS", "Discovery arguments exceed their bounds");
    const auto document = Document();
    const auto world = Call("Page.createIsolatedWorld",
                            {{"frameId", document.at("id")}, {"worldName", "awb-readonly-discovery-v1"}});
    const auto script = std::string(R"JS((function(query, desiredRole, limit) {
// 用途：折叠可见标签空白且限制长度，不读取表单值；日期：2026-11-05。
const label = function(value) { return String(value || '').replace(/\s+/g, ' ').trim().slice(0,256); };
// 用途：仅根据原生标签和显式role给出角色近似，不冒充完整可访问名称算法；日期：2026-11-05。
const roleOf = function(node) {
 const explicit=node.getAttribute('role'); if(explicit) return explicit.split(' ')[0];
 const tag=node.tagName.toLowerCase();
 if(tag==='button') return 'button'; if(tag==='a' && node.hasAttribute('href')) return 'link';
 if(tag==='textarea' || node.isContentEditable) return 'textbox';
 if(tag==='select') return node.multiple ? 'listbox' : 'combobox';
 if(tag==='input') { const type=node.type; if(type==='checkbox'||type==='radio')return type;
  if(type==='submit'||type==='button'||type==='reset')return 'button'; return type==='search'?'searchbox':'textbox'; }
 return 'generic';
};
// 用途：用ID/testId或受限结构路径产生当前唯一CSS，绝不把标签文本插入代码；日期：2026-11-05。
const selectorOf = function(node) {
 let selector='';
 if(node.id) {selector='#'+CSS.escape(node.id); if(document.querySelectorAll(selector).length===1)return selector;}
 const test=node.getAttribute('data-testid');
 if(test) {selector='[data-testid="'+CSS.escape(test)+'"]';if(document.querySelectorAll(selector).length===1)return selector;}
 const segments=[]; let current=node;
 for(let depth=0;current && current.nodeType===1 && depth<16;depth++,current=current.parentElement) {
  const tag=current.tagName.toLowerCase(); let index=1;
  for(let sibling=current.previousElementSibling;sibling;sibling=sibling.previousElementSibling)if(sibling.tagName===current.tagName)index++;
  segments.unshift(tag+':nth-of-type('+index+')'); selector=segments.join(' > ');
  if(selector.length>2048)return '';
  const matches=document.querySelectorAll(selector);if(matches.length===1 && matches[0]===node)return selector;
 }
 return '';
};
const candidates=[], nodes=document.querySelectorAll('button,a[href],input:not([type=hidden]),textarea,select,[role],[contenteditable=true],[tabindex]');
const normalized=label(query).toLowerCase(); const inspected=Math.min(nodes.length,2000);
for(let index=0;index<inspected;index++) {
 const node=nodes[index], style=getComputedStyle(node), rect=node.getBoundingClientRect();
 if(style.display==='none'||style.visibility==='hidden'||rect.width<=0||rect.height<=0||node.closest('[hidden],[inert],[aria-hidden=true]'))continue;
 const role=roleOf(node); if(desiredRole && role!==desiredRole)continue;
 let name=node.getAttribute('aria-label');
 if(!name && node.hasAttribute('aria-labelledby')) {
  name=''; for(const id of node.getAttribute('aria-labelledby').split(/\s+/).slice(0,8)){const target=document.getElementById(id);if(target)name+=' '+target.textContent;}
 }
 if(!name && node.labels) {name='';for(const target of Array.from(node.labels).slice(0,8))name+=' '+target.textContent;}
 if(!name && node.tagName!=='INPUT' && node.tagName!=='TEXTAREA' && node.tagName!=='SELECT')name=node.textContent;
 if(!name)name=node.getAttribute('title')||node.getAttribute('placeholder');
 name=label(name);const lower=name.toLowerCase();
 if(normalized && !lower.includes(normalized))continue;
 const selector=selectorOf(node);if(!selector)continue;
 candidates.push({selector,role,name,enabled:!node.disabled&&node.getAttribute('aria-disabled')!=='true',
  unique:true,score:(normalized && lower===normalized?100:normalized?60:0)+(desiredRole?20:0)});
}
// 用途：稳定按观察得分排序，多个候选始终显式返回供审阅；日期：2026-11-05。
candidates.sort(function(a,b){return b.score-a.score;});
return {candidates:candidates.slice(0,limit),matched:candidates.length,scanned:inspected,
 truncated:nodes.length>inspected||candidates.length>limit,executed:false,
 nameAlgorithm:'bounded-label-heuristic',scope:'visible-main-document'};
}))JS") + "(" + Json(query).dump() +
                        "," + Json(role).dump() + "," + std::to_string(limit) + ")";
    const auto result = Call(
        "Runtime.evaluate",
        {{"expression", script}, {"contextId", world.at("executionContextId")}, {"returnByValue", true}});
    if (result.contains("exceptionDetails") || !result.at("result").contains("value"))
        throw Error("DISCOVERY_FAILED", "Cannot inspect document candidates");
    const auto after = Document();
    if (after.at("id") != document.at("id") || after.value("loaderId", "") != document.value("loaderId", ""))
        throw Error("STALE_ELEMENT_REFERENCE", "Document changed during discovery");
    auto report = result.at("result").at("value");
    report["pageId"] = active_page_;
    report["loaderId"] = document.value("loaderId", "");
    return report;
}
} // namespace awb
