set(PQDIR "../impl/picoquic/")
set(PTLSDIR "../impl/picotls/")
SET(PTLS_FILES
        lib/hpke.c
        lib/picotls.c
        lib/pembase64.c)
#        lib/mbedtls.c
#        lib/mbedtls_sign.c)


set(PICOQUIC_LIBRARY_FILES
        picoquic_mbedtls/ptls_mbedtls.c
        picoquic_mbedtls/ptls_mbedtls_sign.c
        picoquic/bbr.c
        picoquic/bbr1.c
        picoquic/bytestream.c
        picoquic/cc_common.c
        picoquic/config.c
        picoquic/cubic.c
        picoquic/c4.c
        picoquic/ech.c
        picoquic/error_names.c
        picoquic/fastcc.c
        picoquic/frames.c
        picoquic/intformat.c
        picoquic/logger.c
        picoquic/logwriter.c
        picoquic/loss_recovery.c
        picoquic/newreno.c
        picoquic/pacing.c
        picoquic/packet.c
        picoquic/paths.c
        picoquic/performance_log.c
        picoquic/picohash.c
        picoquic/picoquic_lb.c
        picoquic/picoquic_ptls_fusion.c
#        picoquic/picoquic_ptls_minicrypto.c
        picoquic/picoquic_ptls_openssl.c
        picoquic/picoquic_mbedtls.c
        # picoquic/picoquic_esp_log.c
#        picoquic/picosocks.c
        # picoquic/picosocks_esp32.c
        picoquic/picosplay.c
        picoquic/port_blocking.c
        picoquic/prague.c
        picoquic/quicctx.c
        picoquic/register_all_cc_algorithms.c
        picoquic/sacks.c
        picoquic/sender.c
        picoquic/sim_link.c
        picoquic/siphash.c
        picoquic/sockloop.c
        picoquic/spinbit.c
        picoquic/ticket_store.c
        picoquic/timing.c
        picoquic/token_store.c
        picoquic/tls_api.c
        picoquic/transport.c
        picoquic/unified_log.c
        picoquic/util.c)

list(TRANSFORM PICOQUIC_LIBRARY_FILES PREPEND "${PQDIR}")
list(TRANSFORM PTLS_FILES PREPEND "${PTLSDIR}")

idf_component_register(SRCS "port/picosocks_esp32.c"
                            "port/picoquic_esp_log.c"
                            ${PICOQUIC_LIBRARY_FILES}
                            ${PTLS_FILES}
                    INCLUDE_DIRS "${PTLSDIR}/include" "${PQDIR}/picoquic_mbedtls" "${PQDIR}/picoquic"
                    REQUIRES mbedtls)

target_compile_definitions(${COMPONENT_LIB} PRIVATE PTLS_WITHOUT_OPENSSL)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PICOQUIC_WITH_MBEDTLS)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PTLS_WITHOUT_FUSION)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PTLS_HAVE_MBEDTLS=1)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
