-- user_sessions keyed rows by the raw JWT, a bearer secret. Sessions are now recorded by the
-- session_id claim of the JWT. The old rows are not copied: SQL cannot decode the claim out of the
-- base64 payload, and the table is only a session log already purged after manager.sessionExpiryDays.
DROP TABLE IF EXISTS "user_sessions";
CREATE TABLE IF NOT EXISTS "user_auth_sessions" (
    sessionId TEXT NOT NULL UNIQUE PRIMARY KEY,
    userId BLOB NOT NULL,
    creationTimeS INTEGER NOT NULL
);
