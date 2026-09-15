# rc7.8 — automatic HOME/game video backend

- Adds continuous caps:sc `ViLayerStack_Default` capture at a conservative 10 FPS on a dedicated Switch thread.
- Keeps the proven grc:d RTP/H.264 game pipeline unchanged.
- PC automatically prefers H.264 while decoded game frames are arriving; after 500 ms without H.264, MTJ1 JPEG HOME frames become visible.
- Returning to a game automatically restores H.264 on the first decoded frame.
- HOME stream uses the validated 4 MiB JPEG buffer and existing MTJ1 UDP fragmentation.
