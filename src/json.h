#ifndef AEC_DEMO_JSON_H
#define AEC_DEMO_JSON_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Minimal JSON value with a strict parser: enough for the web API payloads.
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;

    static JsonValue make_bool(bool b) {
        JsonValue v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    }
    static JsonValue make_num(double d) {
        JsonValue v;
        v.type = Type::Number;
        v.number = d;
        return v;
    }
    static JsonValue make_str(std::string s) {
        JsonValue v;
        v.type = Type::String;
        v.str = std::move(s);
        return v;
    }
};

bool json_parse(const std::string& text, JsonValue& out, std::string& err);
std::string json_dump(const JsonValue& v);

// Convenience accessors with defaults for request payloads.
std::string json_get_string(const JsonValue& v, const std::string& key,
                            const std::string& def);
double json_get_number(const JsonValue& v, const std::string& key, double def);
bool json_get_bool(const JsonValue& v, const std::string& key, bool def);

#endif
