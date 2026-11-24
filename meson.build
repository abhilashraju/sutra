project(
    'sutra',
    'cpp',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>=1.3.0',
    default_options: [
        'cpp_std=c++23',
        'warning_level=2',
        'werror=false',
        'buildtype=debugoptimized',
    ],
)

cxx = meson.get_compiler('cpp')

# ---------------------------------------------------------------------------
# stdexec: must be installed with -DSTDEXEC_ENABLE_ASIO=ON (boost flavour) so
# that <exec/asio/asio_config.hpp> exists. Point the build at the install
# prefix with CMAKE_PREFIX_PATH (see scripts/build.sh).
# ---------------------------------------------------------------------------
stdexec_dep = dependency('stdexec', method: 'cmake', required: true)
if not cxx.has_header(
    'exec/asio/asio_config.hpp',
    dependencies: stdexec_dep,
    prefix: '#include <boost/asio.hpp>',
)
    error(
        'stdexec was installed without Asio support. Reinstall it with '
        + '-DSTDEXEC_ENABLE_ASIO=ON -DSTDEXEC_ASIO_IMPLEMENTATION=boost',
    )
endif

# Boost is header-only for sutra. Do NOT define BOOST_ASIO_DISABLE_THREADS:
# stdexec's use_sender completion token relies on std::recursive_mutex.
if not cxx.has_header('boost/asio.hpp')
    error('Boost >= 1.86 headers not found')
endif
boost_dep = declare_dependency(
    compile_args: [
        '-DBOOST_ALL_NO_LIB',
        '-DBOOST_ASIO_NO_DEPRECATED',
        '-DBOOST_SYSTEM_NO_DEPRECATED',
    ],
)

openssl_dep = dependency('openssl', required: true)
threads_dep = dependency('threads')
json_dep = dependency('nlohmann_json', required: true)

sutra_inc = include_directories('include')

sutra_dep = declare_dependency(
    include_directories: sutra_inc,
    dependencies: [stdexec_dep, boost_dep, openssl_dep, threads_dep, json_dep],
)

# ---------------------------------------------------------------------------
# Optional D-Bus layer (sdbusplus)
#
# sdbusplus.pc injects -DBOOST_ASIO_DISABLE_THREADS into every consumer, which
# breaks stdexec's use_sender (std::recursive_mutex) and Asio's async resolver
# thread. So the pkg-config dependency is deliberately not used: only the
# library and the systemd dependency are taken from the system.
# ---------------------------------------------------------------------------
sdbusplus_lib = cxx.find_library('sdbusplus', required: get_option('dbus'))
systemd_dep = dependency('libsystemd', required: get_option('dbus'))
have_dbus = sdbusplus_lib.found() and systemd_dep.found()
if have_dbus
    sutra_dbus_dep = declare_dependency(
        dependencies: [sutra_dep, sdbusplus_lib, systemd_dep],
        compile_args: ['-DSUTRA_HAVE_DBUS=1'],
    )
else
    sutra_dbus_dep = sutra_dep
endif

if get_option('tests').allowed()
    subdir('tests')
endif

if get_option('examples').allowed()
    subdir('examples')
endif
