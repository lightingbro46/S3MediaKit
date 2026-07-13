#include <gtest/gtest.h>

#include "Storage/DbSchema.h"
#include "Storage/UserEntity.h"
#include "Storage/Certification.h"

using namespace managerkit;

TEST(OptionalTest, SupportsValueCopyMoveResetAndFallback) {
    Optional<std::string> empty;
    EXPECT_FALSE(empty.has_value());
    EXPECT_EQ("fallback", empty.value_or("fallback"));
    EXPECT_THROW(empty.value(), std::logic_error);

    Optional<std::string> value("camera");
    EXPECT_TRUE(value);
    EXPECT_EQ("camera", value.value());
    Optional<std::string> copied(value);
    Optional<std::string> moved(std::move(copied));
    EXPECT_EQ("camera", moved.value());
    moved.reset();
    EXPECT_FALSE(moved);

    empty = value;
    EXPECT_EQ("camera", empty.value());
    Optional<std::string> assigned;
    assigned = Optional<std::string>("moved");
    EXPECT_EQ("moved", assigned.value());
}

TEST(DbSchemaTest, SerializesAndParsesSqlValues) {
    EXPECT_EQ("42", serialize_sql_value(42));
    EXPECT_EQ("123456789", serialize_sql_value(static_cast<int64_t>(123456789)));
    EXPECT_EQ("text", serialize_sql_value(std::string("text")));
    EXPECT_EQ("NULL", serialize_sql_value(Optional<int>()));
    EXPECT_EQ(7, parse_sql_value<int>("7", 0));
    EXPECT_EQ(0, parse_sql_value<int>("NULL", 0));
    EXPECT_EQ("quoted", parse_sql_value<std::string>("'quoted'", std::string()));
    EXPECT_FALSE(parse_sql_value("NULL", Optional<int>()).has_value());
    EXPECT_EQ(9, parse_sql_value("9", Optional<int>()).value());
}

TEST(EntityTraitsTest, ExposesColumnsValuesAndPrimaryKeys) {
    UserEntity user;
    user.userId = "u1";
    user.userName = Optional<std::string>("Alice");
    EXPECT_EQ("user_entities", EntityTraits<UserEntity>::tableName());
    EXPECT_EQ((std::vector<std::string>{"userId", "userName"}), EntityTraits<UserEntity>::getColumns());
    EXPECT_EQ((std::vector<std::string>{"u1", "Alice"}), EntityTraits<UserEntity>::getValues(user));
    EXPECT_EQ((std::vector<std::string>{"u1"}), EntityTraits<UserEntity>::getPrimaryKeyValue(user));
    EXPECT_TRUE(EntityTraits<UserEntity>::hasPrimaryKey());

    UserEntity restored = EntityTraits<UserEntity>::fromRow({"u2", "NULL"});
    EXPECT_EQ("u2", restored.userId);
    EXPECT_FALSE(restored.userName);
}

TEST(EntityTraitsTest, SupportsNumericEntitiesAndShortRows) {
    Certificate cert;
    cert.id = 5;
    cert.type = "tls";
    cert.pem = "pem";
    EXPECT_EQ((std::vector<std::string>{"5", "tls", "pem"}), EntityTraits<Certificate>::getValues(cert));
    Certificate restored = EntityTraits<Certificate>::fromRow({"6", "ca", "data"});
    EXPECT_EQ(6, restored.id);
    EXPECT_EQ("ca", restored.type);
    Certificate partial = EntityTraits<Certificate>::fromRow({"7"});
    EXPECT_EQ(7, partial.id);
}
