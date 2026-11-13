// The relay sockets on Windows (chat_ws.h): WinHTTP's WebSocket (Windows 8 and later), whose TLS checks the relay's
// certificate against the system's store. One thread per socket connects, then reads; a send goes from the caller's
// thread (WinHTTP takes one send and one receive at a time).
#ifdef _WIN32
#include "chat_ws.h"

#include <windows.h>
#include <winhttp.h>

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace chatws {
namespace {
struct Sock {
    std::mutex mu, send_mu;
    int state = CONNECTING;
    std::deque<std::string> in;
    std::string error;
    HINTERNET session = nullptr, connect = nullptr, ws = nullptr;
    bool closing = false;
};
std::mutex g_mu;
std::map<int, std::shared_ptr<Sock>> g_socks;
int g_next = 1;

std::shared_ptr<Sock> find(int h) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_socks.find(h);
    return it == g_socks.end() ? nullptr : it->second;
}
std::wstring widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
void fail(Sock &s, const char *what) {
    std::lock_guard<std::mutex> l(s.mu);
    if (s.error.empty()) {
        char b[96];
        snprintf(b, sizeof b, "%s (%lu)", what, (unsigned long)GetLastError());
        s.error = b;
    }
    s.state = CLOSED;
}
void run(std::shared_ptr<Sock> s, std::string url) {
    // wss://host[:port][/path]
    std::string rest = url.substr(url.find("://") + 3), host = rest, path = "/";
    size_t slash = rest.find('/');
    if (slash != std::string::npos) host = rest.substr(0, slash), path = rest.substr(slash);
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    size_t colon = host.find(':');
    if (colon != std::string::npos) port = (INTERNET_PORT)atoi(host.c_str() + colon + 1), host = host.substr(0, colon);
    HINTERNET ses = WinHttpOpen(L"MH chat", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"MH chat", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return fail(*s, "WinHttpOpen");
    WinHttpSetTimeouts(ses, 10000, 10000, 10000, 0);  // (the receive waits for ever: a relay may be quiet a long time)
    HINTERNET con = WinHttpConnect(ses, widen(host).c_str(), port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : nullptr;
    bool ok = req && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
              WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr);
    DWORD status = 0, size = sizeof status;
    if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
    HINTERNET ws = ok && status == 101 ? WinHttpWebSocketCompleteUpgrade(req, 0) : nullptr;
    if (req) WinHttpCloseHandle(req);
    {
        std::lock_guard<std::mutex> l(s->mu);
        if (s->closing) {  // (closed while it connected: nobody holds the handle any more)
            if (ws) WinHttpCloseHandle(ws);
            if (con) WinHttpCloseHandle(con);
            WinHttpCloseHandle(ses);
            s->state = CLOSED;
            return;
        }
        s->session = ses, s->connect = con, s->ws = ws;
        if (ws) s->state = OPEN;
    }
    if (!ws) {
        if (ok && status != 101) {
            std::lock_guard<std::mutex> l(s->mu);
            s->error = "HTTP " + std::to_string(status);
        }
        return fail(*s, con ? "the WebSocket handshake failed" : "WinHttpConnect");
    }
    std::string msg;
    std::unique_ptr<char[]> buf(new char[65536]);
    for (;;) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD r = WinHttpWebSocketReceive(ws, buf.get(), 65536, &got, &type);
        if (r != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            std::lock_guard<std::mutex> l(s->mu);
            if (s->error.empty() && !s->closing) s->error = r != NO_ERROR ? "receive " + std::to_string(r) : "closed by the relay";
            s->state = CLOSED;
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            msg.clear();  // (relays send text)
            continue;
        }
        msg.append(buf.get(), got);
        if (msg.size() > (1u << 20)) msg.clear();  // (more than a megabyte: not a chat's)
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            std::lock_guard<std::mutex> l(s->mu);
            if (s->in.size() < 2000) s->in.push_back(std::move(msg));
            msg.clear();
        }
    }
}
}  // namespace

void platform_init(const char *) {}
bool available() { return true; }
int open(const std::string &url) {
    if (url.rfind("wss://", 0) != 0) return 0;
    auto s = std::make_shared<Sock>();
    int h;
    {
        std::lock_guard<std::mutex> l(g_mu);
        h = g_next++;
        g_socks[h] = s;
    }
    std::thread(run, s, url).detach();
    return h;
}
int state(int h) {
    auto s = find(h);
    if (!s) return CLOSED;
    std::lock_guard<std::mutex> l(s->mu);
    return s->state;
}
bool send(int h, const std::string &text) {
    auto s = find(h);
    if (!s) return false;
    HINTERNET ws;
    {
        std::lock_guard<std::mutex> l(s->mu);
        if (s->state != OPEN) return false;
        ws = s->ws;
    }
    std::lock_guard<std::mutex> l(s->send_mu);
    return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void *)text.data(), (DWORD)text.size()) == NO_ERROR;
}
bool poll(int h, std::string &text) {
    auto s = find(h);
    if (!s) return false;
    std::lock_guard<std::mutex> l(s->mu);
    if (s->in.empty()) return false;
    text = std::move(s->in.front());
    s->in.pop_front();
    return true;
}
std::string error(int h) {
    auto s = find(h);
    if (!s) return "";
    std::lock_guard<std::mutex> l(s->mu);
    return s->error;
}
void close(int h) {
    std::shared_ptr<Sock> s;
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_socks.find(h);
        if (it == g_socks.end()) return;
        s = it->second;
        g_socks.erase(it);
    }
    // (the reading thread holds its own reference: closing the handles ends its receive, and it ends)
    std::thread([s] {
        HINTERNET ws, con, ses;
        {
            std::lock_guard<std::mutex> l(s->mu);
            s->closing = true;
            ws = s->ws, con = s->connect, ses = s->session;
        }
        if (ws) {  // (a goodbye that does not wait for the relay's: its answer would go to the reading thread)
            std::lock_guard<std::mutex> l(s->send_mu);
            WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(ws);
        }
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }).detach();
}

}  // namespace chatws
#endif
