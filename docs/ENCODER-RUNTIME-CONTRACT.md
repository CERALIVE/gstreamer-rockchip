# Encoder runtime contract

The MPP H.264 and H.265 encoders report one frame of codec work plus the
currently tracked pending depth as fixed latency. The value is recalculated
after each successful `MPP_ENC_SET_CFG` application.

Runtime failures from frame submission or packet retrieval trigger a complete
MPP context restart unless MPP reports its non-blocking timeout/no-capacity
result. Recovery drains and destroys the old context, creates and initializes a
new one, and reapplies the complete retained configuration. Three restarts are
allowed in any ten-second window. A fourth failure posts
`GST_STREAM_ERROR_ENCODE` with `encoder restart budget exhausted`. The
read-only `encoder-restarts` property reports successful restarts over the
element lifetime.

Negotiated BT.601, BT.709, and BT.2020 colorimetry is written to MPP's
`prep:colorspace`, `prep:colorprim`, `prep:colortrc`, and `prep:range` keys.
Full range maps to MPP's JPEG range and limited range maps to MPEG range. Caps
without an explicit `colorimetry` field write no color keys; renegotiation from
specified to unspecified starts from a fresh MPP config so old values cannot
leak into the new stream.

Both upstream and downstream `GstForceKeyUnit` events request
`MPP_ENC_SET_IDR_FRAME` for the next submitted frame. With
`header-mode=each-idr`, that access unit also carries `GST_BUFFER_FLAG_HEADER`.
The software test backend verifies the control call and parses an H.264 type-5
NAL from a `videotestsrc` pipeline. Real H.264/H.265 bitstream and SPS VUI
verification with `ffprobe -show_streams` remains the board-gated todo 36; this
contract does not claim that hardware result.

The encoders are **PTS-only (no B-frames); DTS = PTS**. Every output frame sets
DTS explicitly rather than relying on an unset or inherited timestamp.
