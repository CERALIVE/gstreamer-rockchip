rockchipmpp_sources = [
  'gstmpp.c',
  'gstmppallocator.c',
  'gstmppdec.c',
  'gstmppjpegdec.c',
  'gstmppvideodec.c',
  'gstmppenc.c',
  'gstmppjpegenc.c',
  'gstmpph264enc.c',
  'gstmpph265enc.c',
  'gstmppvp8enc.c',
]

if vpxalphadec
  rockchipmpp_sources += ['gstmppalphadecodebin.c', 'gstmppvpxalphadecodebin.c']
endif

if not mpp_dep.found()
  subdir_done()
endif

rockchipmpp = library('gstrockchipmpp',
  rockchipmpp_sources,
  c_args : [gst_rockchip_args, '-Wextra'],
  include_directories : configinc,
  dependencies : [gstbase_dep, gstvideo_dep, gstallocators_dep, gstpbutils_dep, mpp_dep, rga_dep],
  install : true,
  install_dir : plugins_install_dir,
)


mock_mpp = shared_library('mppmock', '../../tests/mock_mpp.c',
  c_args : ['-fvisibility=default'],
  dependencies : [glib_dep, mpp_dep], install : false)

gstcheck_dep = dependency('gstreamer-check-1.0', version : gst_req)
mpp_harness = executable('mpp-gstharness', '../../tests/mpp_gstharness.c',
  c_args : gst_rockchip_args,
  include_directories : configinc,
  dependencies : [gst_dep, gstvideo_dep, gstcheck_dep, mpp_dep],
  link_with : mock_mpp)

test('rockchipmpp GstHarness factories and caps', mpp_harness,
  depends : [rockchipmpp, mock_mpp],
  env : [
    'GST_PLUGIN_PATH=' + meson.project_build_root() / 'gst/rockchipmpp',
    'MPP_MOCK_LOG=' + meson.project_build_root() / 'mpp-mock-harness.log',
    'LD_PRELOAD=' + meson.project_build_root() / 'gst/rockchipmpp/libmppmock.so',
  ],
  workdir : meson.project_build_root(),
  is_parallel : false)

mpp_allocator_seam = executable('mpp-allocator-seam',
  ['../../tests/mpp_allocator_seam.c', 'gstmppallocator.c'],
  c_args : [gst_rockchip_args, '-Wextra'],
  include_directories : configinc,
  dependencies : [gst_dep, gstallocators_dep, gstcheck_dep, mpp_dep],
  link_with : mock_mpp,
  link_args : ['-Wl,--wrap=dup', '-Wl,--wrap=gst_fd_allocator_alloc'])

test('rockchipmpp allocator import error handling', mpp_allocator_seam,
  depends : mock_mpp,
  workdir : meson.project_build_root(),
  is_parallel : false)

decoder_seam = executable('mpp-decoder-seam', '../../tests/mpp_decoder_seam.c',
  dependencies : [gst_dep, gstvideo_dep, gstallocators_dep, gstcheck_dep, mpp_dep],
  link_with : mock_mpp)

test('rockchipmpp decoder dmabuf caps and pending-frame bound', decoder_seam,
  depends : [rockchipmpp, mock_mpp],
  env : [
    'GST_PLUGIN_PATH=' + meson.project_build_root() / 'gst/rockchipmpp',
    'MPP_MOCK_LOG=' + meson.project_build_root() / 'mpp-mock-decoder.log',
    'LD_PRELOAD=' + meson.project_build_root() / 'gst/rockchipmpp/libmppmock.so',
  ],
  workdir : meson.project_build_root(),
  is_parallel : false)

test('rockchipmpp plugin registration and properties',
  find_program('../../tests/plugin_tests.sh'),
  env : [
    'MESON_BUILD_ROOT=' + meson.project_build_root(),
    'MESON_SOURCE_ROOT=' + meson.project_source_root(),
  ],
  depends : [rockchipmpp, mock_mpp],
  workdir : meson.project_build_root(),
  is_parallel : false)
