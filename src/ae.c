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

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "ae.h"
#include "zmalloc.h"
#include "config.h"

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
// 如果定义了 evPort
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
    // 如果定义了 epoll
    #ifdef HAVE_EPOLL
    #include "ae_epoll.c"
    #else
        // 如果定义了 kqueue ， MAC 是使用该 nio
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.c"
        #else
        // 如果定义了 select
        #include "ae_select.c"
        #endif
    #endif
#endif

// 初始化事件处理器状态
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    // 创建事件状态结构
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;

    // 初始化文件事件结构和已就绪文件事件结构
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    eventLoop->setsize = setsize;
    eventLoop->lastTime = time(NULL);

    // 初始化时间事件结构
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;

    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
    eventLoop->aftersleep = NULL;
    eventLoop->flags = 0;
    if (aeApiCreate(eventLoop) == -1) goto err;
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* Return the current set size. */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/* Tells the next iteration/s of the event processing to set timeout of 0. */
void aeSetDontWait(aeEventLoop *eventLoop, int noWait) {
    if (noWait)
        eventLoop->flags |= AE_DONT_WAIT;
    else
        eventLoop->flags &= ~AE_DONT_WAIT;
}

/* Resize the maximum set size of the event loop.
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

    eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
    for (i = eventLoop->maxfd+1; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}

// 删除事件处理器
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);

    /* Free the time events list. */
    aeTimeEvent *next_te, *te = eventLoop->timeEventHead;
    while (te) {
        next_te = te->next;
        zfree(te);
        te = next_te;
    }
    zfree(eventLoop);
}

// 停止事件处理器
void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

/*
 * 根据 mask 参数的值，监听 fd 文件的状态，
 * 当 fd 可用时，执行 proc 函数
 */
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
    aeFileEvent *fe = &eventLoop->events[fd];

    // 监听指定 fd
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;

    // 设置文件事件类型
    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;

    // 如果有需要，更新事件处理器的最大 fd
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

// 将 fd 从 mask 指定的监听队列中删除
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    if (fd >= eventLoop->setsize) return;
    aeFileEvent *fe = &eventLoop->events[fd];
    // 未设置监听的事件类型，直接返回
    if (fe->mask == AE_NONE) return;

    /* We want to always remove AE_BARRIER if set when AE_WRITABLE
     * is removed. */
    if (mask & AE_WRITABLE) mask |= AE_BARRIER;

    // 取消监听给定 fd
    aeApiDelEvent(eventLoop, fd, mask);
    fe->mask = fe->mask & (~mask);
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}

//  获取给定 fd 正在监听的事件类型
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}

/*
 * 取出当前时间的秒和毫秒，
 * 并分别将它们保存到 seconds 和 milliseconds 参数中
 */
static void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

//  为当前时间加上 milliseconds 秒。
static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    // 获取当前时间
    aeGetTime(&cur_sec, &cur_ms);

    // 计算增加 milliseconds 之后的秒数和毫秒数
    when_sec = cur_sec + milliseconds/1000;
    when_ms = cur_ms + milliseconds%1000;

    // 进位：
    // 如果 when_ms 大于等于 1000
    // 那么将 when_sec 增大一秒
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}

// 创建时间事件
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
    // 更新时间计数器
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    // 设定处理事件的时间
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    te->prev = NULL;
    // 将新事件放入表头
    te->next = eventLoop->timeEventHead;
    te->refcount = 0;
    if (te->next)
        te->next->prev = te;
    eventLoop->timeEventHead = te;
    return id;
}

// 删除给定 id 的时间事件
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

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
// 寻找里目前执行时间最小的时间事件
// 因为链表是乱序的，所以查找复杂度为 O（N）
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;

    // te 指针先指向 eventLoop 的第一个定时任务，然后不断的往后遍历
    // nearest 是指找到的最合适的
    while(te) {
        if (
                // 如果 nearest 是 空，其实就是第一次进 while 循环的时候成立
                !nearest
                // 如果 te 的执行时间比 nearest 的小，谁小谁小执行
                || te->when_sec < nearest->when_sec
                // 或者秒相等，但是毫秒值小，谁小谁小执行
                || (te->when_sec == nearest->when_sec && te->when_ms < nearest->when_ms))
        {
            nearest = te;
        }

        te = te->next;
    }
    return nearest;
}

/* Process time events */
// 处理所有已到达的时间事件
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    // 通过重置事件的运行时间，
    // 防止因时间穿插（skew）而造成的事件处理混乱
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
    // 更新最后一次处理时间事件的时间
    eventLoop->lastTime = now;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
    while(te) {
        long now_sec, now_ms;
        long long id;

        /* Remove events scheduled for deletion. */
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            /* If a reference exists for this timer event,
             * don't free it. This is currently incremented
             * for recursive timerProc calls */
            if (te->refcount) {
                te = next;
                continue;
            }
            if (te->prev)
                te->prev->next = te->next;
            else
                eventLoop->timeEventHead = te->next;
            if (te->next)
                te->next->prev = te->prev;
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);
            zfree(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
        // 跳过无效事件
        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        // 获取当前时间
        aeGetTime(&now_sec, &now_ms);
        // 如果当前时间大于或等于事件的执行时间，那么执行这个事件
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;

            id = te->id;
            te->refcount++;
            retval = te->timeProc(eventLoop, id, te->clientData);
            te->refcount--;
            processed++;
            // 记录是否有需要循环执行这个事件时间
            if (retval != AE_NOMORE) {
                // 是的， retval 毫秒之后继续执行这个时间事件
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {
                // 不，将这个事件删除
                te->id = AE_DELETED_EVENT_ID;
            }
        }
        te = te->next;
    }
    return processed;
}


/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 *
 * 处理所有已到达的时间事件，以及所有已就绪的文件事件。
 *
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurrs (if any).
 *
 * 如果不传入特殊 flags 的话，那么函数睡眠直到文件事件就绪，
 * 或者下个时间事件到达（如果有的话）。
 *
 * If flags is 0, the function does nothing and returns.
 * 如果 flags 为 0 ，那么函数不作动作，直接返回。
 *
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * 如果 flags 包含 AE_ALL_EVENTS ，所有类型的事件都会被处理。
 *
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * 如果 flags 包含 AE_FILE_EVENTS ，那么处理文件事件。
 *
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * 如果 flags 包含 AE_TIME_EVENTS ，那么处理时间事件。
 *
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * the events that's possible to process without to wait are processed.
 * 如果 flags 包含 AE_DONT_WAIT ，
 * 那么函数在处理完所有不许阻塞的事件之后，即刻返回。
 *
 * The function returns the number of events processed.
 * 函数的返回值为已处理事件的数量
 */
int aeProcessEvents(aeEventLoop *eventLoop,
                    int flags // AE_ALL_EVENTS | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP
                    )
{
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv,
                       *tvp;

        // 获取最小的时间事件
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            // 目前只有 serverCron 一个定时任务
            shortest = aeSearchNearestTimer(eventLoop);

        // 这里只是根据找到的最小要执行的定时任务，看还要过多久再执行，如果大于0，那 aeApiPoll() 方法就可以阻塞那么久
        if (shortest) {
            // 如果时间事件存在的话
            // 那么根据最近可执行时间事件和现在时间的时间差来决定文件事件的阻塞时间
            long now_sec, now_ms;

            // 计算距今最近的时间事件还要多久才能达到
            // 并将该时间距保存在 tv 结构中
            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;

            /* How many milliseconds we need to wait for the next
             * time event to fire? */
            long long ms =
                (shortest->when_sec - now_sec)*1000 +
                shortest->when_ms - now_ms;

            if (ms > 0) {
                tvp->tv_sec = ms/1000;
                tvp->tv_usec = (ms % 1000)*1000;
            } else {
                // 时间差小于 0 ，说明事件已经可以执行了，将秒和毫秒设为 0 （不阻塞）
                tvp->tv_sec = 0;
                tvp->tv_usec = 0;
            }
        } else {

            // 执行到这一步，说明没有时间事件
            // 那么根据 AE_DONT_WAIT 是否设置来决定是否阻塞，以及阻塞的时间长度

            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
            if (flags & AE_DONT_WAIT) {
                // 设置文件事件不阻塞
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                // 文件事件可以阻塞直到有事件到达为止
                tvp = NULL; /* wait forever */
            }
        }

        // 执行到这一步，说明没有时间事件，那么根据 AE_DONT_WAIT 是否设置来决定是否阻塞，以及阻塞的时间长
        if (eventLoop->flags & AE_DONT_WAIT) {
            // 设置文件事件不阻塞
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        }

        // 如果不 AE_DONT_WAIT，文件事件可以阻塞直到有事件到达为止 tvp 默认是 NULL

        if (eventLoop->beforesleep != NULL && flags & AE_CALL_BEFORE_SLEEP)
            eventLoop->beforesleep(eventLoop);

        /* Call the multiplexing API, will return only on timeout or when
         * some event fires. */
        // 处理事件，其实就是网络事件了，阻塞时间由 tvp 决定
        numevents = aeApiPoll(eventLoop, tvp);

        /* After sleep callback. */
        if (eventLoop->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP)
            eventLoop->aftersleep(eventLoop);

        for (j = 0; j < numevents; j++) {
            // 从已就绪数组中获取事件
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
            int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            int fired = 0; /* Number of events fired for current fd. */

            /* Normally we execute the readable event first, and the writable
             * event later. This is useful as sometimes we may be able
             * to serve the reply of a query immediately after processing the
             * query.
             *
             * However if AE_BARRIER is set in the mask, our application is
             * asking us to do the reverse: never fire the writable event
             * after the readable. In such a case, we invert the calls.
             * This is useful when, for instance, we want to do things
             * in the beforeSleep() hook, like fsyncing a file to disk,
             * before replying to a client. */
            int invert = fe->mask & AE_BARRIER;

            /* Note the "fe->mask & mask & ..." code: maybe an already
             * processed event removed an element that fired and we still
             * didn't processed, so we check if the event is still valid.
             *
             * Fire the readable event if the call sequence is not
             * inverted. */
            if (!invert && fe->mask & mask & AE_READABLE) {
                // rfired 确保读/写事件只能执行其中一个
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                fired++;
                fe = &eventLoop->events[fd]; /* Refresh in case of resize. */
            }

            /* Fire the writable event. */
            // 触发写事件
            if (fe->mask & mask & AE_WRITABLE) {
                if (!fired || fe->wfileProc != fe->rfileProc) {
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            /* If we have to invert the call, fire the readable event now
             * after the writable one. */
            if (invert) {
                fe = &eventLoop->events[fd]; /* Refresh in case of resize. */
                if ((fe->mask & mask & AE_READABLE) &&
                    (!fired || fe->wfileProc != fe->rfileProc))
                {
                    fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            processed++;
        }
    }
    /* Check time events */
    // 执行时间事件
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

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

// 事件处理器的主循环
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    //如果eventLoop中的stop标志位不为1，就循环处理
    while (!eventLoop->stop) {
        // 如果有需要在事件处理前执行的函数，那么运行它
        aeProcessEvents(eventLoop, AE_ALL_EVENTS|
                                   AE_CALL_BEFORE_SLEEP|
                                   AE_CALL_AFTER_SLEEP);
    }
}

// 返回所使用的多路复用库的名称
char *aeGetApiName(void) {
    return aeApiName();
}

// 设置处理事件前需要被执行的函数
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}