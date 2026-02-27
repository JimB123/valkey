/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"
extern "C" {
    #include "blocked_inuse.h"
}

class BlockedInuseTest : public ::testing::Test {
    protected:
        MockValkey mock;
        RealValkey real;

        static void SetUpTestSuite() {
            memset(&server, 0, sizeof(valkeyServer));
            server.hz = CONFIG_DEFAULT_HZ;
            server.dbnum = 16;
            server.db = (serverDb **)zcalloc(sizeof(serverDb *) * server.dbnum);
            blockInuse_init();
        }

        void SetUp() override {
            server.unblocked_clients = listCreate();
            ASSERT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
            ASSERT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
        }

        void TearDown() override {
            ASSERT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
            ASSERT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
            ASSERT_EQ(listLength(server.unblocked_clients), 0);
            listRelease(server.unblocked_clients);
            server.unblocked_clients = NULL;
        }

        static void TearDownTestSuite() {
            blockInuse_release();
            for (int i = 0; i < server.dbnum; i++) {
                zfree(server.db[i]);
            }
            zfree(server.db);
        }

        void verifyClientBlockState(client *c, bool blocked, bool unblocked) {
            EXPECT_EQ(c->flag.blocked, 0);
            EXPECT_EQ(c->flag.unblocked, unblocked);
            EXPECT_EQ(blockInuse_clientBlocked(c), blocked);
        }
};

TEST_F(BlockedInuseTest, blockInitialState) {}

TEST_F(BlockedInuseTest, blockClientOnSingleKey) {
    client c = {0};
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), sdsnew("foo"), -1);
    robj *keys[] = {key};

    // Block
    EXPECT_CALL(mock, lookupKeyRead(_, key)).WillOnce(Return(key));
    blockInuse_blockClientOnKeys(&c, 1, keys);
    verifyClientBlockState(&c, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);
    EXPECT_EQ(key->refcount, 3u);

    // Unblock
    blockInuse_unblockClientsOnKey(key);
    verifyClientBlockState(&c, 0, 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_EQ(key->refcount, 1u);
    EXPECT_EQ(listLength(server.unblocked_clients), 1);
    EXPECT_EQ(listFirst(server.unblocked_clients)->value, &c);

    // Process unblocked client in event loop
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(&c)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(&c)).Times(1);
    processUnblockedClients();
    verifyClientBlockState(&c, 0, 0);
    EXPECT_EQ(key->refcount, 1u);
    EXPECT_EQ(listLength(server.unblocked_clients), 0);
    decrRefCount(key);
}

TEST_F(BlockedInuseTest, blockClientOnMultipleKeys) {
    client c = {0};
    robj *key1 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("val1")), sdsnew("key1"), -1);
    robj *key2 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("val2")), sdsnew("key2"), -1);
    robj *keys[] = {key1, key2};

    // Block
    EXPECT_CALL(mock, lookupKeyRead(_, key1)).WillOnce(Return(key1));
    EXPECT_CALL(mock, lookupKeyRead(_, key2)).WillOnce(Return(key2));
    blockInuse_blockClientOnKeys(&c, 2, keys);
    verifyClientBlockState(&c, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 2);
    EXPECT_EQ(key1->refcount, 3u);
    EXPECT_EQ(key2->refcount, 3u);

    // Unblock key1
    blockInuse_unblockClientsOnKey(key1);
    verifyClientBlockState(&c, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);
    EXPECT_EQ(key1->refcount, 1u);
    EXPECT_EQ(key2->refcount, 3u);

    // Unblock key2
    blockInuse_unblockClientsOnKey(key2);
    verifyClientBlockState(&c, 0, 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_EQ(key1->refcount, 1u);
    EXPECT_EQ(key2->refcount, 1u);
    EXPECT_EQ(listLength(server.unblocked_clients), 1);
    EXPECT_EQ(listFirst(server.unblocked_clients)->value, &c);

     // Process unblocked client in event loop
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(&c)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(&c)).Times(1);
    processUnblockedClients();
    verifyClientBlockState(&c, 0, 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0);

    EXPECT_THAT(key1->refcount, 1u);
    EXPECT_THAT(key2->refcount, 1u);
    decrRefCount(key1);
    decrRefCount(key2);
}

TEST_F(BlockedInuseTest, blockMultipleClientsOnSameKey) {
    client c1 = {0}, c2 = {0};
    c1.id = 1;
    c2.id = 2;
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), sdsnew("foo"), -1);
    robj *keys[] = {key};

    // Block
    EXPECT_CALL(mock, lookupKeyRead(_, key)).Times(2).WillRepeatedly(Return(key));
    blockInuse_blockClientOnKeys(&c1, 1, keys);
    blockInuse_blockClientOnKeys(&c2, 1, keys);
    verifyClientBlockState(&c1, 1, 0);
    verifyClientBlockState(&c2, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 2);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);
    EXPECT_EQ(key->refcount, 4u);

    // Unblock
    blockInuse_unblockClientsOnKey(key);
    verifyClientBlockState(&c1, 0, 1);
    verifyClientBlockState(&c2, 0, 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_EQ(key->refcount, 1u);
    EXPECT_EQ(listLength(server.unblocked_clients), 2);

    // Process client in event loop
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(&c1)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(&c1)).Times(1);
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(&c2)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(&c2)).Times(1);
    processUnblockedClients();
    verifyClientBlockState(&c1, 0, 0);
    verifyClientBlockState(&c2, 0, 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0);

    EXPECT_EQ(key->refcount, 1u);
    decrRefCount(key);
}

TEST_F(BlockedInuseTest, unlinkBlockedClient) {
    client c = {0};
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), sdsnew("foo"), -1);
    robj *keys[] = {key};

    // Block
    EXPECT_CALL(mock, lookupKeyRead(_, key)).WillOnce(Return(key));
    blockInuse_blockClientOnKeys(&c, 1, keys);
    verifyClientBlockState(&c, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);
    EXPECT_EQ(key->refcount, 3u);

    // Unlink
    blockInuse_unlinkClient(&c);
    verifyClientBlockState(&c, 0, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0);
    EXPECT_EQ(key->refcount, 1u);
    decrRefCount(key);
}


TEST_F(BlockedInuseTest, blockClientOnDuplicateKeys) {
    client c = {0};
    robj *key1 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), sdsnew("foo"), -1);
    robj *key2 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), sdsnew("foo"), -1);
    robj *keys[] = {key1, key2};

    // Block
    EXPECT_CALL(mock, lookupKeyRead(_, key1)).WillOnce(Return(key1));
    EXPECT_CALL(mock, lookupKeyRead(_, key2)).WillOnce(Return(key2));
    blockInuse_blockClientOnKeys(&c, 2, keys);
    verifyClientBlockState(&c, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);
    EXPECT_EQ(key1->refcount, 3u);
    EXPECT_EQ(key2->refcount, 1u); // Key is deduplicated, only blocked once

    // Unblock
    blockInuse_unblockClientsOnKey(key1);
    verifyClientBlockState(&c, 0, 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 1);
    EXPECT_EQ(listFirst(server.unblocked_clients)->value, &c);

    // Process client in event loop
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(&c)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(&c)).Times(1);
    processUnblockedClients();
    verifyClientBlockState(&c, 0, 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0);
    EXPECT_THAT(key1->refcount, 1u);
    EXPECT_THAT(key2->refcount, 1u);
    decrRefCount(key1);
    decrRefCount(key2);
}

TEST_F(BlockedInuseTest, blockingOnKeysBadArgs) {
    client c = {0};
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), sdsnew("foo"), -1);
    robj *keys[] = {key};

    /* Slave client is not allowed. */
    c.flag.replica = 1;
    EXPECT_DEATH({ blockInuse_blockClientOnKeys(&c, 1, keys); }, "");
    c.flag.replica = 0;

    /* Only allow OBJ_STRING. */
    keys[0]->type = OBJ_LIST;
    EXPECT_DEATH({ blockInuse_blockClientOnKeys(&c, 1, keys); }, "");
    keys[0]->type = OBJ_STRING;

    /* nKeys = 0 is also bad. */
    EXPECT_DEATH({ blockInuse_blockClientOnKeys(&c, 0, keys); }, "");

    EXPECT_THAT(key->refcount, 1u);
    decrRefCount(key);
}
