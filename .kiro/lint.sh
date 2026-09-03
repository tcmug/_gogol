#!/bin/bash
# Usage: .kiro/lint.sh <file>
# Returns unused include warnings. Empty output = clean.
FILE="$1"
[ -z "$FILE" ] && exit 0
case "$FILE" in
  *.cpp|*.h|*.hpp|*.cc) ;;
  *) exit 0 ;;
esac
DB=""
D=$(dirname "$(cd "$(dirname "$FILE")" 2>/dev/null && pwd)/$(basename "$FILE")" 2>/dev/null || dirname "$FILE")
while [ "$D" != "/" ]; do
  [ -f "$D/build/compile_commands.json" ] && DB="$D/build" && break
  D=$(dirname "$D")
done
[ -z "$DB" ] && exit 0
/opt/homebrew/opt/llvm/bin/clang-tidy -checks='-*,misc-include-cleaner' -p "$DB" "$FILE" 2>/dev/null | grep "is not used directly" | sed 's|.*/src/|src/|'
