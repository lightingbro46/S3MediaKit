---
name: export-feature
description: "Use when the user wants to export/package committed changes for transfer to another machine with tools/git-patch/export-feature.sh (xuất gói tính năng, export feature, đóng gói patch). Takes the start commit, names the output itself and prints a copy-ready list of the changed features."
argument-hint: "<commit-bắt-đầu>"
disable-model-invocation: true
---

# Export feature package

Chạy `tools/git-patch/export-feature.sh` để đóng gói bản cuối của các file đã đổi từ một commit
tới `HEAD`, rồi đưa cho user danh sách tính năng để copy. Trả lời user bằng tiếng Việt.

Đầu vào: `$ARGUMENTS` = commit bắt đầu (hash, tag hoặc ref).

## 1. Lấy commit bắt đầu

- Nếu `$ARGUMENTS` rỗng: hỏi user commit bắt đầu (dùng AskUserQuestion, gợi ý vài commit gần
  nhất từ `git log --oneline -10` làm lựa chọn) rồi dừng chờ trả lời.
- Kiểm tra commit tồn tại: `git cat-file -e "<commit>^{commit}"`. Sai thì báo và dừng.

**Commit bắt đầu được tính vào gói.** Script lấy thay đổi *sau* baseline (`git diff <baseline>
HEAD`), nên baseline truyền cho script là `<commit>^`. Ngoại lệ: user nói rõ "sau commit X"/"từ
sau X" thì baseline là chính `X`. Nếu `<commit>` là root commit (không có `^`), báo user và dừng.

## 2. Kiểm tra trước khi chạy

- `git status --short`: script từ chối khi có thay đổi tracked chưa commit. Nếu có, liệt kê file
  và dừng — **không** tự stash, commit hay reset. File untracked (vd. symlink `3rdpart/aws-sdk`)
  chỉ bị cảnh báo và không vào gói, chạy tiếp được.
- Xem danh sách commit sẽ vào gói:
  `git log --reverse --no-merges --format='%h %s' <baseline>..HEAD`.
  Rỗng thì báo "không có thay đổi" và dừng.

## 3. Tự đặt tên đầu ra

Đọc subject (và nếu cần là `git show --stat`) của các commit ở bước 2, đặt một slug:

- kebab-case, ASCII không dấu, 2–4 từ, nói chủ đề chính: `extract-job`, `hls-playback`,
  `backchannel-refactor`, `overlay-replay-gop`. Nhiều chủ đề không liên quan thì lấy chủ đề lớn
  nhất hoặc gom chung (`ffmpeg-threading-overlay`).
- Không được trùng gói đã có: nếu `tools/git-patch/output/transfer-<slug>.tar.gz` đã tồn tại
  (script sẽ ghi đè), thêm hậu tố ngày `-YYYYMMDD`, còn trùng nữa thì `-2`, `-3`…

Không hỏi lại user về tên; chỉ báo tên đã chọn ở kết quả.

## 4. Chạy script

```bash
./tools/git-patch/export-feature.sh <baseline> <slug>
```

Chạy từ gốc repo. Nếu script lỗi, đưa nguyên dòng `[ERROR]` cho user và dừng, không sửa script.
Thành công thì archive nằm ở `tools/git-patch/output/transfer-<slug>.tar.gz`.

## 5. Báo kết quả

Ngắn gọn, theo thứ tự:

1. Đường dẫn archive, baseline (`<commit>^` = hash ngắn) và `HEAD` hash ngắn, số commit, số file
   (lấy từ `git diff --shortstat <baseline> HEAD`). Nhắc cảnh báo untracked nếu script in ra.
2. **Danh sách tính năng** trong một khối ```text để user copy thẳng:

```text
Gói: transfer-<slug>.tar.gz (<baseline ngắn>..<HEAD ngắn>, N commit)

1. <Tính năng/sửa lỗi 1>: <một câu mô tả hành vi thay đổi>
2. <Tính năng/sửa lỗi 2>: ...
```

Quy tắc viết danh sách:

- Mỗi mục là một **tính năng hoặc lỗi được sửa**, không phải một commit: gộp các commit cùng chủ
  đề, bỏ commit chỉ sửa docs/test/format (trừ khi cả gói chỉ có vậy).
- Commit message trong repo này rất dài; rút mỗi mục còn 1–2 câu tiếng Việt, nói hành vi người
  dùng/vận hành thấy được (API mới, lỗi gì hết, cấu hình mới + mặc định), bỏ tên hàm/biến nội bộ
  trừ khi đó là tên API hoặc key cấu hình.
- Có migration mới (`migration/**.sql`) hoặc key cấu hình mới thì ghi rõ trong mục tương ứng, vì
  máy nhận cần biết.
- Không bịa: chỉ viết điều có trong commit message hoặc diff.

Sau khối đó, nhắc lệnh import bên máy nhận:
`./tools/git-patch/import-feature.sh tools/git-patch/output/transfer-<slug>.tar.gz`.
