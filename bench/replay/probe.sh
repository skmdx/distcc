#!/bin/bash
set -euo pipefail
d=$1
# These are the same environment sources used by the saved build, not a
# second compiler/options configuration maintained by the benchmark.
set +u
source "$2"
source "$3"
set -u
export DISTCC_VERBOSE=1 DISTCC_LOG="$d/client.log"
export DISTCC_HOSTS="$4" DISTCC_BROKER="$d/broker.sock"
/usr/bin/distccd --broker "$DISTCC_BROKER" > "$d/broker.log" 2>&1 &
b=$!
cleanup() {
    pump --shutdown
    kill "$b"
    wait "$b" || test "$?" = 143
}
trap cleanup EXIT
for ((i=0;i<100;i++)); do
    test -S "$DISTCC_BROKER" && break
    kill -0 "$b"
    sleep .02
done
test -S "$DISTCC_BROKER"
saved_path=$PATH
eval "$(TMPDIR="$d" pump --startup)"
# pump prepends its own masquerade directory. Keep the build's selected
# cross-compiler wrapper and verify it in probe.py before any compilation.
export PATH=$saved_path
python3 "$(dirname "$0")/probe.py" "$d"
