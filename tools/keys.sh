#!/bin/zsh
# usage: keys.sh OUT.png KEYCODE... : send key codes to the running re1 (0.6 s apart), then capture its window
out=$1; shift
pid=$(pgrep -f "build/re1 --image" | head -1)
osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $pid) to true" >/dev/null
for k in "$@"; do osascript -e "tell application \"System Events\" to key code $k" >/dev/null; sleep 0.6; done
sleep 1.5
id=$(build/winids re1 | awk 'NR==1{print $1}')
screencapture -x -o -l $id "$out" && sips -Z 500 "$out" --out "$out" >/dev/null
