project('DeCross', 'cpp',
        version: '3',
        default_options: ['cpp_std=c++17', 'buildtype=release'],
        license: 'GPL-2.0-or-later',
        license_files: 'LICENSE',
        meson_version: '>=1.2.3')


warnings = [
  '-Wall',
  '-Wextra',
  '-Wshadow',
]

cflags = [
  warnings,
  '-fvisibility=hidden',
]

ldflags = [
]


host_cpu_family = host_machine.cpu_family()


host_system = host_machine.system()

if host_system == 'windows' or host_system == 'cygwin'
  if host_cpu_family == 'x86'
    cflags += '-mstackrealign'
    ldflags += '-Wl,--kill-at'
  endif
endif


cxx = meson.get_compiler('cpp')


sources = [
  'src/decross.cpp',
]

py = import('python').find_installation(pure: false)

# Ask the VapourSynth Python module where its headers live rather than going through
# pkg-config, which is often absent on Windows and unnecessary here: the module is installed
# alongside every VapourSynth install and knows the answer already.
incdir = include_directories(
  run_command(py, '-c', 'import vapoursynth as vs; print(vs.get_include())', check: true).stdout().strip(),
)

shared_module('decross',
              sources,
              include_directories: incdir,
              link_args: ldflags,
              cpp_args: cflags,
              install: true)
