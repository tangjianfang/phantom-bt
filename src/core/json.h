#pragma once
// 极简 JSON 解析（Profile 加载用）
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pb::json {

struct Value {
  enum Type { Null, Bool, Number, String, Array, Object };
  Type type = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Value> arr;
  std::map<std::string, Value> obj;

  const Value* find(const char* key) const {
    if (type != Object) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
  }
  std::string as_str() const { return type == String ? str : ""; }
  double as_num() const { return type == Number ? num : 0; }
  int as_int() const { return (int)as_num(); }
  bool as_bool() const { return type == Bool ? b : false; }
};

Value parse(std::string_view text);  // 抛 std::runtime_error("json:<pos>: <原因>")

}  // namespace pb::json
