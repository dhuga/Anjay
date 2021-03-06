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

#ifndef ANJAY_INTERFACE_REGISTER_H
#define ANJAY_INTERFACE_REGISTER_H

#include "../anjay_core.h"

VISIBILITY_PRIVATE_HEADER_BEGIN

void _anjay_registration_info_cleanup(anjay_registration_info_t *info);

int _anjay_register(anjay_t *anjay);

#define ANJAY_REGISTRATION_UPDATE_REJECTED 1

/**
 * @returns:
 * - 0 on success,
 * - a negative value on error,
 * - ANJAY_REGISTRATION_UPDATE_REJECTED if the server responded with 4.xx error
 *   so the Update message should not be retransmitted.
 */
int _anjay_update_registration(anjay_t *anjay);

int _anjay_deregister(anjay_t *anjay);

/**
 * @returns Amount of time from now until the server registration expires.
 */
avs_time_duration_t
_anjay_register_time_remaining(const anjay_registration_info_t *info);

VISIBILITY_PRIVATE_HEADER_END

#endif // ANJAY_INTERFACE_REGISTER_H
