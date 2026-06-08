// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the webhook handler: resolve real socket function pointers
// and set up the webhooks JSON file path. Must be called once at startup
// (e.g. from the LD_PRELOAD library constructor or hook()).
void webhook_handler_init(void);

// Inject webhooks from webhooks.json into the WebhookManager's internal
// per-user std::vector<std::string>. Called from the sub_125ACA6 hook
// right after the original function runs.
void webhook_inject_into_manager(void* manager);

// Remember the live WebhookManager pointer so CRUD changes can refresh the
// in-memory dispatch list without requiring a PMS restart.
void webhook_set_manager(void* manager);

// Set the resolved address of sub_125E524 (webhook map find/create).
void webhook_set_sub_125E524(void* addr);

#ifdef __cplusplus
}
#endif
