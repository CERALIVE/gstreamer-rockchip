#!/usr/bin/env bash
# 136 s allocation-contract soak: live bitrate, geometry and temporal-SVC
# changes while the encoder exercises its DMA-BUF/RGA allocation path.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tests/board/board-lib.sh
source "$ROOT/tests/board/board-lib.sh"

: "${FORK_DEB:?FORK_DEB must name the CeraLive arm64 .deb}"
readonly BUILD_IMAGE="${BOARD_BUILD_IMAGE:-localhost/gstrk-trixie-arm64}"
command -v podman >/dev/null 2>&1 || { echo 'FAIL: podman is required for the arm64 soak helper build' >&2; exit 1; }
new_report_dir d4-allocation-soak
exec > >(tee "$REPORT_DIR/transcript.log") 2>&1
board_preflight
install_deb "$FORK_DEB"

helper_dir=$(mktemp -d)
trap 'rm -rf "$helper_dir"' EXIT INT TERM
cat >"$helper_dir/allocation-soak.c" <<'C'
#include <gst/gst.h>

typedef struct {
  GMainLoop *loop;
  GstElement *pipeline;
  GstElement *encoder;
  guint64 buffers;
  gboolean failed;
} Soak;

typedef struct {
  Soak *soak;
  guint second;
  guint bitrate;
  guint width;
  guint height;
  guint layers;
} Change;

static void
on_handoff (GstElement * identity, GstBuffer * buffer, gpointer user_data)
{
  Soak *soak = user_data;
  (void) identity;
  (void) buffer;
  soak->buffers++;
}

static gboolean
apply_change (gpointer user_data)
{
  Change *change = user_data;
  g_print ("CHANGE at=%us bitrate=%u geometry=%ux%u layers=%u\n",
      change->second, change->bitrate, change->width, change->height,
      change->layers);
  g_object_set (change->soak->encoder,
      "bitrate", change->bitrate,
      "width", change->width,
      "height", change->height,
      "num-temporal-layers", change->layers, NULL);
  return G_SOURCE_REMOVE;
}

static gboolean
finish_soak (gpointer user_data)
{
  Soak *soak = user_data;
  g_main_loop_quit (soak->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
on_bus_message (GstBus * bus, GstMessage * message, gpointer user_data)
{
  Soak *soak = user_data;
  (void) bus;
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
    GError *error = NULL;
    gchar *debug = NULL;
    gst_message_parse_error (message, &error, &debug);
    g_printerr ("ELEMENT_ERROR source=%s error=%s debug=%s\n",
        GST_OBJECT_NAME (message->src), error->message,
        debug ? debug : "none");
    g_clear_error (&error);
    g_free (debug);
    soak->failed = TRUE;
    g_main_loop_quit (soak->loop);
  }
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  GError *error = NULL;
  GstBus *bus;
  GstElement *identity;
  Soak soak = { 0 };
  Change changes[] = {
    { &soak, 30, 3500000, 1280, 720, 2 },
    { &soak, 60, 8000000, 1920, 1080, 3 },
    { &soak, 90, 5000000, 960, 540, 4 },
    { &soak, 120, 6000000, 1920, 1080, 0 },
  };

  gst_init (&argc, &argv);
  soak.loop = g_main_loop_new (NULL, FALSE);
  soak.pipeline = gst_parse_launch (
      "videotestsrc is-live=true pattern=ball ! "
      "video/x-raw,format=I420,width=1920,height=1080,framerate=30/1 ! "
      "tee ! queue ! videoconvert ! video/x-raw,format=NV12 ! "
      "mpph264enc name=enc rc-mode=cbr bitrate=6000000 gop=60 qp-max=51 "
      "zero-copy-pkt=false num-temporal-layers=0 ! "
      "identity name=aus signal-handoffs=true silent=true ! h264parse ! "
      "fakesink sync=false", &error);
  if (!soak.pipeline) {
    g_printerr ("pipeline parse failed: %s\n", error->message);
    g_clear_error (&error);
    return 1;
  }

  soak.encoder = gst_bin_get_by_name (GST_BIN (soak.pipeline), "enc");
  identity = gst_bin_get_by_name (GST_BIN (soak.pipeline), "aus");
  g_signal_connect (identity, "handoff", G_CALLBACK (on_handoff), &soak);
  gst_object_unref (identity);
  bus = gst_element_get_bus (soak.pipeline);
  gst_bus_add_watch (bus, on_bus_message, &soak);
  gst_object_unref (bus);
  for (guint i = 0; i < G_N_ELEMENTS (changes); i++)
    g_timeout_add_seconds (changes[i].second, apply_change, &changes[i]);
  g_timeout_add_seconds (136, finish_soak, &soak);

  if (gst_element_set_state (soak.pipeline,
          GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("pipeline refused PLAYING\n");
    soak.failed = TRUE;
  } else {
    g_main_loop_run (soak.loop);
  }

  gst_element_set_state (soak.pipeline, GST_STATE_NULL);
  g_print ("AU_COUNT=%" G_GUINT64_FORMAT "\n", soak.buffers);
  g_print ("PIPELINE_ERRORS=%u\n", soak.failed ? 1 : 0);
  gst_object_unref (soak.encoder);
  gst_object_unref (soak.pipeline);
  g_main_loop_unref (soak.loop);
  return soak.failed || soak.buffers < 3000;
}
C
podman run --rm --platform linux/arm64 -v "$helper_dir:/work" "$BUILD_IMAGE" \
	bash -lc 'gcc -std=c11 -Wall -Wextra -Werror /work/allocation-soak.c -o /work/allocation-soak $(pkg-config --cflags --libs gstreamer-1.0)'
board_scp "$helper_dir/allocation-soak" "$BOARD_TARGET:/tmp/ceralive-allocation-soak"
board_ssh 'chmod 0755 /tmp/ceralive-allocation-soak'

since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
soak_log="$REPORT_DIR/soak.log"
if ! board_ssh 'timeout 155 /tmp/ceralive-allocation-soak' 2>&1 | tee "$soak_log"; then
	result=1
else
	result=0
fi
rga_blit=$(journal_count "$since" 'RGA_BLIT fail')
rga_api=$(journal_count "$since" 'rga_api version')
mpp_errors=$(journal_count "$since" 'mpp')
printf 'RGA_BLIT_fail=%s\nrga_api_version=%s\nmpp_journal_lines=%s\n' \
	"$rga_blit" "$rga_api" "$mpp_errors"
board_ssh 'rm -f /tmp/ceralive-allocation-soak'

[[ "$rga_blit" -eq 0 && "$rga_api" -eq 0 ]] || result=1
[[ $(grep -c '^CHANGE ' "$soak_log" || true) -eq 4 ]] || result=1
grep -q '^PIPELINE_ERRORS=0$' "$soak_log" || result=1

if [[ "$result" -ne 0 ]]; then
	seal_report FAIL || true
	exit 1
fi
seal_report PASS
