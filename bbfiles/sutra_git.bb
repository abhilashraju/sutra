SUMMARY = "sutra: std::execution networking building blocks and a GraphQL service"
DESCRIPTION = "C++23 TCP/TLS/HTTP/D-Bus building blocks on stdexec and Boost.Asio. \
Installs the example GraphQL server (Redfish source, optional D-Bus source) and \
its systemd unit."
HOMEPAGE = "https://github.com/abhilashraju/sutra"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=c87d289892671dc3843f34743e535818"

PV = "0.1.0+git${SRCPV}"
SRC_URI = "git://github.com/abhilashraju/sutra.git;branch=main;protocol=https"
# Pin to a commit for reproducible builds.
SRCREV = "${AUTOREV}"

# Needs GCC 14+ (C++23, std::expected) and Boost >= 1.86.
DEPENDS = " \
    boost \
    nlohmann-json \
    openssl \
    stdexec \
    systemd \
"

inherit meson pkgconfig systemd

EXTRA_OEMESON += " \
    --buildtype=minsize \
    -Dexamples=enabled \
    -Dinstall_example=true \
"

# dbus: the D-Bus data source (graphql_server --bus system|session).
# tests: build the GoogleTest unit tests (not installed).
PACKAGECONFIG ??= "dbus"
PACKAGECONFIG[dbus] = "-Ddbus=enabled,-Ddbus=disabled,sdbusplus"
PACKAGECONFIG[tests] = "-Dtests=enabled,-Dtests=disabled,googletest"

# The Redfish password (SUTRA_REDFISH_PASSWORD) lives here, outside the unit.
do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/examples/graphql_server/service/sutra-graphql.service \
        ${D}${systemd_system_unitdir}/sutra-graphql.service

    install -d ${D}${sysconfdir}/default
    cat > ${D}${sysconfdir}/default/sutra-graphql <<ENVFILE
# Credentials for the Redfish user "service" (see sutra-graphql.service).
# Keep this file mode 0600, owned by root.
#SUTRA_REDFISH_PASSWORD=
# or a session token instead of the password:
#SUTRA_REDFISH_TOKEN=
ENVFILE
    chmod 0600 ${D}${sysconfdir}/default/sutra-graphql
}

FILES:${PN} += "${datadir}/sutra-graphql"
CONFFILES:${PN} += "${sysconfdir}/default/sutra-graphql"

SYSTEMD_SERVICE:${PN} = "sutra-graphql.service"
