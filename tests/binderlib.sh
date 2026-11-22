#!/system/bin/sh
# Runs each binderLibTest case in its own process, from the guest. Prints one line per case:
# its name, the exit status of its process, and what gtest reported. A case still running
# after LIMIT seconds (default 60) is killed with SIGKILL. A case whose test body passed but
# whose process did not finish (it hangs in TearDown, waiting for its server) is reported as
# "body ok, no summary". The servicemanager it talks to must already be running (tests/run.sh
# starts it). Report only: see docs/boot.md, B3.
T=/data/nativetest64/binderLibTest/binderLibTest
LIMIT=${LIMIT:-60}
CASE_OUT=/data/local/tmp/binderlib.case
suite=""
$T --gtest_list_tests > /data/local/tmp/binderlib.list 2>/dev/null
while read -r line; do
    case "$line" in
    *.) suite="$line" ;;
    "") ;;
    *)
        name="$suite$(echo "$line" | tr -d ' ')"
        $T --gtest_filter="$name" > $CASE_OUT 2>&1 &
        pid=$!
        n=0
        while kill -0 $pid 2>/dev/null; do
            n=$((n + 1))
            if [ $n -gt $LIMIT ]; then
                kill -9 $pid 2>/dev/null
                break
            fi
            sleep 1
        done
        wait $pid
        rc=$?
        sum=$(grep -E '^\[  (PASSED|FAILED|SKIPPED) +\]' $CASE_OUT | head -1)
        body=$(grep -E '^\[       OK \]|^\[  FAILED  \] [A-Za-z]' $CASE_OUT | head -1)
        if [ -n "$sum" ]; then
            echo "$name rc=$rc $sum"
        elif echo "$body" | grep -q '^\[       OK'; then
            echo "$name rc=$rc body ok, no summary (hung after the body)"
        else
            echo "$name rc=$rc no result"
        fi
        ;;
    esac
done < /data/local/tmp/binderlib.list
echo binderlib-done
