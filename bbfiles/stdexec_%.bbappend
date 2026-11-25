# sutra uses stdexec's Boost.Asio support (<exec/asio/use_sender.hpp>), which
# needs exec/asio/asio_config.hpp. stdexec's CMake build generates that header
# from asio_config.hpp.in (-DSTDEXEC_ENABLE_ASIO=ON
# -DSTDEXEC_ASIO_IMPLEMENTATION=boost), but a meson/header-copy install of
# stdexec (as used for sdbusplus) never does. Generate the same file here.
do_install:append() {
    install -d ${D}${includedir}/exec/asio
    sed -e 's/^#cmakedefine01 STDEXEC_ASIO_USES_STANDALONE$/#define STDEXEC_ASIO_USES_STANDALONE 0/' \
        -e 's/^#cmakedefine01 STDEXEC_ASIO_USES_BOOST$/#define STDEXEC_ASIO_USES_BOOST 1/' \
        ${S}/include/exec/asio/asio_config.hpp.in \
        > ${D}${includedir}/exec/asio/asio_config.hpp
    if grep -q cmakedefine ${D}${includedir}/exec/asio/asio_config.hpp; then
        bbfatal "asio_config.hpp still contains cmakedefine"
    fi
}
