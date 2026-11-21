#!/bin/sh
# Instructions a 128-frame block takes, counted by callgrind in Melodee's profiler image (its build image plus valgrind),
# for each bench mode. A block at 48 kHz is 2,667 us; on the MPC's 1.8 GHz Cortex-A55 that is about 4.8 million cycles,
# so the share printed assumes about one instruction a cycle (the A55 is in-order).
#   tools/bench.sh [SECONDS]
set -eu
cd "$(dirname "$0")/.."
CONTEXT=${DOCKER_CONTEXT:-colima-melodee}
SECONDS_RUN=${1:-4}
docker --context "$CONTEXT" run --rm --platform linux/arm64 -v "$PWD:/work" -w /work melodee-profiler sh -c "
    make -s build/bench >/dev/null
    for mode in worst pad typical silent paint; do
        build/bench \$mode $SECONDS_RUN
        blocks=\$(awk -v s=$SECONDS_RUN 'BEGIN { printf \"%d\", s * 48000 / 128 }')
        if [ \$mode = paint ]; then
            # The page's own cost: a paint run less a pad run of the same length (the paint run plays the pad; and its 40 warm-up
            # blocks), over its frames. (callgrind's --toggle-collect misses the inlined paint.)
            p=\$(valgrind --tool=callgrind --callgrind-out-file=/dev/null build/bench paint $SECONDS_RUN 2>&1 | grep -o 'refs: *[0-9,]*' | tr -d 'refs: ,')
            t=\$(valgrind --tool=callgrind --callgrind-out-file=/dev/null build/bench pad $SECONDS_RUN 2>&1 | grep -o 'refs: *[0-9,]*' | tr -d 'refs: ,')
            w=\$(valgrind --tool=callgrind --callgrind-out-file=/dev/null build/bench pad 0 2>&1 | grep -o 'refs: *[0-9,]*' | tr -d 'refs: ,')
            awk -v p=\$p -v t=\$t -v w=\$w -v n=\$blocks 'BEGIN { frames = int ((n + 14) / 15); printf \"%-8s %9.0f instructions a painted frame (PLAITS page, sounding, 25 frames a second)\\n\", \"paint\", (p - t) / frames }'
            continue
        fi
        refs=\$(valgrind --tool=callgrind --callgrind-out-file=build/callgrind.\$mode build/bench \$mode $SECONDS_RUN 2>&1 | grep -o 'refs: *[0-9,]*' | tr -d 'refs: ,')
        base=\$(valgrind --tool=callgrind --callgrind-out-file=/dev/null build/bench \$mode 0 2>&1 | grep -o 'refs: *[0-9,]*' | tr -d 'refs: ,')
        if false; then :
        else
            awk -v r=\$refs -v b=\$base -v n=\$blocks -v m=\$mode 'BEGIN { per = (r - b) / n; printf \"%-8s %9.0f instructions a block, %5.2f %% of a block on one A55 core at 1 IPC\\n\", m, per, per / 4800000 * 100 }'
        fi
    done"
