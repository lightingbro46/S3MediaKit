#!/usr/bin/env bash
# Build a Debug MediaServer of another git ref for the monitor app fixture (fixture.py).
#
# Usage: build_target.sh <ref> [name]       e.g. build_target.sh develop
#   Worktree: <repo>/../S3MediaKit.worktrees/it-<name> (detached at <ref>, reset on every run).
#   Output:   <worktree>/release/linux/Debug/MediaServer, plus BUILD_REF (commit of the build).
#
# Submodules are symlinked from this checkout and the bundled FFmpeg / AWS SDK / librdkafka are
# reused from <repo>/release/linux/3rdpart (S3_DEPS_PREFIX_ROOT), so only MediaServer itself is
# compiled. That needs the ref to pin the same submodule commits as this checkout; otherwise the
# script stops (run `git submodule update` in the worktree by hand for such a ref).
#
# Env: JOBS (default: 4; 8 GB WSL runs out of memory with nproc on a Debug build)
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
REF="${1:?usage: build_target.sh <ref> [name]}"
NAME="${2:-$(echo "$REF" | tr '/' '-')}"
WT="$(cd "$REPO/.." && pwd)/S3MediaKit.worktrees/it-$NAME"
JOBS="${JOBS:-4}"

commit="$(git -C "$REPO" rev-parse --verify "$REF^{commit}")"
if [ -d "$WT" ]; then
    git -C "$WT" checkout --detach --force "$commit"
else
    git -C "$REPO" worktree add --detach "$WT" "$commit"
fi

# Same submodule pins as this checkout, or the shared 3rdpart prefix would not match.
git -C "$REPO" ls-tree -r "$commit" | awk '$2 == "commit" { print $3, $4 }' | while read -r sha path; do
    have="$(git -C "$REPO/$path" rev-parse HEAD 2>/dev/null || echo none)"
    if [ "$have" != "$sha" ]; then
        echo "submodule $path: $REF pins $sha, this checkout has $have" >&2
        exit 1
    fi
    if [ ! -L "$WT/$path" ]; then
        rm -rf "${WT:?}/$path"
        ln -s "$REPO/$path" "$WT/$path"
    fi
done

cmake -S "$WT" -B "$WT/build" -DCMAKE_BUILD_TYPE=Debug -DENABLE_AWS_SDK=ON \
    -DENABLE_SYNC_CHANGES=ON -DENABLE_TIER_STORAGE=ON -DENABLE_MOTION=ON -DENABLE_TESTS=OFF \
    -DS3_DEPS_PREFIX_ROOT="$REPO/release/linux/3rdpart" > "$WT/build.configure.log"
# The *_runtime targets copy the bundled .so next to MediaServer (RUNPATH $ORIGIN).
cmake --build "$WT/build" --target MediaServer s3_ffmpeg_runtime s3_aws_sdk_runtime -j"$JOBS"
echo "$commit $REF" > "$WT/release/linux/Debug/BUILD_REF"
echo "built $REF ($commit): $WT/release/linux/Debug/MediaServer"
