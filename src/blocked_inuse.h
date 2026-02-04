/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Client blocking mechanism for keys currently in use by other operations.
 *
 * This module provides a specialized blocking system that prevents concurrent access to keys
 * that are actively being modified or processed. Unlike the generic blocking operations in
 * blocked.c (BLPOP, WAIT, etc.), this mechanism blocks clients when they attempt to access
 * keys that are marked as "in use" by internal operations such as bgIteration.
 *
 * Key features:
 * - Blocks clients on multiple keys simultaneously
 * - Automatically unblocks clients when all their requested keys become available
 * - Maintains bidirectional mappings: client->keys and key->clients
 * - Integrates with the server's event loop via processServerBlockedClients()
 * - Tracks blocking statistics and lifetime metrics
 *
 * Typical workflow:
 * 1. blockInuse_blockClientOnKeys() - Block a client on a set of keys
 * 2. Keys remain blocked until explicitly unblocked
 * 3. blockInuse_unblockClientsOnKey() - Unblock specific key, triggering client resumption
 * 4. blockInuse_processServerBlockedClients() - Process unblocked clients in beforeSleep()
 *
 * This is used to ensure data consistency during operations that require exclusive access
 * to keys, preventing race conditions and maintaining transactional integrity.
 */

#ifndef BLOCKED_INUSE_H__
#define BLOCKED_INUSE_H__

#include "hashtable.h"
#include "adlist.h"

struct robj;                     //defined in server.h
struct client;                   //defined in server.h

/* Check if client is blocked by blockInuse */
#define blockInuse_isBlockedClient(c) ((c)->flag.blockInuse_blocked)

/* Initialize blockInuse structures. Call once at server startup. */
void blockInuse_init(void);

/* Free blockInuse data structures, no clients should be blocked by blockInuse at this time. */
void blockInuse_release(void);

/* Returns the total count of currently blocked clients by blockInuse */
int blockInuse_getNumberOfBlockedClients(void);

/*
 * Block given client on set of keys. Duplicated keys are handled.
 * To avoid the extra copy, we keep reference to the passed keys. So passed variable keys, should be heap allocated.
 * API asserts that the client do not already have a blocked/unblocked flag set.
 * Return Value:
 * C_ERR: if
 *      a. Any passed key is not sds.
 *      b. nKeys = 0.
 *      c. Client is slave client.
 *  Otherwise, it blocks the client and returns C_OK.
 * */
// Blocks a client on a set of keys.
// Then client will remain blocked until all keys are unblocked.
int blockInuse_blockClientOnKeys(client *c, int nKeys, robj *keys[]);

/* Unblock given key. A client will be unblocked if it has no more dependency on any key and will be
 * put into unblocked_clients list. Clients from this list are processed during processUnblockedClients.
 */
void blockInuse_unblockClientsOnKey(robj *key);

/* Unblock all clients on all keys */
void blockInuse_unblockClientsOnAllKeys(void);

/* If clientBlocking is enabled, this function is called in beforeSleep each time, to resume clients which were previously blocked. */
void blockInuse_processServerBlockedClients(void);

/*
 * This API is to force unlinking of a blocked client. Typically required when we want to free the client while its blocked (e.g. memory pressure).
 * This will clean up the current command arguments and detach all the references in blocking structures.
 */
void blockInuse_unlinkClient(client *c);

#endif
