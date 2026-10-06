#include "json.h"
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace net {
namespace json {

namespace {
const Value& nullValue() {
    static const Value v;
    return v;
}
const std::string& emptyString() {
    static const std::string s;
    return s;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}
}  // namespace

// ---- Value ----

int64_t Value::asInt(int64_t def) const {
    if (type_ != Type::Number || std::isnan(n_)) return def;
    if (n_ >= 9.2233720368547758e18) return INT64_MAX;
    if (n_ <= -9.2233720368547758e18) return INT64_MIN;
    return int64_t(n_);
}

const std::string& Value::asString() const { return type_ == Type::String ? s_ : emptyString(); }

const Value& Value::operator[](size_t i) const {
    return type_ == Type::Array && i < items_.size() ? items_[i] : nullValue();
}

const Value& Value::operator[](const std::string& key) const {
    if (type_ == Type::Object)
        for (auto& m : members_)
            if (m.first == key) return m.second;
    return nullValue();
}

bool Value::has(const std::string& key) const {
    if (type_ == Type::Object)
        for (auto& m : members_)
            if (m.first == key) return true;
    return false;
}

Value& Value::push(Value v) {
    if (type_ != Type::Array) { *this = array(); }
    items_.push_back(std::move(v));
    return items_.back();
}

Value& Value::set(const std::string& key, Value v) {
    if (type_ != Type::Object) { *this = object(); }
    for (auto& m : members_)
        if (m.first == key) { m.second = std::move(v); return m.second; }
    members_.emplace_back(key, std::move(v));
    return members_.back().second;
}

void quote(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    out += '"';
}

void Value::dumpTo(std::string& out) const {
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: {
        if (!std::isfinite(n_)) { out += "null"; break; }
        char buf[32];
        // Integers print without exponent or fraction; others in the shortest exact form.
        if (n_ == std::floor(n_) && std::fabs(n_) < 9007199254740992.0) {
            std::snprintf(buf, sizeof(buf), "%lld", (long long)n_);
            out += buf;
        } else {
            auto r = std::to_chars(buf, buf + sizeof(buf), n_);
            out.append(buf, r.ptr);
        }
        break;
    }
    case Type::String: quote(s_, out); break;
    case Type::Array:
        out += '[';
        for (size_t i = 0; i < items_.size(); ++i) {
            if (i) out += ',';
            items_[i].dumpTo(out);
        }
        out += ']';
        break;
    case Type::Object:
        out += '{';
        for (size_t i = 0; i < members_.size(); ++i) {
            if (i) out += ',';
            quote(members_[i].first, out);
            out += ':';
            members_[i].second.dumpTo(out);
        }
        out += '}';
        break;
    }
}

std::string Value::dump() const {
    std::string s;
    dumpTo(s);
    return s;
}

// ---- Parser ----

class Parser {
public:
    Parser(const std::string& t, const Limits& l) : s_(t.data()), n_(t.size()), lim_(l) {}

    bool document(Value& out) {
        if (n_ > lim_.maxBytes) return fail("document too large");
        ws();
        if (!value(out, 0)) return false;
        ws();
        if (p_ != n_) return fail("trailing characters");
        return true;
    }
    std::string error;

private:
    const char* s_;
    size_t n_, p_ = 0, elements_ = 0;
    const Limits& lim_;

    bool fail(const char* what) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s at byte %u", what, unsigned(p_));
        error = buf;
        return false;
    }
    void ws() {
        while (p_ < n_ && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
    }
    bool literal(const char* w) {
        size_t k = 0;
        while (w[k]) {
            if (p_ + k >= n_ || s_[p_ + k] != w[k]) return fail("invalid literal");
            ++k;
        }
        p_ += k;
        return true;
    }

    bool value(Value& out, int depth) {
        if (++elements_ > lim_.maxElements) return fail("too many elements");
        if (p_ >= n_) return fail("unexpected end");
        char c = s_[p_];
        switch (c) {
        case '{': return object(out, depth + 1);
        case '[': return array(out, depth + 1);
        case '"': out.type_ = Value::Type::String; return string(out.s_);
        case 't': out = Value(true); return literal("true");
        case 'f': out = Value(false); return literal("false");
        case 'n': out = Value(); return literal("null");
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return number(out);
            return fail("unexpected character");
        }
    }

    // Members kept before the names are looked up through an index rather than one by one.
    static constexpr size_t kIndexFrom = 16;

    bool object(Value& out, int depth) {
        if (depth > lim_.maxDepth) return fail("nesting too deep");
        out = Value::object();
        ++p_;
        ws();
        if (p_ < n_ && s_[p_] == '}') { ++p_; return true; }
        // Name -> position of the kept members, once there are kIndexFrom of them (a large object
        // would otherwise cost a scan of the members before it per member: quadratic).
        std::unordered_map<std::string, size_t> index;
        for (size_t kept = 0;; ++kept) {
            ws();
            if (depth <= lim_.keepDepth && kept >= lim_.maxKept) return fail("too many members");
            if (p_ >= n_ || s_[p_] != '"') return fail("expected a member name");
            std::string key;
            if (!string(key)) return false;
            ws();
            if (p_ >= n_ || s_[p_] != ':') return fail("expected ':'");
            ++p_;
            ws();
            Value v;
            if (!value(v, depth)) return false;
            if (depth <= lim_.keepDepth) keep(out.members_, index, std::move(key), std::move(v));
            ws();
            if (p_ < n_ && s_[p_] == ',') { ++p_; continue; }
            if (p_ < n_ && s_[p_] == '}') { ++p_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    // A member of an object being read, as Value::set() stores it: a duplicate name keeps the last
    // value, at the position of the first.
    static void keep(std::vector<std::pair<std::string, Value>>& members, std::unordered_map<std::string, size_t>& index,
                     std::string key, Value v) {
        if (members.size() < kIndexFrom) {
            for (auto& m : members)
                if (m.first == key) { m.second = std::move(v); return; }
        } else {
            if (index.empty())
                for (size_t i = 0; i < members.size(); ++i) index.emplace(members[i].first, i);
            auto at = index.emplace(key, members.size());
            if (!at.second) { members[at.first->second].second = std::move(v); return; }
        }
        members.emplace_back(std::move(key), std::move(v));
    }

    bool array(Value& out, int depth) {
        if (depth > lim_.maxDepth) return fail("nesting too deep");
        out = Value::array();
        ++p_;
        ws();
        if (p_ < n_ && s_[p_] == ']') { ++p_; return true; }
        for (size_t kept = 0;; ++kept) {
            ws();
            if (depth <= lim_.keepDepth && kept >= lim_.maxKept) return fail("too many items");
            Value v;
            if (!value(v, depth)) return false;
            if (depth <= lim_.keepDepth) out.items_.push_back(std::move(v));
            ws();
            if (p_ < n_ && s_[p_] == ',') { ++p_; continue; }
            if (p_ < n_ && s_[p_] == ']') { ++p_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool hex4(uint32_t& v) {
        if (n_ - p_ < 4) return fail("truncated \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool string(std::string& out) {
        ++p_;  // opening quote
        out.clear();
        while (p_ < n_) {
            unsigned char c = (unsigned char)s_[p_];
            if (c == '"') { ++p_; return true; }
            if (c < 0x20) return fail("control character in string");
            if (c == '\\') {
                if (++p_ >= n_) return fail("truncated escape");
                char e = s_[p_++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo;
                        if (n_ - p_ < 2 || s_[p_] != '\\' || s_[p_ + 1] != 'u') return fail("lone surrogate");
                        p_ += 2;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("lone surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("lone surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("invalid escape");
                }
                continue;
            }
            if (c < 0x80) { out += char(c); ++p_; continue; }
            // Raw UTF-8 sequence: validate and copy.
            size_t len;
            uint32_t cp, min;
            if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
            else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
            else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
            else return fail("invalid UTF-8");
            if (n_ - p_ < len) return fail("invalid UTF-8");
            for (size_t k = 1; k < len; ++k) {
                unsigned char cc = (unsigned char)s_[p_ + k];
                if ((cc & 0xC0) != 0x80) return fail("invalid UTF-8");
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail("invalid UTF-8");
            out.append(s_ + p_, len);
            p_ += len;
        }
        return fail("unterminated string");
    }

    bool number(Value& out) {
        size_t start = p_;
        if (s_[p_] == '-') ++p_;
        if (p_ >= n_) return fail("invalid number");
        if (s_[p_] == '0') {
            ++p_;
        } else if (s_[p_] >= '1' && s_[p_] <= '9') {
            while (p_ < n_ && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
        } else {
            return fail("invalid number");
        }
        if (p_ < n_ && s_[p_] == '.') {
            ++p_;
            if (p_ >= n_ || s_[p_] < '0' || s_[p_] > '9') return fail("invalid number");
            while (p_ < n_ && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
        }
        if (p_ < n_ && (s_[p_] == 'e' || s_[p_] == 'E')) {
            ++p_;
            if (p_ < n_ && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
            if (p_ >= n_ || s_[p_] < '0' || s_[p_] > '9') return fail("invalid number");
            while (p_ < n_ && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
        }
        // from_chars is locale-independent and exact; a magnitude beyond double becomes +-inf.
        double v = 0;
#if defined(__ANDROID__)
        // The NDK's libc++ deletes the floating-point overload of from_chars. strtod is exact too,
        // and the game never calls setlocale: the C locale's decimal point is '.', so a JSON
        // number parses the same here as on the desktop builds.
        {
            std::string text(s_ + start, s_ + p_);
            errno = 0;
            char* end = nullptr;
            v = std::strtod(text.c_str(), &end);
            if (end != text.c_str() + text.size()) return fail("invalid number");
            if (errno == ERANGE) {
                if (v != 0.0) v = s_[start] == '-' ? -HUGE_VAL : HUGE_VAL;   // overflow
                else if (s_[start] == '-') v = -0.0;                         // underflow keeps the sign
            }
        }
        out = Value(v);
#else
        auto r = std::from_chars(s_ + start, s_ + p_, v);
        if (r.ec != std::errc() || r.ptr != s_ + p_) {
            if (r.ec != std::errc::result_out_of_range) return fail("invalid number");
            v = s_[start] == '-' ? -HUGE_VAL : HUGE_VAL;
            // Tiny values (underflow) are zero.
            for (size_t k = start; k < p_; ++k)
                if (s_[k] == 'e' || s_[k] == 'E') {
                    if (k + 1 < p_ && s_[k + 1] == '-') v = s_[start] == '-' ? -0.0 : 0.0;
                    break;
                }
        }
        out = Value(v);
#endif
        return true;
    }
};

bool parse(const std::string& text, Value& out, std::string* error, const Limits& limits) {
    Parser p(text, limits);
    Value v;
    if (!p.document(v)) {
        if (error) *error = p.error;
        return false;
    }
    out = std::move(v);
    return true;
}

}  // namespace json
}  // namespace net
