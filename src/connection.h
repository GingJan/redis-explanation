
/*
 * Copyright (c) 2019, Redis Labs
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
 */

#ifndef __REDIS_CONNECTION_H
#define __REDIS_CONNECTION_H

#include <errno.h>
#include <sys/uio.h>

#define CONN_INFO_LEN   32

struct aeEventLoop;
typedef struct connection connection;

typedef enum {
    CONN_STATE_NONE = 0,
    CONN_STATE_CONNECTING,//连接建立中
    CONN_STATE_ACCEPTING,//从全连接队列取出中
    CONN_STATE_CONNECTED,//连接已建立
    CONN_STATE_CLOSED,//关闭
    CONN_STATE_ERROR
} ConnectionState;//连接状态

#define CONN_FLAG_CLOSE_SCHEDULED   (1<<0)      /* 由conn->close的handler执行关闭操作 Closed scheduled by a handler */
#define CONN_FLAG_WRITE_BARRIER     (1<<1)      /* 写屏障标志，当client->flag有这个标志时，则先处理写事件再处理读事件 Write barrier requested */

#define CONN_TYPE_SOCKET            1
#define CONN_TYPE_TLS               2

typedef void (*ConnectionCallbackFunc)(struct connection *conn);

typedef struct ConnectionType { //实例有 CT_Socket 和 CT_TLS
    void (*ae_handler)(struct aeEventLoop *el, int fd, void *clientData, int mask);//当epoll有事件触发时，调用本函数，本函数的实现有 connSocketEventHandler()（connection.c） 或 tlsEventHandler()（tls.c）
    int (*connect)(struct connection *conn, const char *addr, int port, const char *source_addr, ConnectionCallbackFunc connect_handler);//给connection.conn_handler注册处理函数，本方法的默认实现是 connSocketConnect
    int (*write)(struct connection *conn, const void *data, size_t data_len); // 本方法的默认实现 connSocketWrite
    int (*writev)(struct connection *conn, const struct iovec *iov, int iovcnt);
    int (*read)(struct connection *conn, void *buf, size_t buf_len);
    void (*close)(struct connection *conn);
    int (*accept)(struct connection *conn, ConnectionCallbackFunc accept_handler);//本方法的默认实现 connSocketAccept，
    int (*set_write_handler)(struct connection *conn, ConnectionCallbackFunc handler, int barrier);//给connection.write_handler注册处理函数，本方法的默认实现是 connSocketSetWriteHandler
    int (*set_read_handler)(struct connection *conn, ConnectionCallbackFunc handler); //给connection.read_handler注册处理函数，本方法的默认实现是 connSocketSetReadHandler()（connection.c） 或 connTLSSetReadHandler()（tls.c）
    const char *(*get_last_error)(struct connection *conn);
    int (*blocking_connect)(struct connection *conn, const char *addr, int port, long long timeout);
    ssize_t (*sync_write)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    ssize_t (*sync_read)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    ssize_t (*sync_readline)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    int (*get_type)(struct connection *conn);
} ConnectionType;

struct connection {
    ConnectionType *type;//存放方法的具体实现（不同的连接类型（有connection普通连接和tls安全连接）方法的实现都不同）
    ConnectionState state;//当前conn连接的状态
    short int flags;
    short int refs;//当前conn正在被refs个handler处理中，当refs为0时，connection才可被释放，但可以先关闭连接close(fd)
    int last_errno;
    void *private_data;//存着client实例
    ConnectionCallbackFunc conn_handler; //conn的连接建立handler clusterLinkConnectHandler 或 syncWithMaster
    ConnectionCallbackFunc write_handler; //conn的写handler，当发生写事件时，调用本函数, sendReplyToClient 或 rdbPipeWriteHandler 或 sendBulkToSlave
    ConnectionCallbackFunc read_handler; //conn的读handler
    int fd; //连接的fd
};

/* The connection module does not deal with listening and accepting sockets,
 * so we assume we have a socket when an incoming connection is created.
 *
 * The fd supplied should therefore be associated with an already accept()ed
 * socket.
 *
 * connAccept() may directly call accept_handler(), or return and call it
 * at a later time. This behavior is a bit awkward but aims to reduce the need
 * to wait for the next event loop, if no additional handshake is required.
 *
 * IMPORTANT: accept_handler may decide to close the connection, calling connClose().
 * To make this safe, the connection is only marked with CONN_FLAG_CLOSE_SCHEDULED
 * in this case, and connAccept() returns with an error.
 *
 * connAccept() callers must always check the return value and on error (C_ERR)
 * a connClose() must be called.
 */
// 给连接conn的accept事件设置回调handler =》 accept_handler
static inline int connAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    return conn->type->accept(conn, accept_handler);
}

/* Establish a connection.  The connect_handler will be called when the connection
 * is established, or if an error has occurred.
 *
 * The connection handler will be responsible to set up any read/write handlers
 * as needed.
 *
 * If C_ERR is returned, the operation failed and the connection handler shall
 * not be expected.
 */
//注册 连接建立 处理函数
static inline int connConnect(connection *conn, const char *addr, int port, const char *src_addr,
        ConnectionCallbackFunc connect_handler) {
    return conn->type->connect(conn, addr, port, src_addr, connect_handler);
}

/* Blocking connect.
 *
 * NOTE: This is implemented in order to simplify the transition to the abstract
 * connections, but should probably be refactored out of cluster.c and replication.c,
 * in favor of a pure async implementation.
 */
static inline int connBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    return conn->type->blocking_connect(conn, addr, port, timeout);
}

/* Write to connection, behaves the same as write(2).
 *
 * Like write(2), a short write is possible. A -1 return indicates an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
// 最终的发送数据给客户端
static inline int connWrite(connection *conn, const void *data, size_t data_len) {
    return conn->type->write(conn, data, data_len);// connSocketWrite 或 connTLSWrite
}

/* Gather output data from the iovcnt buffers specified by the members of the iov
 * array: iov[0], iov[1], ..., iov[iovcnt-1] and write to connection, behaves the same as writev(3).
 *
 * Like writev(3), a short write is possible. A -1 return indicates an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
// 最终的发送数据给客户端，本函数比connWrite()高效
static inline int connWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    return conn->type->writev(conn, iov, iovcnt);// connSocketWritev 或 connTLSWritev
}

/* 从连接读取数据，和read(2)的行为一致
 * 就如read(2)，可能只是读取了一小部份数据，如果连接关闭了则返回0，发生错误则返回-1
 * Like read(2), a short read is possible.  A return value of 0 will indicate the
 * connection was closed, and -1 will indicate an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
static inline int connRead(connection *conn, void *buf, size_t buf_len) {
    int ret = conn->type->read(conn, buf, buf_len);
    return ret;
}

/* Register a write handler, to be called when the connection is writable.
 * If NULL, the existing handler is removed.
 */
//注册到 conn->write_handler
static inline int connSetWriteHandler(connection *conn, ConnectionCallbackFunc func) {
    return conn->type->set_write_handler(conn, func, 0);
}

/* 注册一个read handler，当连接可读时，该handler会被调用
 * 如果是NULL，则表示删除已注册的handler
 */
static inline int connSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    return conn->type->set_read_handler(conn, func);
}

/* Set a write handler, and possibly enable a write barrier, this flag is
 * cleared when write handler is changed or removed.
 * With barrier enabled, we never fire the event if the read handler already
 * fired in the same event loop iteration. Useful when you want to persist
 * things to disk before sending replies, and want to do that in a group fashion.
 *
 * 为连接conn设置一个write handler（func），并可设置写屏蔽barrier标志，当write handler被移除时，该标志也会被删掉
 * 当屏障开启时，若在同一次事件循环迭代里，read handler已被调用了，那么事件的write handler就不会调用
 * 当你在发送数据给其他redis副本前，想持久化一些数据到磁盘或以组的方式发送时，可开启写屏障
 * */
static inline int connSetWriteHandlerWithBarrier(connection *conn, ConnectionCallbackFunc func, int barrier) {
    return conn->type->set_write_handler(conn, func, barrier);//把func注册到conn->write_handler
}

static inline void connClose(connection *conn) {
    conn->type->close(conn);
}

/* Returns the last error encountered by the connection, as a string.  If no error,
 * a NULL is returned.
 */
static inline const char *connGetLastError(connection *conn) {
    return conn->type->get_last_error(conn);
}

static inline ssize_t connSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_write(conn, ptr, size, timeout);
}

static inline ssize_t connSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_read(conn, ptr, size, timeout);
}

static inline ssize_t connSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_readline(conn, ptr, size, timeout);
}

/* Return CONN_TYPE_* for the specified connection */
static inline int connGetType(connection *conn) {
    return conn->type->get_type(conn);
}

static inline int connLastErrorRetryable(connection *conn) {
    return conn->last_errno == EINTR;
}

connection *connCreateSocket();
connection *connCreateAcceptedSocket(int fd);

connection *connCreateTLS();
connection *connCreateAcceptedTLS(int fd, int require_auth);

void connSetPrivateData(connection *conn, void *data);
void *connGetPrivateData(connection *conn);
int connGetState(connection *conn);
int connHasWriteHandler(connection *conn);
int connHasReadHandler(connection *conn);
int connGetSocketError(connection *conn);

/* anet-style wrappers to conns */
int connBlock(connection *conn);
int connNonBlock(connection *conn);
int connEnableTcpNoDelay(connection *conn);
int connDisableTcpNoDelay(connection *conn);
int connKeepAlive(connection *conn, int interval);
int connSendTimeout(connection *conn, long long ms);
int connRecvTimeout(connection *conn, long long ms);
int connPeerToString(connection *conn, char *ip, size_t ip_len, int *port);
int connFormatFdAddr(connection *conn, char *buf, size_t buf_len, int fd_to_str_type);
int connSockName(connection *conn, char *ip, size_t ip_len, int *port);
const char *connGetInfo(connection *conn, char *buf, size_t buf_len);

/* Helpers for tls special considerations */
sds connTLSGetPeerCert(connection *conn);
int tlsHasPendingData();
int tlsProcessPendingData();

#endif  /* __REDIS_CONNECTION_H */
