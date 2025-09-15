CREATE TABLE IF NOT EXISTS "user_entities"(
    "userId" BLOB NOT NULL UNIQUE PRIMARY KEY,
    "userName" TEXT NULL
);