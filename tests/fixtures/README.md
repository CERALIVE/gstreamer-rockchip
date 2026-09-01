# Test fixtures

## `main10-318x178-10frames.hevc`

Pinned HEVC Main 10 elementary stream for the report-only stride A/B board drill.
Its 318×178 geometry is deliberately not divisible by 16, so decode exercises
the coded-versus-aligned stride boundary. It contains ten 5 fps frames in
`yuv420p10le` and has SHA-256:

```text
dec12e046130c870a2c9a7732377a121cb0d0c8c4ab9791223a864bbf26a10f5
```

Reproduction command (FFmpeg/libx265 versions can change the encoded bytes, so a
regeneration is an explicit fixture update, not a routine build step):

```bash
ffmpeg -f lavfi -i 'testsrc2=size=318x178:rate=5' -frames:v 10 \
  -pix_fmt yuv420p10le -c:v libx265 -profile:v main10 \
  -x265-params 'lossless=1:repeat-headers=1:keyint=10:min-keyint=10:scenecut=0:pools=1' \
  -f hevc main10-318x178-10frames.hevc
```
