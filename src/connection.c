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

#include "server.h"
#include "connhelpers.h"

/* The connections module provides a lean abstraction of network connections
 * to avoid direct socket and async event management across the Redis code base.
 *
 * It does NOT provide advanced connection features commonly found in similar
 * libraries such as complete in/out buffer management, throttling, etc. These
 * functions remain in networking.c.
 *
 * The primary goal is to allow transparent handling of TCP and TLS based
 * connections. To do so, connections have the following properties:
 *
 * 1. A connection may live before its corresponding socket exists.  This
 *    allows various context and configuration setting to be handled before
 *    establishing the actual connection.
 * 2. The caller may register/unregister logical read/write handlers to be
 *    called when the connection has data to read from/can accept writes.
 *    These logical handlers may or may not correspond to actual AE events,
 *    depending on the implementation (for TCP they are; for TLS they aren't).
 */

ConnectionType CT_Socket;

/* When a connection is created we must know its type already, but the
 * underlying socket may or may not exist:
 *
 * - For accepted connections, it exists as we do not model the listen/accept
 *   part; So caller calls connCreateSocket() followed by connAccept().
 * - For outgoing connections, the socket is created by the connection module
 *   itself; So caller calls connCreateSocket() followed by connConnect(),
 *   which registers a connect callback that fires on connected/error state
 *   (and after any transport level handshake was done).
 *
 * NOTE: An earlier version relied on connections being part of other structs
 * and not independently allocated. This could lead to further optimizations
 * like using container_of(), etc.  However it was discontinued in favor of
 * this approach for these reasons:
 *
 * 1. In some cases conns are created/handled outside the context of the
 * containing struct, in which case it gets a bit awkward to copy them.
 * 2. Future implementations may wish to allocate arbitrary data for the
 * connection.
 * 3. The container_of() approach is anyway risky because connections may
 * be embedded in different structs, not just client.
 */

connection *connCreateSocket() {
    connection *conn = zcalloc(sizeof(connection));
    conn->type = &CT_Socket;
    conn->fd = -1;

    return conn;
}

/* Create a new socket-type connection that is already associated with
 * an accepted connection.
 *
 * The socket is not ready for I/O until connAccept() was called and
 * invoked the connection-level accept handler.
 *
 * Callers should use connGetState() and verify the created connection
 * is not in an error state (which is not possible for a socket connection,
 * but could but possible with other protocols).
 */
connection *connCreateAcceptedSocket(int fd) {//对fd（新连接）进行封装wrapper并返回一个conn实例
    connection *conn = connCreateSocket();//为conn封装具体方法
    conn->fd = fd;
    conn->state = CONN_STATE_ACCEPTING;
    return conn;
}

//注册 连接建立（established）的处理函数
static int connSocketConnect(connection *conn, const char *addr, int port, const char *src_addr, ConnectionCallbackFunc connect_handler) {
    int fd = anetTcpNonBlockBestEffortBindConnect(NULL,addr,port,src_addr);//创建fd
    if (fd == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = errno;
        return C_ERR;
    }

    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTING;

    conn->conn_handler = connect_handler;

    //往el注册对fd的可写事件监听，当有可写事件触发时，调用ae_handler函数并传入conn数据
    aeCreateFileEvent(server.el, conn->fd, AE_WRITABLE, conn->type->ae_handler, conn);//注册epoll的可写handler

    return C_OK;
}

/* Returns true if a write handler is registered */
int connHasWriteHandler(connection *conn) {
    return conn->write_handler != NULL;
}

/* Returns true if a read handler is registered */
int connHasReadHandler(connection *conn) {
    return conn->read_handler != NULL;
}

/* Associate a private data pointer with the connection */
void connSetPrivateData(connection *conn, void *data) {
    conn->private_data = data;
}

/* Get the associated private data pointer */
// 获取conn底层指向的client
void *connGetPrivateData(connection *conn) {
    return conn->private_data;
}

/* ------ Pure socket connections ------- */

/* A very incomplete list of implementation-specific calls.  Much of the above shall
 * move here as we implement additional connection types.
 */

/* Close the connection and free resources. */
/* 关闭连接，同时释放资源 */
static void connSocketClose(connection *conn) {
    if (conn->fd != -1) {
        aeDeleteFileEvent(server.el,conn->fd, AE_READABLE | AE_WRITABLE);//从epoll删除fd的监听
        close(conn->fd);//调用系统close()关闭fd
        conn->fd = -1;//此时，conn对应的fd已被关闭 且 不在epoll的监听列表里，但是conn这个实例还未释放，因为此时可能还有其他的handler正在处理该conn
    }

    /* 若从handler内部调用
     *
     * If called from within a handler, schedule the close but
     * keep the connection until the handler returns.
     */
    if (connHasRefs(conn)) {//若还有其他handler正在处理该conn，则现在不能马上释放conn，添加scheduled标识，等待全部handler处理完毕该conn时，再调用本函数进行conn的释放
        conn->flags |= CONN_FLAG_CLOSE_SCHEDULED;
        return;
    }

    zfree(conn);
}

//本函数最终发送数据给客户端的，通过调用系统函数write()实现
//conn就是对应和该客户端的连接
static int connSocketWrite(connection *conn, const void *data, size_t data_len) {
    int ret = write(conn->fd, data, data_len);//write()系统调用
    if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;

        /* Don't overwrite the state of a connection that is not already
         * connected, not to mess with handler callbacks.
         */
        // 当连接还没建立时，不要覆盖conn的状态，不要和回调handler混淆
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;//conn 连接出错
    }

    return ret;
}

//本函数最终发送数据给客户端的，通过调用系统函数writev()实现
//conn就是对应和该客户端的连接
static int connSocketWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    // 是 write 的增强版本，允许将多个不同的内存块（缓冲区）合并成一个单独的写操作。
    // 通过使用一个 iovec 结构体数组来指定多个缓冲区，这样可以一次性将多个数据块写入同一个文件描述符。
    // 如果你有多个数据缓冲区，并且希望一次性将它们写入同一个文件或套接字，可以使用 writev。
    // 它能够减少多次系统调用带来的开销，提高效率。
    int ret = writev(conn->fd, iov, iovcnt);//系统调用，发出数据
    if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;

        /* Don't overwrite the state of a connection that is not already
         * connected, not to mess with handler callbacks.
         */
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;
    }

    return ret;
}

//数据读取handler，当连接有可读数据时，调用本函数进行处理
static int connSocketRead(connection *conn, void *buf, size_t buf_len) {
    int ret = read(conn->fd, buf, buf_len);
    if (!ret) {//ret=0，则表示连接已被关闭
        conn->state = CONN_STATE_CLOSED;//对端已关闭连接，即client无数据发送给server
    } else if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;

        /* Don't overwrite the state of a connection that is not already
         * connected, not to mess with handler callbacks.
         */
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;//连接异常
    }

    return ret;
}
//conn连接accept()后的回调函数
static int connSocketAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    int ret = C_OK;

    if (conn->state != CONN_STATE_ACCEPTING) return C_ERR;//状态异常
    conn->state = CONN_STATE_CONNECTED;

    connIncrRefs(conn);
    if (!callHandler(conn, accept_handler)) ret = C_ERR;
    connDecrRefs(conn);

    return ret;
}

/* Register a write handler, to be called when the connection is writable.
 * If NULL, the existing handler is removed.
 *
 * The barrier flag indicates a write barrier is requested, resulting with
 * CONN_FLAG_WRITE_BARRIER set. This will ensure that the write handler is
 * always called before and not after the read handler in a single event
 * loop.
 *
 * 注册write handler，当连接conn可写时会被调用，如果传入func=NULL，则已有的handler会被删除
 * barrier 标识表示是否需要写屏障，当是CONN_FLAG_WRITE_BARRIER时，
 * 在一个循环里，先处理写handler，再处理读handler
 *
 * 创建文件事件，并往底层epoll添加/注册fd监听
 * barrier=1时，则
 */
static int connSocketSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    if (func == conn->write_handler) return C_OK;

    conn->write_handler = func;
    if (barrier)//开启写屏蔽
        conn->flags |= CONN_FLAG_WRITE_BARRIER;//开启写屏障
    else
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;//关闭写屏障

    //对fd的写事件的监听进行处理
    if (!conn->write_handler) //如果conn上的write handler被移除了，则把底层epoll对本conn的可写事件的监听也移除
        aeDeleteFileEvent(server.el,conn->fd,AE_WRITABLE);//在事件循环实例里，删除本conn的可写文件事件的监听
    else
        //在epoll创建对本conn的可写文件事件的监听
        if (aeCreateFileEvent(server.el,conn->fd,AE_WRITABLE,conn->type->ae_handler,conn) == AE_ERR) return C_ERR;
    return C_OK;
}

/* Register a read handler, to be called when the connection is readable.
 * If NULL, the existing handler is removed.
 */
static int connSocketSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    if (func == conn->read_handler) return C_OK;

    conn->read_handler = func;
    if (!conn->read_handler)//如果传入的func是空，则删除对该conn连接的监听（从epoll移除 ）
        aeDeleteFileEvent(server.el,conn->fd,AE_READABLE);
    else
        //把本conn添加到epoll里监听可读事件，当有事件发生时，回调ae_handler函数
        if (aeCreateFileEvent(server.el,conn->fd,
                    AE_READABLE,conn->type->ae_handler,conn) == AE_ERR) return C_ERR;
    return C_OK;
}

static const char *connSocketGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

//当el监听到fd有事件时（无论可写/可读），本函数会被调用（已被注册作为ae_handler，当epoll底层触发事件时，调用ae_handler）
static void connSocketEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask)
{
    UNUSED(el);
    UNUSED(fd);
    connection *conn = clientData;

    //触发的是 连接建立established事件
    if (conn->state == CONN_STATE_CONNECTING && (mask & AE_WRITABLE) && conn->conn_handler) {

        int conn_error = connGetSocketError(conn);
        if (conn_error) {//连接有异常
            conn->last_errno = conn_error;
            conn->state = CONN_STATE_ERROR;
        } else {//连接正常
            conn->state = CONN_STATE_CONNECTED;
        }

        //如果只注册了conn_handler，write_handler没注册，那么把fd的可写事件从epoll里移除监听
        //因为连接建立established事件已触发（该事件对当前连接来说只会触发一次），因此可以移除对该fd的该事件监听
        if (!conn->write_handler) aeDeleteFileEvent(server.el,conn->fd,AE_WRITABLE);

        if (!callHandler(conn, conn->conn_handler)) return;
        conn->conn_handler = NULL;
    }

    /* Normally we execute the readable event first, and the writable
     * event later. This is useful as sometimes we may be able
     * to serve the reply of a query immediately after processing the
     * query.
     *
     * However if WRITE_BARRIER is set in the mask, our application is
     * asking us to do the reverse: never fire the writable event
     * after the readable. In such a case, we invert the calls.
     * This is useful when, for instance, we want to do things
     * in the beforeSleep() hook, like fsync'ing a file to disk,
     * before replying to a client.
     *
     * 通常，是先执行可读事件的handler，再执行可写事件的，这有时候很有用，因为我们可能在
     * 处理查询后需立即返回该查询的响应
     * 然而如果设置了WRITE_BARRIER标识，则把执行顺序倒转：先执行可写再执行可读事件
     * 这也很有用，例如想在beforeSleep()前做些如在回复响应前执行fsync刷盘等操作
     * */
    int invert = conn->flags & CONN_FLAG_WRITE_BARRIER;//invert开启了（是否反转操作，即先写再读）

    int call_write = (mask & AE_WRITABLE) && conn->write_handler;//如果触发的是属于可写事件 并且 已经注册了write_hander
    int call_read = (mask & AE_READABLE) && conn->read_handler;//如果触发的是属于可读事件 并且 已经注册了read_hander

    /* 处理普通的I/O流 Handle normal I/O flows */
    if (!invert && call_read) {//没有开启invert 则先处理读事件（如果有触发读事件）
        if (!callHandler(conn, conn->read_handler)) return;
    }
    /* 触发了可写事件 Fire the writable event. */
    if (call_write) {
        if (!callHandler(conn, conn->write_handler)) return;//调用conn的write_handler
    }
    /* If we have to invert the call, fire the readable event now
     * after the writable one. */
    if (invert && call_read) {//开启了invert 则先处理写事件，再处理读事件（如果）
        if (!callHandler(conn, conn->read_handler)) return;
    }
}

static int connSocketBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    int fd = anetTcpNonBlockConnect(NULL,addr,port);
    if (fd == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = errno;
        return C_ERR;
    }

    if ((aeWait(fd, AE_WRITABLE, timeout) & AE_WRITABLE) == 0) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = ETIMEDOUT;
    }

    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTED;
    return C_OK;
}

/* Connection-based versions of syncio.c functions.
 * NOTE: This should ideally be refactored out in favor of pure async work.
 */

static ssize_t connSocketSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncWrite(conn->fd, ptr, size, timeout);
}

static ssize_t connSocketSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncRead(conn->fd, ptr, size, timeout);
}

static ssize_t connSocketSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncReadLine(conn->fd, ptr, size, timeout);
}

static int connSocketGetType(connection *conn) {
    (void) conn;

    return CONN_TYPE_SOCKET;
}

ConnectionType CT_Socket = {//连接socket的处理函数具体实现
    .ae_handler = connSocketEventHandler,//当el监听到fd有事件时（无论可写/可读），本函数会被调用
    .close = connSocketClose,
    .write = connSocketWrite,//调用系统write()函数，写数据给client
    .writev = connSocketWritev,//调用系统writev()函数，写数据给client
    .read = connSocketRead,//调用系统read()函数，从client连接里读取数据
    .accept = connSocketAccept,//accept 回调函数
    .connect = connSocketConnect,
    .set_write_handler = connSocketSetWriteHandler,
    .set_read_handler = connSocketSetReadHandler,
    .get_last_error = connSocketGetLastError,
    .blocking_connect = connSocketBlockingConnect,
    .sync_write = connSocketSyncWrite,
    .sync_read = connSocketSyncRead,
    .sync_readline = connSocketSyncReadLine,
    .get_type = connSocketGetType
};


int connGetSocketError(connection *conn) {
    int sockerr = 0;
    socklen_t errlen = sizeof(sockerr);

    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    return sockerr;
}

int connPeerToString(connection *conn, char *ip, size_t ip_len, int *port) {
    return anetFdToString(conn ? conn->fd : -1, ip, ip_len, port, FD_TO_PEER_NAME);
}

int connSockName(connection *conn, char *ip, size_t ip_len, int *port) {
    return anetFdToString(conn->fd, ip, ip_len, port, FD_TO_SOCK_NAME);
}

int connFormatFdAddr(connection *conn, char *buf, size_t buf_len, int fd_to_str_type) {
    return anetFormatFdAddr(conn ? conn->fd : -1, buf, buf_len, fd_to_str_type);
}

int connBlock(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetBlock(NULL, conn->fd);
}

int connNonBlock(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetNonBlock(NULL, conn->fd);
}

int connEnableTcpNoDelay(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetEnableTcpNoDelay(NULL, conn->fd);
}

int connDisableTcpNoDelay(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetDisableTcpNoDelay(NULL, conn->fd);
}

int connKeepAlive(connection *conn, int interval) {
    if (conn->fd == -1) return C_ERR;
    return anetKeepAlive(NULL, conn->fd, interval);
}

int connSendTimeout(connection *conn, long long ms) {
    return anetSendTimeout(NULL, conn->fd, ms);
}

int connRecvTimeout(connection *conn, long long ms) {
    return anetRecvTimeout(NULL, conn->fd, ms);
}

int connGetState(connection *conn) {
    return conn->state;
}

/* Return a text that describes the connection, suitable for inclusion
 * in CLIENT LIST and similar outputs.
 *
 * For sockets, we always return "fd=<fdnum>" to maintain compatibility.
 */
const char *connGetInfo(connection *conn, char *buf, size_t buf_len) {
    snprintf(buf, buf_len-1, "fd=%i", conn == NULL ? -1 : conn->fd);
    return buf;
}

