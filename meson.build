project('ghostcon', 'c',
    version: '0.1.0',
    license: 'GPL-2.0-or-later',
    default_options: [
        'c_std=c11',
        'buildtype=release',
        'warning_level=2',
    ],
)

# Dependencies
dep_ghostty_vt = dependency('libghostty-vt', version: '>=0.0')
dep_libdrm     = dependency('libdrm',     required: false)
dep_libgbm     = dependency('gbm',        required: false)
dep_egl        = dependency('egl',        required: false)
dep_glesv2     = dependency('glesv2',     required: false)
dep_libinput   = dependency('libinput',   required: false)
dep_libudev    = dependency('libudev',    required: false)
dep_xkbcommon  = dependency('xkbcommon',  required: false)
dep_fontconfig = dependency('fontconfig', required: false)
dep_freetype   = dependency('freetype2',  required: false)
dep_systemd    = dependency('libsystemd', required: false)

tomlc99_proj = subproject('tomlc99')
dep_tomlc99  = tomlc99_proj.get_variable('tomlc99_dep')

install_data('share/ghostcon.toml.example',
    install_dir: get_option('datadir') / 'ghostcon',
)

if get_option('install_systemd_unit')
    dep_systemd_pkgconfig = dependency('systemd', required: false)
    if dep_systemd_pkgconfig.found()
        # ExecStart's path must be absolute and match wherever this
        # actual configure's libexecdir resolves to (e.g. relative to a
        # non-/usr prefix, this would otherwise silently point at a path
        # nothing was installed to).
        configure_file(
            input: 'packaging/ghostcon.service.in',
            output: 'ghostcon.service',
            configuration: {'LIBEXECDIR': get_option('prefix') / get_option('libexecdir') / 'ghostcon'},
            install: true,
            install_dir: dep_systemd_pkgconfig.get_variable(pkgconfig: 'systemdsystemunitdir'),
        )
    else
        warning('systemd pkg-config module not found -- skipping ghostcon.service install (set install_systemd_unit=false to silence this)')
    endif
endif

subdir('src')
