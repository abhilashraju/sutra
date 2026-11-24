SUMMARY = "stdexec: NVIDIA's reference implementation of std::execution (P2300)"
DESCRIPTION = "Header-only library. Installed without CMake because stdexec's \
CMake build downloads rapids-cmake (and Boost for the Asio support) at \
configure time, which is not possible in the Yocto sandbox. This recipe copies \
the headers, generates exec/asio/asio_config.hpp for the Boost.Asio flavour \
that sutra needs and installs a pkg-config file."
HOMEPAGE = "https://github.com/NVIDIA/stdexec"
LICENSE = "Apache-2.0-with-LLVM-exception"
LIC_FILES_CHKSUM = "file://LICENSE.txt;md5=2e982d844baa4df1c80de75470e0c5cb"

SRC_URI = "git://github.com/NVIDIA/stdexec.git;protocol=https;nobranch=1;tag=v${PV}"
# v0.11.0
SRCREV = "f99b23f08b78db410f26b58080042c6721d7af8b"

S = "${WORKDIR}/git"

inherit allarch

# Requires a C++20 compiler with P2300 support (GCC 12+); sutra needs GCC 14+.
do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${includedir}
    cp -r --no-preserve=ownership ${S}/include/exec ${S}/include/stdexec ${D}${includedir}/

    # Same result as `cmake -DSTDEXEC_ENABLE_ASIO=ON -DSTDEXEC_ASIO_IMPLEMENTATION=boost`.
    sed -e 's/^#cmakedefine01 STDEXEC_ASIO_USES_STANDALONE$/#define STDEXEC_ASIO_USES_STANDALONE 0/' \
        -e 's/^#cmakedefine01 STDEXEC_ASIO_USES_BOOST$/#define STDEXEC_ASIO_USES_BOOST 1/' \
        ${S}/include/exec/asio/asio_config.hpp.in \
        > ${D}${includedir}/exec/asio/asio_config.hpp
    rm -f ${D}${includedir}/exec/asio/asio_config.hpp.in
    grep -q cmakedefine ${D}${includedir}/exec/asio/asio_config.hpp && \
        bbfatal "asio_config.hpp still contains cmakedefine"

    install -d ${D}${datadir}/pkgconfig
    cat > ${D}${datadir}/pkgconfig/stdexec.pc <<PC
Name: stdexec
Description: std::execution (P2300) reference implementation with Boost.Asio support
Version: ${PV}
Cflags: -I${includedir} -DSTDEXEC_PARALLEL_SCHEDULER_HEADER_ONLY
PC
}

# Header-only: everything lands in ${PN}-dev; the main package stays empty.
ALLOW_EMPTY:${PN} = "1"
