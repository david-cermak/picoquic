/*
* Author: Christian Huitema
* Copyright (c) 2020, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <picoquic.h>
#include <picoquic_internal.h>
#include <picoquic_utils.h>
#include <tls_api.h>
#include <picosocks.h>
#include "nvs_flash.h"
#include "esp_event.h"
#include "protocol_examples_common.h"
#include <picoquic_packet_loop.h>
#include "picoquic_bbr.h"
#include "picoquic_esp_log.h"
#include "esp_log.h"

#define PICOQUIC_SAMPLE_ALPN "picoquic_sample"
#define PICOQUIC_SAMPLE_SNI "test.example.com"

#define PICOQUIC_SAMPLE_NO_ERROR 0

static const char* TAG = "pquic";
#define SAMPLE_CLIENT_MAX_DOWNLOAD_BYTES (64 * 1024)

int picoquic_serialize_ticket(const picoquic_stored_ticket_t* ticket, uint8_t* bytes, size_t bytes_max, size_t* consumed);
int picoquic_deserialize_ticket(picoquic_stored_ticket_t** ticket, uint8_t* bytes, size_t bytes_max, size_t* consumed);

static uint8_t* g_ticket_blob = NULL;
static size_t g_ticket_blob_len = 0;

static int append_bytes(uint8_t** buf, size_t* len, size_t* cap, const uint8_t* src, size_t src_len)
{
    if (src_len == 0) {
        return 0;
    }
    size_t needed = *len + src_len;
    if (needed > *cap) {
        size_t new_cap = (*cap == 0) ? 256 : *cap;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        uint8_t* grown = (uint8_t*)realloc(*buf, new_cap);
        if (grown == NULL) {
            return -1;
        }
        *buf = grown;
        *cap = new_cap;
    }
    memcpy(*buf + *len, src, src_len);
    *len += src_len;
    return 0;
}

static void persist_tickets_to_heap(picoquic_quic_t* quic)
{
    if (quic == NULL) {
        return;
    }
    uint8_t* blob = NULL;
    size_t blob_len = 0;
    size_t blob_cap = 0;
    size_t stored = 0;
    const uint64_t current_time = picoquic_get_tls_time(quic);
    const picoquic_stored_ticket_t* next = quic->p_first_ticket;

    while (next != NULL) {
        if (next->time_valid_until > current_time && next->was_used == 0) {
            uint8_t buffer[2048];
            size_t record_size = 0;
            int ret = picoquic_serialize_ticket(next, buffer, sizeof(buffer), &record_size);
            if (ret == 0 && record_size <= UINT32_MAX) {
                uint32_t size32 = (uint32_t)record_size;
                if (append_bytes(&blob, &blob_len, &blob_cap, (uint8_t*)&size32, sizeof(size32)) != 0 ||
                    append_bytes(&blob, &blob_len, &blob_cap, buffer, record_size) != 0) {
                    free(blob);
                    ESP_LOGW(TAG, "Out of memory while caching tickets");
                    return;
                }
                stored++;
            }
        }
        next = next->next_ticket;
    }

    free(g_ticket_blob);
    g_ticket_blob = blob;
    g_ticket_blob_len = blob_len;
    ESP_LOGD(TAG, "stored %u ticket(s) in heap (%u bytes)",
        (unsigned)stored, (unsigned)g_ticket_blob_len);
}

static void restore_tickets_from_heap(picoquic_quic_t* quic)
{
    if (quic == NULL) {
        return;
    }
    if (g_ticket_blob == NULL || g_ticket_blob_len == 0) {
        ESP_LOGD(TAG, "no cached tickets in heap");
        return;
    }

    picoquic_free_tickets(&quic->p_first_ticket);
    picoquic_stored_ticket_t* previous = NULL;
    const uint64_t current_time = picoquic_get_tls_time(quic);
    size_t restored = 0;

    size_t offset = 0;
    while (offset + sizeof(uint32_t) <= g_ticket_blob_len) {
        uint32_t storage_size = 0;
        memcpy(&storage_size, g_ticket_blob + offset, sizeof(storage_size));
        offset += sizeof(storage_size);
        if (storage_size > 2048 || offset + storage_size > g_ticket_blob_len) {
            ESP_LOGW(TAG, "ticket blob corrupted or truncated");
            break;
        }

        picoquic_stored_ticket_t* ticket = NULL;
        size_t consumed = 0;
        int ret = picoquic_deserialize_ticket(&ticket, g_ticket_blob + offset, storage_size, &consumed);
        offset += storage_size;

        if (ret != 0 || consumed != storage_size || ticket == NULL) {
            if (ticket != NULL) {
                free(ticket);
            }
            ESP_LOGW(TAG, "ticket deserialize failed: %d", ret);
            continue;
        }

        if (ticket->time_valid_until < current_time) {
            free(ticket);
            continue;
        }

        ticket->next_ticket = NULL;
        if (previous == NULL) {
            quic->p_first_ticket = ticket;
        } else {
            previous->next_ticket = ticket;
        }
        previous = ticket;
        restored++;
    }

    ESP_LOGD(TAG, "restored %u ticket(s) from heap (%u bytes)",
        (unsigned)restored, (unsigned)g_ticket_blob_len);
}

typedef struct st_sample_client_stream_ctx_t {
    struct st_sample_client_stream_ctx_t* next_stream;
    size_t file_rank;
    uint64_t stream_id;
    size_t name_length;
    size_t name_sent_length;
    size_t bytes_received;
    uint8_t* recv_buf;
    size_t recv_cap;
    uint64_t remote_error;
    unsigned int is_name_sent : 1;
    unsigned int is_stream_reset : 1;
    unsigned int is_stream_finished : 1;
} sample_client_stream_ctx_t;

typedef struct st_sample_client_ctx_t {
    picoquic_cnx_t* cnx;
    char const** file_names;
    sample_client_stream_ctx_t* first_stream;
    sample_client_stream_ctx_t* last_stream;
    int nb_files;
    int nb_files_received;
    int nb_files_failed;
    int is_disconnected;
} sample_client_ctx_t;

static int sample_client_append_bytes(sample_client_stream_ctx_t* stream_ctx, const uint8_t* bytes, size_t length)
{
    if (length == 0) {
        return 0;
    }
    if (stream_ctx->bytes_received + length > SAMPLE_CLIENT_MAX_DOWNLOAD_BYTES) {
        ESP_LOGE(TAG, "Download too large (%zu + %zu > %u), aborting",
            stream_ctx->bytes_received, length, (unsigned)SAMPLE_CLIENT_MAX_DOWNLOAD_BYTES);
        return -1;
    }

    size_t needed = stream_ctx->bytes_received + length;
    if (needed > stream_ctx->recv_cap) {
        size_t new_cap = (stream_ctx->recv_cap == 0) ? 1024 : stream_ctx->recv_cap;
        while (new_cap < needed) {
            new_cap *= 2;
            if (new_cap > SAMPLE_CLIENT_MAX_DOWNLOAD_BYTES) {
                new_cap = SAMPLE_CLIENT_MAX_DOWNLOAD_BYTES;
                break;
            }
        }
        uint8_t* new_buf = (uint8_t*)realloc(stream_ctx->recv_buf, new_cap);
        if (new_buf == NULL) {
            ESP_LOGE(TAG, "Out of memory while buffering response (%zu bytes)", new_cap);
            return -1;
        }
        stream_ctx->recv_buf = new_buf;
        stream_ctx->recv_cap = new_cap;
    }

    memcpy(stream_ctx->recv_buf + stream_ctx->bytes_received, bytes, length);
    stream_ctx->bytes_received += length;
    return 0;
}

static int sample_client_create_stream(picoquic_cnx_t* cnx,
    sample_client_ctx_t* client_ctx, int file_rank)
{
    int ret = 0;
    sample_client_stream_ctx_t* stream_ctx = (sample_client_stream_ctx_t*)
        malloc(sizeof(sample_client_stream_ctx_t));

    if (stream_ctx == NULL) {
        ESP_LOGE(TAG, "Memory Error, cannot create stream for file number %d", (int)file_rank);
        ret = -1;
    }
    else {
        memset(stream_ctx, 0, sizeof(sample_client_stream_ctx_t));
        if (client_ctx->first_stream == NULL) {
            client_ctx->first_stream = stream_ctx;
            client_ctx->last_stream = stream_ctx;
        }
        else {
            client_ctx->last_stream->next_stream = stream_ctx;
            client_ctx->last_stream = stream_ctx;
        }
        stream_ctx->file_rank = file_rank;
        stream_ctx->stream_id = picoquic_get_next_local_stream_id(client_ctx->cnx, 0);
        stream_ctx->name_length = strlen(client_ctx->file_names[file_rank]);

        ret = picoquic_mark_active_stream(cnx, stream_ctx->stream_id, 1, stream_ctx);
        if (ret != 0) {
            ESP_LOGE(TAG, "Error %d, cannot initialize stream for file number %d", ret, (int)file_rank);
        }
        else {
            ESP_LOGI(TAG, "Opened stream %" PRIu64 " for file %s", stream_ctx->stream_id, client_ctx->file_names[file_rank]);
        }
    }

    return ret;
}

static void sample_client_report(sample_client_ctx_t* client_ctx)
{
    sample_client_stream_ctx_t* stream_ctx = client_ctx->first_stream;

    while (stream_ctx != NULL) {
        char const* status;
        if (stream_ctx->is_stream_finished) {
            status = "complete";
        }
        else if (stream_ctx->is_stream_reset) {
            status = "reset";
        }
        else {
            status = "unknown status";
        }
        ESP_LOGI(TAG, "%s: %s, received %zu bytes", client_ctx->file_names[stream_ctx->file_rank], status, stream_ctx->bytes_received);
        if (stream_ctx->is_stream_reset && stream_ctx->remote_error != PICOQUIC_SAMPLE_NO_ERROR){
            ESP_LOGI(TAG, "remote error 0x%" PRIx64 "(%s)", stream_ctx->remote_error, picoquic_error_name(stream_ctx->remote_error));
        }
        stream_ctx = stream_ctx->next_stream;
    }
}

static void sample_client_free_context(sample_client_ctx_t* client_ctx)
{
    sample_client_stream_ctx_t* stream_ctx;

    while ((stream_ctx = client_ctx->first_stream) != NULL) {
        client_ctx->first_stream = stream_ctx->next_stream;
        free(stream_ctx->recv_buf);
        free(stream_ctx);
    }
    client_ctx->last_stream = NULL;
}


int sample_client_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    int ret = 0;
    sample_client_ctx_t* client_ctx = (sample_client_ctx_t*)callback_ctx;
    sample_client_stream_ctx_t* stream_ctx = (sample_client_stream_ctx_t*)v_stream_ctx;

    if (client_ctx == NULL) {
        /* This should never happen, because the callback context for the client is initialized
         * when creating the client connection. */
        return -1;
    }

    if (ret == 0) {
        switch (fin_or_event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin:
            if (stream_ctx == NULL) {
                return -1;
            }
            else if (!stream_ctx->is_name_sent) {
                return -1;
            }
            else if (stream_ctx->is_stream_reset || stream_ctx->is_stream_finished) {
                return -1;
            }
            else
            {
                if (ret == 0 && length > 0) {
                    ret = sample_client_append_bytes(stream_ctx, bytes, length);
                }

                if (ret == 0 && fin_or_event == picoquic_callback_stream_fin) {
                    stream_ctx->is_stream_finished = 1;
                    client_ctx->nb_files_received++;

                    {
                        const char* fname = client_ctx->file_names[stream_ctx->file_rank];
                        size_t n = stream_ctx->bytes_received;
                        if (stream_ctx->recv_cap < n + 1) {
                            uint8_t* grown = (uint8_t*)realloc(stream_ctx->recv_buf, n + 1);
                            if (grown != NULL) {
                                stream_ctx->recv_buf = grown;
                                stream_ctx->recv_cap = n + 1;
                            }
                        }
                        if (stream_ctx->recv_buf != NULL && stream_ctx->recv_cap >= n + 1) {
                            stream_ctx->recv_buf[n] = 0;
                            ESP_LOGI(TAG, "%s: %s", fname, (char*)stream_ctx->recv_buf);
                        } else {
                            ESP_LOGI(TAG, "%s: received %zu bytes", fname, n);
                        }
                    }

                    if ((client_ctx->nb_files_received + client_ctx->nb_files_failed) >= client_ctx->nb_files) {
                        ret = picoquic_close(cnx, 0);
                    }
                }
            }
            break;
        case picoquic_callback_stop_sending:
            picoquic_reset_stream(cnx, stream_id, 0);
            /* Fall through */
        case picoquic_callback_stream_reset:
            if (stream_ctx == NULL) {
                return -1;
            }
            else if (stream_ctx->is_stream_reset || stream_ctx->is_stream_finished) {
                return -1;
            }
            else {
                stream_ctx->remote_error = picoquic_get_remote_stream_error(cnx, stream_id);
                stream_ctx->is_stream_reset = 1;
                client_ctx->nb_files_failed++;

                if ((client_ctx->nb_files_received + client_ctx->nb_files_failed) >= client_ctx->nb_files) {
                    ESP_LOGI(TAG, "All done, closing the connection.");
                    ret = picoquic_close(cnx, 0);
                }
            }
            break;
        case picoquic_callback_stateless_reset:
        case picoquic_callback_close:
        case picoquic_callback_application_close:
        {
            uint64_t local_reason = 0, remote_reason = 0;
            uint64_t local_app_reason = 0, remote_app_reason = 0;
            uint64_t local_error = picoquic_get_local_error(cnx);
            uint64_t remote_error = picoquic_get_remote_error(cnx);
            picoquic_get_close_reasons(cnx, &local_reason, &remote_reason, &local_app_reason, &remote_app_reason);

            ESP_LOGI(TAG, "Connection closed. local=0x%" PRIx64 " (%s) remote=0x%" PRIx64 " (%s) local_app=0x%" PRIx64 " remote_app=0x%" PRIx64,
                local_reason, picoquic_error_name(local_reason),
                remote_reason, picoquic_error_name(remote_reason),
                local_app_reason, remote_app_reason);
            ESP_LOGI(TAG, "Connection errors. local_error=0x%" PRIx64 " (%s) remote_error=0x%" PRIx64 " (%s)",
                local_error, picoquic_error_name(local_error),
                remote_error, picoquic_error_name(remote_error));

            for (sample_client_stream_ctx_t* s = client_ctx->first_stream; s != NULL; s = s->next_stream) {
                if (!s->is_stream_finished && !s->is_stream_reset) {
                    s->remote_error = picoquic_get_remote_stream_error(cnx, s->stream_id);
                    if (s->remote_error == 0) {
                        s->remote_error = (remote_app_reason != 0) ? remote_app_reason : remote_reason;
                    }
                    s->is_stream_reset = 1;
                    client_ctx->nb_files_failed++;
                }
            }

            client_ctx->is_disconnected = 1;
            picoquic_set_callback(cnx, NULL, NULL);
        }
            break;
        case picoquic_callback_version_negotiation:
            ESP_LOGI(TAG, "Received a version negotiation request:");
            for (size_t byte_index = 0; byte_index + 4 <= length; byte_index += 4) {
                uint32_t vn = 0;
                for (int i = 0; i < 4; i++) {
                    vn <<= 8;
                    vn += bytes[byte_index + i];
                }
                ESP_LOGI(TAG, "%s%08x", (byte_index == 0) ? " " : ", ", vn);
            }
            break;
        case picoquic_callback_stream_gap:
            break;
        case picoquic_callback_prepare_to_send:
            if (stream_ctx == NULL) {
                return -1;
            } else if (stream_ctx->name_sent_length < stream_ctx->name_length){
                uint8_t* buffer;
                size_t available = stream_ctx->name_length - stream_ctx->name_sent_length;
                int is_fin = 1;

                if (available > length) {
                    available = length;
                    is_fin = 0;
                }
                buffer = picoquic_provide_stream_data_buffer(bytes, available, is_fin, !is_fin);
                if (buffer != NULL) {
                    char const* filename = client_ctx->file_names[stream_ctx->file_rank];
                    memcpy(buffer, filename + stream_ctx->name_sent_length, available);
                    stream_ctx->name_sent_length += available;
                    stream_ctx->is_name_sent = is_fin;
                }
                else {
                    ESP_LOGE(TAG, "Error, could not get data buffer.");
                    ret = -1;
                }
            }
            else {
            }
            break;
        case picoquic_callback_almost_ready:
            ESP_LOGI(TAG, "Connection to the server completed, almost ready.");
            break;
        case picoquic_callback_ready:
            ESP_LOGI(TAG, "Connection to the server confirmed.");
            break;
        default:
            break;
        }
    }

    return ret;
}

static int sample_client_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode,
    void* callback_ctx, void * callback_arg)
{
    int ret = 0;
    sample_client_ctx_t* cb_ctx = (sample_client_ctx_t*)callback_ctx;

    if (cb_ctx == NULL) {
        ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    else {
        switch (cb_mode) {
        case picoquic_packet_loop_ready:
            ESP_LOGI(TAG, "Waiting for packets.");
            break;
        case picoquic_packet_loop_after_receive:
            break;
        case picoquic_packet_loop_after_send:
            if (cb_ctx->is_disconnected) {
                ret = PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
            }
            break;
        case picoquic_packet_loop_port_update:
            break;
        default:
            ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
            break;
        }
    }
    return ret;
}

static int sample_client_init(char const* server_name, int server_port,
    struct sockaddr_storage * server_address, picoquic_quic_t** quic, picoquic_cnx_t** cnx, sample_client_ctx_t *client_ctx)
{
    int ret = 0;
    char const* sni = PICOQUIC_SAMPLE_SNI;
    uint64_t current_time = picoquic_current_time();

    *quic = NULL;
    *cnx = NULL;

    if (ret == 0) {
        int is_name = 0;

        ret = picoquic_get_server_address(server_name, server_port, server_address, &is_name);
        if (ret != 0) {
            ESP_LOGE(TAG, "Cannot get the IP address for <%s> port <%d>", server_name, server_port);
        }
        else if (is_name) {
            sni = server_name;
        }
    }

    if (ret == 0) {
        static const char* kTicketStore = "picoquic_sample_ticket_store.bin";
        *quic = picoquic_create(1, NULL, NULL, NULL, PICOQUIC_SAMPLE_ALPN, NULL, NULL,
            NULL, NULL, NULL, current_time, NULL,
            kTicketStore, NULL, 0);

        if (*quic == NULL) {
            ESP_LOGE(TAG, "Could not create quic context");
            ret = -1;
        }
        else {
            picoquic_set_default_congestion_algorithm(*quic, picoquic_bbr_algorithm);
            picoquic_set_log_level(*quic, 10);
            (void)picoquic_set_esp_log(*quic, TAG, 1 /* log_packets */);
            restore_tickets_from_heap(*quic);
        }
    }

    if (ret == 0) {
        ESP_LOGI(TAG, "Starting connection to %s, port %d", server_name, server_port);

        *cnx = picoquic_create_cnx(*quic, picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)server_address, current_time, 0, sni, PICOQUIC_SAMPLE_ALPN, 1);

        if (*cnx == NULL) {
            ESP_LOGE(TAG, "Could not create connection context");
            ret = -1;
        }
        else {
            client_ctx->cnx = *cnx;
            picoquic_set_callback(*cnx, sample_client_callback, client_ctx);
            ret = picoquic_start_client_cnx(*cnx);
            if (ret < 0) {
                ESP_LOGE(TAG, "Could not activate connection");
            }
            else {
                picoquic_connection_id_t icid = picoquic_get_initial_cnxid(*cnx);
                char icid_hex[2 * PICOQUIC_CONNECTION_ID_MAX_SIZE + 1];
                size_t pos = 0;
                for (uint8_t i = 0; i < icid.id_len && (pos + 2) < sizeof(icid_hex); i++) {
                    pos += (size_t)snprintf(icid_hex + pos, sizeof(icid_hex) - pos, "%02x", icid.id[i]);
                }
                icid_hex[pos] = 0;
                ESP_LOGI(TAG, "Initial connection ID: %s", icid_hex);
            }
        }
    }

    return ret;
}

int picoquic_sample_client(char const * server_name, int server_port,
    int nb_files, char const ** file_names)
{
    int ret = 0;
    struct sockaddr_storage server_address;
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    sample_client_ctx_t client_ctx = { 0 };

    ret = sample_client_init(server_name, server_port, &server_address, &quic, &cnx, &client_ctx);

    if (ret == 0) {
        client_ctx.file_names = file_names;
        client_ctx.nb_files = nb_files;

        for (int i = 0; ret == 0 && i < client_ctx.nb_files; i++) {
            ret = sample_client_create_stream(cnx, &client_ctx, i);
            if (ret < 0) {
                ESP_LOGE(TAG, "Could not initiate stream for file #%d", i);
            }
        }
    }

    ret = picoquic_packet_loop(quic, 0, server_address.ss_family, 0, 0, 0, sample_client_loop_cb, &client_ctx);
    if (ret != 0) {
        ESP_LOGW(TAG, "picoquic_packet_loop returned %d (%s)", ret, picoquic_error_name((uint64_t)ret));
    }

    sample_client_report(&client_ctx);

    if (quic != NULL) {
        persist_tickets_to_heap(quic);
    }

    if (quic != NULL) {
        picoquic_free(quic);
    }

    sample_client_free_context(&client_ctx);

    return ret;
}

// int main(void)
void app_main(void)
{

    printf("app_main\n");
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
    #if !defined(CONFIG_IDF_TARGET_LINUX)
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(example_connect());
    #endif

    const char* server_name = CONFIG_PQUIC_SERVER_NAME;
    int server_port = CONFIG_PQUIC_SERVER_PORT;
    static const char* file_names[] = { "index.htm" };
    ESP_LOGI(TAG, "Connecting (1/2)...");
    (void)picoquic_sample_client(server_name, server_port, 1, file_names);

    ESP_LOGD(TAG, "ticket cache after first run: %u bytes", (unsigned)g_ticket_blob_len);
    ESP_LOGI(TAG, "Waiting 2 seconds before reconnect...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Reconnecting (2/2)...");
    (void)picoquic_sample_client(server_name, server_port, 1, file_names);
    ESP_LOGD(TAG, "ticket cache after second run: %u bytes", (unsigned)g_ticket_blob_len);
}

#ifdef CONFIG_IDF_TARGET_LINUX
int main(void)
{
    app_main();
    return 0;
}
#endif