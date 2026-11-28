/**
 * @file cert_generator.hpp
 * @brief Self-signed certificate generation and TLS context helpers.
 *
 * The generator is meant for tests, examples and first-boot bootstrap. The TLS
 * context helpers always enforce TLS 1.2+ and, on the client side, peer
 * certificate verification (there is deliberately no way to switch it off).
 *
 * Every function returns `Expected<T>`. An OpenSSL failure carries the
 * OpenSSL error code (in the `ssl` category) when the library reported one,
 * and `Errc::opensslFailure` otherwise.
 */
#pragma once

#include "sutra/config.hpp"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <filesystem>
#include <memory>
#include <string>

namespace sutra
{

struct PemPair
{
    std::string certificate;
    std::string privateKey;
};

namespace details
{

inline std::string pemOf(BIO* bio)
{
    char* data = nullptr;
    long length = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<size_t>(length));
}

/** @brief The pending OpenSSL error, or `Errc::opensslFailure`. */
inline error_code opensslError()
{
    if (auto code = ::ERR_get_error(); code != 0)
    {
        return error_code(static_cast<int>(code),
                          net::error::get_ssl_category());
    }
    return make_error_code(Errc::opensslFailure);
}

} // namespace details

/**
 * @brief Creates a self-signed ECDSA P-256 certificate.
 * @param commonName         subject CN.
 * @param subjectAltNames    OpenSSL SAN list, e.g.
 * "DNS:localhost,IP:127.0.0.1".
 * @param validDays          lifetime in days.
 */
inline Expected<PemPair> generateSelfSignedCertificate(
    const std::string& commonName,
    const std::string& subjectAltNames = "DNS:localhost,IP:127.0.0.1",
    int validDays = 1)
{
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_EC_gen("P-256"), EVP_PKEY_free);
    if (!key)
    {
        return std::unexpected(details::opensslError());
    }
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    if (!cert)
    {
        return std::unexpected(details::opensslError());
    }

    X509_set_version(cert.get(), 2);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> serial(BN_new(), BN_free);
    if (!serial ||
        BN_rand(serial.get(), 64, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1 ||
        BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(cert.get())) ==
            nullptr)
    {
        return std::unexpected(details::opensslError());
    }
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()),
                    static_cast<long>(validDays) * 24 * 3600);
    X509_set_pubkey(cert.get(), key.get());

    // Build the name ourselves: X509_get_subject_name() returns a const
    // pointer in OpenSSL 4, so it cannot be filled in place.
    std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> name(
        X509_NAME_new(), X509_NAME_free);
    if (!name ||
        X509_NAME_add_entry_by_txt(
            name.get(), "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(commonName.c_str()), -1, -1,
            0) != 1 ||
        X509_set_subject_name(cert.get(), name.get()) != 1 ||
        X509_set_issuer_name(cert.get(), name.get()) != 1)
    {
        return std::unexpected(details::opensslError());
    }

    X509V3_CTX v3;
    X509V3_set_ctx(&v3, cert.get(), cert.get(), nullptr, nullptr, 0);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
        X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name,
                            subjectAltNames.c_str()),
        X509_EXTENSION_free);
    if (!san || X509_add_ext(cert.get(), san.get(), -1) != 1)
    {
        return std::unexpected(details::opensslError());
    }

    if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0)
    {
        return std::unexpected(details::opensslError());
    }

    std::unique_ptr<BIO, decltype(&BIO_free_all)> certBio(
        BIO_new(BIO_s_mem()), BIO_free_all);
    std::unique_ptr<BIO, decltype(&BIO_free_all)> keyBio(BIO_new(BIO_s_mem()),
                                                         BIO_free_all);
    if (!certBio || !keyBio ||
        PEM_write_bio_X509(certBio.get(), cert.get()) != 1 ||
        PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0,
                                 nullptr, nullptr) != 1)
    {
        return std::unexpected(details::opensslError());
    }
    return PemPair{details::pemOf(certBio.get()), details::pemOf(keyBio.get())};
}

/** @brief TLS 1.2+ server context from PEM strings. */
inline Expected<ssl::context> makeServerContext(
    const std::string& certificatePem, const std::string& privateKeyPem)
{
    ssl::context ctx(ssl::context::tls_server);
    error_code ec;
    ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                        ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                        ssl::context::no_tlsv1_1 | ssl::context::single_dh_use,
                    ec);
    if (ec)
    {
        return std::unexpected(ec);
    }
    SSL_CTX_set_min_proto_version(ctx.native_handle(), TLS1_2_VERSION);
    ctx.use_certificate_chain(
        net::buffer(certificatePem.data(), certificatePem.size()), ec);
    if (ec)
    {
        return std::unexpected(ec);
    }
    ctx.use_private_key(net::buffer(privateKeyPem.data(), privateKeyPem.size()),
                        ssl::context::pem, ec);
    if (ec)
    {
        return std::unexpected(ec);
    }
    return ctx;
}

/**
 * @brief TLS 1.2+ client context that verifies the server certificate.
 * @param trustedPem extra trust anchor (PEM). When empty the system store is
 *                   used.
 */
inline Expected<ssl::context> makeClientContext(
    const std::string& trustedPem = {})
{
    ssl::context ctx(ssl::context::tls_client);
    error_code ec;
    ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                        ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                        ssl::context::no_tlsv1_1,
                    ec);
    if (ec)
    {
        return std::unexpected(ec);
    }
    SSL_CTX_set_min_proto_version(ctx.native_handle(), TLS1_2_VERSION);
    ctx.set_verify_mode(ssl::verify_peer, ec);
    if (ec)
    {
        return std::unexpected(ec);
    }
    if (trustedPem.empty())
    {
        ctx.set_default_verify_paths(ec);
    }
    else
    {
        ctx.add_certificate_authority(
            net::buffer(trustedPem.data(), trustedPem.size()), ec);
    }
    if (ec)
    {
        return std::unexpected(ec);
    }
    return ctx;
}

/**
 * @brief TLS 1.2+ client context that trusts the CAs in a directory.
 * @param caPath OpenSSL "CApath": a directory of PEM certificates, each
 *               reachable through its subject hash (`<hash>.0`, as created by
 *               `openssl rehash`). Nothing else is trusted.
 * @param clientCertPath optional PEM client certificate chain presented to the
 *               server for mutual TLS. Must be given together with
 *               `clientKeyPath`.
 * @param clientKeyPath optional PEM private key for `clientCertPath`.
 * @return the context, `Errc::incompleteClientCredentials` when only one of
 *         the client cert/key is given, `Errc::invalidCaPath` when `caPath` is
 *         not a directory, or the failure to load the client cert/key (also
 *         when they do not match).
 */
inline Expected<ssl::context> makeClientContextWithCaPath(
    const std::string& caPath, const std::string& clientCertPath = {},
    const std::string& clientKeyPath = {})
{
    if (clientCertPath.empty() != clientKeyPath.empty())
    {
        return std::unexpected(
            make_error_code(Errc::incompleteClientCredentials));
    }
    error_code ec;
    if (!std::filesystem::is_directory(caPath, ec))
    {
        return std::unexpected(make_error_code(Errc::invalidCaPath));
    }
    ssl::context ctx(ssl::context::tls_client);
    ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                        ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                        ssl::context::no_tlsv1_1,
                    ec);
    if (ec)
    {
        return std::unexpected(ec);
    }
    SSL_CTX_set_min_proto_version(ctx.native_handle(), TLS1_2_VERSION);
    ctx.set_verify_mode(ssl::verify_peer, ec);
    if (ec)
    {
        return std::unexpected(ec);
    }
    ctx.add_verify_path(caPath, ec);
    if (ec)
    {
        return std::unexpected(ec);
    }
    if (!clientCertPath.empty())
    {
        ctx.use_certificate_chain_file(clientCertPath, ec);
        if (ec)
        {
            return std::unexpected(ec);
        }
        ctx.use_private_key_file(clientKeyPath, ssl::context::pem, ec);
        if (ec)
        {
            return std::unexpected(ec);
        }
    }
    return ctx;
}

} // namespace sutra
