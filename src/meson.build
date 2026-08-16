# Config (TOML, loaded only by undead-head — see config.h's doc comment)
config_lib = static_library('ghostcon-config',
    'config/config.c',
    include_directories: include_directories('../include'),
    dependencies: [dep_tomlc99],
    install: false,
)

if not meson.is_subproject()
    test_config_exe = executable('test_config',
        '../tests/test_config.c',
        include_directories: include_directories('../include'),
        link_with: config_lib,
        dependencies: [dep_tomlc99],
    )
    test('test_config', test_config_exe)
endif

# Terminal core (Phase 0)
term_srcs = files(
    'term/cell.c',
    'term/row.c',
    'term/style.c',
    'term/color.c',
    'term/kitty.c',
    'term/hyperlink.c',
    'term/selection.c',
    'term/screen.c',
    'term/stream.c',
    'term/term.c',
)

term_lib = static_library('ghostcon-term',
    term_srcs,
    include_directories: include_directories('../include'),
    dependencies: [dep_ghostty_vt],
    install: false,
)

if not meson.is_subproject()
    ghostcon_dump_exe = executable('ghostcon_dump',
        '../tools/ghostcon_dump.c',
        include_directories: include_directories('../include'),
        link_with: term_lib,
        dependencies: [dep_ghostty_vt],
    )

    test_exe = executable('test_term',
        '../tests/test_term.c',
        include_directories: include_directories('../include'),
        link_with: term_lib,
        dependencies: [dep_ghostty_vt],
    )
    test('test_term', test_exe)

    conformance_exe = executable('test_conformance',
        '../tests/test_conformance.c',
        include_directories: include_directories('../include'),
        link_with: term_lib,
        dependencies: [dep_ghostty_vt],
    )
    test('test_conformance', conformance_exe)

    test_osc_exe = executable('test_osc',
        '../tests/test_osc.c',
        include_directories: include_directories('../include'),
        link_with: term_lib,
        dependencies: [dep_ghostty_vt],
    )
    test('test_osc', test_osc_exe)
endif

# Renderer (Phase 1 item 2)
render_srcs = files(
    'render/atlas.c',
    'render/box_draw.c',
    'render/gles.c',
    'render/machine.c',
)

render_lib = static_library('ghostcon-render',
    render_srcs,
    include_directories: include_directories('../include'),
    dependencies: [dep_fontconfig, dep_freetype, dep_glesv2],
    link_with: term_lib,
    install: false,
)

core_egl_lib = static_library('ghostcon-core-egl',
    'core/egl.c',
    include_directories: include_directories('../include'),
    dependencies: [dep_egl, dep_libgbm, dep_libdrm],
    install: false,
)

# core/kms.c — compile-checked only, not runtime-tested; see kms.h's
# doc comment and PLAN.md's Phase 1 item 2 note for why.
core_kms_lib = static_library('ghostcon-core-kms',
    'core/kms.c',
    include_directories: include_directories('../include'),
    dependencies: [dep_libdrm, dep_libgbm],
    install: false,
)

# core/vtctl.c — compile-checked only, not runtime-tested; see vtctl.h's
# doc comment and PLAN.md's Phase 1 item 3 note for why.
core_vtctl_lib = static_library('ghostcon-core-vtctl',
    'core/vtctl.c',
    include_directories: include_directories('../include'),
    install: false,
)

if not meson.is_subproject()
    test_render_exe = executable('test_render',
        '../tests/test_render.c',
        include_directories: include_directories('../include'),
        link_with: [render_lib, core_egl_lib, term_lib],
        dependencies: [dep_ghostty_vt, dep_fontconfig, dep_freetype,
                       dep_glesv2, dep_egl, dep_libgbm, dep_libdrm],
    )
    test('test_render', test_render_exe)

    test_box_draw_exe = executable('test_box_draw',
        '../tests/test_box_draw.c',
        include_directories: include_directories('../include'),
        link_with: [render_lib],
        dependencies: [dep_fontconfig, dep_freetype, dep_glesv2],
    )
    test('test_box_draw', test_box_draw_exe)
endif

# ptyserv (Phase 1)
ptyserv_protocol_lib = static_library('ghostcon-ptyserv-protocol',
    'ptyserv/protocol.c',
    include_directories: include_directories('../include'),
    install: false,
)

pty_child_exe = executable('pty-ttyN',
    'ptyserv/pty_child.c',
    include_directories: include_directories('../include'),
    link_with: ptyserv_protocol_lib,
    install: true,
    install_dir: get_option('libexecdir') / 'ghostcon',
)

ghost_ptyserv_exe = executable('ghost-ptyserv',
    'ptyserv/main.c',
    include_directories: include_directories('../include'),
    link_with: ptyserv_protocol_lib,
    install: true,
    install_dir: get_option('libexecdir') / 'ghostcon',
)

if not meson.is_subproject()
    test_ptyserv_exe = executable('test_ptyserv',
        '../tests/test_ptyserv.c',
        include_directories: include_directories('../include'),
        link_with: ptyserv_protocol_lib,
    )
    test('test_ptyserv', test_ptyserv_exe,
        env: {
            'GHOSTCON_PTYSERV_BIN': ghost_ptyserv_exe.full_path(),
            'GHOSTCON_PTY_CHILD_BIN': pty_child_exe.full_path(),
            'GHOSTCON_PTY_SKIP_LOGIN': '1',
        },
        depends: [ghost_ptyserv_exe, pty_child_exe],
        timeout: 30,
    )
endif

# core/transport.c (Phase 1 item 4) — pty child socket client
core_transport_lib = static_library('ghostcon-core-transport',
    'core/transport.c',
    include_directories: include_directories('../include'),
    link_with: ptyserv_protocol_lib,
    install: false,
)

# core/diag.c (Phase 1 item 4) — crash self-reporting to journald
core_diag_lib = static_library('ghostcon-core-diag',
    'core/diag.c',
    include_directories: include_directories('../include'),
    dependencies: [dep_systemd],
    install: false,
)

if not meson.is_subproject()
    test_transport_exe = executable('test_transport',
        '../tests/test_transport.c',
        include_directories: include_directories('../include'),
        link_with: [core_transport_lib, ptyserv_protocol_lib],
    )
    test('test_transport', test_transport_exe,
        env: {
            'GHOSTCON_PTYSERV_BIN': ghost_ptyserv_exe.full_path(),
            'GHOSTCON_PTY_CHILD_BIN': pty_child_exe.full_path(),
            'GHOSTCON_PTY_SKIP_LOGIN': '1',
        },
        depends: [ghost_ptyserv_exe, pty_child_exe],
        timeout: 30,
    )

    test_diag_exe = executable('test_diag',
        '../tests/test_diag.c',
        include_directories: include_directories('../include'),
        link_with: core_diag_lib,
        dependencies: [dep_systemd],
    )
    test('test_diag', test_diag_exe, timeout: 30)
endif

# core/input.c (Phase 1 item 5) — libinput + xkbcommon + key encoding
core_input_lib = static_library('ghostcon-core-input',
    'core/input.c',
    include_directories: include_directories('../include'),
    dependencies: [dep_ghostty_vt, dep_libinput, dep_libudev, dep_xkbcommon],
    link_with: [term_lib, core_transport_lib, ptyserv_protocol_lib],
    install: false,
)

if not meson.is_subproject()
    test_input_exe = executable('test_input',
        '../tests/test_input.c',
        include_directories: include_directories('../include'),
        link_with: [core_input_lib, core_transport_lib, ptyserv_protocol_lib],
        dependencies: [dep_ghostty_vt, dep_libinput, dep_libudev, dep_xkbcommon],
    )
    test('test_input', test_input_exe)
endif

# core/main.c (Phase 1 item 6) — ghostcon-core entry point
ghostcon_core_exe = executable('ghostcon-core',
    'core/main.c',
    include_directories: include_directories('../include'),
    link_with: [
        core_vtctl_lib, core_kms_lib, core_egl_lib, core_transport_lib,
        core_input_lib, core_diag_lib, render_lib, term_lib, ptyserv_protocol_lib,
    ],
    dependencies: [
        dep_ghostty_vt, dep_libdrm, dep_libgbm, dep_egl, dep_glesv2,
        dep_libinput, dep_libudev, dep_xkbcommon, dep_systemd,
        dep_fontconfig, dep_freetype,
    ],
    install: true,
    install_dir: get_option('libexecdir') / 'ghostcon',
)

# supervisor (Phase 1 item 7)
supervisor_exe = executable('supervisor',
    'supervisor/main.c',
    install: true,
    install_dir: get_option('libexecdir') / 'ghostcon',
    include_directories: include_directories('../include'),
    link_with: ptyserv_protocol_lib,
)

# undead-head (Phase 1 item 7)
undead_head_exe = executable('undead-head',
    'undead_head/main.c',
    include_directories: include_directories('../include'),
    link_with: config_lib,
    dependencies: [dep_tomlc99],
    install: true,
    install_dir: get_option('libexecdir') / 'ghostcon',
)

if not meson.is_subproject()
    fake_renderer_exe = executable('test_fake_renderer',
        '../tests/fake_renderer.c',
        include_directories: include_directories('../include'),
    )

    test_supervisor_exe = executable('test_supervisor',
        '../tests/test_supervisor.c',
        include_directories: include_directories('../include'),
    )
    test('test_supervisor', test_supervisor_exe,
        env: {
            'GHOSTCON_CORE_BIN': fake_renderer_exe.full_path(),
            'SUPERVISOR_BIN': supervisor_exe.full_path(),
        },
        depends: [fake_renderer_exe, supervisor_exe],
        timeout: 30,
    )

    test_undead_head_exe = executable('test_undead_head',
        '../tests/test_undead_head.c',
        include_directories: include_directories('../include'),
        link_with: ptyserv_protocol_lib,
    )
    test('test_undead_head', test_undead_head_exe,
        env: {
            'UNDEAD_HEAD_BIN': undead_head_exe.full_path(),
            'GHOSTCON_PTYSERV_BIN': ghost_ptyserv_exe.full_path(),
            'GHOSTCON_SUPERVISOR_BIN': supervisor_exe.full_path(),
            'GHOSTCON_CORE_BIN': fake_renderer_exe.full_path(),
            'GHOSTCON_PTY_CHILD_BIN': pty_child_exe.full_path(),
            'GHOSTCON_DISABLE_WALL': '1',
            'GHOSTCON_PTY_SKIP_LOGIN': '1',
        },
        depends: [undead_head_exe, ghost_ptyserv_exe, supervisor_exe, fake_renderer_exe, pty_child_exe],
        timeout: 30,
    )
endif

# core (Phase 1) — stubs for now
