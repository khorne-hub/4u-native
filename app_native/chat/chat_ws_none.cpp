// The relay sockets elsewhere (a desktop Linux build): none, the chat's relays stay off; the hub's chat still works.
#if !defined(_WIN32) && !defined(__ANDROID__)
#include "chat_ws.h"

namespace chatws {
void platform_init(const char *) {}
bool available() { return false; }
int open(const std::string &) { return 0; }
int state(int) { return CLOSED; }
bool send(int, const std::string &) { return false; }
bool poll(int, std::string &) { return false; }
std::string error(int) { return "no relay sockets on this system"; }
void close(int) {}
}  // namespace chatws
#endif
