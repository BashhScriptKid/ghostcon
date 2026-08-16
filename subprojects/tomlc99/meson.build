# tomlc99 — vendored, pinned to commit 29076dfd095bbbbd50a3c1b2760d29f4b83e74ac
# (https://github.com/cktan/tomlc99, MIT). No WrapDB entry exists for a C
# TOML parser (only C++ ones are packaged system-wide), so the two source
# files are copied in directly rather than fetched at build time — see
# PLAN.md's config-loading entry for the full rationale.
project('tomlc99', 'c', default_options: ['warning_level=0'])

tomlc99_lib = static_library('tomlc99',
    'toml.c',
    include_directories: include_directories('.'),
    install: false,
)

tomlc99_dep = declare_dependency(
    link_with: tomlc99_lib,
    include_directories: include_directories('.'),
)
