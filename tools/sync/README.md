# Measuring sync against another Sendspin player

Ears can't resolve offsets under about 10 ms, so measure with a microphone instead. The scripts need
Python 3 with numpy; recording uses ffmpeg.

1. Generate and serve a click track (`make_clicks.py`), then play it in Music Assistant on a group
   of this player and a reference player (e.g. a Home Assistant Voice).
2. Record from a mic near both speakers:
   `ffmpeg -f avfoundation -i ":0" -t 10 -ac 1 -ar 48000 rec.wav` (macOS; `-f alsa` on Linux).
3. `python3 tools/sync/measure.py rec.wav`. This prints the gap between the two arrivals of each click.
4. Which speaker came first is ambiguous, so shift this player's latency by a known amount and
   measure again. `kill -USR1 <pid>` adds 10 ms (plays earlier) and `kill -USR2` subtracts 10 ms.
   With shifts of +30 and -30 ms, offset = (gap(+30) - gap(-30)) / 2. A positive offset means this
   player is ahead.

Each metre of extra distance from the mic to one speaker adds 2.9 ms; keep both at the same
distance or subtract it.

To find a platform's output latency below SDL (`--audio-os-latency`), measure with the latency
tuning at 0 and add the offset to the current value until the offset is about 0. That's how the
62 ms default on muOS was found; with it, and with CoreAudio's own report on macOS, this player is
within about 2 ms of a Home Assistant Voice.
