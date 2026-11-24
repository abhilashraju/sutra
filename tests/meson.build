gtest_dep = dependency('gtest', main: true, required: true)

spike = executable(
    'spike_task_asio',
    'spike_task_asio.cpp',
    dependencies: sutra_dep,
)
test('spike_task_asio', spike, timeout: 30)

sutra_tests = {
    'tcp_test': ['tcp_test.cpp'],
    'http_test': ['http_test.cpp'],
    'web_client_test': ['web_client_test.cpp'],
    'graphql_parser_test': ['graphql_parser_test.cpp'],
    'graphql_executor_test': ['graphql_executor_test.cpp'],
    'graphql_endpoint_test': ['graphql_endpoint_test.cpp'],
    'graphql_dbus_test': ['graphql_dbus_test.cpp'],
}

foreach name, srcs : sutra_tests
    exe = executable(name, srcs, dependencies: [sutra_dbus_dep, gtest_dep])
    test(name, exe, timeout: 60)
endforeach

# The D-Bus tests need a session bus; dbus-run-session provides a private one.
dbus_run_session = find_program('dbus-run-session', required: false)
if have_dbus and dbus_run_session.found()
    dbus_test = executable(
        'dbus_test',
        'dbus_test.cpp',
        dependencies: [sutra_dbus_dep, gtest_dep],
    )
    test(
        'dbus_test',
        dbus_run_session,
        args: ['--', dbus_test],
        timeout: 60,
    )
endif
