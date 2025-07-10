CREATE TABLE IF NOT EXISTS "bookmark_tags" (
    "bookmark_guid"   BLOB NOT NULL,
    "name"            TEXT NOT NULL,
    PRIMARY KEY("bookmark_guid", "name"));

CREATE TABLE IF NOT EXISTS "bookmark_tag_counts" (
    "tag"     TEXT NOT NULL PRIMARY KEY,
    "count"   INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS "bookmarks" (
    "guid" BLOB NOT NULL UNIQUE PRIMARY KEY,
    "camera_guid" BLOB(16),
    "start_time" INTEGER NOT NULL,
    "duration" INTEGER NOT NULL,
    "end_time" INTEGER,
    "name" TEXT NULL,
    "description" TEXT NULL,
    "timeout" INTEGER NULL -- period of time during which the bookmarked archive part should not be deleted
, "creator_guid" BLOB(16) NULL, "created" INTEGER NULL);