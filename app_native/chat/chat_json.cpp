// The chat's JSON: chat_json.h.
#include "chat_json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace chatj {

const Value *Value::get(const char *key) const {
    if (type != Object) return nullptr;
    for (size_t i = 0; i < keys.size(); i++)
        if (keys[i] == key) return &items[i];
    return nullptr;
}
std::string Value::str(const char *key) const {
    const Value *v = get(key);
    return v && v->type == String ? v->s : std::string();
}
int64_t Value::integer(const char *key, int64_t fallback) const {
    const Value *v = get(key);
    return v && v->type == Number ? (int64_t)strtoll(v->s.c_str(), nullptr, 10) : fallback;
}

namespace {
struct Reader {
    const char *p, *end;
    int depth = 0;
    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    static void utf8(std::string &o, uint32_t c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) o += (char)(0xC0 | c >> 6), o += (char)(0x80 | (c & 63));
        else if (c < 0x10000) o += (char)(0xE0 | c >> 12), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
        else
            o += (char)(0xF0 | c >> 18), o += (char)(0x80 | (c >> 12 & 63)), o += (char)(0x80 | (c >> 6 & 63)),
                o += (char)(0x80 | (c & 63));
    }
    bool hex4(uint32_t &v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string &o) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            if ((unsigned char)*p < 0x20) return false;
            if (*p != '\\') {
                o += *p++;
                continue;
            }
            if (++p >= end) return false;
            char e = *p++;
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                uint32_t c;
                if (!hex4(c)) return false;
                if (c >= 0xD800 && c < 0xDC00) {  // (a surrogate pair)
                    uint32_t lo;
                    if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
                    p += 2;
                    if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                } else if (c >= 0xDC00 && c < 0xE000) return false;
                utf8(o, c);
                break;
            }
            default: return false;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(Value &v) {
        ws();
        if (p >= end || ++depth > 32) return false;
        bool ok = true;
        if (*p == '{') {
            v.type = Value::Object;
            p++, ws();
            if (p < end && *p == '}') p++;
            else
                for (;;) {
                    std::string k;
                    ws();
                    if (!string(k)) return false;
                    ws();
                    if (p >= end || *p++ != ':') return false;
                    v.keys.push_back(std::move(k));
                    v.items.emplace_back();
                    if (!value(v.items.back())) return false;
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == '}') { p++; break; }
                    return false;
                }
        } else if (*p == '[') {
            v.type = Value::Array;
            p++, ws();
            if (p < end && *p == ']') p++;
            else
                for (;;) {
                    v.items.emplace_back();
                    if (!value(v.items.back())) return false;
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == ']') { p++; break; }
                    return false;
                }
        } else if (*p == '"') {
            v.type = Value::String;
            ok = string(v.s);
        } else if (end - p >= 4 && !strncmp(p, "true", 4)) {
            v.type = Value::Bool, v.b = true, p += 4;
        } else if (end - p >= 5 && !strncmp(p, "false", 5)) {
            v.type = Value::Bool, p += 5;
        } else if (end - p >= 4 && !strncmp(p, "null", 4)) {
            p += 4;
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            const char *s = p;
            while (p < end && strchr("+-0123456789.eE", *p) && *p) p++;
            v.type = Value::Number;
            v.s.assign(s, p);
            v.n = strtod(v.s.c_str(), nullptr);
        } else
            ok = false;
        depth--;
        return ok;
    }
};
}  // namespace

bool parse(const std::string &text, Value &out) {
    out = Value();
    Reader r{text.data(), text.data() + text.size()};
    if (!r.value(out)) return false;
    r.ws();
    return r.p == r.end;
}

std::string quote(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else
                o += (char)c;
        }
    }
    return o + "\"";
}

}  // namespace chatj
