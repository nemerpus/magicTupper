# rc7.9 — HOME low latency

- HOME compositor target raised from 10 FPS to 20 FPS (50 ms frame cadence, including capture time).
- caps:sc stream automatically suspends while recent GRC/H.264 frames are flowing and resumes when H.264 goes quiet.
- PC HOME takeover threshold reduced from 500 ms to 180 ms.
- MTJ1 receiver uses latest-frame-wins and ignores delayed fragments from superseded JPEG frames.
- PC accepts HOME JPEG payloads up to the sysmodule 4 MiB capture buffer.
- Stable H.264 decoder pipeline, audio and controller transport are otherwise unchanged.
