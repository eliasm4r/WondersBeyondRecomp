CREATE TABLE IF NOT EXISTS saves (
  key TEXT PRIMARY KEY,
  revision INTEGER NOT NULL,
  data BLOB NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS saves_updated_at ON saves (updated_at);
