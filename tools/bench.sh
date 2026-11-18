#!/bin/zsh
# usage: tools/bench.sh [seconds] [NAME=value ...]
#   Starts the app, loads save slot 1 from the title menu, then logs frame times (RT_RN_FPS) for <seconds> (default 30)
#   and prints a summary: fps and GPU time per frame. NAME=value pairs go to the game's environment (RT_METAL_PASSES=1,
#   RT_METAL_NO_CLEAR_DEFER=1, ...). Output: build/bench.log, a screenshot of the end in build/bench.png.
#
#   The intro and title are skipped by the runtime (RT_AUTO_UNTIL_LOAD presses Return until the game reads its saved
#   data); the menus after that take key presses from System Events (the save notice ignores keys posted inside the
#   process), so the game window takes the focus while this runs.
cd "${0:A:h}/.."
secs=${1:-30}; [ $# -gt 0 ] && shift
app="build/Resident Evil HD Remaster.app/Contents/MacOS/re1"
log=build/bench.log
key() { osascript -e "tell application \"System Events\" to key code $1" >/dev/null; }
pause() { perl -e "select(undef,undef,undef,$1)"; }

pkill -x re1; pause 1
env RT_LOG_STDERR=1 RT_AUTO_UNTIL_LOAD=ret RT_RN_FPS=1 "$@" "$app" > $log 2>&1 &
for i in {1..180}; do grep -q "\[auto\] saved data read" $log && break; pause 0.5; done
grep -q "\[auto\] saved data read" $log || { echo "bench: the game never reached its saved data (see $log)"; pkill -x re1; exit 1; }
pid=$(pgrep -x re1)
osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $pid) to true" >/dev/null
# Return every 2 s through the save notice, the title menu (Load game is the default) and slot 1, until the game
# loads its in-game shaders (the big shader package: the jump from the menus into the game); presses a menu isn't
# ready for are dropped, so no timing guesses
for i in {1..40}; do
    [ $(grep -c "\[rn\] shader package" $log) -ge 2 ] && break
    pause 2; key 36
done
[ $(grep -c "\[rn\] shader package" $log) -ge 2 ] || { echo "bench: never got into the game (see $log)"; }
pause 15            # "Load successful", the opening text, the room
start=$(( $(wc -l < $log) + 1 ))
pause $secs
id=$(build/winids "Resident Evil HD Remaster" | awk 'NR==1{print $1}')
[ -n "$id" ] && screencapture -x -o -l $id build/bench.png
pkill -x re1; pause 1
tail -n +$start $log | grep "\[rn\] fps" | awk '
    { g = 0; for (i = 1; i <= NF; i++) if ($i == "gpu" && !g) g = $(i + 1); n++; f += $3; gs += g; if ($3 < fmin || n == 1) fmin = $3; if (g > gmax) gmax = g }
    END { if (!n) { print "bench: no frame timings (see build/bench.log)"; exit 1 }
          printf "bench: %d samples (120 frames each): fps avg %.1f min %.1f | gpu avg %.2f ms max %.2f ms per frame\n", n, f / n, fmin, gs / n, gmax }'
echo "screenshot: build/bench.png"
