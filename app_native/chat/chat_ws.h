// A WebSocket over TLS (wss://) to a chat relay, by the system's own TLS, which checks the relay's certificate: Android's
// (javax.net.ssl, the app's ChatSocket class, through JNI: chat_ws_android.cpp) or Windows' (WinHTTP: chat_ws_win.cpp).
// Each socket connects and reads on a thread of its own; these calls never block for long. Text messages only.
// The same file is in both ports.
#pragma once
#include <string>

namespace chatws {

enum State { CONNECTING, OPEN, CLOSED };

// Android: the app's ChatSocket class ("com/mhgu/recomp/ChatSocket"), found once from a thread the system made (the
// interface's): a thread of the runtime's own sees only the system's classes
void platform_init(const char *java_class);
bool available();  // false: this platform has no relay sockets (the chat's relays stay off)

int open(const std::string &url);           // a handle (> 0), or 0
int state(int h);                           // State
bool send(int h, const std::string &text);  // false: not open
bool poll(int h, std::string &text);        // the next message received, if there is one
std::string error(int h);                   // why it closed ("" if it did not say)
void close(int h);                          // the handle is gone after this

}  // namespace chatws
