// ============================================================================
//  qq_deepseek_bot.cpp
//  QQ + DeepSeek 自动智能回复机器人（OneBot v11 正向 WebSocket 插件）
//
//  功能：
//    - 通过 OneBot v11 正向 WebSocket 接入 QQ（适配 NapCat / Lagrange / go-cqhttp 等）
//    - 收到私聊/群聊消息后调用 DeepSeek API 生成智能回复并自动发回
//    - 提供明确的「自动回复开关」：开启 / 关闭 / 状态 / 帮助，状态实时持久化
//
//  依赖：
//    - Windows 内置 WinHTTP（ws:// 正向连接 + 调用 DeepSeek 的 https 接口）
//    - nlohmann/json（已随项目附带，third_party/json.hpp）
//
//  编译：运行 build.bat（使用 MSVC cl），详见 README.md
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S

#include <windows.h>
#include <winhttp.h>
#include <stdlib.h>          // rand_s

#include <string>
#include <vector>
#include <map>
#include <deque>
#include <sstream>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <cctype>
#include <ctime>

#include "json.hpp"
using json = nlohmann::json;

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

// ============================================================================
// 基础工具：编码转换 / 字符串处理 / 日志
// ============================================================================

static std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string wide_to_utf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

// 去掉结尾常见中英文标点，便于识别命令（例如 "开启自动回复！"）
static std::string strip_trailing_punct(const std::string& s)
{
    std::string r = trim(s);
    static const std::vector<std::string> puncts = {
        "。", "！", "？", "，", "；", "、", "…",
        "!", "?", ".", ",", ";", " ", "\t"
    };
    bool changed = true;
    while (changed && !r.empty())
    {
        changed = false;
        for (auto& p : puncts)
        {
            if (r.size() >= p.size() && r.compare(r.size() - p.size(), p.size(), p) == 0)
            {
                r = r.substr(0, r.size() - p.size());
                changed = true;
                break;
            }
        }
    }
    return r;
}

// 去掉 raw_message 中的 [CQ:...] 段（@、图片、表情等）
static std::string strip_cq_codes(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size())
    {
        if (s[i] == '[' && i + 3 < s.size() && s[i + 1] == 'C' && s[i + 2] == 'Q' && s[i + 3] == ':')
        {
            size_t j = s.find(']', i);
            if (j == std::string::npos) { out += s.substr(i); break; }
            i = j + 1;
        }
        else
        {
            out += s[i];
            i++;
        }
    }
    return out;
}

static std::string base64_encode(const std::string& in)
{
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    int val = 0, bits = -6;
    for (unsigned char c : in)
    {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) { out.push_back(T[(val >> bits) & 0x3F]); bits -= 6; }
    }
    if (bits > -6) out.push_back(T[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// 生成 16 字节随机数的 Base64（WebSocket 握手 Sec-WebSocket-Key）
static std::string make_ws_key()
{
    unsigned int r[4];
    for (int i = 0; i < 4; i++) rand_s(&r[i]);
    std::string bytes;
    for (int i = 0; i < 4; i++)
    {
        bytes.push_back((char)(r[i] & 0xFF));
        bytes.push_back((char)((r[i] >> 8) & 0xFF));
        bytes.push_back((char)((r[i] >> 16) & 0xFF));
        bytes.push_back((char)((r[i] >> 24) & 0xFF));
    }
    return base64_encode(bytes);
}

static std::string now_str()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    sprintf_s(buf, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 日志级别：0=debug 1=info 2=warn 3=error
static int g_logLevel = 1;

static void log_line(int level, const char* tag, const char* color, const std::string& msg)
{
    if (level < g_logLevel) return;
    std::cout << color << "[" << now_str() << "][" << tag << "] " << msg << "\033[0m" << std::endl;
}
static void log_debug(const std::string& m) { log_line(0, "调试", "\033[90m", m); }
static void log_info(const std::string& m)  { log_line(1, "信息", "\033[36m", m); }
static void log_warn(const std::string& m)  { log_line(2, "警告", "\033[33m", m); }
static void log_error(const std::string& m) { log_line(3, "错误", "\033[31m", m); }

// ============================================================================
// 配置
// ============================================================================

struct Config
{
    std::string api_key;
    std::string base_url = "https://api.deepseek.com";
    std::string model = "deepseek-chat";
    std::string system_prompt = "你是一个友好、简洁、准确的 QQ 智能助手，始终用中文回答。";
    double temperature = 0.7;
    int max_tokens = 1024;

    std::string ws_url = "ws://127.0.0.1:3001";

    bool auto_reply_enabled = true;
    std::string reply_trigger = "at_me";   // "at_me" 仅被@时回复；"all" 全部消息都回复
    bool reply_to_self = false;
    int history_size = 10;                 // 保留的上下文条数（不含 system）
    int cooldown_seconds = 1;              // 同一会话最小回复间隔（秒）
    int timeout_seconds = 60;              // DeepSeek 请求超时（秒）
    long long admin_qq = 0;                // 0 表示所有人可开关；否则仅该 QQ 号可开关
    std::vector<std::string> wake_words;   // 群聊中命中这些关键词也触发回复

    struct CannedReply { std::vector<std::string> keywords; std::string reply; };
    std::vector<CannedReply> canned_replies;  // 关键词固定回复（命中即回固定文本）

    std::string log_level = "info";
};

static Config g_cfg;
static json g_cfgJson;                    // 原始 JSON，用于回写持久化
static std::string g_cfgPath = "config.json";

static bool load_config()
{
    std::ifstream f(g_cfgPath, std::ios::binary);
    if (!f)
    {
        // 尝试 exe 同目录
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        size_t p = dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) dir = dir.substr(0, p + 1);
        f.open(wide_to_utf8(dir) + "config.json", std::ios::binary);
        if (!f) return false;
        g_cfgPath = wide_to_utf8(dir) + "config.json";
    }
    std::stringstream ss;
    ss << f.rdbuf();
    try
    {
        g_cfgJson = json::parse(ss.str());
    }
    catch (...)
    {
        log_error("config.json 解析失败，请检查 JSON 格式");
        return false;
    }

    auto& d = g_cfgJson.value("deepseek", json::object());
    g_cfg.api_key = d.value("api_key", g_cfg.api_key);
    g_cfg.base_url = d.value("base_url", g_cfg.base_url);
    g_cfg.model = d.value("model", g_cfg.model);
    g_cfg.system_prompt = d.value("system_prompt", g_cfg.system_prompt);
    g_cfg.temperature = d.value("temperature", g_cfg.temperature);
    g_cfg.max_tokens = d.value("max_tokens", g_cfg.max_tokens);

    auto& o = g_cfgJson.value("onebot", json::object());
    g_cfg.ws_url = o.value("ws_url", g_cfg.ws_url);

    auto& b = g_cfgJson.value("bot", json::object());
    g_cfg.auto_reply_enabled = b.value("auto_reply_enabled", g_cfg.auto_reply_enabled);
    g_cfg.reply_trigger = b.value("reply_trigger", g_cfg.reply_trigger);
    g_cfg.reply_to_self = b.value("reply_to_self", g_cfg.reply_to_self);
    g_cfg.history_size = b.value("history_size", g_cfg.history_size);
    g_cfg.cooldown_seconds = b.value("cooldown_seconds", g_cfg.cooldown_seconds);
    g_cfg.timeout_seconds = b.value("timeout_seconds", g_cfg.timeout_seconds);
    g_cfg.admin_qq = b.value("admin_qq", (long long)0);
    if (b.contains("wake_words") && b["wake_words"].is_array())
    {
        g_cfg.wake_words.clear();
        for (auto& w : b["wake_words"]) if (w.is_string()) g_cfg.wake_words.push_back(w.get<std::string>());
    }
    if (b.contains("canned_replies") && b["canned_replies"].is_array())
    {
        g_cfg.canned_replies.clear();
        for (auto& cr : b["canned_replies"])
        {
            Config::CannedReply c;
            if (cr.contains("keywords") && cr["keywords"].is_array())
                for (auto& k : cr["keywords"]) if (k.is_string()) c.keywords.push_back(k.get<std::string>());
            if (cr.contains("reply") && cr["reply"].is_string()) c.reply = cr["reply"].get<std::string>();
            if (!c.keywords.empty() && !c.reply.empty()) g_cfg.canned_replies.push_back(c);
        }
    }

    auto& l = g_cfgJson.value("log", json::object());
    g_cfg.log_level = l.value("level", g_cfg.log_level);
    if (g_cfg.log_level == "debug") g_logLevel = 0;
    else if (g_cfg.log_level == "warn") g_logLevel = 2;
    else if (g_cfg.log_level == "error") g_logLevel = 3;
    else g_logLevel = 1;

    return true;
}

// 持久化当前开关状态（改写 config.json 中的 bot.auto_reply_enabled）
static void save_auto_reply_state()
{
    try
    {
        if (!g_cfgJson.contains("bot") || !g_cfgJson["bot"].is_object())
            g_cfgJson["bot"] = json::object();
        g_cfgJson["bot"]["auto_reply_enabled"] = g_cfg.auto_reply_enabled;
        std::ofstream f(g_cfgPath, std::ios::binary | std::ios::trunc);
        f << g_cfgJson.dump(2);
    }
    catch (...)
    {
        log_warn("写入 config.json 失败（开关状态未持久化）");
    }
}

// ============================================================================
// URL 解析
// ============================================================================

struct Url { bool secure = false; std::string host; int port = 80; std::string path = "/"; bool ok = false; };

static Url parse_url(const std::string& raw)
{
    Url u;
    std::string s = raw;
    size_t p = s.find("://");
    if (p != std::string::npos)
    {
        std::string scheme = s.substr(0, p);
        u.secure = (scheme == "wss" || scheme == "https");
        s = s.substr(p + 3);
    }
    size_t slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    u.path = (slash == std::string::npos) ? "/" : s.substr(slash);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos)
    {
        u.host = hostport.substr(0, colon);
        try { u.port = std::stoi(hostport.substr(colon + 1)); }
        catch (...) { u.port = u.secure ? 443 : 80; }
    }
    else
    {
        u.host = hostport;
        u.port = u.secure ? 443 : 80;
    }
    if (!u.host.empty()) u.ok = true;
    return u;
}

// ============================================================================
// WinHTTP：HTTPS POST（调用 DeepSeek API）
// ============================================================================

struct HttpResult { long status = 0; std::string body; bool ok = false; };

static HttpResult http_post_json(const std::string& url,
                                 const std::string& body,
                                 const std::vector<std::pair<std::string, std::string>>& headers,
                                 int timeout_ms)
{
    HttpResult res;
    Url u = parse_url(url);
    if (!u.ok) { res.body = "URL 解析失败"; return res; }

    HINTERNET session = WinHttpOpen(L"QQ-DeepSeek-Bot/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { res.body = "WinHttpOpen 失败"; return res; }
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET conn = WinHttpConnect(session, utf8_to_wide(u.host).c_str(), (INTERNET_PORT)u.port, 0);
    if (!conn) { WinHttpCloseHandle(session); res.body = "WinHttpConnect 失败"; return res; }

    DWORD flags = u.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", utf8_to_wide(u.path).c_str(),
                                       nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); res.body = "WinHttpOpenRequest 失败"; return res; }

    std::wstring hdr = L"Content-Type: application/json\r\nUser-Agent: QQ-DeepSeek-Bot/1.0\r\n";
    for (auto& h : headers)
        hdr += utf8_to_wide(h.first) + L": " + utf8_to_wide(h.second) + L"\r\n";
    WinHttpAddRequestHeaders(req, hdr.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sent) { WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session); res.body = "请求发送失败"; return res; }

    if (!WinHttpReceiveResponse(req, nullptr))
    { WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session); res.body = "接收响应失败"; return res; }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    res.status = (long)status;

    DWORD avail = 0, read = 0;
    char buf[16384];
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0)
    {
        DWORD toRead = (avail < sizeof(buf)) ? avail : sizeof(buf);
        if (!WinHttpReadData(req, buf, toRead, &read) || read == 0) break;
        res.body.append(buf, read);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);

    res.ok = (status >= 200 && status < 300);
    return res;
}

// ============================================================================
// DeepSeek 调用
// ============================================================================

static bool call_deepseek(const json& messages, std::string& out)
{
    std::string base = g_cfg.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string url = base + "/chat/completions";

    json body;
    body["model"] = g_cfg.model;
    body["messages"] = messages;
    body["stream"] = false;
    body["temperature"] = g_cfg.temperature;
    body["max_tokens"] = g_cfg.max_tokens;

    std::vector<std::pair<std::string, std::string>> headers;
    headers.push_back({ "Authorization", "Bearer " + g_cfg.api_key });

    HttpResult r = http_post_json(url, body.dump(), headers, g_cfg.timeout_seconds * 1000);
    if (!r.ok)
    {
        out = "HTTP 状态 " + std::to_string(r.status);
        if (!r.body.empty()) out += "：" + r.body.substr(0, 200);
        return false;
    }
    try
    {
        json j = json::parse(r.body);
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty())
        {
            out = j["choices"][0]["message"].value("content", std::string(""));
            if (out.empty())
                out = j["choices"][0]["message"].value("reasoning_content", std::string(""));
            return true;
        }
        out = j.value("error", json::object()).value("message", std::string("未知错误"));
        return false;
    }
    catch (...)
    {
        out = "解析 DeepSeek 响应失败";
        return false;
    }
}

// ============================================================================
// WinHTTP WebSocket 客户端（连接 OneBot 正向 WS）
// ============================================================================

struct Ws { HINTERNET session = nullptr; HINTERNET conn = nullptr; HINTERNET ws = nullptr; };

static HINTERNET g_ws = nullptr;   // 全局当前 WebSocket 句柄

static bool ws_connect(const std::string& url, Ws& c)
{
    Url u = parse_url(url);
    if (!u.ok) { log_error("WebSocket URL 解析失败: " + url); return false; }

    c.session = WinHttpOpen(L"QQ-DeepSeek-Bot/1.0",
                            WINHTTP_ACCESS_TYPE_NO_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c.session) { log_error("WinHttpOpen 失败，错误码 " + std::to_string(GetLastError())); return false; }
    WinHttpSetTimeouts(c.session, 30000, 30000, 30000, 0xFFFFFFFF);

    DWORD flags = u.secure ? WINHTTP_FLAG_SECURE : 0;
    c.conn = WinHttpConnect(c.session, utf8_to_wide(u.host).c_str(), (INTERNET_PORT)u.port, 0);
    if (!c.conn) { log_error("WinHttpConnect 失败，错误码 " + std::to_string(GetLastError())); return false; }

    std::string path = u.path.empty() ? "/" : u.path;
    HINTERNET req = WinHttpOpenRequest(c.conn, L"GET", utf8_to_wide(path).c_str(),
                                       nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { log_error("WinHttpOpenRequest 失败，错误码 " + std::to_string(GetLastError())); return false; }

    // 关键：标记此请求为 WebSocket 升级（需在发送前设置）
    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) { log_error("SetOption UPGRADE_TO_WEB_SOCKET 失败，错误码 " + std::to_string(GetLastError())); WinHttpCloseHandle(req); return false; }

    std::wstring hdr = L"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
                       + utf8_to_wide(make_ws_key()) + L"\r\n";
    if (!WinHttpAddRequestHeaders(req, hdr.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) { log_error("AddRequestHeaders 失败，错误码 " + std::to_string(GetLastError())); WinHttpCloseHandle(req); return false; }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) { log_error("SendRequest 失败，错误码 " + std::to_string(GetLastError())); WinHttpCloseHandle(req); return false; }
    if (!WinHttpReceiveResponse(req, NULL)) { log_error("ReceiveResponse 失败，错误码 " + std::to_string(GetLastError())); WinHttpCloseHandle(req); return false; }

    c.ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    if (!c.ws) { log_error("CompleteUpgrade 失败，错误码 " + std::to_string(GetLastError())); WinHttpCloseHandle(req); return false; }
    return true;
}

static void ws_close(Ws& c)
{
    if (c.ws)      { WinHttpCloseHandle(c.ws); c.ws = nullptr; }
    if (c.conn)    { WinHttpCloseHandle(c.conn); c.conn = nullptr; }
    if (c.session) { WinHttpCloseHandle(c.session); c.session = nullptr; }
}

static bool ws_send(const std::string& text)
{
    if (!g_ws) return false;
    DWORD err = WinHttpWebSocketSend(g_ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                     (PVOID)text.data(), (DWORD)text.size());
    return (err == ERROR_SUCCESS);
}

// ============================================================================
// 机器人核心逻辑
// ============================================================================

struct ChatSession { std::deque<std::pair<std::string, std::string>> history; };

static std::map<std::string, ChatSession> g_sessions;
static std::map<std::string, ULONGLONG> g_lastReplyMs;   // 冷却用
static long long g_selfId = 0;
static ULONGLONG g_echo = 1;

static ULONGLONG now_ms() { return GetTickCount64(); }

// 从 OneBot 事件中提取纯文本（优先 message 数组里的 text 段）
static std::string extract_text(const json& ev)
{
    if (ev.contains("message") && ev["message"].is_array())
    {
        std::string text;
        for (auto& seg : ev["message"])
        {
            if (seg.value("type", "") == "text")
                text += seg.value("data", json::object()).value("text", std::string(""));
        }
        if (!trim(text).empty()) return text;
    }
    return strip_cq_codes(ev.value("raw_message", std::string("")));
}

// 判断事件里是否 @ 了机器人自己
static bool is_mentioned(const json& ev, long long self_id)
{
    if (ev.contains("message") && ev["message"].is_array())
    {
        for (auto& seg : ev["message"])
        {
            if (seg.value("type", "") == "at")
            {
                auto& d = seg.value("data", json::object());
                if (d.contains("qq"))
                {
                    std::string qq = d["qq"].is_number()
                        ? std::to_string(d["qq"].get<long long>())
                        : d["qq"].get<std::string>();
                    if (qq == std::to_string(self_id)) return true;
                }
            }
        }
        return false;
    }
    std::string raw = ev.value("raw_message", std::string(""));
    return raw.find("[CQ:at,qq=" + std::to_string(self_id) + "]") != std::string::npos;
}

static std::string session_key(const json& ev)
{
    std::string mt = ev.value("message_type", std::string(""));
    if (mt == "group")   return "group:" + std::to_string(ev.value("group_id", (long long)0));
    if (mt == "private") return "private:" + std::to_string(ev.value("user_id", (long long)0));
    return "other:0";
}

static bool contains_wake_word(const std::string& text)
{
    for (auto& w : g_cfg.wake_words)
        if (!w.empty() && text.find(w) != std::string::npos) return true;
    return false;
}

// ---- 命令识别 ----
enum class Cmd { None, On, Off, Status, Help };

static Cmd parse_command(const std::string& raw)
{
    std::string t = strip_trailing_punct(raw);

    static const std::vector<std::string> on  = { "开启自动回复","打开自动回复","启用自动回复","开始自动回复","自动回复开","开启","打开","/on","/开启","/打开","开启机器人","on" };
    static const std::vector<std::string> off = { "关闭自动回复","停用自动回复","停止自动回复","自动回复关","关闭","停止","停用","/off","/关闭","/停止","关闭机器人","off" };
    static const std::vector<std::string> st  = { "状态","查询状态","查看状态","当前状态","状态查询","/status","/状态","status" };
    static const std::vector<std::string> hp  = { "帮助","使用说明","用法","菜单","/help","help" };

    auto hit = [&](const std::vector<std::string>& v) {
        for (auto& k : v) if (t == k) return true;
        return false;
    };
    if (hit(on))  return Cmd::On;
    if (hit(off)) return Cmd::Off;
    if (hit(st))  return Cmd::Status;
    if (hit(hp))  return Cmd::Help;
    return Cmd::None;
}

static std::string on_off_str(bool on)
{
    return on ? "✅ 已开启" : "⛔ 已关闭";
}

static std::string status_card()
{
    std::string s;
    s += "🤖 QQ × DeepSeek 智能回复助手\n";
    s += "──────────────────\n";
    s += "▶ 自动回复：" + on_off_str(g_cfg.auto_reply_enabled) + "\n";
    s += "▶ 触发方式：" + std::string(g_cfg.reply_trigger == "all" ? "全部消息" : "仅@我/关键词") + "\n";
    s += "▶ 模型：" + g_cfg.model + "\n";
    s += "▶ 上下文：" + std::to_string(g_cfg.history_size) + " 条\n";
    return s;
}

static std::string help_card()
{
    std::string s;
    s += "📖 使用说明\n";
    s += "──────────────────\n";
    s += "✅ 开启自动回复：发「开启自动回复」\n";
    s += "⛔ 关闭自动回复：发「关闭自动回复」\n";
    s += "📊 查看状态：发「状态」\n";
    s += "✉️  私聊直接回复；群聊 @我 即回复\n";
    return s;
}

// ---- 发送回复（通过 OneBot API action）----
static void send_reply(const json& ev, const std::string& text)
{
    static const size_t MAX_MSG = 3800;
    std::string msg = text;
    if (msg.size() > MAX_MSG) { msg = msg.substr(0, MAX_MSG) + "\n...(已截断)"; }

    json action;
    action["echo"] = "reply_" + std::to_string(g_echo++);
    if (ev.value("message_type", "") == "group")
    {
        action["action"] = "send_group_msg";
        action["params"]["group_id"] = ev.value("group_id", (long long)0);
        action["params"]["message"] = msg;
    }
    else
    {
        action["action"] = "send_private_msg";
        action["params"]["user_id"] = ev.value("user_id", (long long)0);
        action["params"]["message"] = msg;
    }
    if (!ws_send(action.dump()))
        log_warn("回复发送失败（WebSocket 未连接）");
}

// ---- 自动回复 ----
static void do_auto_reply(const json& ev)
{
    std::string key = session_key(ev);
    std::string user_text = extract_text(ev);
    if (trim(user_text).empty()) return;

    // 冷却
    ULONGLONG now = now_ms();
    auto it = g_lastReplyMs.find(key);
    if (it != g_lastReplyMs.end() && (now - it->second) < (ULONGLONG)g_cfg.cooldown_seconds * 1000ULL)
        return;
    g_lastReplyMs[key] = now;

    ChatSession& sess = g_sessions[key];
    sess.history.push_back({ "user", user_text });

    json messages = json::array();
    messages.push_back({ { "role", "system" }, { "content", g_cfg.system_prompt } });
    for (auto& h : sess.history)
        messages.push_back({ { "role", h.first }, { "content", h.second } });

    log_info("调用 DeepSeek 生成回复（" + key + "）...");
    std::string reply;
    if (!call_deepseek(messages, reply))
    {
        sess.history.pop_back(); // 失败不保留该条用户消息
        send_reply(ev, "⚠️ 自动回复失败：" + reply);
        return;
    }
    sess.history.push_back({ "assistant", reply });
    while ((int)sess.history.size() > g_cfg.history_size)
        sess.history.pop_front();

    send_reply(ev, reply);
}

// ---- 关键词匹配辅助 ----
static std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool contains_ignore_case(const std::string& haystack, const std::string& needle)
{
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

// ---- 事件处理 ----
static void handle_message(const json& ev)
{
    std::string mt = ev.value("message_type", "");
    if (mt != "group" && mt != "private") return;

    long long self_id = ev.value("self_id", g_selfId);
    if (self_id) g_selfId = self_id;
    long long user_id = ev.value("user_id", (long long)0);

    if (!g_cfg.reply_to_self && user_id == g_selfId) return; // 忽略自己发的消息，防止死循环

    bool mentioned = (mt == "group") && is_mentioned(ev, g_selfId);
    std::string text = extract_text(ev);
    Cmd cmd = parse_command(text);

    // 群聊：命令仅在 @ 我 时生效（避免误触发）
    if (mt == "group" && !mentioned && cmd != Cmd::None)
        return;

    bool is_admin_ok = (g_cfg.admin_qq == 0) || (user_id == g_cfg.admin_qq);

    switch (cmd)
    {
    case Cmd::On:
        if (!is_admin_ok) { send_reply(ev, "⚠️ 你没有权限操作"); break; }
        g_cfg.auto_reply_enabled = true;
        save_auto_reply_state();
        log_info("自动回复已开启");
        send_reply(ev, "✅ 自动回复已开启。现在可以直接和我聊天啦！");
        break;
    case Cmd::Off:
        if (!is_admin_ok) { send_reply(ev, "⚠️ 你没有权限操作"); break; }
        g_cfg.auto_reply_enabled = false;
        save_auto_reply_state();
        log_info("自动回复已关闭");
        send_reply(ev, "⛔ 自动回复已关闭，我会保持沉默。");
        break;
    case Cmd::Status:
        send_reply(ev, status_card());
        break;
    case Cmd::Help:
        send_reply(ev, help_card());
        break;
    default:
        break;
    }

    // 命令消息不再触发智能回复
    if (cmd != Cmd::None) return;

    // 关键词固定回复（私聊直接触发；群聊需 @我 或全部消息模式）
    if (mt == "private" || mentioned || g_cfg.reply_trigger == "all")
    {
        for (auto& cr : g_cfg.canned_replies)
        {
            bool hit = false;
            for (auto& k : cr.keywords)
                if (contains_ignore_case(text, k)) { hit = true; break; }
            if (hit)
            {
                log_info("命中固定回复关键词（" + session_key(ev) + "）");
                send_reply(ev, cr.reply);
                return;
            }
        }
    }

    if (!g_cfg.auto_reply_enabled) return;

    if (mt == "private")
    {
        do_auto_reply(ev);
    }
    else // group
    {
        bool trigger = false;
        if (g_cfg.reply_trigger == "all") trigger = true;
        else if (mentioned) trigger = true;
        else if (contains_wake_word(text)) trigger = true;
        if (trigger) do_auto_reply(ev);
    }
}

static void handle_frame(const std::string& text)
{
    json j;
    try { j = json::parse(text); }
    catch (...) { log_debug("收到无法解析的 JSON"); return; }

    if (!j.contains("post_type"))
    {
        if (j.contains("echo") && j.contains("retcode"))
            log_debug("API 响应 echo=" + j.value("echo", std::string("")) + " retcode=" + std::to_string(j.value("retcode", 0)));
        return;
    }
    if (j.value("post_type", "") == "message") handle_message(j);
}

// ---- 主事件循环 ----
static void ws_event_loop()
{
    std::string accumulated;
    char buf[65536];
    while (g_ws)
    {
        DWORD bytes = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD err = WinHttpWebSocketReceive(g_ws, buf, sizeof(buf), &bytes, &type);
        if (err != ERROR_SUCCESS)
        {
            if (err == ERROR_WINHTTP_TIMEOUT) continue; // 正常超时，继续等待
            log_warn("WebSocket 接收错误，代码 " + std::to_string(err));
            return;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
        {
            log_info("服务器关闭了 WebSocket 连接");
            return;
        }
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
        {
            accumulated.append(buf, bytes);
            handle_frame(accumulated);
            accumulated.clear();
        }
        else if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
        {
            accumulated.append(buf, bytes);
        }
    }
}

// ============================================================================
// main
// ============================================================================

static void enable_console_utf8()
{
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h && GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

static void print_banner()
{
    std::cout << "\033[1;35m==============================================\033[0m" << std::endl;
    std::cout << "\033[1;35m   QQ x DeepSeek 智能自动回复机器人\033[0m" << std::endl;
    std::cout << "\033[1;35m==============================================\033[0m" << std::endl;
    log_info("OneBot 地址：" + g_cfg.ws_url);
    log_info("模型：" + g_cfg.model);
    if (g_cfg.auto_reply_enabled)
        log_info("自动回复状态：\033[1;32m开启\033[0m");
    else
        log_info("自动回复状态：\033[1;31m关闭\033[0m");
    std::cout << std::endl;
}

int main()
{
    enable_console_utf8();

    if (!load_config())
    {
        log_error("无法加载 config.json，请确认文件存在且格式正确");
        return 1;
    }
    if (g_cfg.api_key.empty() || g_cfg.api_key.find("sk-") != 0)
        log_warn("警告：DeepSeek api_key 未填写或格式异常，自动回复将无法工作");

    print_banner();

    for (;;)
    {
        Ws c;
        if (ws_connect(g_cfg.ws_url, c))
        {
            g_ws = c.ws;
            log_info("已连接 OneBot：" + g_cfg.ws_url);
            ws_event_loop();
            g_ws = nullptr;
            ws_close(c);
            log_warn("与 OneBot 的连接已断开，3 秒后重连...");
        }
        else
        {
            log_warn("无法连接 OneBot（" + g_cfg.ws_url + "），请确认 OneBot 已启动且开启正向 WebSocket。3 秒后重试...");
            ws_close(c);
        }
        Sleep(3000);
    }
    return 0;
}
