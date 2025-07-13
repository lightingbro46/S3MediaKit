CREATE TABLE IF NOT EXISTS "certificate" (
    "id" INTEGER PRIMARY KEY, --< "rowid" alias
    "type" TEXT NOT NULL,
    "pem" TEXT NOT NULL
);