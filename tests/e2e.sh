#!/usr/bin/env bash
# End-to-end exercise of every CLI command against a live VNC server.
# usage: tests/e2e.sh HOST:PORT PASSWORD REMOTE_ROOT
set -uo pipefail

ENDPOINT=${1:?endpoint}
PASSWORD=${2:-}
ROOT=${3:?remote root}
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/vnxfer"
WORK=$(mktemp -d /tmp/vnxfer-e2e-XXXXXX)
PASS=0
FAIL=0

run() {
  if [ -n "$PASSWORD" ]; then "$BIN" "$@" --password "$PASSWORD"; else "$BIN" "$@"; fi
}

check() {
  local name=$1; shift
  if "$@" >/dev/null 2>&1; then
    echo "  ok   $name"; PASS=$((PASS+1))
  else
    echo "  FAIL $name"; FAIL=$((FAIL+1))
  fi
}

DIR="$ROOT/vnxfer-e2e"
FILE1="$WORK/one.bin"
head -c 524288 /dev/urandom > "$FILE1"
mkdir -p "$WORK/tree/sub"
echo hello > "$WORK/tree/a.txt"
cp "$FILE1" "$WORK/tree/sub/b.bin"

cleanup() {
  run rm "$ENDPOINT" --path "$DIR" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "== $ENDPOINT ($ROOT) =="
run rm "$ENDPOINT" --path "$DIR" >/dev/null 2>&1 || true

check "mkdir" run mkdir "$ENDPOINT" --path "$DIR"
check "put file" run put "$ENDPOINT" --local "$FILE1" --remote "$DIR/one.bin"
check "get file" run get "$ENDPOINT" --remote "$DIR/one.bin" --local "$WORK/one.out"
check "content matches" cmp "$FILE1" "$WORK/one.out"
check "put tree" run put "$ENDPOINT" --local "$WORK/tree" --remote "$DIR/tree" --recursive
check "get tree" run get "$ENDPOINT" --remote "$DIR/tree" --local "$WORK/tree.out" --recursive
check "tree matches" diff -r "$WORK/tree" "$WORK/tree.out"

# resume: upload a truncated copy, then continue with the full file
head -c 131072 "$FILE1" > "$WORK/part.bin"
check "put part" run put "$ENDPOINT" --local "$WORK/part.bin" --remote "$DIR/resume.bin"
check "put resume" run put "$ENDPOINT" --local "$FILE1" --remote "$DIR/resume.bin" --resume
check "get resumed" run get "$ENDPOINT" --remote "$DIR/resume.bin" --local "$WORK/resume.out"
check "resume matches" cmp "$FILE1" "$WORK/resume.out"

# download resume: keep a prefix locally
head -c 65536 "$FILE1" > "$WORK/prefix.bin"
check "get resume" run get "$ENDPOINT" --remote "$DIR/one.bin" --local "$WORK/prefix.bin" --resume
check "download resume matches" cmp "$FILE1" "$WORK/prefix.bin"

check "rename" run mv "$ENDPOINT" --from "$DIR/one.bin" --to "$DIR/renamed.bin"
check "renamed exists" run ls "$ENDPOINT" --path "$DIR"

check "rm file" run rm "$ENDPOINT" --path "$DIR/renamed.bin"
check "rm non-empty dir" run rm "$ENDPOINT" --path "$DIR/tree" --recursive
check "rm dir (recursive)" run rm "$ENDPOINT" --path "$DIR" --recursive

echo "-- $PASS passed, $FAIL failed --"
rm -rf "$WORK"
[ "$FAIL" -eq 0 ]
