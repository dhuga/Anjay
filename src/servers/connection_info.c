/*
 * Copyright 2017-2018 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include <inttypes.h>

#include <avsystem/commons/stream/stream_net.h>
#include <avsystem/commons/utils.h>

#include "../utils_core.h"
#include "../dm/query.h"

#define ANJAY_SERVERS_CONNECTION_INFO_C
#define ANJAY_SERVERS_INTERNALS

#include "connection_info.h"

VISIBILITY_SOURCE_BEGIN

typedef struct {
    anjay_server_connection_mode_t mode;
    char local_port[ANJAY_MAX_URL_PORT_SIZE];
    anjay_udp_security_mode_t security_mode;
} udp_connection_info_t;


typedef struct {
    anjay_iid_t security_iid;
    const anjay_url_t *uri;
    udp_connection_info_t udp;
} server_connection_info_t;

#define EMPTY_SERVER_INFO_INITIALIZER \
        { \
            .udp = { ANJAY_CONNECTION_DISABLED }, \
        }

avs_net_abstract_socket_t *_anjay_connection_internal_get_socket(
        const anjay_server_connection_t *connection) {
    return connection->conn_priv_data_.socket;
}

void
_anjay_connection_internal_clean_socket(anjay_server_connection_t *connection) {
    avs_net_socket_cleanup(&connection->conn_priv_data_.socket);
    memset(&connection->conn_priv_data_, 0,
           sizeof(connection->conn_priv_data_));
}

static anjay_binding_mode_t read_binding_mode(anjay_t *anjay,
                                              anjay_ssid_t ssid) {
    char buf[8];
    anjay_uri_path_t path =
            MAKE_RESOURCE_PATH(ANJAY_DM_OID_SERVER, ANJAY_IID_INVALID,
                               ANJAY_DM_RID_SERVER_BINDING);

    if (!_anjay_find_server_iid(anjay, ssid, &path.iid)
            && !_anjay_dm_res_read_string(anjay, &path, buf, sizeof(buf))) {
        return anjay_binding_mode_from_str(buf);
    } else {
        anjay_log(WARNING, "could not read binding mode for LwM2M server %u",
                  ssid);
        // pass through
    }
    return ANJAY_BINDING_NONE;
}

static struct {
    anjay_binding_mode_t binding;
    struct {
        anjay_server_connection_mode_t udp;
        anjay_server_connection_mode_t sms;
    } connection;
} const BINDING_TO_CONNECTIONS[] = {
    { ANJAY_BINDING_U,   { .udp = ANJAY_CONNECTION_ONLINE,
                           .sms = ANJAY_CONNECTION_DISABLED } },
    { ANJAY_BINDING_UQ,  { .udp = ANJAY_CONNECTION_QUEUE,
                           .sms = ANJAY_CONNECTION_DISABLED } },
    { ANJAY_BINDING_S,   { .udp = ANJAY_CONNECTION_DISABLED,
                           .sms = ANJAY_CONNECTION_ONLINE } },
    { ANJAY_BINDING_SQ,  { .udp = ANJAY_CONNECTION_DISABLED,
                           .sms = ANJAY_CONNECTION_QUEUE } },
    { ANJAY_BINDING_US,  { .udp = ANJAY_CONNECTION_ONLINE,
                           .sms = ANJAY_CONNECTION_ONLINE } },
    { ANJAY_BINDING_UQS, { .udp = ANJAY_CONNECTION_QUEUE,
                           .sms = ANJAY_CONNECTION_ONLINE } }
};

static int read_connection_modes(anjay_t *anjay,
                                 const anjay_active_server_info_t *server,
                                 anjay_server_connection_mode_t *out_udp_mode,
                                 anjay_server_connection_mode_t *out_sms_mode) {
    if (server->ssid != ANJAY_SSID_BOOTSTRAP) {
        anjay_binding_mode_t binding_mode = read_binding_mode(anjay,
                                                              server->ssid);
        for (size_t i = 0; i < AVS_ARRAY_SIZE(BINDING_TO_CONNECTIONS); ++i) {
            if (BINDING_TO_CONNECTIONS[i].binding == binding_mode) {
                if (out_udp_mode) {
                    *out_udp_mode = BINDING_TO_CONNECTIONS[i].connection.udp;
                }
                if (out_sms_mode) {
                    *out_sms_mode = BINDING_TO_CONNECTIONS[i].connection.sms;
                }
                return 0;
            }
        }
        anjay_log(ERROR, "could not read binding mode");
        return -1;
    } else {
        if (out_udp_mode) {
            *out_udp_mode = ANJAY_CONNECTION_ONLINE;
        }
        if (out_sms_mode) {
            *out_sms_mode = (_anjay_sms_router(anjay)
                    ? *out_udp_mode : ANJAY_CONNECTION_DISABLED);
        }
        return 0;
    }
}

anjay_server_connection_mode_t
_anjay_connection_current_mode(anjay_connection_ref_t ref) {
    anjay_server_connection_t *connection = _anjay_get_server_connection(ref);
    if (connection && _anjay_connection_internal_get_socket(connection)) {
        if (connection->queue_mode) {
            return ANJAY_CONNECTION_QUEUE;
        } else {
            return ANJAY_CONNECTION_ONLINE;
        }
    } else {
        return ANJAY_CONNECTION_DISABLED;
    }
}

bool
_anjay_connection_internal_is_online(anjay_server_connection_t *connection) {
    avs_net_abstract_socket_t *socket =
            _anjay_connection_internal_get_socket(connection);
    if (!socket) {
        return false;
    }
    avs_net_socket_opt_value_t opt;
    if (avs_net_socket_get_opt(socket, AVS_NET_SOCKET_OPT_STATE, &opt)) {
        anjay_log(ERROR, "Could not get socket state");
        return false;
    }
    return opt.state == AVS_NET_SOCKET_STATE_CONNECTED;
}

bool _anjay_connection_is_online(anjay_connection_ref_t ref) {
    anjay_server_connection_t *connection = _anjay_get_server_connection(ref);
    return connection && _anjay_connection_internal_is_online(connection);
}

static anjay_binding_mode_t
binding_mode_from_connection_modes(anjay_server_connection_mode_t udp_mode,
                                   anjay_server_connection_mode_t sms_mode) {
    for (size_t i = 0; i < AVS_ARRAY_SIZE(BINDING_TO_CONNECTIONS); ++i) {
        if (BINDING_TO_CONNECTIONS[i].connection.udp == udp_mode
                && BINDING_TO_CONNECTIONS[i].connection.sms == sms_mode) {
            return BINDING_TO_CONNECTIONS[i].binding;
        }
    }
    return ANJAY_BINDING_NONE;
}

anjay_binding_mode_t
_anjay_server_cached_binding_mode(anjay_active_server_info_t *server) {
    if (!server) {
        return ANJAY_BINDING_NONE;
    }
    anjay_server_connection_mode_t udp_mode =
            _anjay_connection_current_mode((anjay_connection_ref_t) {
                                               .server = server,
                                               .conn_type = ANJAY_CONNECTION_UDP
                                           });
    anjay_server_connection_mode_t sms_mode =
            _anjay_connection_current_mode((anjay_connection_ref_t) {
                                               .server = server,
                                               .conn_type = ANJAY_CONNECTION_SMS
                                           });
    return binding_mode_from_connection_modes(udp_mode, sms_mode);
}

typedef anjay_server_connection_mode_t
get_connection_mode_t(const server_connection_info_t *info);

typedef int get_connection_info_t(anjay_t *anjay,
                                  server_connection_info_t *inout_info,
                                  anjay_server_dtls_keys_t *out_dtls_keys,
                                  avs_net_abstract_socket_t *existing_socket);

typedef int get_net_security_info_t(avs_net_security_info_t *out_net_info,
                                    const server_connection_info_t *info,
                                    const anjay_server_dtls_keys_t *dtls_keys);

typedef int
create_connected_socket_t(anjay_t *anjay,
                          anjay_server_connection_t *out_connection,
                          avs_net_ssl_configuration_t *inout_socket_config,
                          const server_connection_info_t *info);

typedef struct {
    const char *name;
    anjay_connection_type_t type;
    get_connection_mode_t *get_connection_mode;
    get_connection_info_t *get_connection_info;
    get_net_security_info_t *get_net_security_info;
    create_connected_socket_t *create_connected_socket;
} connection_type_definition_t;

static int recreate_socket(anjay_t *anjay,
                           const connection_type_definition_t *def,
                           anjay_server_connection_t *connection,
                           server_connection_info_t *inout_info) {
    anjay_server_dtls_keys_t dtls_keys;
    memset(&dtls_keys, 0, sizeof(dtls_keys));

    // At this point, inout_info has "global" settings filled,
    // but transport-specific (i.e. UDP or SMS) fields are not
    if (def->get_connection_info(
            anjay, inout_info, &dtls_keys,
            _anjay_connection_internal_get_socket(connection))) {
        anjay_log(DEBUG, "could not get %s connection info for server /%u/%u",
                  def->name, ANJAY_DM_OID_SECURITY, inout_info->security_iid);
        return -1;
    }
    _anjay_connection_internal_clean_socket(connection);

    // Socket configuration is slightly different between UDP and SMS
    // connections. That's why we do the common configuration here...
    avs_net_ssl_configuration_t socket_config;
    memset(&socket_config, 0, sizeof(socket_config));
    socket_config.version = anjay->dtls_version;
    socket_config.use_session_resumption = true;
    int result = def->get_net_security_info(&socket_config.security,
                                            inout_info, &dtls_keys);
    // ...and pass it as an in/out argument to create_connected_socket() so that
    // it can do any protocol-specific modifications.
    if (!result) {
        result = def->create_connected_socket(anjay, connection, &socket_config,
                                              inout_info);
    }
    if (!result) {
        result = avs_net_socket_get_local_port(
                connection->conn_priv_data_.socket,
                connection->conn_priv_data_.last_local_port,
                sizeof(connection->conn_priv_data_.last_local_port));
    }
    if (result) {
        avs_net_abstract_socket_t *sock = connection->conn_priv_data_.socket;
        if (sock) {
            avs_net_socket_close(sock);
        }
    }
    return result;
}

typedef enum {
    RESULT_ERROR,
    RESULT_DISABLED,
    RESULT_RESUMED,
    RESULT_NEW_CONNECTION
} refresh_connection_result_t;

static refresh_connection_result_t
ensure_socket_connected(anjay_t *anjay,
                        const connection_type_definition_t *def,
                        anjay_server_connection_t *connection,
                        server_connection_info_t *inout_info,
                        bool reconnect,
                        int *out_socket_errno) {
    bool session_resume = false;
    avs_net_abstract_socket_t *existing_socket =
            _anjay_connection_internal_get_socket(connection);

    *out_socket_errno = 0;

    if (existing_socket == NULL) {
        int result = recreate_socket(anjay, def, connection, inout_info);
        if (result) {
            *out_socket_errno = -result;
            return RESULT_ERROR;
        }
    } else {
        if (reconnect) {
            avs_net_socket_close(existing_socket);
        }
        if (_anjay_connection_internal_is_online(connection)) {
            session_resume = true;
        } else {
            int result = _anjay_connection_bring_online(connection,
                                                        &session_resume);
            if (result) {
                *out_socket_errno = -result;
                return RESULT_ERROR;
            }
        }
    }
    return session_resume ? RESULT_RESUMED : RESULT_NEW_CONNECTION;
}

static refresh_connection_result_t
refresh_connection(anjay_t *anjay,
                   const connection_type_definition_t *def,
                   anjay_active_server_info_t *server,
                   server_connection_info_t *inout_info,
                   bool force_reconnect,
                   int *out_socket_errno) {
    anjay_server_connection_t *out_connection =
            _anjay_get_server_connection((anjay_connection_ref_t) {
                .server = server,
                .conn_type = def->type
            });
    assert(out_connection);
    refresh_connection_result_t result = RESULT_DISABLED;

    *out_socket_errno = 0;

    if (def->get_connection_mode(inout_info) == ANJAY_CONNECTION_DISABLED) {
        _anjay_connection_internal_clean_socket(out_connection);
    } else {
        result = ensure_socket_connected(
                anjay, def, out_connection, inout_info,
                force_reconnect || out_connection->needs_reconnect,
                out_socket_errno);
    }
    out_connection->needs_reconnect = false;
    out_connection->queue_mode =
            (def->get_connection_mode(inout_info) == ANJAY_CONNECTION_QUEUE);
    return result;
}

static anjay_server_connection_mode_t
get_udp_connection_mode(const server_connection_info_t *info) {
    return info->udp.mode;
}

static int init_psk_security(avs_net_security_info_t *security,
                             const anjay_server_dtls_keys_t *keys) {
    *security = avs_net_security_info_from_psk(
        (avs_net_psk_t){
            .psk = keys->secret_key,
            .psk_size = keys->secret_key_size,
            .identity = keys->pk_or_identity,
            .identity_size = keys->pk_or_identity_size
        });
    return 0;
}

static int init_cert_security(avs_net_security_info_t *security,
                              const anjay_server_dtls_keys_t *keys) {
    avs_net_client_cert_t client_cert =
            avs_net_client_cert_from_x509(keys->pk_or_identity,
                                          keys->pk_or_identity_size);

    avs_net_private_key_t private_key =
            avs_net_private_key_from_pkcs8(keys->secret_key,
                                           keys->secret_key_size, NULL);

    const void *raw_cert_der = keys->server_pk_or_identity_size > 0
            ? keys->server_pk_or_identity
            : NULL;
    avs_net_trusted_cert_source_t ca = avs_net_trusted_cert_source_from_x509(
            raw_cert_der, keys->server_pk_or_identity_size);

    *security = avs_net_security_info_from_certificates(
            (avs_net_certificate_info_t) {
                .server_cert_validation = !!raw_cert_der,
                .trusted_certs = ca,
                .client_cert = client_cert,
                .client_key = private_key
            });

    return 0;
}

static int get_udp_net_security_info(avs_net_security_info_t *out_net_info,
                                     const server_connection_info_t *info,
                                     const anjay_server_dtls_keys_t *dtls_keys) {
    switch (info->udp.security_mode) {
    case ANJAY_UDP_SECURITY_NOSEC:
        return 0;
    case ANJAY_UDP_SECURITY_PSK:
        return init_psk_security(out_net_info, dtls_keys);
    case ANJAY_UDP_SECURITY_CERTIFICATE:
        return init_cert_security(out_net_info, dtls_keys);
    case ANJAY_UDP_SECURITY_RPK:
    default:
        anjay_log(ERROR, "unsupported security mode: %d",
                  (int) info->udp.security_mode);
        return -1;
    }
}

static int get_udp_security_mode(anjay_t *anjay,
                                 anjay_iid_t security_iid,
                                 anjay_udp_security_mode_t *out_mode) {
    int64_t mode;
    const anjay_uri_path_t path =
            MAKE_RESOURCE_PATH(ANJAY_DM_OID_SECURITY, security_iid,
                               ANJAY_DM_RID_SECURITY_MODE);

    if (_anjay_dm_res_read_i64(anjay, &path, &mode)) {
        anjay_log(ERROR, "could not read LwM2M server security mode");
        return -1;
    }

    switch (mode) {
    case ANJAY_UDP_SECURITY_RPK:
        anjay_log(ERROR, "unsupported security mode: %" PRId64, mode);
        return -1;
    case ANJAY_UDP_SECURITY_NOSEC:
    case ANJAY_UDP_SECURITY_PSK:
    case ANJAY_UDP_SECURITY_CERTIFICATE:
        *out_mode = (anjay_udp_security_mode_t) mode;
        return 0;
    default:
        anjay_log(ERROR, "invalid security mode: %" PRId64, mode);
        return -1;
    }
}

static bool uri_protocol_matching(anjay_udp_security_mode_t security_mode,
                                  const anjay_url_t *uri) {
    const char *expected_proto = (security_mode == ANJAY_UDP_SECURITY_NOSEC)
            ? "coap" : "coaps";

    if (strcmp(uri->protocol, expected_proto)) {
        anjay_log(WARNING, "URI protocol mismatch: security mode %d requires "
                  "'%s', but '%s' was configured", (int) security_mode,
                  expected_proto, uri->protocol);
        return false;
    }

    return true;
}

static int get_udp_dtls_keys(anjay_t *anjay,
                             anjay_iid_t security_iid,
                             anjay_udp_security_mode_t security_mode,
                             anjay_server_dtls_keys_t *out_keys) {
    if (security_mode == ANJAY_UDP_SECURITY_NOSEC) {
        return 0;
    }

    const struct {
        bool required;
        anjay_rid_t rid;
        char *buffer;
        size_t buffer_capacity;
        size_t *buffer_size_ptr;
    } values[] = {
        {
            true,
            ANJAY_DM_RID_SECURITY_PK_OR_IDENTITY,
            out_keys->pk_or_identity,
            sizeof(out_keys->pk_or_identity),
            &out_keys->pk_or_identity_size
        }, {
            security_mode != ANJAY_UDP_SECURITY_PSK,
            ANJAY_DM_RID_SECURITY_SERVER_PK_OR_IDENTITY,
            out_keys->server_pk_or_identity,
            sizeof(out_keys->server_pk_or_identity),
            &out_keys->server_pk_or_identity_size
        }, {
            true,
            ANJAY_DM_RID_SECURITY_SECRET_KEY,
            out_keys->secret_key,
            sizeof(out_keys->secret_key),
            &out_keys->secret_key_size
        }
    };

    for (size_t i = 0; i < AVS_ARRAY_SIZE(values); ++i) {
        const anjay_uri_path_t path =
                MAKE_RESOURCE_PATH(ANJAY_DM_OID_SECURITY, security_iid,
                                   values[i].rid);
        if (_anjay_dm_res_read(anjay, &path, values[i].buffer,
                               values[i].buffer_capacity,
                               values[i].buffer_size_ptr)
                && values[i].required) {
            anjay_log(WARNING, "read %s failed", ANJAY_DEBUG_MAKE_PATH(&path));
            return -1;
        }
    }

    return 0;
}

static void
get_requested_local_port(char out_port[static ANJAY_MAX_URL_PORT_SIZE],
                         anjay_t *anjay,
                         avs_net_abstract_socket_t *socket) {
    if (socket) {
        if (!avs_net_socket_get_local_port(socket, out_port,
                                           ANJAY_MAX_URL_PORT_SIZE)) {
            return;
        }

        anjay_log(DEBUG, "could not read local port from old socket");
    }

    if (anjay->udp_listen_port > 0
            && avs_simple_snprintf(out_port, ANJAY_MAX_URL_PORT_SIZE,
                                   "%" PRIu16, anjay->udp_listen_port) >= 0) {
        return;
    }

    out_port[0] = '\0';
}

static int get_udp_connection_info(anjay_t *anjay,
                                   server_connection_info_t *inout_info,
                                   anjay_server_dtls_keys_t *dtls_keys,
                                   avs_net_abstract_socket_t *old_socket) {
    if (get_udp_security_mode(anjay, inout_info->security_iid,
                              &inout_info->udp.security_mode)
            || (inout_info->uri
                    && !uri_protocol_matching(inout_info->udp.security_mode,
                                              inout_info->uri))
            || get_udp_dtls_keys(anjay, inout_info->security_iid,
                                 inout_info->udp.security_mode, dtls_keys)) {
        return -1;
    }

    get_requested_local_port(inout_info->udp.local_port, anjay, old_socket);

    anjay_log(DEBUG, "server /%u/%u: local port %s, UDP security mode = %d",
              ANJAY_DM_OID_SECURITY, inout_info->security_iid,
              inout_info->udp.local_port, (int) inout_info->udp.security_mode);
    return 0;
}

static int
create_connected_udp_socket(anjay_t *anjay,
                            anjay_server_connection_t *out_conn,
                            avs_net_ssl_configuration_t *inout_socket_config,
                            const server_connection_info_t *info) {
    avs_net_socket_type_t type =
            info->udp.security_mode == ANJAY_UDP_SECURITY_NOSEC
                ? AVS_NET_UDP_SOCKET : AVS_NET_DTLS_SOCKET;

    inout_socket_config->backend_configuration = anjay->udp_socket_config;
    inout_socket_config->backend_configuration.reuse_addr = 1;
    inout_socket_config->backend_configuration.preferred_endpoint =
            &out_conn->conn_priv_data_.preferred_endpoint;

    const void *config_ptr = (type == AVS_NET_DTLS_SOCKET)
            ? (const void *) inout_socket_config
            : (const void *) &inout_socket_config->backend_configuration;

    avs_net_abstract_socket_t *socket = NULL;
    int result = _anjay_create_connected_udp_socket(
            anjay, &socket, type, info->udp.local_port, config_ptr, info->uri);
    if (!socket) {
        assert(result);
        anjay_log(ERROR, "could not create CoAP socket");
        return result;
    }

    anjay_log(INFO, "connected to %s:%s", info->uri->host, info->uri->port);
    out_conn->conn_priv_data_.socket = socket;
    return 0;
}

static const connection_type_definition_t UDP_CONNECTION = {
    .name = "UDP",
    .type = ANJAY_CONNECTION_UDP,
    .get_connection_mode = get_udp_connection_mode,
    .get_connection_info = get_udp_connection_info,
    .get_net_security_info = get_udp_net_security_info,
    .create_connected_socket = create_connected_udp_socket
};


static int get_common_connection_info(anjay_t *anjay,
                                      const anjay_active_server_info_t *server,
                                      server_connection_info_t *out_info) {
    if (_anjay_find_security_iid(anjay, server->ssid,
                                 &out_info->security_iid)) {
        anjay_log(ERROR, "could not find server Security IID");
        return -1;
    }

    out_info->uri = &server->uri;

    if (read_connection_modes(anjay, server, &out_info->udp.mode,
                              NULL
                              )) {
        return -1;
    }

    return 0;
}

static bool is_connected(refresh_connection_result_t result) {
    return result == RESULT_RESUMED || result == RESULT_NEW_CONNECTION;
}

int _anjay_server_refresh(anjay_t *anjay,
                          anjay_active_server_info_t *server,
                          bool force_reconnect) {
    anjay_log(TRACE, "refreshing SSID %u, force_reconnect == %d",
              server->ssid, (int) force_reconnect);

    server_connection_info_t server_info = EMPTY_SERVER_INFO_INITIALIZER;
    if (get_common_connection_info(anjay, server, &server_info)) {
        anjay_log(DEBUG, "could not get connection info for SSID %u",
                  server->ssid);
        return -1;
    }

    refresh_connection_result_t udp_result = RESULT_DISABLED;
    int udp_errno = 0;
    refresh_connection_result_t sms_result = RESULT_DISABLED;
    int sms_errno = 0;
    udp_result = refresh_connection(anjay, &UDP_CONNECTION, server,
                                    &server_info, force_reconnect,
                                    &udp_errno);
    (void) sms_errno;

    if (!is_connected(udp_result) && !is_connected(sms_result)) {
        return udp_errno ? udp_errno : -1;
    }


    if ((server->registration_info.conn_type == ANJAY_CONNECTION_UDP
                    && udp_result == RESULT_NEW_CONNECTION)
            || (server->registration_info.conn_type == ANJAY_CONNECTION_SMS
                    && sms_result == RESULT_NEW_CONNECTION)) {
        // mark that the registration connection is no longer valid;
        // forces re-register
        server->registration_info.conn_type = ANJAY_CONNECTION_UNSET;
    }

    return udp_errno;
}

int _anjay_server_setup_registration_connection(
        anjay_active_server_info_t *server) {
    server->registration_info.conn_type = ANJAY_CONNECTION_UNSET;
    anjay_connection_ref_t ref = {
        .server = server
    };
    for (; ref.conn_type < ANJAY_CONNECTION_UNSET;
            ref.conn_type = (anjay_connection_type_t) (ref.conn_type + 1)) {
        if (_anjay_connection_is_online(ref)) {
            server->registration_info.conn_type = ref.conn_type;
            return 0;
        }
    }

    anjay_log(ERROR, "No suitable connection found for SSID = %u",
              server->ssid);
    return -1;
}

static void connection_suspend(anjay_connection_ref_t conn_ref) {
    const anjay_server_connection_t *connection =
            _anjay_get_server_connection(conn_ref);
    if (connection) {
        avs_net_abstract_socket_t *socket =
                _anjay_connection_internal_get_socket(connection);
        if (socket) {
            avs_net_socket_close(socket);
        }
    }
}

void _anjay_connection_suspend(anjay_connection_ref_t conn_ref) {
    if (conn_ref.conn_type == ANJAY_CONNECTION_UNSET) {
        for (conn_ref.conn_type = (anjay_connection_type_t) 0;
                conn_ref.conn_type < ANJAY_CONNECTION_UNSET;
                conn_ref.conn_type =
                        (anjay_connection_type_t) (conn_ref.conn_type + 1)) {
            connection_suspend(conn_ref);
        }
    } else {
        connection_suspend(conn_ref);
    }
}

int _anjay_connection_bring_online(anjay_server_connection_t *connection,
                                   bool *out_session_resumed) {
    assert(connection);
    assert(connection->conn_priv_data_.socket);
    assert(!_anjay_connection_internal_is_online(connection));

    char remote_host[ANJAY_MAX_URL_HOSTNAME_SIZE];
    char remote_port[ANJAY_MAX_URL_PORT_SIZE];
    if (avs_net_socket_get_remote_hostname(connection->conn_priv_data_.socket,
                                           remote_host, sizeof(remote_host))
            || avs_net_socket_get_remote_port(
                    connection->conn_priv_data_.socket,
                    remote_port, sizeof(remote_port))) {
        anjay_log(ERROR, "Could not get peer address and port "
                         "of a suspended connection");
        return -1;
    }

    /*
     * avs_net_socket_bind() is usually called, EXCEPT when:
     * - it's an SMS socket
     * - it's a UDP socket and:
     *   - no listening port has been explicitly specified, and
     *   - it's a fresh socket and previously used listening port is unknown
     * It is safe not to call bind(), because connect() is called below, which
     * will automatically bind the socket to a new ephemeral port.
     */
    if (*connection->conn_priv_data_.last_local_port) {
        /*
         * Whenever the socket is bound by connect(), the address family is
         * set to match the remote address. If the socket is bound by a
         * bind() call with NULL local_addr argument, the address family
         * falls back to the original socket preference - by default,
         * AF_UNSPEC. This causes avs_net to attempt to bind to [::]:$PORT,
         * even though the remote host may be an IPv4 address. This generally
         * works, because IPv4-mapped IPv6 addresses are a thing.
         *
         * On FreeBSD though, IPv4-mapped IPv6 are disabled by default (see:
         * "Interaction between IPv4/v6 sockets" at
         * https://www.freebsd.org/cgi/man.cgi?query=inet6&sektion=4), which
         * effectively breaks all connect() calls after re-binding to a
         * recently used port.
         *
         * To avoid that, we need to provide a local wildcard address
         * appropriate for the family used by the remote host.
         */
        const char *local_addr = NULL;
        if (strchr(remote_host, ':') != NULL) {
            local_addr = "::";
        } else if (strchr(remote_host, '.') != NULL) {
            local_addr = "0.0.0.0";
        }

        if (avs_net_socket_bind(
                    connection->conn_priv_data_.socket, local_addr,
                    connection->conn_priv_data_.last_local_port)) {
            anjay_log(ERROR, "could not bind socket to port %s",
                      connection->conn_priv_data_.last_local_port);
            goto close_and_fail;
        }
    }

    if (avs_net_socket_connect(connection->conn_priv_data_.socket,
                               remote_host, remote_port)) {
        anjay_log(ERROR, "could not connect to %s:%s",
                  remote_host, remote_port);
        goto close_and_fail;
    }

    avs_net_socket_opt_value_t session_resumed = { .flag = true };
    if (avs_net_socket_get_opt(connection->conn_priv_data_.socket,
                               AVS_NET_SOCKET_OPT_SESSION_RESUMED,
                               &session_resumed)) {
        // if avs_net_socket_get_opt() failed, it means that it's not a DTLS
        // socket; if remote_port is empty, it means that it's an SMS socket;
        // we treat a non-DTLS SMS socket as always "resumed",
        // because MSISDN will not change during the library lifetime
        *out_session_resumed = !*remote_port;
    } else {
        *out_session_resumed = session_resumed.flag;
    }
    anjay_log(INFO, "%s to %s:%s",
              *out_session_resumed ? "resumed connection" : "reconnected",
              remote_host, remote_port);
    return 0;

    // Labels must be followed by a statement, not a declaration.
    // Fortunately, `;` is a perfectly fine statement in C.
close_and_fail:;
    int result = avs_net_socket_errno(connection->conn_priv_data_.socket);
    if (avs_net_socket_close(connection->conn_priv_data_.socket)) {
        anjay_log(ERROR, "Could not close the socket (?!)");
    }
    return result;
}

int _anjay_get_security_info(anjay_t *anjay,
                             avs_net_security_info_t *out_net_info,
                             anjay_server_dtls_keys_t *out_dtls_keys,
                             anjay_iid_t security_iid,
                             anjay_connection_type_t conn_type) {
    const connection_type_definition_t *conn_def;
    switch (conn_type) {
    case ANJAY_CONNECTION_UDP:
        conn_def = &UDP_CONNECTION;
        break;
    default:
        assert(0 && "Should never happen");
        return -1;
    }

    server_connection_info_t info = {
        .security_iid = security_iid
    };

    memset(out_dtls_keys, 0, sizeof(*out_dtls_keys));
    int result;
    (void) ((result = conn_def->get_connection_info(anjay, &info, out_dtls_keys,
                                                    NULL))
            || (result = conn_def->get_net_security_info(out_net_info, &info,
                                                         out_dtls_keys)));
    return result;
}
