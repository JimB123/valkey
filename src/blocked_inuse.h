/*
Harry TODO:
1. write a brief introduction here
2. remove unblocked client once done
3. do we need the oldest?
*/

#ifndef BLOCKED_INUSE_H__
#define BLOCKED_INUSE_H__

#include "hashtable.h"
#include "adlist.h"

struct robj;                     //defined in server.h
struct serverCommand;            //defined in server.h
struct client;                   //defined in server.h

/* Check if client is blocked/unblocked by blockInuse */
#define blockInuse_isBlockedClient(c) ((c)->flag.blockInuse_blocked)
#define blockInuse_isUnblockedClient(c) ((c)->flag.blockInuse_unblocked)

// Harry Check: do I need them in here, since they are totally private now.
typedef struct blockInuse_clientMetadata blockInuse_clientMetadata;
typedef struct blockInuse_blockingInfo blockInuse_blockingInfo;

/* Initialize global blockInuse structures. Call once at server startup. */
void blockInuse_init(void);

/* If no clients are blocked and all the previously blocked clients have processed the blocked command,
   then this will free the data structures and return true. Otherwise, this will return false, and we
   still have few clients to process and cleanup failed. */
bool blockInuse_cleanDbBlockingInfo(void);

/* Returns the total count of currently blocked clients by blockInuse */
int blockInuse_getNumberOfBlockedClients(void);
long blockInuse_getNumberOfUnblockedClients(void);

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
