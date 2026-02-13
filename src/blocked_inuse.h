/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 *
 * Client blocking mechanism for keys currently in use by other operations.
 *
 * This module provides a specialized blocking system that prevents concurrent
 * access to keys that are actively being modified or processed. Unlike the
 * generic blocking operations in blocked.c, this mechanism blocks clients when
 * they attempt to access keys that are marked as "in use" by internal
 * operations such as bgIteration.
 *
 * Key features:
 *   - Blocks clients on multiple keys simultaneously
 *   - Automatically unblocks clients when all their requested keys become
 *     available
 *   - Maintains bidirectional mappings: client->keys and key->clients
 *   - Integrates with the server's event loop via processUnblockedClients()
 *     in blocked.c
 *
 * Workflow:
 *   1. blockInuse_blockClientOnKeys() - Block a client on a set of keys
 *   2. Keys remain blocked until explicitly unblocked
 *   3. blockInuse_unblockClientsOnKey() - Unblock specific key, triggering
 *      clients resumption
 *   4. processUnblockedClients() - Process unblocked clients in beforeSleep()
 *
 * This is used to ensure data consistency during operations that require
 * exclusive access to keys, preventing race conditions and maintaining
 * transactional integrity.
 *
 */

#ifndef BLOCKED_INUSE_H__
#define BLOCKED_INUSE_H__

struct robj;   // defined in server.h
struct client; // defined in server.h

/* Check if client is blocked by blockInuse */
int blockInuse_clientBlocked(client *c);

/* Initialize blockInuse structures, must be called once during server startup. */
void blockInuse_init(void);

/* Free blockInuse data structures, no clients must be blocked by blockInuse. */
void blockInuse_release(void);

/* Return the number of clients currently blocked by blockInuse. */
int blockInuse_getNumberOfBlockedClients(void);

/*
 * Block a client on a set of keys. Duplicate keys are allowed and handled.
 *
 * To avoid extra copying, this API keeps references to the passed key objects.
 */
void blockInuse_blockClientOnKeys(client *c, int nKeys, robj *keys[]);

/*
 * Unblock clients blocked on the given key.
 *
 * A client is unblocked only when it has no remaining dependencies on any
 * blocked keys. Such clients are added to the server.unblocked_clients list and
 * resumed later during processUnblockedClients() in blocked.c.
 */
void blockInuse_unblockClientsOnKey(robj *key);

/*
 * Unblock all clients blocked by blockInuse on all keys.
 *
 * Clients that become unblocked are added to the server.unblocked_clients
 * list and resumed later during processUnblockedClients().
 */
void blockInuse_unblockClientsOnAllKeys(void);

/*
 * Unlink a client currently blocked by blockInuse. Typically used when
 * a client is being freed while still blocked (e.g., client-initiated disconnect).
 *
 * This function removes the client from all blockInuse data structures.
 */
void blockInuse_unlinkClient(client *c);

#endif
