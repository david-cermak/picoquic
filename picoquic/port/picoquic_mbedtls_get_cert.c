#include "ptls_mbedtls.h"

ptls_verify_certificate_t* __real_ptls_mbedtls_get_certificate_verifier(char const* pem_fname, unsigned int* is_cert_store_not_empty);

ptls_verify_certificate_t* __wrap_ptls_mbedtls_get_certificate_verifier(char const* pem_fname, unsigned int* is_cert_store_not_empty)
{
    if (pem_fname == NULL) {
        return NULL;
    }
    return __real_ptls_mbedtls_get_certificate_verifier(pem_fname, is_cert_store_not_empty);
}