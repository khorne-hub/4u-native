// A small JSON reader and writer for the chat's relay messages (runtime/chat/chat.cpp): Nostr's ["EVENT", ...], ["OK",
// ...] and the events in them. Strings come out as UTF-8; numbers as doubles (and their text, for exact integers).
// The same file is in both ports.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace chatj {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;                // a string, or a number's text
    std::vector<Value> items;     // an array's, an object's values
    std::vector<std::string> keys;  // an object's keys (beside items)
    const Value *get(const char *key) const;  // an object's member, or null
    const Value *at(size_t i) const { return type == Array && i < items.size() ? &items[i] : nullptr; }
    std::string str(const char *key) const;   // a member that is a string, or ""
    int64_t integer(const char *key, int64_t fallback = 0) const;
};

bool parse(const std::string &text, Value &out);  // false: not JSON (or nested too deep)
// A string's JSON text, quoted, as Nostr hashes events (NIP-01): ", \ and the control characters escaped, the rest as is
std::string quote(const std::string &s);

}  // namespace chatj
