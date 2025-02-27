/* Kqueue(2)-based ae.c module
 *
 * Copyright (C) 2009 Harish Mallipeddi - harish.mallipeddi@gmail.com
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


#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

typedef struct aeApiState {
    int kqfd;//epoll实例的fd（在drawin就是kqueue的fd）
    struct kevent *events;//事件池，用于存放事件，可放setsize个kevent，一个kevent对应一个fd

    /* Events mask for merge read and write event.
     * To reduce memory consumption, we use 2 bits to store the mask
     * of an event, so that 1 byte will store the mask of 4 events. */
    // 事件掩码，01可读事件，10可写事件，11可读+写事件
    // 为了减少内存使用，使用2个位存储一个fd的事件掩码mask，所以1个字节可存放4个fd的事件掩码
    // 指向的是连续一片的内存空间
    char *eventsMask; //事件掩码
} aeApiState;

/*
 *
 *
 */
#define EVENT_MASK_MALLOC_SIZE(sz) (((sz) + 3) / 4)
/*
 * fd=1，返回2，
 * fd=2，返回4，
 * fd=3，返回6，
 * fd=4，返回0，
 * fd=5，返回2，
 * fd=6，返回4
 */
#define EVENT_MASK_OFFSET(fd) ((fd) % 4 * 2)//返回0，2，4，6
#define EVENT_MASK_ENCODE(fd, mask) (((mask) & 0x3) << EVENT_MASK_OFFSET(fd))//0x3=00000011，取mask低两位左移0/2/4/6位

static inline int getEventMask(const char *eventsMask, int fd) {
    return (eventsMask[fd/4] >> EVENT_MASK_OFFSET(fd)) & 0x3;
}

static inline void addEventMask(char *eventsMask, int fd, int mask) {
    eventsMask[fd/4] |= EVENT_MASK_ENCODE(fd, mask);//1个字节存4种事件的mask
}

static inline void resetEventMask(char *eventsMask, int fd) {
    eventsMask[fd/4] &= ~EVENT_MASK_ENCODE(fd, 0x3);
}

//创建epoll实例（epoll_create()，darwin对应的是kqueue函数），并赋给eventLoop
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    state->events = zmalloc(sizeof(struct kevent) * eventLoop->setsize);
    if (!state->events) {
        zfree(state);//回收已分配的内存
        return -1;
    }
    state->kqfd = kqueue();
    if (state->kqfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    anetCloexec(state->kqfd);//避免fd泄漏
    state->eventsMask = zmalloc(EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));//空间分配
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));//memset把指定内存空间都初始化为 0
    eventLoop->apidata = state;
    return 0;
}

// aeApiState 实例扩缩容到setsize
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    // 重新分配内存，调整 events 的大小
    state->events = zrealloc(state->events, sizeof(struct kevent)*setsize);//扩缩容
    state->eventsMask = zrealloc(state->eventsMask, EVENT_MASK_MALLOC_SIZE(setsize));//扩缩容
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(setsize));
    return 0;
}

//释放eventLoop的epoll实例
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->kqfd);//关闭epoll fd
    zfree(state->events);//释放eventLoop时，同时释放state的events的空间，state是epoll实例
    zfree(state->eventsMask);//释放eventLoop时，同时释放 state的eventsMask 的空间，state是epoll实例
    zfree(state);//释放state state是epoll实例
}

//添加需要监听的fd到epoll里（调用epoll_ctl(ADD)）
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    return 0;
}

//删除对fd的指定事件监听，mask=指定的事件（调用epoll_ctl(DEL)）
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    //删除对fd的读事件监听
    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
    //删除对fd的写事件监听
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
}

//阻塞等待事件触发，底层调用epoll_wait实现，tvp为阻塞的时长，超时后退出本函数
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0; //

    if (tvp != NULL) {//设置了超时时间tvp，阻塞超时后需返回
        struct timespec timeout;
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        //当state->kqfd的epoll实例里注册的fd有事件触发时，把有触发事件的fd存到events空间里，eventLoop->setsize告诉kevent存储的空间有多大
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,&timeout);
    } else {
        //没有设置超时时间 timeout，因此可以无限期阻塞等待
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,NULL);
    }

    if (retval > 0) {//有事件触发
        int j;

        /* Normally we execute the read event first and then the write event.
         * When the barrier is set, we will do it reverse.
         * 
         * However, under kqueue, read and write events would be separate
         * events, which would make it impossible to control the order of
         * reads and writes. So we store the event's mask we've got and merge
         * the same fd events later.
         *
         * 通常先执行读事件，然后再执行写事件，当设置了barrier标识，则先处理写，再处理读事件
         * 然而，在kqueue里，读和写事件是单独分开的，导致无法控制读和写的顺序，
         * 所以先存好已触发的事件然后再把该事件对应fd的读写合并
         * */
        for (j = 0; j < retval; j++) {
            struct kevent *e = state->events+j;
            int fd = e->ident;//有事件触发的fd
            int mask = 0; 

            if (e->filter == EVFILT_READ) mask = AE_READABLE;//该fd触发了可读事件
            else if (e->filter == EVFILT_WRITE) mask = AE_WRITABLE;//该fd触发了可写事件
            addEventMask(state->eventsMask, fd, mask);//把有触发事件的fd和事件类型 存入 state->eventsMask
        }

        /* Re-traversal to merge read and write events, and set the fd's mask to
         * 0 so that events are not added again when the fd is encountered again. */
        numevents = 0;
        for (j = 0; j < retval; j++) {
            struct kevent *e = state->events+j;
            int fd = e->ident;
            int mask = getEventMask(state->eventsMask, fd);//获取该fd上含有的事件，mask可能是 读 或 写 或 读写都有 或 读写都无

            if (mask) {//fd上有事件触发，mask是触发的事件
                //把有事件的fd和触发的事件类型移到eventLoop的fired集合里，等待被其他对象进一步处理
                eventLoop->fired[numevents].fd = fd;//把从epoll_wait里获取到的事件和对应fd填入eventLoop的fired
                eventLoop->fired[numevents].mask = mask;//把从epoll_wait里获取到的事件和对应fd填入eventLoop的fired
                resetEventMask(state->eventsMask, fd);//重置state->eventsMask，以便下一次kevent使用
                numevents++;//有事件的fd个数
            }
        }
    } else if (retval == -1 && errno != EINTR) {
        panic("aeApiPoll: kevent, %s", strerror(errno));
    }

    return numevents;
}

static char *aeApiName(void) {
    return "kqueue";
}
