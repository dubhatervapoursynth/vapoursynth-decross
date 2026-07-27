project('DeCross', 'cpp',
        version: '3',
        default_options: ['cpp_std=c++17', 'buildtype=release'],
        license: 'GPL-2.0-or-later',
        license_files: 'LICENSE',
        meson_version: '>=1.2.3')


sources = [
  'src/decross.cpp',
]

cxx = meson.get_compiler('cpp')

py = import('python').find_installation(pure: false)

shared_module('decross',
    sources,
    gnu_symbol_visibility: 'hidden',
    include_directories: incdir,
    install: true,
    install_dir: py.get_install_dir() / 'vapoursynth/plugins',
    name_prefix: '',
)
