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
            for (int i = 0; i < server.dbnum; i++) {
                server.db[i] = (serverDb *)zcalloc(sizeof(serverDb));
                server.db[i]->id = i;
            }
            blockInuse_init();
        }

        void SetUp() override {
            server.unblocked_clients = listCreate();
            EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
            EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
        }

        void TearDown() override {
            blockInuse_unblockClientsOnAllKeys();
            // TODO: here I think we should assert no more unblocked client?
            if (server.unblocked_clients) {
                listRelease(server.unblocked_clients);
                server.unblocked_clients = NULL;
            }
        }

        static void TearDownTestSuite() {
            ASSERT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
            ASSERT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
            blockInuse_release();
            for (int i = 0; i < server.dbnum; i++) {
                zfree(server.db[i]);
            }
            zfree(server.db);
        }
};

TEST_F(BlockedInuseTest, blockInitialState) {}

TEST_F(BlockedInuseTest, blockClientOnSingleKey) {
    client c = {0};
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), sdsnew("foo"), -1);
    robj *keys[] = {key};

    EXPECT_CALL(mock, lookupKeyRead(_, key)).WillOnce(Return(key));
    blockInuse_blockClientOnKeys(&c, 1, keys);
    EXPECT_EQ(blockInuse_clientBlocked(&c), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);

    blockInuse_unblockClientsOnKey(key);
    EXPECT_EQ(blockInuse_clientBlocked(&c), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    // todo： verify in server.unblocked lists
    EXPECT_THAT(key->refcount, 1u);
    decrRefCount(key);
}

TEST_F(BlockedInuseTest, blockClientOnMultipleKeys) {
    client c = {0};
    robj *key1 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("val1")), sdsnew("key1"), -1);
    robj *key2 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("val2")), sdsnew("key2"), -1);
    robj *keys[] = {key1, key2};

    EXPECT_CALL(mock, lookupKeyRead(_, key1)).WillOnce(Return(key1));
    EXPECT_CALL(mock, lookupKeyRead(_, key2)).WillOnce(Return(key2));
    blockInuse_blockClientOnKeys(&c, 2, keys);
    EXPECT_EQ(blockInuse_clientBlocked(&c), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 2);

    blockInuse_unblockClientsOnKey(key1);
    EXPECT_EQ(blockInuse_clientBlocked(&c), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);

    blockInuse_unblockClientsOnKey(key2);
    EXPECT_EQ(blockInuse_clientBlocked(&c), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);

    // todo： verify in server.unblocked lists
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

    EXPECT_CALL(mock, lookupKeyRead(_, key)).Times(2).WillRepeatedly(Return(key));
    blockInuse_blockClientOnKeys(&c1, 1, keys);
    blockInuse_blockClientOnKeys(&c2, 1, keys);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 2);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);

    blockInuse_unblockClientsOnKey(key);
    EXPECT_EQ(blockInuse_clientBlocked(&c1), 0);
    EXPECT_EQ(blockInuse_clientBlocked(&c2), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_THAT(key->refcount, 1u);
    decrRefCount(key);
}

TEST_F(BlockedInuseTest, unlinkBlockedClient) {
    client c = {0};
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), sdsnew("foo"), -1);
    robj *keys[] = {key};

    EXPECT_CALL(mock, lookupKeyRead(_, key)).WillOnce(Return(key));
    blockInuse_blockClientOnKeys(&c, 1, keys);
    EXPECT_EQ(blockInuse_clientBlocked(&c), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);

    blockInuse_unlinkClient(&c);
    EXPECT_EQ(blockInuse_clientBlocked(&c), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_THAT(key->refcount, 1u);
    decrRefCount(key);
}


TEST_F(BlockedInuseTest, blockClientOnDuplicateKeys) {
    client c = {0};
    robj *key1 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), sdsnew("foo"), -1);
    robj *key2 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), sdsnew("foo"), -1);
    robj *keys[] = {key1, key2};

    EXPECT_CALL(mock, lookupKeyRead(_, key1)).WillOnce(Return(key1));
    EXPECT_CALL(mock, lookupKeyRead(_, key2)).WillOnce(Return(key2));
    blockInuse_blockClientOnKeys(&c, 2, keys);
    EXPECT_EQ(blockInuse_clientBlocked(&c), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);

    blockInuse_unblockClientsOnKey(key1);
    EXPECT_EQ(blockInuse_clientBlocked(&c), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_THAT(key1->refcount, 1u);
    EXPECT_THAT(key2->refcount, 1u);
    decrRefCount(key1);
    decrRefCount(key2);
}
