-- Cleanup to allow re-run safely
DROP TRIGGER IF EXISTS bookmark_ai;
DROP TRIGGER IF EXISTS bookmark_ad;
DROP TRIGGER IF EXISTS bookmark_au;
DROP TRIGGER IF EXISTS bookmark_tag_ai;
DROP TRIGGER IF EXISTS bookmark_tag_ad;
DROP TRIGGER IF EXISTS bookmark_tag_au;
DROP TABLE IF EXISTS bookmark_fts;
DROP TABLE IF EXISTS bookmark_tag_fts;

-- Virtual table cho bookmark
CREATE VIRTUAL TABLE bookmark_fts USING fts5(
  guid UNINDEXED,
  description, 
  name, 
  tokenize='trigram'
);

-- Virtual table cho tags
CREATE VIRTUAL TABLE bookmark_tag_fts USING fts5(
  bookmark_guid UNINDEXED,
  tag,
  tokenize='trigram'
);

-- Backfill existing data (align rowid to allow efficient delete/update)
INSERT INTO bookmark_fts(rowid, guid, description, name)
  SELECT rowid, guid, COALESCE(description, ''), COALESCE(name, '') FROM bookmarks;

INSERT INTO bookmark_tag_fts(rowid, bookmark_guid, tag)
  SELECT rowid, bookmark_guid, COALESCE(name, '') FROM bookmark_tags;

-- Optional: optimize FTS indexes after bulk load
INSERT INTO bookmark_fts(bookmark_fts) VALUES('optimize');
INSERT INTO bookmark_tag_fts(bookmark_tag_fts) VALUES('optimize');


-- Trigger bookmark khi có bản ghi mới được thêm
CREATE TRIGGER bookmark_ai AFTER INSERT ON bookmarks BEGIN
  INSERT INTO bookmark_fts(rowid, guid, description, name)
    VALUES (new.rowid, new.guid, COALESCE(new.description, ''), COALESCE(new.name, ''));
END;

-- Trigger bookmark khi có bản ghi bị xóa
CREATE TRIGGER bookmark_ad AFTER DELETE ON bookmarks BEGIN
  DELETE FROM bookmark_fts WHERE rowid = old.rowid;
END;

-- Trigger bookmark khi có bản ghi được cập nhật
CREATE TRIGGER bookmark_au AFTER UPDATE ON bookmarks BEGIN
  DELETE FROM bookmark_fts WHERE rowid = old.rowid;
  INSERT INTO bookmark_fts(rowid, guid, description, name)
    VALUES (new.rowid, new.guid, COALESCE(new.description, ''), COALESCE(new.name, ''));
END;


-- Thêm mới tag
CREATE TRIGGER bookmark_tag_ai AFTER INSERT ON bookmark_tags BEGIN
  INSERT INTO bookmark_tag_fts(rowid, bookmark_guid, tag)
    VALUES (new.rowid, new.bookmark_guid, COALESCE(new.name, ''));
END;

-- Xóa tag
CREATE TRIGGER bookmark_tag_ad AFTER DELETE ON bookmark_tags BEGIN
  DELETE FROM bookmark_tag_fts WHERE rowid = old.rowid;
END;

-- Cập nhật tag
CREATE TRIGGER bookmark_tag_au AFTER UPDATE ON bookmark_tags BEGIN
  DELETE FROM bookmark_tag_fts WHERE rowid = old.rowid;
  INSERT INTO bookmark_tag_fts(rowid, bookmark_guid, tag)
    VALUES (new.rowid, new.bookmark_guid, COALESCE(new.name, ''));
END;