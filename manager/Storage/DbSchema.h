#ifndef LOCAL_DBSCHEMA_H
#define LOCAL_DBSCHEMA_H

#include <string>
#include <vector>

namespace managerkit {

// Nullable value
template<typename T>
struct Optional {
    bool has_value_;
    T value_;

    Optional() : has_value_(false), value_() {}

    Optional(const T& value) : has_value_(true), value_(value) {}

    Optional(T&& value) : has_value_(true), value_(std::move(value)) {}

    Optional(const Optional& other) : has_value_(other.has_value_) {
        if (has_value_) value_ = other.value_;
    }

    Optional(Optional&& other) noexcept : has_value_(other.has_value_) {
        if (has_value_) value_ = std::move(other.value_);
    }

    Optional& operator=(const Optional& other) {
        if (this != &other) {
            has_value_ = other.has_value_;
            if (has_value_) value_ = other.value_;
        }
        return *this;
    }

    Optional& operator=(Optional&& other) noexcept {
        if (this != &other) {
            has_value_ = other.has_value_;
            if (has_value_) value_ = std::move(other.value_);
        }
        return *this;
    }

    void reset() {
        has_value_ = false;
        value_ = T{};
    }

    bool has_value() const {
        return has_value_;
    }

    explicit operator bool() const {
        return has_value_;
    }

    T& value() {
        if (!has_value_) throw std::logic_error("bad Optional access");
        return value_;
    }

    const T& value() const {
        if (!has_value_) throw std::logic_error("bad Optional access");
        return value_;
    }

    T value_or(const T& default_value) const {
        return has_value_ ? value_ : default_value;
    }
};

// ===============================
// Serialize value to string
// ===============================
template<typename FieldType>
inline std::string serialize_sql_value(const FieldType& val);

template<>
inline std::string serialize_sql_value(const int& val) {
    return std::to_string(val);
}

template<>
inline std::string serialize_sql_value(const int64_t& val) {
    return std::to_string(val);
}

template<>
inline std::string serialize_sql_value(const std::string& val) {
    return val;
}

template<typename T>
inline std::string serialize_sql_value(const Optional<T>& val) {
    return val.has_value() ? serialize_sql_value(val.value()) : "NULL";
}

// ===============================
// Parse value from string
// ===============================
template<typename FieldType>
inline FieldType parse_sql_value(const std::string& s, FieldType);

template<>
inline int parse_sql_value<int>(const std::string& s, int) {
    return std::stoi(s);
}

template<>
inline int64_t parse_sql_value<int64_t>(const std::string& s, int64_t) {
    return std::stol(s);
}

template<>
inline std::string parse_sql_value<std::string>(const std::string& s, std::string) {
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

template<typename T>
inline Optional<T> parse_sql_value(const std::string& s, Optional<T> mem) {
    if (s == "NULL") return Optional<T>();
    return Optional<T>(parse_sql_value<T>(s, T{}));
}

// Macro DECLARE_ENTITY

template<class T>
struct EntityTraits;

#define DECLARE_ENTITY(EntityType, TableName, PrimaryKey, ...)                                                                                             \
template <>                                                                                                                                                \
struct EntityTraits<EntityType> {                                                                                                                          \
    static std::string tableName() {                                                                                                                       \
        return TableName;                                                                                                                                  \
    }                                                                                                                                                      \
    static std::vector<std::string> getColumns() {                                                                                                         \
        return getColumnsImpl(__VA_ARGS__);                                                                                                                \
    }                                                                                                                                                      \
    static std::vector<std::string> getValues(const EntityType &obj) {                                                                                     \
        return getValuesImpl(obj, __VA_ARGS__);                                                                                                            \
    }                                                                                                                                                      \
    static std::vector<std::string> getPrimaryKey() {                                                                                                      \
        return PrimaryKey;                                                                                                                                 \
    }                                                                                                                                                      \
    static std::vector<std::string> getPrimaryKeyValue(const EntityType &obj) {                                                                            \
        return getPrimaryKeyValuesImpl(obj, PrimaryKey, __VA_ARGS__);                                                                                      \
    }                                                                                                                                                      \
    static EntityType fromRow(const std::vector<std::string> &row) {                                                                                       \
        return fromVectorImpl<EntityType>(row, __VA_ARGS__);                                                                                               \
    }                                                                                                                                                      \
};

// getColumnsImpl 
// Base case
inline void getColumnsImpl(std::vector<std::string>& result) {}

// Recursive 
template<typename Member, typename... Rest>
void getColumnsImpl(std::vector<std::string>& result, Member, const char* colName, Rest... rest) {
    result.push_back(colName);
    getColumnsImpl(result, rest...);
}

// Entry
template<typename... Args>
std::vector<std::string> getColumnsImpl(Args... args) {
    std::vector<std::string> result;
    getColumnsImpl(result, args...);
    return result;
}

// getValuesImpl 
// Base case
template<typename T>
void getValuesImpl(const T&, std::vector<std::string>& result) {}

// Recursive 
template<typename Entity, typename Member, typename... Rest>
void getValuesImpl(const Entity& obj, std::vector<std::string>& result, Member Entity::*member, const char* , Rest... rest) {
    result.push_back(serialize_sql_value(obj.*member));
    getValuesImpl(obj, result, rest...);
}

// Entry
template<typename Entity, typename... Args>
std::vector<std::string> getValuesImpl(const Entity& obj, Args... args) {
    std::vector<std::string> result;
    getValuesImpl(obj, result, args...);
    return result;
}

// parseRowImpl
// Base case
template<typename Entity>
inline void parseRowImpl(const std::vector<std::string>&, size_t&, Entity&) {}

// Recursive 
template<typename Entity, typename Member, typename... Rest>
void parseRowImpl(const std::vector<std::string>& row, size_t& i, Entity& obj, Member Entity::*member, const char*, Rest... rest) {
    if (i < row.size()) {
        obj.*member = parse_sql_value(row[i], obj.*member);
    }
    ++i;
    parseRowImpl(row, i, obj, rest...);
}

// Entry
template<typename Entity, typename... Args>
Entity fromVectorImpl(const std::vector<std::string>& row, Args... args) {
    Entity obj;
    size_t i = 0;
    parseRowImpl(row, i, obj, args...);
    return obj;
}

template<typename Entity, typename... Args>
std::vector<Entity> fromVector(const std::vector<std::vector<std::string>>& rows, Args... args) {
    std::vector<Entity> result;
    for (const auto& row : rows) {
        result.push_back(fromVectorImpl<Entity>(row, args...));
    }
    return result;
}

// getPrimaryKeyValuesImpl
template<typename Entity, typename... Args>
std::vector<std::string> getPrimaryKeyValuesImpl(const Entity& obj, const std::vector<std::string>& primaryKeys, Args... args) {
    std::vector<std::string> result;
    auto cols = getColumnsImpl(args...);
    auto values = getValuesImpl(obj, args...);
    for (size_t i = 0; i < primaryKeys.size(); ++i) {
        for (size_t j = 0; j < cols.size(); ++j) {
            if (cols[j] == primaryKeys[i]) {
                result.push_back(values[j]);
                break;
            }
        }
    }
    return result;
}

} // namespace managerkit

#endif // LOCAL_DBSCHEMA_H
