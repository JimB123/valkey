/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BlockInuse - Client blocking mechanism for keys that are currently
 * in use by other operations.
 */

#include "server.h"
#include "blocked_inuse.h"

/* External hashtable functions from server.c */
extern uint64_t dictEncObjHash(const void *key);
extern int hashtableEncObjKeyCompare(const void *key1, const void *key2);
extern uint64_t hashtableClientHash(const void *key);
extern int hashtableClientKeyCompare(const void *key1, const void *key2);

// Internal blockInuse data structure
static hashtable *client_to_keys;        /* Maps client pointers to blockInuse_clientMetadata (list of keys). */
static hashtable *key_to_clients;        /* Maps keys to a list of clients blocked on them. */
static uint32_t blocked_clients_on_keys; /* Current number of clients blocked on keys. */

/* Metadata for a blocked client */
typedef struct blockInuse_clientMetadata {
    robj **keys;          /* Array of keys the client is blocked on */
    int n_keys;           /* Number of keys in the array */
    long long blocked_at; /* Timestamp when client was blocked (from server.mstime) */
} blockInuse_clientMetadata;

/* ----------------------------- client_to_keys Hashtable util ------------------------- */
/* Entry for client_to_keys hashtable */
typedef struct {
    client *c;
    blockInuse_clientMetadata metadata;
} clientDataEntry;

/* Hashtable callbacks for client -> blockInuse_clientMetadata */
static const void *clientDataEntryGetKey(const void *entry) {
    return ((clientDataEntry *)entry)->c;
}

static void clientDataEntryDestructor(void *entry) {
    clientDataEntry *e = entry;
    if (e->metadata.keys) {
        // Refcounts for all keys should be decreased before calling this function
        // Ensure that n_keys is 0 before freeing keys
        serverAssert(e->metadata.n_keys == 0);

        // Decrease refcount for each key (defensive, in case n_keys > 0)
        for (int i = 0; i < e->metadata.n_keys; i++) {
            decrRefCount(e->metadata.keys[i]);
        }
        zfree(e->metadata.keys);
    }
    zfree(entry);
}

static hashtableType clientDataHashtableType = {
    .entryGetKey = clientDataEntryGetKey,
    .hashFunction = hashtableClientHash,
    .keyCompare = hashtableClientKeyCompare,
    .entryDestructor = clientDataEntryDestructor,
};

/* Utility functions for client_to_keys hashtable */

// Find metadata for a client; create if not found
static blockInuse_clientMetadata *addOrFindClientMetadata(client *c) {
    clientDataEntry *entry;
    if (hashtableFind(client_to_keys, c, (void **)&entry)) {
        return &entry->metadata;
    }

    entry = zcalloc(sizeof(clientDataEntry));
    entry->c = c;
    hashtableAdd(client_to_keys, entry);
    return &entry->metadata;
}

// Get metadata for a client; return NULL if not found
static blockInuse_clientMetadata *getClientMetadata(client *c) {
    clientDataEntry *entry;
    if (hashtableFind(client_to_keys, c, (void **)&entry)) {
        return &entry->metadata;
    }
    return NULL;
}

// Remove a client and its metadata from the hashtable
static void removeClientMetadata(client *c) {
    hashtablePop(client_to_keys, c, NULL);
}

/* ----------------------------- key_to_clients Hashtable Util ------------------------- */

/* Hashtable entry: maps a key object to the list of clients blocked on it */
typedef struct {
    robj *key;     /* Key object */
    list *clients; /* List of clients blocked on this key */
} keyToClientsEntry;

/* Hashtable callbacks */

static const void *keyToClientsGetKey(const void *entry) {
    return ((keyToClientsEntry *)entry)->key;
}

static void keyToClientsDestructor(void *entry) {
    keyToClientsEntry *e = entry;
    decrRefCount(e->key);
    listRelease(e->clients);
    zfree(entry);
}

static hashtableType keyToClientsHashtableType = {
    .entryGetKey = keyToClientsGetKey,
    .hashFunction = dictEncObjHash,
    .keyCompare = hashtableEncObjKeyCompare,
    .entryDestructor = keyToClientsDestructor,
};

/* Utility functions for key_to_clients hashtable */

// Get or create the list of clients blocked on a key
static list *addOrFindBlockedClientsListsByKey(robj *key) {
    keyToClientsEntry *entry;
    if (hashtableFind(key_to_clients, key, (void **)&entry)) {
        return entry->clients;
    }

    entry = zcalloc(sizeof(keyToClientsEntry));
    entry->key = key;
    incrRefCount(key);
    entry->clients = listCreate();
    hashtableAdd(key_to_clients, entry);
    return entry->clients;
}

// Get the list of clients blocked on a key, or NULL if none
static list *getBlockedClientsListsByKey(robj *key) {
    keyToClientsEntry *entry;
    if (hashtableFind(key_to_clients, key, (void **)&entry)) {
        return entry->clients;
    }
    return NULL;
}

// Remove a key and its client list from the hashtable
static void removeBlockedClientsListsByKey(robj *key) {
    hashtablePop(key_to_clients, key, NULL);
}

/* ----------------------------- util ------------------------- */

static void markClientBlocked(client *c) {
    c->flag.blockInuse_blocked = 1;
    c->flag.pending_command = 1;
}

/*
 * Initialize metadata for a client and insert it into client_to_keys hashtable.
 *
 * nKeys specifies the size for the keys array.
 * Returns the pointer to the initialized metadata.
 */
static blockInuse_clientMetadata *initClientMetadata(client *c, int nKeys) {
    serverAssert(!getClientMetadata(c)); // client must not already exist
    serverAssert(nKeys >= 0);

    blockInuse_clientMetadata *metadata = addOrFindClientMetadata(c);
    metadata->n_keys = 0;
    metadata->keys = NULL;
    metadata->blocked_at = server.mstime;
    if (nKeys > 0) {
        metadata->keys = zmalloc(sizeof(robj *) * nKeys);
    }
    return metadata;
}

/*
 * Unlink a blocked client from all key_to_clients entries
 * and release references in its client metadata.
 */
static void unlinkBlockedClientOnKeys(client *c) {
    blockInuse_clientMetadata *metadata = getClientMetadata(c);
    if (!metadata) return;

    for (int i = 0; i < metadata->n_keys; ++i) {
        robj *key = metadata->keys[i];

        list *clientList = getBlockedClientsListsByKey(key);
        serverAssert(clientList != NULL);
        listDelNode(clientList, listSearchKey(clientList, c));

        if (listLength(clientList) == 0) removeBlockedClientsListsByKey(key);

        decrRefCount(key);
        metadata->keys[i] = NULL;
    }
    metadata->n_keys = 0;
    blocked_clients_on_keys--;
}


/*
 * Remove a specific key from a client's blocked keys array.
 */
static blockInuse_clientMetadata *removeBlockingKeyFromClient(client *c, robj *key) {
    blockInuse_clientMetadata *metadata = getClientMetadata(c);
    if (metadata == NULL) return NULL;

    sds key_sds = objectGetKey(key);
    for (int i = 0; i < metadata->n_keys; ++i) {
        sds curr_key = objectGetKey(metadata->keys[i]);
        if (sdscmp(curr_key, key_sds) == 0) {
            decrRefCount(metadata->keys[i]);
            metadata->keys[i] = metadata->keys[metadata->n_keys - 1];
            metadata->keys[metadata->n_keys - 1] = NULL;
            metadata->n_keys--;
            return metadata;
        }
    }
    serverAssert(false); // key must exist
}

/* ----------------------------- API implementation ------------------------- */

/*
 * Initialize blockInuse data structures.
 */
void blockInuse_init(void) {
    client_to_keys = hashtableCreate(&clientDataHashtableType);
    key_to_clients = hashtableCreate(&keyToClientsHashtableType);
    blocked_clients_on_keys = 0;
}

/*
 * Release blockInuse data structures.
 * Only allowed if no clients are currently blocked.
 */
void blockInuse_release(void) {
    serverAssert(blocked_clients_on_keys == 0);

    if (client_to_keys) {
        hashtableRelease(client_to_keys);
        client_to_keys = NULL;
    }
    if (key_to_clients) {
        hashtableRelease(key_to_clients);
        key_to_clients = NULL;
    }
    blocked_clients_on_keys = 0;
}

/* Get the current number of clients blocked by blockInuse. */
int blockInuse_getNumberOfBlockedClients(void) {
    return blocked_clients_on_keys;
}

/* Block a client on a set of keys. */
int blockInuse_blockClientOnKeys(client *c, int nKeys, robj *keys[]) {
    // Ensure client is not already blocked or unblocked
    serverAssert(!(blockInuse_clientBlocked(c) || (c)->flag.unblocked));

    if (nKeys == 0) return C_ERR;
    if (c->flag.replica) return C_ERR;
    for (int i = 0; i < nKeys; ++i) {
        if (keys[i]->type != OBJ_STRING) return C_ERR;
    }

    // Initialize client metadata and insert into client_to_keys table
    blockInuse_clientMetadata *metadata = initClientMetadata(c, nKeys);
    markClientBlocked(c);

    for (int i = 0; i < nKeys; ++i) {
        robj *key = keys[i];

        // Get or create the list of clients blocked on this key
        list *blockedClientsList = addOrFindBlockedClientsListsByKey(key);

        // Deduplicate: add client only if it’s not already the last in the list
        listNode *last_client = listLast(blockedClientsList);
        if (last_client == NULL || last_client->value != c) {
            // Add client to the key’s blocked clients list
            listAddNodeTail(blockedClientsList, c);

            // Add key to the client’s metadata and increment reference count
            incrRefCount(key);
            metadata->keys[metadata->n_keys] = key;
            metadata->n_keys++;
        }
    }

    // Disable client’s Read Handler to prevent reading commands while blocked
    if (c->conn) {
        connSetReadHandler(c->conn, NULL);
    }
    blocked_clients_on_keys++;
    return C_OK;
}

/*
 * Unblock all clients blocked on the given key.
 *
 * - Each client is unblocked only when it has no remaining dependencies on other keys.
 * - Clients that become fully unblocked are added to server.unblocked_clients
 *   and will be resumed later in processUnblockedClients().
 */
void blockInuse_unblockClientsOnKey(robj *key) {
    list *blockedClientsList = getBlockedClientsListsByKey(key);
    if (blockedClientsList == NULL) return;

    serverAssert(listLength(blockedClientsList) > 0);

    while (listLength(blockedClientsList) > 0) {
        listNode *ln = listFirst(blockedClientsList);
        client *c = listNodeValue(ln);

        // Remove client from this key's blocked list
        listDelNode(blockedClientsList, ln);

        // Remove this key from the client's blocked key list
        blockInuse_clientMetadata *metadata = removeBlockingKeyFromClient(c, key);

        if (metadata->n_keys == 0) {
            // Client has no more blocked keys → mark unblocked
            serverAssert(c->flag.unblocked == 0);
            c->flag.unblocked = 1;
            listAddNodeTail(server.unblocked_clients, c);

            // Remove client metadata from client_to_keys table
            removeClientMetadata(c);
            blocked_clients_on_keys--;
        }
    }

    // Remove the key entry from key_to_clients table
    removeBlockedClientsListsByKey(key);
}

/*
 * Unblock all clients on all keys.
 */
void blockInuse_unblockClientsOnAllKeys(void) {
    hashtableIterator iter;
    hashtableInitIterator(&iter, key_to_clients, HASHTABLE_ITER_SAFE);
    void *entry;
    while (hashtableNext(&iter, &entry)) {
        keyToClientsEntry *e = entry;
        robj *key = e->key;
        incrRefCount(key);
        blockInuse_unblockClientsOnKey(key);
        decrRefCount(key);
    }
    hashtableCleanupIterator(&iter);
}

/*
 * Unlink a blocked client from all blockInuse structures, the client must be blocked by blockInuse.
 */
void blockInuse_unlinkClient(client *c) {
    serverAssert(blockInuse_clientBlocked(c) && c->flag.unblocked == 0);

    blockInuse_clientMetadata *metadata = getClientMetadata(c);
    if (metadata == NULL) return; // Client has no blocking metadata

    // Remove client from all key-to-client lists
    unlinkBlockedClientOnKeys(c);

    // Clear the blocked flag and remove client metadata
    c->flag.blockInuse_blocked = 0;
    removeClientMetadata(c);
}
