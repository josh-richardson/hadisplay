#pragma once

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace hadisplay::json {

class Value {
  public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    Value();
    Value(std::nullptr_t);
    Value(bool value);
    Value(double value);
    Value(int value);
    Value(std::string value);
    Value(const char* value);
    Value(Array value);
    Value(Object value);

    [[nodiscard]] bool is_null() const;
    [[nodiscard]] bool is_bool() const;
    [[nodiscard]] bool is_number() const;
    [[nodiscard]] bool is_string() const;
    [[nodiscard]] bool is_array() const;
    [[nodiscard]] bool is_object() const;

    [[nodiscard]] const bool* as_bool_if() const;
    [[nodiscard]] const double* as_number_if() const;
    [[nodiscard]] const std::string* as_string_if() const;
    [[nodiscard]] const Array* as_array_if() const;
    [[nodiscard]] const Object* as_object_if() const;

    [[nodiscard]] const Value* get(std::string_view key) const;

  private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data_;
};

struct ParseResult {
    bool ok = false;
    Value value;
    std::string error;
};

ParseResult parse(const std::string& text);
std::string stringify(const Value& value);
std::string escape_string(std::string_view value);

}  // namespace hadisplay::json
