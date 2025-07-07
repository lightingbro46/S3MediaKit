#ifndef LOCAL_DBSCHEMA_H
#define LOCAL_DBSCHEMA_H

#include <string>
#include <vector>

namespace managerkit {

// Nullable value
template<typename T>
struct Nullable {
    bool has_value;
    T value;

    Nullable() : has_value(false) {}
    Nullable(const T& v) : has_value(true), value(v) {}

    operator bool() const { return has_value; }
    const T& operator*() const { return value; }
};

// serialize_sql_value
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
inline std::string serialize_sql_value(const Nullable<T>& val) {
    return val.has_value ? serialize_sql_value(val.value) : "NULL";
}

// parse_sql_value
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
inline Nullable<T> parse_sql_value(const std::string& s, Nullable<T>) {
    if (s == "NULL") return Nullable<T>();
    return Nullable<T>(parse_sql_value<T>(s));
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
    static std::string getPrimaryKey() {                                                                                                                   \
        return std::string(#PrimaryKey);                                                                                                                   \
    }                                                                                                                                                      \
    static std::string getPrimaryKeyValue(const EntityType &obj) {                                                                                         \
        return serialize_sql_value(obj.PrimaryKey);                                                                                                        \
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

} // namespace managerkit

#endif // LOCAL_DBSCHEMA_H
