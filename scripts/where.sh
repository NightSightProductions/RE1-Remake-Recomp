#!/bin/bash
# Run the game under lldb until it faults; print the faulting guest instruction (from the /*@addr*/ tags in gen/).
cd "$(dirname "$0")/.."
out=$(lldb --batch -o "process handle SIGBUS -s true -p false" -o "process handle SIGSEGV -s true -p false" -o run \
      -o "bt 12" -- ./build/re1 "$@" 2>&1)
echo "$out" | grep -E "stop reason|frame #" | head -14
echo "$out" | grep -oE "code_[0-9]+\.c:[0-9]+" | head -6 | while IFS=: read f l; do
  printf '%s:%s  %s\n' "$f" "$l" "$(sed -n "${l}p" "gen/$f" | grep -oE '/\*@[0-9a-f]+\*/' | head -1)"
done
