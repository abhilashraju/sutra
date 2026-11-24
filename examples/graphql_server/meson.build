executable(
    'graphql_server',
    'main.cpp',
    dependencies: sutra_dbus_dep,
    cpp_args: ['-DSUTRA_EXAMPLE_SCHEMA="' + meson.current_source_dir() / 'schema.json"'],
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
