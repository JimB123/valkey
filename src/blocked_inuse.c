#include "server.h"
#include "hashtable.h"
#include "blocked_inuse.h"

/* External functions from server.c */
extern uint64_t dictEncObjHash(const void *key);
extern int hashtableEncObjKeyCompare(const void *key1, const void *key2);
extern uint64_t hashtableClientHash(const void *key);
extern int hashtableClientKeyCompare(const void *key1, const void *key2);

static hashtable *client_to_keys; // client memory address -> blockInuse_clientMetadata (contains a list of keys)
static hashtable *key_to_clients; /* Map(key)->list(blocked clients).  */
static list *unblocked_clients;    /* list of clients which are unblocked. We need to resume the blocked command and then process the unread querybuf before we remove it.*/
uint32_t blocked_clients_on_keys;                                 /* Num of clients blocked on keys now. */
uint32_t total_clients_blocked_on_keys_lifetime;                  /* Num of clients blocked on keys lifetime. */
uint32_t total_clients_unblocked_on_keys_lifetime;                /* Num of clients unblocked on keys lifetime. */
uint32_t total_clients_resumed_lifetime;                          /* Num of clients resumed lifetime. */

typedef struct blockInuse_clientMetadata {
    robj **keys;                        /* Keys this client is blocked on. */
    int n_keys;                         /* number of keys this client is blocked on. */
    long long blocked_at;               /* what time this client is blocked in milli second. It uses value from server.mstime. */
} blockInuse_clientMetadata;

/* ----------------------------- client_to_keys Hashtable Util ------------------------- */
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
        //Refcount for all the keys should be decreased before calling this function. Hence n_keys should be 0.
        serverAssert(e->metadata.keys == 0);
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

static blockInuse_clientMetadata *getClientMetadata(client *c) {
    clientDataEntry *entry;
    if (hashtableFind(client_to_keys, c, (void **)&entry)) {
        return &entry->metadata;
    }
    return NULL;
}

static void removeClientMetadata(client *c) {
    hashtablePop(client_to_keys, c, NULL);
}


/* ----------------------------- keyToClientsEntry Hashtable Util ------------------------- */
/* Entry type for key_to_clients: robj key -> list of clients */
typedef struct {
    robj *key;
    list *clients;
} keyToClientsEntry;

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

static list *getBlockedClientsListsByKey(robj *key) {
    keyToClientsEntry *entry;
    if (hashtableFind(key_to_clients, key, (void **)&entry)) {
        return entry->clients;
    }
    return NULL;
}

static void removeBlockedClientsListsByKey(robj *key) {
    hashtablePop(key_to_clients, key, NULL);
}

/* ----------------------------- util ------------------------- */
static void markClientBlocked(client *c) {
    c->flag.blockInuse_blocked = 1;
    c->flag.pending_command = 1; // Harrt TODO do we need this, and why?
}

static void markClientUnblocked(client *c) {
    c->flag.blockInuse_blocked = 0;
    c->flag.blockInuse_unblocked = 1;
}

// Init the client Metadata and insert it into our global hash table, key is client, and value is metadata
static blockInuse_clientMetadata *initClientMetadata(client *c, int nKeys) {
    serverAssert(!getClientMetadata(c)); // this client must not be in our global table
    serverAssert(nKeys >= 0); // non negative check
    blockInuse_clientMetadata *metadata = addOrFindClientMetadata(c); // add metada into the hashtable
    metadata->n_keys = 0;
    metadata->keys = NULL;
    metadata->blocked_at = server.mstime;
    if (nKeys > 0) {
        metadata->keys = zmalloc(sizeof(robj *) * nKeys);
    }
    return metadata;
}

// Remove this client totally from every lists in the key_to_clients table.
static void unlinkBlockedClientOnKeys(client *c) {
    blockInuse_clientMetadata *metadata = getClientMetadata(c);
    if (!metadata) return;
    for (int i = 0; i < metadata->n_keys; ++i) {
        robj *key = metadata->keys[i];

        list *clientList = getBlockedClientsListsByKey(key);
        serverAssert(clientList != NULL);
        listDelNode(clientList, listSearchKey(clientList, c));

        if (listLength(clientList) == 0) removeBlockedClientsListsByKey(key);
        decrRefCount(key); // Harry TODO: double check where to increase and where to decrease.
        metadata->keys[i] = NULL;
    }
    metadata->n_keys = 0;
    blocked_clients_on_keys--;
    total_clients_unblocked_on_keys_lifetime++;
    total_clients_resumed_lifetime++; // Harry TODO why ??
}

// Remove a key from a client entry in client_to_keys table
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
    // we expect to find a key
    serverAssert(false);
}

/* Process all the commands for an unblocked client. First we read the blocked command which is already parsed by calling processCommand.
 * Then we process the commands present in querybuf by calling processInputBuffer.
 */
static void processBlockedCommand(client *c) {
    if (c->flag.close_asap) return;
    c->flag.pending_command = 0;
    int retval = processCommandAndResetClient(c);
    if (retval != C_OK || blockInuse_isBlockedClient(c)) {
        return;
    }
    //process the pending commands in the buffer.
    if (processInputBuffer(c) == C_OK && !c->flag.close_asap) {
        beforeNextClient(c);
    }
}
/* ----------------------------- API implementation ------------------------- */

/* Initialize global client_to_keys hashtable. Call once at server startup. */
/* Initializes the blockInuse data structures needed for DB. Called at server startup per db. */
// Harry check Done
void blockInuse_init(void) {
    client_to_keys = hashtableCreate(&clientDataHashtableType);
    key_to_clients = hashtableCreate(&keyToClientsHashtableType);
    unblocked_clients = listCreate();
    blocked_clients_on_keys = 0;
    total_clients_blocked_on_keys_lifetime = 0;
    total_clients_unblocked_on_keys_lifetime = 0;
    total_clients_resumed_lifetime = 0;
}

/* Clean up the blockInuse data structures for the database if possible.
 * Returns false if there are any blocked or unblocked clients (no cleanup performed).
 * Returns true if cleanup succeeded or was already done.
 * Note: This will not free the struct itself but cleans up the internal data. */
// Harry check Done
bool blockInuse_cleanDbBlockingInfo(void) {
    if (blocked_clients_on_keys > 0 || (listLength(unblocked_clients) > 0)) return false;
    hashtableRelease(key_to_clients);
    listRelease(unblocked_clients);
    key_to_clients = NULL;
    unblocked_clients = NULL;
    blocked_clients_on_keys = 0;
    return true;
}

// Harry check Done
int blockInuse_getNumberOfBlockedClients(void) {
    return blocked_clients_on_keys;
}

long blockInuse_getNumberOfUnblockedClients(void) {
    return listLength(unblocked_clients);
}

// Harry check Done
int blockInuse_blockClientOnKeys(client *c, int nKeys, robj *keys[]) {
    // some checks
    serverAssert(!(blockInuse_isBlockedClient(c) || blockInuse_isUnblockedClient(c)));
    if (nKeys == 0) return C_ERR;
    if (c->flag.replica) return C_ERR; // Maybe remove this?
    for (int i = 0; i < nKeys; ++i) {
        if (keys[i]->type != OBJ_STRING) return C_ERR;
    }

    // add into the global table
    blockInuse_clientMetadata *metadata = initClientMetadata(c, nKeys); // this will add into the global table and assign memory, but the keys in metadata is still empty
    markClientBlocked(c);
    for (int i = 0; i < nKeys; ++i) {
        // This loop is just for book keeping, we want to do 2 things in the loop:
        // 1. adding the key into the client metadata (global table), such that c -> list of keys (append key to tail)
        // 2. add the entry into the blocking info, such that key -> list of clients (append c to tail)

        list *blockedClientsList = addOrFindBlockedClientsListsByKey(keys[i]); // this will add entry of key -> [client 1, client 2 ...], incrRefCount in included.

        // If the last client blocked on this key is not c, then we add c onto the list.
        // Otherwise this is a duplicated key and we should ignore it.
        listNode *last_client = listLast(blockedClientsList);

        // this if check is for deduplica of keys
        if (last_client == NULL || last_client->value != c) {
            // 2. add the client into the blocking info at tail, either create a new entry or add to tail
            listAddNodeTail(blockedClientsList, c);

            // 1. add the key into the client metadata, so it would be client -> [key1, key2, key3]
            incrRefCount(keys[i]);
            metadata->keys[metadata->n_keys] = keys[i];
            metadata->n_keys++;
        }
    }

    if (c->conn) {
        // Delete the readable event from the event loop for the blocked client.
        connSetReadHandler(c->conn, NULL);
    }
    blocked_clients_on_keys++;
    total_clients_blocked_on_keys_lifetime++;
    return C_OK;
}

/* Unblock given key. A client will be unblocked, if it has no more dependency on any key and will be put into unblocked_clients list. */
// Harry check Done
void blockInuse_unblockClientsOnKey(robj *key) {
    list *blockedClientsList = getBlockedClientsListsByKey(key);
    if (blockedClientsList == NULL) return;
    serverAssert(listLength(blockedClientsList) > 0);
    while (listLength(blockedClientsList) > 0) {
        listNode *ln = listFirst(blockedClientsList);
        client *c = listNodeValue(ln);
        listDelNode(blockedClientsList, ln);
        blockInuse_clientMetadata *metadata = removeBlockingKeyFromClient(c, key);
        if (metadata->n_keys == 0) {
            // time to remove this entry in our global table
            markClientUnblocked(c);
            removeClientMetadata(c);
            listAddNodeTail(unblocked_clients, c);
            blocked_clients_on_keys--;
            total_clients_blocked_on_keys_lifetime++;
        }
    }
    removeBlockedClientsListsByKey(key);
}

// Harry check Done
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

// Harry check Done
void blockInuse_processServerBlockedClients(void) {
    if(!unblocked_clients) return;

    while(listLength(unblocked_clients) > 0) {
        // we need to check it every time, so that if one of the unblocked
        // clients executed pause command, then we stop processing further.
        if (isPausedActionsWithUpdate(PAUSE_ACTIONS_CLIENT_ALL_SET)) return;
        listNode *ln = listFirst(unblocked_clients);
        serverAssert(ln != NULL);
        client *c = listNodeValue(ln);
        listDelNode(unblocked_clients, ln);
        c->flag.blockInuse_unblocked = 0;
        total_clients_resumed_lifetime++;
        // make the fd readble again. This should succeed as we are not adding a
        // new client. If it fails because epoll_ctl failed then freeClient.
        // We avoid setting the read handler for fake client that does not have a connection.
        if (c->conn && connSetReadHandler(c->conn, readQueryFromClient) == C_ERR) {
            freeClient(c);
            return;
        }
        processBlockedCommand(c);
    }
}

// Harry check Done
// remove a client from everywhere
void blockInuse_unlinkClient(client *c) {
    serverAssert(blockInuse_isBlockedClient(c) || blockInuse_isUnblockedClient(c));
    if (blockInuse_isBlockedClient(c)) {
        blockInuse_clientMetadata *metadata = getClientMetadata(c);
        if (metadata == NULL) return; // return immediately if the client was not blocked on any keys.
        unlinkBlockedClientOnKeys(c);
        c->flag.blockInuse_blocked = 0;
        removeClientMetadata(c); // remove the global hashtable entry
    }

    if (blockInuse_isUnblockedClient(c)) {
        listNode *ln = listSearchKey(unblocked_clients, c);
        serverAssert(ln != NULL);
        listDelNode(unblocked_clients, ln);
        c->flag.blockInuse_unblocked = 0;
        total_clients_resumed_lifetime++;
    }
}
