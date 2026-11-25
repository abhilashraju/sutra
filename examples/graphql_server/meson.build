# Default for --schema. An installed binary must not embed the build host's
# source path (Yocto's buildpaths QA rejects it), so it points at the
# installed copy instead. `prefix / datadir` also works when datadir is
# absolute (meson then keeps the absolute path).
if get_option('install_example')
    default_schema = get_option('prefix') / get_option('datadir') / 'sutra-graphql' / 'schema.json'
else
    default_schema = meson.current_source_dir() / 'schema.json'
endif

executable(
    'graphql_server',
    'main.cpp',
    dependencies: sutra_dbus_dep,
    cpp_args: [
        '-DSUTRA_EXAMPLE_SCHEMA="' + default_schema + '"',
        # Log everything (debug and up) to the journal.
        '-DSUTRA_LOG_LEVEL=0',
    ],
    install: get_option('install_example'),
)

# The systemd unit (service/sutra-graphql.service) is installed by the Yocto
# recipe, which knows the distro's unit directory.
if get_option('install_example')
    install_data(
        'schema.json',
        install_dir: get_option('datadir') / 'sutra-graphql',
    )
endif
