project('DeCross', 'cpp',
        version: '3',
        default_options: ['cpp_std=c++17', 'buildtype=release'],
        license: 'GPL-2.0-or-later',
        license_files: 'LICENSE',
        meson_version: '>=1.2.3')


sources = [
  'src/decross.cpp',
]

py = import('python').find_installation(pure: false)

incdir = include_directories(
    run_command(py, '-c', 'import vapoursynth as vs; print(vs.get_include())', check: true).stdout().strip(),
)

shared_module('decross',
    sources,
    gnu_symbol_visibility: 'hidden',
    include_directories: incdir,
    install: true,
    install_dir: py.get_install_dir() / 'vapoursynth/plugins',
    name_prefix: '',
)
