/* blocked.c - generic support for blocking operations like BLPOP & WAIT.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * ---------------------------------------------------------------------------
 *
 * API:
 *
 * blockClient() set the CLIENT_BLOCKED flag in the client, and set the
 * specified block type 'btype' filed to one of BLOCKED_* macros.
 *
 * unblockClient() unblocks the client doing the following:
 * 1) It calls the btype-specific function to cleanup the state.
 * 2) It unblocks the client by unsetting the CLIENT_BLOCKED flag.
 * 3) It puts the client into a list of just unblocked clients that are
 *    processed ASAP in the beforeSleep() event loop callback, so that
 *    if there is some query buffer to process, we do it. This is also
 *    required because otherwise there is no 'readable' event fired, we
 *    already read the pending commands. We also set the CLIENT_UNBLOCKED
 *    flag to remember the client is in the unblocked_clients list.
 *
 * processUnblockedClients() is called inside the beforeSleep() function
 * to process the query buffer from unblocked clients and remove the clients
 * from the blocked_clients queue.
 *
 * replyToBlockedClientTimedOut() is called by the cron function when
 * a client blocked reaches the specified timeout (if the timeout is set
 * to 0, no timeout is processed).
 * It usually just needs to send a reply to the client.
 *
 * When implementing a new type of blocking operation, the implementation
 * should modify unblockClient() and replyToBlockedClientTimedOut() in order
 * to handle the btype-specific behavior of this two functions.
 * If the blocking operation waits for certain keys to change state, the
 * clusterRedirectBlockedClientIfNeeded() function should also be updated.
 */

#include "server.h"
#include "slowlog.h"
#include "latency.h"
#include "monotonic.h"

void serveClientBlockedOnList(client *receiver, robj *o, robj *key, robj *dstkey, redisDb *db, int wherefrom, int whereto, int *deleted);
int getListPositionFromObjectOrReply(client *c, robj *arg, int *position);

/* This structure represents the blocked key information that we store
 * in the client structure. Each client blocked on keys, has a
 * client->bpop.keys hash table. The keys of the hash table are Redis
 * keys pointers to 'robj' structures. The value is this structure.
 * The structure has two goals: firstly we store the list node that this
 * client uses to be listed in the database "blocked clients for this key"
 * list, so we can later unblock in O(1) without a list scan.
 * Secondly for certain blocking types, we have additional info. Right now
 * the only use for additional info we have is when clients are blocked
 * on streams, as we have to remember the ID it blocked for. */
typedef struct bkinfo {
    listNode *listnode;     /* List node for db->blocking_keys[key] list. */
    streamID stream_id;     /* Stream ID if we blocked in a stream. */
} bkinfo;

/* Block a client for the specific operation type. Once the CLIENT_BLOCKED
 * flag is set client query buffer is not longer processed, but accumulated,
 * and will be processed when the client is unblocked. */
void blockClient(client *c, int btype) {
    /* Master client should never be blocked unless pause or module */
    serverAssert(!(c->flags & CLIENT_MASTER &&
                   btype != BLOCKED_MODULE &&
                   btype != BLOCKED_POSTPONE));

    c->flags |= CLIENT_BLOCKED;
    c->btype = btype;
    server.blocked_clients++;//统计被阻塞的客户端个数
    server.blocked_clients_by_type[btype]++;//统计 阻塞类型 的个数
    addClientToTimeoutTable(c);
    if (btype == BLOCKED_POSTPONE) {
        listAddNodeTail(server.postponed_clients, c);
        c->postponed_list_node = listLast(server.postponed_clients);
        /* Mark this client to execute its command */
        c->flags |= CLIENT_PENDING_COMMAND;
    }
}

/* This function is called after a client has finished a blocking operation
 * in order to update the total command duration, log the command into
 * the Slow log if needed, and log the reply duration event if needed. */
void updateStatsOnUnblock(client *c, long blocked_us, long reply_us, int had_errors){
    const ustime_t total_cmd_duration = c->duration + blocked_us + reply_us;
    c->lastcmd->microseconds += total_cmd_duration;
    if (had_errors)
        c->lastcmd->failed_calls++;
    if (server.latency_tracking_enabled)
        updateCommandLatencyHistogram(&(c->lastcmd->latency_histogram), total_cmd_duration*1000);
    /* Log the command into the Slow log if needed. */
    slowlogPushCurrentCommand(c, c->lastcmd, total_cmd_duration);
    /* Log the reply duration event. */
    latencyAddSampleIfNeeded("command-unblocking",reply_us/1000);
}

/* 本函数在事件循环的beforeSleep()函数里调用，
 * 处理已从阻塞态里解除的且有正等待处理的input buffer 的 clients
 * in order to process the pending input buffer of clients that were
 * unblocked after a blocking operation. */
void processUnblockedClients(void) {
    listNode *ln;
    client *c;

    while (listLength(server.unblocked_clients)) {
        ln = listFirst(server.unblocked_clients);
        serverAssert(ln != NULL);//抛出异常
        c = ln->value;
        listDelNode(server.unblocked_clients,ln);//从队列里弹出
        c->flags &= ~CLIENT_UNBLOCKED;//清除 CLIENT_UNBLOCKED 标志

        /* 在 Redis 中，某些命令（如 BLPOP 等阻塞命令）会使客户端进入阻塞状态，直到满足解除阻塞的条件（例如有新的数据可用）。
         * 在继续处理输入缓冲区中的数据之前，要特别注意客户端是否处于阻塞状态，因为客户端可能在任何时候由于某些操作再次进入阻塞状态。
         * 所以，虽然 processInputBuffer() 会检查阻塞状态，但为了代码的正确性和避免遗漏，最好在处理之前重新确认一次客户端的状态。. */
        if (!(c->flags & CLIENT_BLOCKED)) {//确认c没有处于阻塞
            /* 如果队列里有待处理的命令，则执行/处理它 */
            if (processPendingCommandAndInputBuffer(c) == C_ERR) {//执行client请求的命令
                c = NULL;
            }
        }
        beforeNextClient(c);
    }
}

/* This function will schedule the client for reprocessing at a safe time.
 *
 * This is useful when a client was blocked for some reason (blocking operation,
 * CLIENT PAUSE, or whatever), because it may end with some accumulated query
 * buffer that needs to be processed ASAP:
 *
 * 1. When a client is blocked, its readable handler is still active.
 * 2. However in this case it only gets data into the query buffer, but the
 *    query is not parsed or executed once there is enough to proceed as
 *    usually (because the client is blocked... so we can't execute commands).
 * 3. When the client is unblocked, without this function, the client would
 *    have to write some query in order for the readable handler to finally
 *    call processQueryBuffer*() on it.
 * 4. With this function instead we can put the client in a queue that will
 *    process it for queries ready to be executed at a safe time.
 */
void queueClientForReprocessing(client *c) {
    /* The client may already be into the unblocked list because of a previous
     * blocking operation, don't add back it into the list multiple times. */
    if (!(c->flags & CLIENT_UNBLOCKED)) {
        c->flags |= CLIENT_UNBLOCKED;//c已解除阻塞状态，等待进一步处理
        listAddNodeTail(server.unblocked_clients,c);//添加到队列里，等待处理
    }
}

/* 解除client的阻塞 Unblock a client calling the right function depending on the kind
 * of operation the client is blocking for. */
void unblockClient(client *c) {
    if (c->btype == BLOCKED_LIST ||
        c->btype == BLOCKED_ZSET ||
        c->btype == BLOCKED_STREAM) {
        unblockClientWaitingData(c);
    } else if (c->btype == BLOCKED_WAIT) {
        unblockClientWaitingReplicas(c);
    } else if (c->btype == BLOCKED_MODULE) {
        if (moduleClientIsBlockedOnKeys(c)) unblockClientWaitingData(c);
        unblockClientFromModule(c);
    } else if (c->btype == BLOCKED_POSTPONE) {
        listDelNode(server.postponed_clients,c->postponed_list_node);
        c->postponed_list_node = NULL;
    } else if (c->btype == BLOCKED_SHUTDOWN) {
        /* No special cleanup. */
    } else {
        serverPanic("Unknown btype in unblockClient().");
    }

    /* Reset the client for a new query since, for blocking commands
     * we do not do it immediately after the command returns (when the
     * client got blocked) in order to be still able to access the argument
     * vector from module callbacks and updateStatsOnUnblock. */
    if (c->btype != BLOCKED_POSTPONE && c->btype != BLOCKED_SHUTDOWN) {
        freeClientOriginalArgv(c);
        resetClient(c);
    }

    /* Clear the flags, and put the client in the unblocked list so that
     * we'll process new commands in its query buffer ASAP. */
    server.blocked_clients--;
    server.blocked_clients_by_type[c->btype]--;
    c->flags &= ~CLIENT_BLOCKED;
    c->btype = BLOCKED_NONE;
    removeClientFromTimeoutTable(c);
    queueClientForReprocessing(c);
}

/* 该函数在client超时后会被调用。以便发送响应数据给客户端
 * 该函数执行完后，应该要调用unblockClient()函数并传入相同的client作为参数
 */
void replyToBlockedClientTimedOut(client *c) {
    if (c->btype == BLOCKED_LIST ||
        c->btype == BLOCKED_ZSET ||
        c->btype == BLOCKED_STREAM) {
        addReplyNullArray(c);//添加数据截止的标识
    } else if (c->btype == BLOCKED_WAIT) {
        addReplyLongLong(c,replicationCountAcksByOffset(c->bpop.reploffset));
    } else if (c->btype == BLOCKED_MODULE) {
        moduleBlockedClientTimedOut(c);
    } else {
        serverPanic("Unknown btype in replyToBlockedClientTimedOut().");
    }
}

/* If one or more clients are blocked on the SHUTDOWN command, this function
 * sends them an error reply and unblocks them. */
void replyToClientsBlockedOnShutdown(void) {
    if (server.blocked_clients_by_type[BLOCKED_SHUTDOWN] == 0) return;
    listNode *ln;
    listIter li;
    listRewind(server.clients, &li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        if (c->flags & CLIENT_BLOCKED && c->btype == BLOCKED_SHUTDOWN) {
            addReplyError(c, "Errors trying to SHUTDOWN. Check logs.");
            unblockClient(c);
        }
    }
}

/* Mass-unblock clients because something changed in the instance that makes
 * blocking no longer safe. For example clients blocked in list operations
 * in an instance which turns from master to slave is unsafe, so this function
 * is called when a master turns into a slave.
 *
 * The semantics is to send an -UNBLOCKED error to the client, disconnecting
 * it at the same time. */
void disconnectAllBlockedClients(void) {
    listNode *ln;
    listIter li;

    listRewind(server.clients,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        if (c->flags & CLIENT_BLOCKED) {
            /* POSTPONEd clients are an exception, when they'll be unblocked, the
             * command processing will start from scratch, and the command will
             * be either executed or rejected. (unlike LIST blocked clients for
             * which the command is already in progress in a way. */
            if (c->btype == BLOCKED_POSTPONE)
                continue;

            addReplyError(c,
                "-UNBLOCKED force unblock from blocking operation, "
                "instance state changed (master -> replica?)");
            unblockClient(c);
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked on a list key, and there may be new
 * data to fetch (the key is ready). */
void serveClientsBlockedOnListKey(robj *o, readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_LIST,
     * we can skip this loop. */
    if (!server.blocked_clients_by_type[BLOCKED_LIST]) return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_LIST) continue;

            int deleted = 0;
            robj *dstkey = receiver->bpop.target;
            int wherefrom = receiver->bpop.blockpos.wherefrom;
            int whereto = receiver->bpop.blockpos.whereto;

            /* Protect receiver->bpop.target, that will be
             * freed by the next unblockClient()
             * call. */
            if (dstkey) incrRefCount(dstkey);

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            serveClientBlockedOnList(receiver, o,
                                     rl->key, dstkey, rl->db,
                                     wherefrom, whereto,
                                     &deleted);
            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            unblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;

            if (dstkey) decrRefCount(dstkey);

            /* The list is empty and has been deleted. */
            if (deleted) break;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked on a sorted set key, and there may be new
 * data to fetch (the key is ready). */
void serveClientsBlockedOnSortedSetKey(robj *o, readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_ZSET,
     * we can skip this loop. */
    if (!server.blocked_clients_by_type[BLOCKED_ZSET]) return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_ZSET) continue;

            int deleted = 0;
            long llen = zsetLength(o);
            long count = receiver->bpop.count;
            int where = receiver->bpop.blockpos.wherefrom;
            int use_nested_array = (receiver->lastcmd &&
                                    receiver->lastcmd->proc == bzmpopCommand)
                                    ? 1 : 0;
            int reply_nil_when_empty = use_nested_array;

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            genericZpopCommand(receiver, &rl->key, 1, where, 1, count, use_nested_array, reply_nil_when_empty, &deleted);

            /* Replicate the command. */
            int argc = 2;
            robj *argv[3];
            argv[0] = where == ZSET_MIN ? shared.zpopmin : shared.zpopmax;
            argv[1] = rl->key;
            incrRefCount(rl->key);
            if (count != -1) {
                /* Replicate it as command with COUNT. */
                robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
                argv[2] = count_obj;
                argc++;
            }
            alsoPropagate(receiver->db->id, argv, argc, PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(argv[1]);
            if (count != -1) decrRefCount(argv[2]);

            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            unblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;

            /* The zset is empty and has been deleted. */
            if (deleted) break;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked on a stream key, and there may be new
 * data to fetch (the key is ready). */
void serveClientsBlockedOnStreamKey(robj *o, readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_STREAM,
     * we can skip this loop. */
    if (!server.blocked_clients_by_type[BLOCKED_STREAM]) return;

    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
    stream *s = o->ptr;

    /* We need to provide the new data arrived on the stream
     * to all the clients that are waiting for an offset smaller
     * than the current top item. */
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_STREAM) continue;
            bkinfo *bki = dictFetchValue(receiver->bpop.keys,rl->key);
            streamID *gt = &bki->stream_id;

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);

            /* If we blocked in the context of a consumer
             * group, we need to resolve the group and update the
             * last ID the client is blocked for: this is needed
             * because serving other clients in the same consumer
             * group will alter the "last ID" of the consumer
             * group, and clients blocked in a consumer group are
             * always blocked for the ">" ID: we need to deliver
             * only new messages and avoid unblocking the client
             * otherwise. */
            streamCG *group = NULL;
            if (receiver->bpop.xread_group) {
                group = streamLookupCG(s,
                        receiver->bpop.xread_group->ptr);
                /* If the group was not found, send an error
                 * to the consumer. */
                if (!group) {
                    addReplyError(receiver,
                        "-NOGROUP the consumer group this client "
                        "was blocked on no longer exists");
                    goto unblock_receiver;
                } else {
                    *gt = group->last_id;
                }
            }

            if (streamCompareID(&s->last_id, gt) > 0) {
                streamID start = *gt;
                streamIncrID(&start);

                /* Lookup the consumer for the group, if any. */
                streamConsumer *consumer = NULL;
                int noack = 0;

                if (group) {
                    noack = receiver->bpop.xread_group_noack;
                    sds name = receiver->bpop.xread_consumer->ptr;
                    consumer = streamLookupConsumer(group,name,SLC_DEFAULT);
                    if (consumer == NULL) {
                        consumer = streamCreateConsumer(group,name,rl->key,
                                                        rl->db->id,SCC_DEFAULT);
                        if (noack) {
                            streamPropagateConsumerCreation(receiver,rl->key,
                                                            receiver->bpop.xread_group,
                                                            consumer->name);
                        }
                    }
                }

                /* Emit the two elements sub-array consisting of
                 * the name of the stream and the data we
                 * extracted from it. Wrapped in a single-item
                 * array, since we have just one key. */
                if (receiver->resp == 2) {
                    addReplyArrayLen(receiver,1);
                    addReplyArrayLen(receiver,2);
                } else {
                    addReplyMapLen(receiver,1);
                }
                addReplyBulk(receiver,rl->key);

                streamPropInfo pi = {
                    rl->key,
                    receiver->bpop.xread_group
                };
                streamReplyWithRange(receiver,s,&start,NULL,
                                     receiver->bpop.xread_count,
                                     0, group, consumer, noack, &pi);
                /* Note that after we unblock the client, 'gt'
                 * and other receiver->bpop stuff are no longer
                 * valid, so we must do the setup above before
                 * the unblockClient call. */

unblock_receiver:
                updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
                unblockClient(receiver);
                afterCommand(receiver);
                server.current_client = old_client;
            }
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * in order to check if we can serve clients blocked by modules using
 * RM_BlockClientOnKeys(), when the corresponding key was signaled as ready:
 * our goal here is to call the RedisModuleBlockedClient reply() callback to
 * see if the key is really able to serve the client, and in that case,
 * unblock it. */
void serveClientsBlockedOnKeyByModule(readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_MODULE,
     * we can skip this loop. */
    if (!server.blocked_clients_by_type[BLOCKED_MODULE]) return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_MODULE) continue;

            /* Note that if *this* client cannot be served by this key,
             * it does not mean that another client that is next into the
             * list cannot be served as well: they may be blocked by
             * different modules with different triggers to consider if a key
             * is ready or not. This means we can't exit the loop but need
             * to continue after the first failure. */
            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            if (!moduleTryServeClientBlockedOnKey(receiver, rl->key)) continue;
            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            moduleUnblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked, via XREADGROUP, on an existing stream which
 * was deleted. We need to unblock the clients in that case.
 * The idea is that a client that is blocked via XREADGROUP is different from
 * any other blocking type in the sense that it depends on the existence of both
 * the key and the group. Even if the key is deleted and then revived with XADD
 * it won't help any clients blocked on XREADGROUP because the group no longer
 * exist, so they would fail with -NOGROUP anyway.
 * The conclusion is that it's better to unblock these client (with error) upon
 * the deletion of the key, rather than waiting for the first XADD. */
void unblockDeletedStreamReadgroupClients(readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_STREAM,
     * we can skip this loop. */
    if (!server.blocked_clients_by_type[BLOCKED_STREAM]) return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_STREAM || !receiver->bpop.xread_group)
                continue;

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            addReplyError(receiver, "-UNBLOCKED the stream key no longer exists");
            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            unblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;
        }
    }
}

/* This function should be called by Redis every time a single command,
 * a MULTI/EXEC block, or a Lua script, terminated its execution after
 * being called by a client. It handles serving clients blocked in
 * lists, streams, and sorted sets, via a blocking commands.
 *
 * All the keys with at least one client blocked that received at least
 * one new element via some write operation are accumulated into
 * the server.ready_keys list. This function will run the list and will
 * serve clients accordingly. Note that the function will iterate again and
 * again as a result of serving BLMOVE we can have new blocking clients
 * to serve because of the PUSH side of BLMOVE.
 *
 * This function is normally "fair", that is, it will server clients
 * using a FIFO behavior. However this fairness is violated in certain
 * edge cases, that is, when we have clients blocked at the same time
 * in a sorted set and in a list, for the same key (a very odd thing to
 * do client side, indeed!). Because mismatching clients (blocking for
 * a different type compared to the current key type) are moved in the
 * other side of the linked list. However as long as the key starts to
 * be used only for a single type, like virtually any Redis application will
 * do, the function is already fair. */
void handleClientsBlockedOnKeys(void) {
    /* This function is called only when also_propagate is in its basic state
     * (i.e. not from call(), module context, etc.) */
    serverAssert(server.also_propagate.numops == 0);
    server.core_propagates = 1;

    while(listLength(server.ready_keys) != 0) {
        list *l;

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalKeyAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BLMOVE. */
        l = server.ready_keys;
        server.ready_keys = listCreate();

        while(listLength(l) != 0) {
            listNode *ln = listFirst(l);
            readyList *rl = ln->value;

            /* First of all remove this key from db->ready_keys so that
             * we can safely call signalKeyAsReady() against this key. */
            dictDelete(rl->db->ready_keys,rl->key);

            /* Even if we are not inside call(), increment the call depth
             * in order to make sure that keys are expired against a fixed
             * reference time, and not against the wallclock time. This
             * way we can lookup an object multiple times (BLMOVE does
             * that) without the risk of it being freed in the second
             * lookup, invalidating the first one.
             * See https://github.com/redis/redis/pull/6554. */
            server.fixed_time_expire++;
            updateCachedTime(0);

            /* Serve clients blocked on the key. */
            robj *o = lookupKeyReadWithFlags(rl->db, rl->key, LOOKUP_NONOTIFY | LOOKUP_NOSTATS);
            if (o != NULL) {
                int objtype = o->type;
                if (objtype == OBJ_LIST)
                    serveClientsBlockedOnListKey(o,rl);
                else if (objtype == OBJ_ZSET)
                    serveClientsBlockedOnSortedSetKey(o,rl);
                else if (objtype == OBJ_STREAM)
                    serveClientsBlockedOnStreamKey(o,rl);
                /* We want to serve clients blocked on module keys
                 * regardless of the object type: we don't know what the
                 * module is trying to accomplish right now. */
                serveClientsBlockedOnKeyByModule(rl);
                /* If we have XREADGROUP clients blocked on this key, and
                 * the key is not a stream, it must mean that the key was
                 * overwritten by either SET or something like
                 * (MULTI, DEL key, SADD key e, EXEC).
                 * In this case we need to unblock all these clients. */
                 if (objtype != OBJ_STREAM)
                     unblockDeletedStreamReadgroupClients(rl);
            } else {
                /* Unblock all XREADGROUP clients of this deleted key */
                unblockDeletedStreamReadgroupClients(rl);
                /* Edge case: If lookupKeyReadWithFlags decides to expire the key we have to
                 * take care of the propagation here, because afterCommand wasn't called */
                if (server.also_propagate.numops > 0)
                    propagatePendingCommands();
            }
            server.fixed_time_expire--;

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l,ln);
        }
        listRelease(l); /* We have the new list on place at this point. */
    }

    serverAssert(server.core_propagates); /* This function should not be re-entrant */

    server.core_propagates = 0;
}

/* This is how the current blocking lists/sorted sets/streams work, we use
 * BLPOP as example, but the concept is the same for other list ops, sorted
 * sets and XREAD.
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if blocking is not required.
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we mark this key as "ready", and after the current command,
 *   MULTI/EXEC block, or script, is executed, we serve all the clients waiting
 *   for this list, from the one that blocked first, to the last, accordingly
 *   to the number of elements we have in the ready list.
 */

/* Set a client in blocking mode for the specified key (list, zset or stream),
 * with the specified timeout. The 'type' argument is BLOCKED_LIST,
 * BLOCKED_ZSET or BLOCKED_STREAM depending on the kind of operation we are
 * waiting for an empty key in order to awake the client. The client is blocked
 * for all the 'numkeys' keys as in the 'keys' argument. When we block for
 * stream keys, we also provide an array of streamID structures: clients will
 * be unblocked only when items with an ID greater or equal to the specified
 * one is appended to the stream.
 *
 * 'count' for those commands that support the optional count argument.
 * Otherwise the value is 0. */
void blockForKeys(client *c, int btype, robj **keys, int numkeys, long count, mstime_t timeout, robj *target, struct blockPos *blockpos, streamID *ids) {
    dictEntry *de;
    list *l;
    int j;

    c->bpop.count = count;
    c->bpop.timeout = timeout;
    c->bpop.target = target;

    if (blockpos != NULL) c->bpop.blockpos = *blockpos;

    if (target != NULL) incrRefCount(target);

    for (j = 0; j < numkeys; j++) {
        /* Allocate our bkinfo structure, associated to each key the client
         * is blocked for. */
        bkinfo *bki = zmalloc(sizeof(*bki));
        if (btype == BLOCKED_STREAM)//阻塞类型=stream
            bki->stream_id = ids[j];

        /* If the key already exists in the dictionary ignore it. */
        if (dictAdd(c->bpop.keys,keys[j],bki) != DICT_OK) {
            zfree(bki);
            continue;
        }
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
        // 把客户端加到blocking_keys字典里
        de = dictFind(c->db->blocking_keys,keys[j]);
        if (de == NULL) {
            int retval;

            /* For every key we take a list of clients blocked for it */
            //没有则创建链表，添加到字典
            l = listCreate();
            retval = dictAdd(c->db->blocking_keys,keys[j],l);//把当前key和对应
            incrRefCount(keys[j]);
            serverAssertWithInfo(c,keys[j],retval == DICT_OK);
        } else {
            l = dictGetVal(de);
        }
        //客户端追加到链表尾部
        listAddNodeTail(l,c);
        bki->listnode = listLast(l);
    }
    //阻塞客户端
    blockClient(c,btype);
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP.
 * You should never call this function directly, but unblockClient() instead. */
void unblockClientWaitingData(client *c) {
    dictEntry *de;
    dictIterator *di;
    list *l;

    serverAssertWithInfo(c,NULL,dictSize(c->bpop.keys) != 0);
    di = dictGetIterator(c->bpop.keys);
    /* The client may wait for multiple keys, so unblock it for every key. */
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        bkinfo *bki = dictGetVal(de);

        /* Remove this client from the list of clients waiting for this key. */
        l = dictFetchValue(c->db->blocking_keys,key);
        serverAssertWithInfo(c,key,l != NULL);
        listDelNode(l,bki->listnode);
        /* If the list is empty we need to remove it to avoid wasting memory */
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,key);
    }
    dictReleaseIterator(di);

    /* Cleanup the client structure */
    dictEmpty(c->bpop.keys,NULL);
    if (c->bpop.target) {
        decrRefCount(c->bpop.target);
        c->bpop.target = NULL;
    }
    if (c->bpop.xread_group) {
        decrRefCount(c->bpop.xread_group);
        decrRefCount(c->bpop.xread_consumer);
        c->bpop.xread_group = NULL;
        c->bpop.xread_consumer = NULL;
    }
}

static int getBlockedTypeByType(int type) {
    switch (type) {
        case OBJ_LIST: return BLOCKED_LIST;
        case OBJ_ZSET: return BLOCKED_ZSET;
        case OBJ_MODULE: return BLOCKED_MODULE;
        case OBJ_STREAM: return BLOCKED_STREAM;
        default: return BLOCKED_NONE;
    }
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is a hash table that allows us to avoid putting
 * the same key again and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnKeys() */
void signalKeyAsReady(redisDb *db, robj *key, int type) {
    readyList *rl;

    /* Quick returns. */
    int btype = getBlockedTypeByType(type);
    if (btype == BLOCKED_NONE) {
        /* The type can never block. */
        return;
    }
    if (!server.blocked_clients_by_type[btype] &&
        !server.blocked_clients_by_type[BLOCKED_MODULE]) {
        /* No clients block on this type. Note: Blocked modules are represented
         * by BLOCKED_MODULE, even if the intention is to wake up by normal
         * types (list, zset, stream), so we need to check that there are no
         * blocked modules before we do a quick return here. */
        return;
    }

    /* No clients blocking for this key? No need to queue it. */
    if (dictFind(db->blocking_keys,key) == NULL) return;

    /* Key was already signaled? No need to queue it again. */
    if (dictFind(db->ready_keys,key) != NULL) return;

    /* Ok, we need to queue this key into server.ready_keys. */
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);

    /* We also add the key in the db->ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1)
     * check. */
    incrRefCount(key);
    serverAssert(dictAdd(db->ready_keys,key,NULL) == DICT_OK);
}
