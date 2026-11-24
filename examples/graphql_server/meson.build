executable(
    'graphql_server',
    'main.cpp',
    dependencies: sutra_dbus_dep,
    cpp_args: ['-DSUTRA_EXAMPLE_SCHEMA="' + meson.current_source_dir() / 'schema.json"'],
)
