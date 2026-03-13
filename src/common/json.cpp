#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace hadisplay::json {
namespace {

class Parser {
  public:
    explicit Parser(const std::string& text) : text_(text) {}

    ParseResult parse_document() {
        ParseResult result;
        skip_whitespace();
        result.value = parse_value();
        skip_whitespace();
        if (!error_.empty()) {
            result.ok = false;
            result.error = error_;
            return result;
        }
        if (pos_ != text_.size()) {
            result.ok = false;
            result.error = "Unexpected trailing characters";
            return result;
        }
        result.ok = true;
        return result;
    }

  private:
    Value parse_value() {
        if (pos_ >= text_.size()) {
            set_error("Unexpected end of input");
            return {};
        }

        const char ch = text_[pos_];
        if (ch == '{') {
            return parse_object();
        }
        if (ch == '[') {
            return parse_array();
        }
        if (ch == '"') {
            return Value(parse_string());
        }
        if (ch == 't' || ch == 'f') {
            return Value(parse_bool());
        }
        if (ch == 'n') {
            parse_null();
            return {};
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            return Value(parse_number());
        }

        set_error("Unexpected character");
        return {};
    }

    Value parse_object() {
        Object object;
        ++pos_;
        skip_whitespace();
        if (match('}')) {
            return Value(std::move(object));
        }

        while (pos_ < text_.size()) {
            if (text_[pos_] != '"') {
                set_error("Expected object key");
                return {};
            }

            std::string key = parse_string();
            skip_whitespace();
            if (!match(':')) {
                set_error("Expected ':' after object key");
                return {};
            }
            skip_whitespace();
            object.emplace(std::move(key), parse_value());
            if (!error_.empty()) {
                return {};
            }

            skip_whitespace();
            if (match('}')) {
                return Value(std::move(object));
            }
            if (!match(',')) {
                set_error("Expected ',' or '}' in object");
                return {};
            }
            skip_whitespace();
        }

        set_error("Unterminated object");
        return {};
    }

    Value parse_array() {
        Array array;
        ++pos_;
        skip_whitespace();
        if (match(']')) {
            return Value(std::move(array));
        }

        while (pos_ < text_.size()) {
            array.push_back(parse_value());
            if (!error_.empty()) {
                return {};
            }

            skip_whitespace();
            if (match(']')) {
                return Value(std::move(array));
            }
            if (!match(',')) {
                set_error("Expected ',' or ']' in array");
                return {};
            }
            skip_whitespace();
        }

        set_error("Unterminated array");
        return {};
    }

    std::string parse_string() {
        std::string out;
        ++pos_;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') {
                return out;
            }
            if (ch == '\\') {
                if (pos_ >= text_.size()) {
                    set_error("Invalid escape sequence");
                    return {};
                }
                const char escaped = text_[pos_++];
                switch (escaped) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                        for (int i = 0; i < 4; ++i) {
                            if (pos_ >= text_.size() ||
                                std::isxdigit(static_cast<unsigned char>(text_[pos_])) == 0) {
                                set_error("Invalid unicode escape");
                                return {};
                            }
                            ++pos_;
                        }
                        out.push_back('?');
                        break;
                    default:
                        set_error("Unknown escape sequence");
                        return {};
                }
                continue;
            }
            out.push_back(ch);
        }

        set_error("Unterminated string");
        return {};
    }

    bool parse_bool() {
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return false;
        }
        set_error("Invalid boolean literal");
        return false;
    }

    void parse_null() {
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return;
        }
        set_error("Invalid null literal");
    }

    double parse_number() {
        const std::size_t start = pos_;
        if (text_[pos_] == '-') {
            ++pos_;
        }

        if (pos_ >= text_.size()) {
            set_error("Invalid number");
            return 0.0;
        }

        if (text_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }

        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || std::isdigit(static_cast<unsigned char>(text_[pos_])) == 0) {
                set_error("Invalid number");
                return 0.0;
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }

        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= text_.size() || std::isdigit(static_cast<unsigned char>(text_[pos_])) == 0) {
                set_error("Invalid number");
                return 0.0;
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }

        char* end = nullptr;
        const std::string_view view(text_.data() + start, pos_ - start);
        const double parsed = std::strtod(std::string(view).c_str(), &end);
        if (end == nullptr || *end != '\0') {
            set_error("Invalid number");
            return 0.0;
        }
        return parsed;
    }

    void skip_whitespace() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool match(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void set_error(const std::string& message) {
        if (error_.empty()) {
            error_ = message;
        }
    }

    using Array = Value::Array;
    using Object = Value::Object;

    const std::string& text_;
    std::size_t pos_ = 0;
    std::string error_;
};

std::string stringify_number(double value) {
    if (std::isfinite(value) == 0) {
        return "null";
    }

    std::ostringstream stream;
    stream << std::setprecision(12) << value;
    std::string text = stream.str();
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text.empty() ? "0" : text;
}

}  // namespace

Value::Value() : data_(nullptr) {}
Value::Value(std::nullptr_t) : data_(nullptr) {}
Value::Value(bool value) : data_(value) {}
Value::Value(double value) : data_(value) {}
Value::Value(int value) : data_(static_cast<double>(value)) {}
Value::Value(std::string value) : data_(std::move(value)) {}
Value::Value(const char* value) : data_(std::string(value != nullptr ? value : "")) {}
Value::Value(Array value) : data_(std::move(value)) {}
Value::Value(Object value) : data_(std::move(value)) {}

bool Value::is_null() const {
    return std::holds_alternative<std::nullptr_t>(data_);
}

bool Value::is_bool() const {
    return std::holds_alternative<bool>(data_);
}

bool Value::is_number() const {
    return std::holds_alternative<double>(data_);
}

bool Value::is_string() const {
    return std::holds_alternative<std::string>(data_);
}

bool Value::is_array() const {
    return std::holds_alternative<Array>(data_);
}

bool Value::is_object() const {
    return std::holds_alternative<Object>(data_);
}

const bool* Value::as_bool_if() const {
    return std::get_if<bool>(&data_);
}

const double* Value::as_number_if() const {
    return std::get_if<double>(&data_);
}

const std::string* Value::as_string_if() const {
    return std::get_if<std::string>(&data_);
}

const Value::Array* Value::as_array_if() const {
    return std::get_if<Array>(&data_);
}

const Value::Object* Value::as_object_if() const {
    return std::get_if<Object>(&data_);
}

const Value* Value::get(std::string_view key) const {
    const Object* object = as_object_if();
    if (object == nullptr) {
        return nullptr;
    }
    const auto it = object->find(key);
    if (it == object->end()) {
        return nullptr;
    }
    return &it->second;
}

ParseResult parse(const std::string& text) {
    Parser parser(text);
    return parser.parse_document();
}

std::string escape_string(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20u) {
                    out += "\\u001f";
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string stringify(const Value& value) {
    if (value.is_null()) {
        return "null";
    }
    if (const bool* boolean = value.as_bool_if()) {
        return *boolean ? "true" : "false";
    }
    if (const double* number = value.as_number_if()) {
        return stringify_number(*number);
    }
    if (const std::string* string = value.as_string_if()) {
        return escape_string(*string);
    }
    if (const Value::Array* array = value.as_array_if()) {
        std::string out = "[";
        for (std::size_t i = 0; i < array->size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            out += stringify((*array)[i]);
        }
        out.push_back(']');
        return out;
    }
    if (const Value::Object* object = value.as_object_if()) {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, child] : *object) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            out += escape_string(key);
            out.push_back(':');
            out += stringify(child);
        }
        out.push_back('}');
        return out;
    }
    throw std::runtime_error("Unknown JSON value type");
}

}  // namespace hadisplay::json
