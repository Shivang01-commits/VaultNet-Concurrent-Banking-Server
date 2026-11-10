#!/bin/bash
# Builds everything and runs ALL test programs.
# Usage:  ./tests/run_tests.sh      (or:  make test)
#   SHOW_SERVER_LOG=1 ./tests/run_tests.sh   shows the server's own log lines

cd "$(dirname "$0")/.." || exit 1
make -s all || { echo "BUILD FAILED"; exit 1; }
ulimit -n 65536 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null   # the server test opens ~1800 sockets

failed=0
for t in atomicity consistency isolation durability lists crash_ops server_basic server_sessions server_crash; do
    echo
    echo "=============================================================="
    if ! ./bin/test_$t; then failed=1; fi
done

echo
echo "=============================================================="
if [ $failed -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; fi
exit $failed
