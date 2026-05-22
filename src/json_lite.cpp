// json_lite.cpp — hand-rolled JSON parser/serializer.
#include "json_lite.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace dnp {

Json Json::make_null() { Json j; j.type_ = TNull; return j; }
Json Json::make_bool(bool v) { Json j; j.type_ = TBool; j.b_ = v; return j; }
Json Json::make_int(int64_t v) { Json j; j.type_ = TInt; j.i_ = v; return j; }
Json Json::make_str(std::string v) { Json j; j.type_ = TStr; j.s_ = std::move(v); return j; }
Json Json::make_arr() { Json j; j.type_ = TArr; return j; }
Json Json::make_obj() { Json j; j.type_ = TObj; return j; }

Json* Json::find(const std::string& key) {
    if (type_ != TObj) return nullptr;
    for (auto& kv : obj_) if (kv.first == key) return &kv.second;
    return nullptr;
}
const Json* Json::find(const std::string& key) const {
    if (type_ != TObj) return nullptr;
    for (auto& kv : obj_) if (kv.first == key) return &kv.second;
    return nullptr;
}
void Json::set(const std::string& key, Json v) {
    if (type_ != TObj) { type_ = TObj; obj_.clear(); }
    for (auto& kv : obj_) {
        if (kv.first == key) { kv.second = std::move(v); return; }
    }
    obj_.emplace_back(key, std::move(v));
}
bool Json::remove(const std::string& key) {
    if (type_ != TObj) return false;
    for (auto it = obj_.begin(); it != obj_.end(); ++it) {
        if (it->first == key) { obj_.erase(it); return true; }
    }
    return false;
}

// ============================================================================
// Parser
// ============================================================================
namespace {

struct Parser {
    const char* p;
    const char* end;

    bool eof() const { return p >= end; }
    char peek() const { return eof() ? '\0' : *p; }

    void skip_ws() {
        while (p < end) {
            char c = *p;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p;
            else break;
        }
    }

    bool consume(char c) {
        skip_ws();
        if (eof() || *p != c) return false;
        ++p;
        return true;
    }

    bool parse_value(Json& out);

    bool parse_string(std::string& out) {
        skip_ws();
        if (eof() || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return false;
                char esc = *p++;
                switch (esc) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (end - p < 4) return false;
                        uint32_t cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else return false;
                        }
                        // Handle surrogate pair.
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (end - p < 6 || p[0] != '\\' || p[1] != 'u') {
                                // Invalid; emit replacement.
                                cp = 0xFFFD;
                            } else {
                                p += 2;
                                uint32_t lo = 0;
                                for (int i = 0; i < 4; ++i) {
                                    char h = *p++;
                                    lo <<= 4;
                                    if (h >= '0' && h <= '9') lo |= h - '0';
                                    else if (h >= 'a' && h <= 'f') lo |= h - 'a' + 10;
                                    else if (h >= 'A' && h <= 'F') lo |= h - 'A' + 10;
                                    else return false;
                                }
                                if (lo < 0xDC00 || lo > 0xDFFF) cp = 0xFFFD;
                                else cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                            }
                        }
                        // Encode as UTF-8.
                        if (cp < 0x80) {
                            out.push_back((char)cp);
                        } else if (cp < 0x800) {
                            out.push_back((char)(0xC0 | (cp >> 6)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        } else if (cp < 0x10000) {
                            out.push_back((char)(0xE0 | (cp >> 12)));
                            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back((char)(0xF0 | (cp >> 18)));
                            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false; // unterminated
    }

    bool parse_number(Json& out) {
        skip_ws();
        const char* start = p;
        bool neg = false;
        if (p < end && *p == '-') { neg = true; ++p; }
        if (p >= end || *p < '0' || *p > '9') return false;
        int64_t v = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            ++p;
        }
        // Reject fractional / exponent for asar (we don't expect floats).
        if (p < end && (*p == '.' || *p == 'e' || *p == 'E')) {
            // Tolerate by scanning to end of number but degrade to 0 — should not happen in asar.
            while (p < end && (*p == '.' || *p == 'e' || *p == 'E' ||
                               *p == '+' || *p == '-' || (*p >= '0' && *p <= '9'))) ++p;
            out = Json::make_int(0);
            (void)start;
            return true;
        }
        out = Json::make_int(neg ? -v : v);
        return true;
    }

    bool parse_array(Json& out) {
        out = Json::make_arr();
        if (!consume('[')) return false;
        skip_ws();
        if (consume(']')) return true;
        while (true) {
            Json v;
            if (!parse_value(v)) return false;
            out.as_arr().push_back(std::move(v));
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) return true;
            return false;
        }
    }

    bool parse_object(Json& out) {
        out = Json::make_obj();
        if (!consume('{')) return false;
        skip_ws();
        if (consume('}')) return true;
        while (true) {
            std::string key;
            if (!parse_string(key)) return false;
            if (!consume(':')) return false;
            Json v;
            if (!parse_value(v)) return false;
            out.set(key, std::move(v));
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return false;
        }
    }
};

bool Parser::parse_value(Json& out) {
    skip_ws();
    if (eof()) return false;
    char c = *p;
    if (c == '{') return parse_object(out);
    if (c == '[') return parse_array(out);
    if (c == '"') {
        std::string s;
        if (!parse_string(s)) return false;
        out = Json::make_str(std::move(s));
        return true;
    }
    if (c == 't') {
        if (end - p >= 4 && memcmp(p, "true", 4) == 0) { p += 4; out = Json::make_bool(true); return true; }
        return false;
    }
    if (c == 'f') {
        if (end - p >= 5 && memcmp(p, "false", 5) == 0) { p += 5; out = Json::make_bool(false); return true; }
        return false;
    }
    if (c == 'n') {
        if (end - p >= 4 && memcmp(p, "null", 4) == 0) { p += 4; out = Json::make_null(); return true; }
        return false;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
    return false;
}

} // namespace

bool Json::parse(const std::string& text) {
    return parse(text.data(), text.size());
}
bool Json::parse(const char* data, size_t size) {
    Parser ps{data, data + size};
    if (!ps.parse_value(*this)) return false;
    ps.skip_ws();
    return ps.p == ps.end;
}

// ============================================================================
// Serializer
// ============================================================================
namespace {

void emit_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out.append(buf);
                } else {
                    out.push_back((char)c);
                }
        }
    }
    out.push_back('"');
}

void emit(const Json& j, std::string& out);

void emit_arr(const Json& j, std::string& out) {
    out.push_back('[');
    const auto& a = j.as_arr();
    for (size_t i = 0; i < a.size(); ++i) {
        if (i) out.push_back(',');
        emit(a[i], out);
    }
    out.push_back(']');
}

void emit_obj(const Json& j, std::string& out) {
    out.push_back('{');
    const auto& o = j.as_obj();
    for (size_t i = 0; i < o.size(); ++i) {
        if (i) out.push_back(',');
        emit_string(out, o[i].first);
        out.push_back(':');
        emit(o[i].second, out);
    }
    out.push_back('}');
}

void emit(const Json& j, std::string& out) {
    switch (j.type()) {
        case Json::TNull: out.append("null"); return;
        case Json::TBool: out.append(j.as_bool() ? "true" : "false"); return;
        case Json::TInt: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)j.as_int());
            out.append(buf);
            return;
        }
        case Json::TStr: emit_string(out, j.as_str()); return;
        case Json::TArr: emit_arr(j, out); return;
        case Json::TObj: emit_obj(j, out); return;
    }
}

} // namespace

std::string Json::dump() const {
    std::string out;
    out.reserve(256);
    emit(*this, out);
    return out;
}

} // namespace dnp
