// json tree
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dnp {

class Json {
public:
    enum Type { TNull, TBool, TInt, TStr, TArr, TObj };

    Json() = default;
    static Json make_null();
    static Json make_bool(bool v);
    static Json make_int(int64_t v);
    static Json make_str(std::string v);
    static Json make_arr();
    static Json make_obj();

    Type type() const { return type_; }
    bool        is_null() const { return type_ == TNull; }
    bool        is_bool() const { return type_ == TBool; }
    bool        is_int()  const { return type_ == TInt;  }
    bool        is_str()  const { return type_ == TStr;  }
    bool        is_arr()  const { return type_ == TArr;  }
    bool        is_obj()  const { return type_ == TObj;  }

    bool                                                    as_bool() const { return b_; }
    int64_t                                                 as_int()  const { return i_; }
    const std::string&                                      as_str()  const { return s_; }
    std::vector<Json>&                                      as_arr()        { return arr_; }
    const std::vector<Json>&                                as_arr()  const { return arr_; }
    std::vector<std::pair<std::string, Json>>&              as_obj()        { return obj_; }
    const std::vector<std::pair<std::string, Json>>&        as_obj()  const { return obj_; }

    Json*       find(const std::string& key);
    const Json* find(const std::string& key) const;
    void        set(const std::string& key, Json v);
    bool        remove(const std::string& key);
    bool        parse(const std::string& text);
    bool        parse(const char* data, size_t size);
    std::string dump() const;

private:
    Type type_ = TNull;
    bool b_ = false;
    int64_t i_ = 0;
    std::string s_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

} // namespace dnp
