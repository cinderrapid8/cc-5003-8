#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace {

class Parser {
public:
    Parser(const std::string& s) : s_(s) {}

    bool parse(JsonValue& out, std::string& err) {
        skip_ws();
        if (!parse_value(out)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "json parse error at byte %zu",
                          pos_);
            err = buf;
            return false;
        }
        skip_ws();
        if (pos_ != s_.size()) {
            err = "trailing bytes after JSON value";
            return false;
        }
        return true;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' ||
                s_[pos_] == '\r'))
            ++pos_;
    }

    bool parse_value(JsonValue& v) {
        skip_ws();
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '{') return parse_object(v);
        if (c == '[') return parse_array(v);
        if (c == '"') {
            std::string t;
            if (!parse_string(t)) return false;
            v = JsonValue::make_str(t);
            return true;
        }
        if (c == 't' || c == 'f') return parse_bool(v);
        if (c == 'n') return parse_null(v);
        return parse_number(v);
    }

    bool parse_object(JsonValue& v) {
        v.type = JsonValue::Type::Object;
        ++pos_;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') return false;
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            JsonValue child;
            if (!parse_value(child)) return false;
            v.obj[key] = std::move(child);
            skip_ws();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool parse_array(JsonValue& v) {
        v.type = JsonValue::Type::Array;
        ++pos_;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue child;
            if (!parse_value(child)) return false;
            v.arr.push_back(std::move(child));
            skip_ws();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool parse_string(std::string& out) {
        ++pos_; // opening quote
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return false;
                char e = s_[pos_++];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) return false;
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s_[pos_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= h - '0';
                        else if (h >= 'a' && h <= 'f') code |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') code |= h - 'A' + 10;
                        else return false;
                    }
                    if (code < 0x80) out.push_back(static_cast<char>(code));
                    else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    } else {
                        out.push_back(static_cast<char>(0xe0 | (code >> 12)));
                        out.push_back(
                            static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    }
                    break;
                }
                default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_bool(JsonValue& v) {
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            v = JsonValue::make_bool(true);
            return true;
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            v = JsonValue::make_bool(false);
            return true;
        }
        return false;
    }

    bool parse_null(JsonValue& v) {
        if (s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            v.type = JsonValue::Type::Null;
            return true;
        }
        return false;
    }

    bool parse_number(JsonValue& v) {
        size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
                s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' ||
                s_[pos_] == '+' || s_[pos_] == '-'))
            ++pos_;
        if (pos_ == start) return false;
        try {
            v = JsonValue::make_num(std::stod(s_.substr(start, pos_ - start)));
        } catch (...) {
            return false;
        }
        return true;
    }
};

std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\t': o += "\\t"; break;
        case '\r': o += "\\r"; break;
        default: o.push_back(c);
        }
    }
    return o;
}

void dump_num(std::string& o, double d) {
    if (d == static_cast<long long>(d) && std::fabs(d) < 1e15) {
        o += std::to_string(static_cast<long long>(d));
    } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.6g", d);
        o += buf;
    }
}

void dump_rec(const JsonValue& v, std::string& o) {
    switch (v.type) {
    case JsonValue::Type::Null: o += "null"; break;
    case JsonValue::Type::Bool: o += v.boolean ? "true" : "false"; break;
    case JsonValue::Type::Number: dump_num(o, v.number); break;
    case JsonValue::Type::String:
        o += '"';
        o += escape(v.str);
        o += '"';
        break;
    case JsonValue::Type::Array: {
        o += '[';
        for (size_t i = 0; i < v.arr.size(); ++i) {
            if (i) o += ',';
            dump_rec(v.arr[i], o);
        }
        o += ']';
        break;
    }
    case JsonValue::Type::Object: {
        o += '{';
        bool first = true;
        for (const auto& kv : v.obj) {
            if (!first) o += ',';
            first = false;
            o += '"';
            o += escape(kv.first);
            o += "\":";
            dump_rec(kv.second, o);
        }
        o += '}';
        break;
    }
    }
}

} // namespace

bool json_parse(const std::string& text, JsonValue& out, std::string& err) {
    Parser p(text);
    return p.parse(out, err);
}

std::string json_dump(const JsonValue& v) {
    std::string o;
    dump_rec(v, o);
    return o;
}

std::string json_get_string(const JsonValue& v, const std::string& key,
                            const std::string& def) {
    auto it = v.obj.find(key);
    if (it == v.obj.end() || it->second.type != JsonValue::Type::String)
        return def;
    return it->second.str;
}

double json_get_number(const JsonValue& v, const std::string& key,
                       double def) {
    auto it = v.obj.find(key);
    if (it == v.obj.end() || it->second.type != JsonValue::Type::Number)
        return def;
    return it->second.number;
}

bool json_get_bool(const JsonValue& v, const std::string& key, bool def) {
    auto it = v.obj.find(key);
    if (it == v.obj.end() || it->second.type != JsonValue::Type::Bool)
        return def;
    return it->second.boolean;
}
