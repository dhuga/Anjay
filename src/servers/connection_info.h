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

#ifndef ANJAY_SERVERS_CONNECTION_INFO_H
#define ANJAY_SERVERS_CONNECTION_INFO_H

#include "../anjay_core.h"
#include "../utils_core.h"

#ifndef ANJAY_SERVERS_INTERNALS
#error "Headers from servers/ are not meant to be included from outside"
#endif

VISIBILITY_PRIVATE_HEADER_BEGIN

avs_net_abstract_socket_t *_anjay_connection_internal_get_socket(
        const anjay_server_connection_t *connection);

void
_anjay_connection_internal_clean_socket(anjay_server_connection_t *connection);

bool
_anjay_connection_internal_is_online(anjay_server_connection_t *connection);

/**
 * @returns @li 0 on success,
 *          @li a positive errno value in case of a primary socket (UDP) error,
 *          @li a negative value in case of other error.
 */
int _anjay_server_refresh(anjay_t *anjay,
                          anjay_active_server_info_t *server,
                          bool force_reconnect);

VISIBILITY_PRIVATE_HEADER_END

#endif // ANJAY_SERVERS_CONNECTION_INFO_H
