#!/bin/sh
# Build src/commentary.ul, the test clip the SCO audio test streams to the
# headset: 16 kHz mono 8-bit mu-law (16 KB/s), max ~170 s in the 3 MB app slot.
#
#   ./make_commentary.sh                       # synthetic race commentary (say)
#   ./make_commentary.sh my_clip.m4a           # your own recording, any format
#                                            # afconvert/sox can read
#
# The chain imitates an FM broadcast feed: 50 Hz-15 kHz band, broadcast-style
# compression, normalised to -1 dBFS. mSBC then cuts it to 8 kHz bandwidth,
# which is what the headset will actually play.
set -e
cd "$(dirname "$0")"
OUT=src/commentary.ul
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
MAX_S=150

if [ -n "$1" ]; then
    # afconvert handles m4a/mp3/aac; sox takes it from a plain wav.
    afconvert -f WAVE -d LEI16@44100 -c 1 "$1" "$TMP/in.wav"
else
    cat > "$TMP/script.txt" <<'EOF'
And the lights go out, and away we go! A clean start at the front, the leader
gets away well, second place tucks in behind him into turn one.
[[slnc 600]]
Big lunge down the inside at turn four! They're side by side, wheel to wheel,
and he makes it stick! What a move that is, on the opening lap!
[[slnc 800]]
Lap three of fifty seven. The gap at the front is one point two seconds, and
the leader is setting the fastest lap of the race so far.
[[slnc 700]]
Into the pit lane now for the car in fourth. Tyres off, tyres on, two point
four seconds, a superb stop by the crew, and he rejoins just ahead of the
battle for sixth.
[[slnc 900]]
Oh, and there's a car stopped at turn eleven! Yellow flags in sector two.
We might see a safety car here. Yes, the safety car has been deployed.
[[slnc 800]]
Green flag, green flag! The restart, and the leader gets the jump, the
gap is back up to eight tenths. Into the final laps now, and this one is
going right down to the wire.
EOF
    say -v Daniel -r 200 -f "$TMP/script.txt" -o "$TMP/voice.aiff"
    sox "$TMP/voice.aiff" -r 44100 -c 1 "$TMP/voice.wav"
    LEN=$(soxi -D "$TMP/voice.wav")
    # Crowd/engine bed under the voice: filtered brown noise plus a low engine
    # drone, ~-26 dB, like a trackside commentary booth.
    sox -n -r 44100 -c 1 "$TMP/bed.wav" synth "$LEN" brownnoise \
        synth "$LEN" sine amod 0.3 30 lowpass 1200 vol 0.05
    sox -m "$TMP/voice.wav" "$TMP/bed.wav" "$TMP/in.wav"
fi

sox "$TMP/in.wav" -r 16000 -c 1 -e mu-law -b 8 -t ul "$OUT" \
    trim 0 "$MAX_S" highpass 50 lowpass 15000 \
    compand 0.01,0.3 -60,-60,-30,-15,-10,-8,0,-6 0 -90 0.05 norm -1
echo "wrote $OUT: $(wc -c < "$OUT") bytes, $(( $(wc -c < "$OUT") / 16000 )) s"
