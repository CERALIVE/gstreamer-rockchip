project('ceralive-mpi-interposer-tests', 'c', meson_version: '>=0.56',
        license: 'LGPL-2.1-or-later',
        default_options: ['warning_level=2', 'werror=true', 'c_std=c11'])

if not get_option('test-only') or get_option('buildtype') != 'debug' or get_option('b_ndebug') != 'false'
  error('MPI interposer requires standalone -Dtest-only=true --buildtype=debug -Db_ndebug=false')
endif

cc = meson.get_compiler('c')
glib = dependency('glib-2.0')
mpp = dependency('rockchip_mpp')
video = dependency('gstreamer-video-1.0')
allocators = dependency('gstreamer-allocators-1.0')
check = dependency('gstreamer-check-1.0')
dl = cc.find_library('dl')
test_args = ['-DCERALIVE_MPI_TEST_ONLY']

interposer = shared_library('mpi-test-interposer',
    ['interposer.c', 'program-target.c'], c_args: test_args,
    dependencies: [glib, mpp, video, allocators, dl], install: false)
fake = shared_library('fake-mpi', 'fake-mpi.c',
    dependencies: [glib, mpp], install: false)
unit = executable('interposer-test', 'interposer-test.c',
    dependencies: [glib, mpp], link_with: [interposer, fake], install: false)
fixture = shared_library('fixture-mpp', 'fixture-mpp.c', c_args: test_args,
    dependencies: [glib, mpp, dl], install: false)
executable('plugin-test', 'plugin-test.c',
    dependencies: [check, video, mpp], link_with: [interposer, fixture], install: false)

foreach name: ['passthrough', 'pre-submit', 'readiness', 'concurrent-one-shot', 'destroy-inflight']
  test(name, unit, args: ['-p', '/mpi/' + name],
       env: ['LD_PRELOAD=' + interposer.full_path() + ':' + fake.full_path()],
       timeout: 30)
endforeach
