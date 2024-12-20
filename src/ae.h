/* 事件驱动库，一开始这些代码是为了Jim事件循环写的（Jim是一个Tcl解析器），之后把它以库形式写了一次以便复用
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __AE_H__
#define __AE_H__

#include "monotonic.h"

#define AE_OK 0
#define AE_ERR -1

#define AE_NONE 0       /* 000 无已注册的事件*/
#define AE_READABLE 1   /* 001 当fd可读时触发*/
#define AE_WRITABLE 2   /* 010 当fd可写时触发*/
#define AE_BARRIER 4    /* 100 读写事件处理顺序是否反转，默认情况下，先处理读再处理写，当在批量发送响应数据给client时，需把内容先持久化到磁盘时 非常有用 */

#define AE_FILE_EVENTS (1<<0)//1
#define AE_TIME_EVENTS (1<<1)//2
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)//3
#define AE_DONT_WAIT (1<<2)//4 不阻塞等待，立即返回
#define AE_CALL_BEFORE_SLEEP (1<<3)//8
#define AE_CALL_AFTER_SLEEP (1<<4)//16

#define AE_NOMORE -1// 「本」时间事件 结束
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);//id=fd
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* 文件事件 结构体 */
typedef struct aeFileEvent {
    int mask; /* 事件，AE_READABLE 0001 或 AE_WRITABLE 0010 或 AE_BARRIER 0100 */
    aeFileProc *rfileProc; //可读事件处理函数
    aeFileProc *wfileProc; //可写事件处理函数
    void *clientData; //数据
} aeFileEvent;

/* 时间事件 结构体 */
typedef struct aeTimeEvent {
    long long id; /* 时间事件fd */
    monotime when;
    aeTimeProc *timeProc;//时间事件的处理函数，触发时间事件时回调该函数
    aeEventFinalizerProc *finalizerProc;
    void *clientData;
    struct aeTimeEvent *prev;
    struct aeTimeEvent *next;
    int refcount; /* refcount to prevent timer events from being
  		   * freed in recursive time event calls. */
} aeTimeEvent;//时间事件

/* 某个被触发的事件 */
typedef struct aeFiredEvent {
    int fd;//事件对应的fd
    int mask;//事件需要进行的操作，0001读，0010写，0100，当这个fd同时有读写事件时，mask=0011
} aeFiredEvent;

/* 基于事件状态的程序 */
typedef struct aeEventLoop {
    int maxfd;   /* 当前已注册的最大fd（注fd是一个整型数字） */
    int setsize; /* 可注册？还是已注册？ 的fd的数量 */
    long long timeEventNextId; //时间事件的id生成器
    aeFileEvent *events; /* 已注册的事件，是一个数组，下标是fd */
    aeFiredEvent *fired; /* 被触发的事件，是一个数组，下标是fd */
    aeTimeEvent *timeEventHead; //时间事件链表 的头节点
    int stop;//是否停止，0否，1是
    void *apidata; /* 该字段用于存放对应系统创建的epoll实例，指向 aeApiState */
    aeBeforeSleepProc *beforesleep; // 在调用epoll_wait前执行
    aeBeforeSleepProc *aftersleep; // 在epoll_wait返回后执行
    int flags;
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);// 创建 eventLoop实例，setsize是可监听event的数量，本函数有以下逻辑，申请空间，初始化事件的mask，当前对应系统（Linux？Windows？Unix？等）的多路复用接口epoll_create以创建epoll
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
void *aeGetFileClientData(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,aeTimeProc *proc, void *clientData,aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);
void aeSetDontWait(aeEventLoop *eventLoop, int noWait);

#endif
