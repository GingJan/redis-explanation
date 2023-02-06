/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "ae.h"
#include "anet.h"
#include "redisassert.h"

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "zmalloc.h"
#include "config.h"

/* 引入本系统支持的最好的多路复用层
 * 根据性能倒序排列，逐个尝试引入 */
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
    #ifdef HAVE_EPOLL
    #include "ae_epoll.c"
    #else
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.c"
        #else
        #include "ae_select.c"
        #endif
    #endif
#endif

// 创建事件循环实例
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    monotonicInit();    /* 再次调用monotonic时钟的初始化，以防未初始化monotonic时钟 */

    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;//给eventLoop分配空间失败
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);//给events分配空间
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);//给fired分配空间
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;//在创建eventLoop时，申请events 或 fired 空间失败
    eventLoop->setsize = setsize;
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
    eventLoop->aftersleep = NULL;
    eventLoop->flags = 0;
    if (aeApiCreate(eventLoop) == -1) goto err;//aeApiCreate 调用当前系统对应的多路复用接口,epoll_create
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);//当创建eventLoop错误时，释放events已申请的空间
        zfree(eventLoop->fired);//当创建eventLoop错误时，释放fired已申请的空间
        zfree(eventLoop);//当创建eventLoop错误时，释放eventLoop已申请的空间
    }
    return NULL;
}

/* Return the current set size. */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;//被追踪的fd的最大数量
}

/* Tells the next iteration/s of the event processing to set timeout of 0. */
void aeSetDontWait(aeEventLoop *eventLoop, int noWait) {
    if (noWait)
        eventLoop->flags |= AE_DONT_WAIT;
    else
        eventLoop->flags &= ~AE_DONT_WAIT;
}

/* 对eventloop进行扩缩容
 * 如果传入的setsize比当前setsize小，但是当前已使用的size大于传入setsize，
 * 则返回AE_ERR，同时不会进行任何处理，否则返回AE_OK并执行扩/缩容逻辑
 *
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    int i;

    if (setsize == eventLoop->setsize) return AE_OK;
    if (eventLoop->maxfd >= setsize) return AE_ERR;
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;

    eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);//events指向的空间扩缩容
    eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);//fired指向的空间扩缩容
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
    for (i = eventLoop->maxfd+1; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}
//删除eventLoop，同时释放空间
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);//释放eventLoop的epoll实例
    zfree(eventLoop->events);//删除eventLoop时，释放events 的空间
    zfree(eventLoop->fired);//删除eventLoop时，释放fired 的空间

    /* 释放 时间事件链表 的空间 */
    aeTimeEvent *next_te, *te = eventLoop->timeEventHead;
    while (te) {
        next_te = te->next;
        zfree(te);//删除eventLoop时，释放时间事件链表 的空间
        te = next_te;
    }
    zfree(eventLoop);//释放eventLoop的空间
}
//关闭事件循环
void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}
// 创建文件事件
//eventLoop：事件循环实例
//fd：需要监听的fd
//mask：需要监听的事件
//proc：事件处理函数
//clientData：客户端发来的数据
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData)
{
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
    aeFileEvent *fe = &eventLoop->events[fd];

    if (aeApiAddEvent(eventLoop, fd, mask) == -1)//调用系统对应的epoll_add()
        return AE_ERR;
    fe->mask |= mask;// 0001读，0010写，0100
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}
//删除对fd的指定事件监听
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    if (fd >= eventLoop->setsize) return;
    aeFileEvent *fe = &eventLoop->events[fd];//获取该fd对应的文件事件
    if (fe->mask == AE_NONE) return;

    /* We want to always remove AE_BARRIER if set when AE_WRITABLE
     * is removed.
     *
     * 在删除AE_WRITABLE时总是删除AE_BARRIER（若设置了）
     *
     */
    if (mask & AE_WRITABLE) mask |= AE_BARRIER; // mask | 0100，取mask=X1XX

    aeApiDelEvent(eventLoop, fd, mask);//调用系统底层的epoll_ct(DEL)，删除对fd的指定事件监听，mask=指定的事件
    fe->mask = fe->mask & (~mask);//更新 剩余的事件
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}

void *aeGetFileClientData(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return NULL;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return NULL;

    return fe->clientData;
}

int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}
//创建时间事件池（双向链表）
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds, aeTimeProc *proc, void *clientData, aeEventFinalizerProc *finalizerProc) {
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    te->when = getMonotonicUs() + milliseconds * 1000;
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    te->prev = NULL;
    te->next = eventLoop->timeEventHead;
    te->refcount = 0;
    if (te->next)
        te->next->prev = te;
    eventLoop->timeEventHead = te;
    return id;
}

int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    while(te) {
        if (te->id == id) {
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* How many microseconds until the first timer should fire.
 * If there are no timers, -1 is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
static int64_t usUntilEarliestTimer(aeEventLoop *eventLoop) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    if (te == NULL) return -1;

    aeTimeEvent *earliest = NULL;
    while (te) {
        if (!earliest || te->when < earliest->when)
            earliest = te;
        te = te->next;
    }

    monotime now = getMonotonicUs();
    return (now >= earliest->when) ? 0 : earliest->when - now;
}

/* Process time events */
// 处理时间事件
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
    monotime now = getMonotonicUs();
    while(te) {
        long long id;

        /* Remove events scheduled for deletion. */
        // 删除id=-1的时间事件
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            /* If a reference exists for this timer event,
             * don't free it. This is currently incremented
             * for recursive timerProc calls */
            if (te->refcount) {//当refcount不为0 说明该te正在被timerProc函数处理
                te = next;
                continue;
            }
            if (te->prev)
                te->prev->next = te->next;
            else
                eventLoop->timeEventHead = te->next;
            if (te->next)
                te->next->prev = te->prev;
            if (te->finalizerProc) {
                te->finalizerProc(eventLoop, te->clientData);
                now = getMonotonicUs();
            }
            zfree(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
        if (te->id > maxId) {//在本次循环开始后添加的时间事件不会被当前循环处理
            te = te->next;
            continue;
        }

        if (te->when <= now) {//当 到了/过了 时间事件 的执行时间点，则执行该事件
            int retval;

            id = te->id;
            te->refcount++;//当前时间事件正在被处理
            retval = te->timeProc(eventLoop, id, te->clientData);
            te->refcount--;//当前时间事件处理完毕
            processed++;
            now = getMonotonicUs();
            if (retval != AE_NOMORE) {
                te->when = now + retval * 1000;//更新下一次触发的时间点
            } else {//当返回的时NOMORE时，则把该时间事件删除
                te->id = AE_DELETED_EVENT_ID;
            }
        }
        te = te->next;
    }
    return processed;
}

/* 事件循环的核心函数，外层被一个while循环调用
 * 先处理每个在等待的时间事件，然后再处理每个在等待的文件事件
 * （可能有刚处理的时间事件注册的文件时间）.
 * 若无指定flags，本函数会sleep直到有文件事件触发或下一个时间事件发生
 *
 * 若 flags 是 0, 本函数不做任何逻辑并且立刻返回
 * 若 flags 有 AE_ALL_EVENTS 标志, 所有类型的事件都会被处理
 * 若 flags 有 AE_FILE_EVENTS 标志, 文件事件才会被处理.
 * if flags has AE_TIME_EVENTS set, 时间事件才会被处理.
 * if flags has AE_DONT_WAIT set, 那些无需等待就可被处理的事件一旦被处理完毕后，本函数就会尽快返回
 * if flags has AE_CALL_AFTER_SLEEP set, aftersleep回调会被调用
 * if flags has AE_CALL_BEFORE_SLEEP set, beforesleep回调会被调用.
 *
 * 本函数返回 被处理事件 的个数，本函数底层是通过调用系统的epoll_wait()来实现 */
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want to call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire.
     *
     * 注意，只要我们想处理时间事件，即使没有文件事件要处理，我们也要调用 select()也即epoll_wait，
     * 以便 休眠/阻塞 到下一个时间事件准备好触发（这样就不会空跑浪费）
     * */
    if (eventLoop->maxfd != -1 || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        struct timeval tv, *tvp;
        int64_t usUntilTimer = -1;

        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            usUntilTimer = usUntilEarliestTimer(eventLoop);//距离最近一个时间事件还剩下 usUntilTimer微秒

        if (usUntilTimer >= 0) {
            tv.tv_sec = usUntilTimer / 1000000;
            tv.tv_usec = usUntilTimer % 1000000;
            tvp = &tv;
        } else {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
            //如果设置了AE_DONT_WAIT，则需要尽快返回，因此把tv.tv_sec设置为0
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                //否则，则可以阻塞等待
                tvp = NULL; /* wait forever */
            }
        }

        if (eventLoop->flags & AE_DONT_WAIT) {
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        }

        if (eventLoop->beforesleep != NULL && flags & AE_CALL_BEFORE_SLEEP)
            eventLoop->beforesleep(eventLoop);

        /* 调用多路复用api，一直阻塞直到 超时 或 有事件触发时才返回 */
        numevents = aeApiPoll(eventLoop, tvp);//numevents 触发事件的个数，aeApiPoll()底层是调用对应系统的epoll_wait()

        /* 回调aftersleep函数 */
        if (eventLoop->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP)
            eventLoop->aftersleep(eventLoop);

        for (j = 0; j < numevents; j++) {
            int fd = eventLoop->fired[j].fd;//触发事件的fd
            aeFileEvent *fe = &eventLoop->events[fd];// 注册的文件事件
            int mask = eventLoop->fired[j].mask;//触发事件的mask
            int fired = 0; /* 当前fd 已触发事件的个数 Number of events fired for current fd. */

            /* 通常先处理可读事件，然后再处理可写事件，因为这样可以在处理完查询请求后立即把响应返回给该查询请求
             *
             * 然而如果mask里有 AE_BARRIER 标志, 则以相反的方式处理：可读事件处理完毕后先不触发可写事件，
             * 在这种情况下，我们要反转调用次序。这非常有用，例如想在beforeSleep()钩子函数里做些逻辑，像
             * 在返回响应给客户端前，先执行文件同步fsync刷到磁盘的逻辑
             */
            int invert = fe->mask & AE_BARRIER;// 当注册的文件事件 设置了AE_BARRIER

            /* 注意，"fe->mask & mask & ..." 代码： maybe an already
             * processed event removed an element that fired and we still
             * didn't processed, so we check if the event is still valid.
             *
             * 若调用次序无需反转，则触发可读事件 */
            if (!invert && fe->mask & mask & AE_READABLE) {//如果事件的mask里没AE_BARRIER标志，但设置了AE_READABLE，说明该fd对应的事件是可读事件
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);//处理该可读事件
                fired++;
                fe = &eventLoop->events[fd]; /* 重新获取fe，以防eventLoop当前正在进行扩缩容导致fe指向的地址变化 Refresh in case of resize. */
            }

            /* 触发可写事件 */
            if (fe->mask & mask & AE_WRITABLE) {
                if (!fired || fe->wfileProc != fe->rfileProc) {
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            /* 若需反转调用次序，则在处理完可写事件后，再处理可读事件 */
            if (invert) {
                fe = &eventLoop->events[fd]; /* 重新获取fe，以防当前eventLoop被扩缩容 Refresh in case of resize. */
                if ((fe->mask & mask & AE_READABLE) &&
                    (!fired || fe->wfileProc != fe->rfileProc))
                {
                    fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            processed++;//已处理事件个数
        }
    }

    /* 检查时间事件 */
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);//处理时间事件

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;//事件循环的停止标识
    while (!eventLoop->stop) {
        //以阻塞的方式 处理文件和时间事件
        aeProcessEvents(eventLoop, AE_ALL_EVENTS|
                                   AE_CALL_BEFORE_SLEEP|
                                   AE_CALL_AFTER_SLEEP);
    }
}

char *aeGetApiName(void) {
    return aeApiName();
}

void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}
