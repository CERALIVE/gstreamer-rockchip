rockchiprga_sources = [
  'gstrockchiprga.c',
  'gstrgacompositor.c',
  'gstrgaconvert.c',
  'gstrgautil.c',
  '../rockchipmpp/gstmppconversionstats.c',
  '../rockchipmpp/gstmpprgabackend.c',
  '../rockchipmpp/gstmpprgatuple.c',
]

rockchiprga_inc = include_directories('../rockchipmpp')

rockchiprga = library('gstrockchiprga',
  rockchiprga_sources,
  c_args : [gst_rockchip_args, '-Wextra', '-DHAVE_RGA',
            '-DGST_MPP_RGA_ENABLE_IM2D'],
  include_directories : [configinc, rockchiprga_inc],
  dependencies : [gstbase_dep, gstvideo_dep, gstallocators_dep, rga_dep],
  install : true,
  install_dir : plugins_install_dir,
)

gstcheck_dep = dependency('gstreamer-check-1.0', version : gst_req)
rgaconvert_check = executable('rgaconvert-check',
  ['../../tests/check/rgaconvert.c', 'gstrgaconvert.c', 'gstrgautil.c',
   '../rockchipmpp/gstmppconversionstats.c',
   '../rockchipmpp/gstmpprgabackend.c',
   '../rockchipmpp/gstmpprgatuple.c'],
  c_args : [gst_rockchip_args, '-Wextra', '-DHAVE_RGA',
            '-DGST_MPP_RGA_ENABLE_IM2D'],
  include_directories : [configinc, rockchiprga_inc],
  dependencies : [gstbase_dep, gstvideo_dep, gstallocators_dep, gstcheck_dep,
                  rga_dep])

test('rockchiprga rgaconvert negotiation and DMA-BUF transform',
  rgaconvert_check,
  depends : rockchiprga,
  workdir : meson.project_build_root(),
  is_parallel : false)

rgacompositor_check = executable('rgacompositor-check',
  ['../../tests/check/rgacompositor.c', 'gstrgacompositor.c', 'gstrgautil.c',
   '../rockchipmpp/gstmppconversionstats.c',
   '../rockchipmpp/gstmpprgabackend.c',
   '../rockchipmpp/gstmpprgatuple.c'],
  c_args : [gst_rockchip_args, '-Wextra', '-DHAVE_RGA',
            '-DGST_MPP_RGA_ENABLE_IM2D'],
  include_directories : [configinc, rockchiprga_inc],
  dependencies : [gstbase_dep, gstvideo_dep, gstallocators_dep, gstcheck_dep,
                  rga_dep])

test('rockchiprga rgacompositor layouts and DMA-BUF aggregation',
  rgacompositor_check,
  depends : rockchiprga,
  workdir : meson.project_build_root(),
  is_parallel : false)
