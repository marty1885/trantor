/**
 *
 *  EpollPoller.cc
 *  An Tao
 *
 *  Public header file in trantor lib.
 *
 *  Copyright 2018, An Tao.  All rights reserved.
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the License file.
 *
 *
 */

#include <trantor/utils/Logger.h>
#include "Channel.h"
#include "EpollPoller.h"
#ifdef __linux__
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <assert.h>
#include <strings.h>
#include <iostream>
#elif defined _WIN32
#include "Wepoll.h"
#include <assert.h>
#include <iostream>
#include <winsock2.h>
#include <fcntl.h>
#define EPOLL_CLOEXEC _O_NOINHERIT
#endif
namespace trantor
{
#if defined __linux__ || defined _WIN32

#if defined __linux__
static_assert(EPOLLIN == POLLIN, "EPOLLIN != POLLIN");
static_assert(EPOLLPRI == POLLPRI, "EPOLLPRI != POLLPRI");
static_assert(EPOLLOUT == POLLOUT, "EPOLLOUT != POLLOUT");
static_assert(EPOLLRDHUP == POLLRDHUP, "EPOLLRDHUP != POLLRDHUP");
static_assert(EPOLLERR == POLLERR, "EPOLLERR != POLLERR");
static_assert(EPOLLHUP == POLLHUP, "EPOLLHUP != POLLHUP");
#endif

namespace
{
const int kNew = -1;
const int kAdded = 1;
const int kDeleted = 2;
}  // namespace

EpollPoller::EpollPoller(EventLoop *loop)
    : Poller(loop),
#ifdef _WIN32
      // wepoll does not suppor flags
      epollfd_(::epoll_create1(0)),
#else
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
#endif
      events_(kInitEventListSize)
{
#ifdef __linux__
    if (io_uring_queue_init(8, &ring, 0) != 0)
    {
        LOG_DEBUG << "Failed to initialize io_uring. code " << errno;
        haveIoUring_ = false;
    }
    else
    {
        haveIoUring_ = true;
    }
#endif
}
EpollPoller::~EpollPoller()
{
#ifdef _WIN32
    epoll_close(epollfd_);
#else
    close(epollfd_);
#endif
}
#ifdef _WIN32
void EpollPoller::postEvent(uint64_t event)
{
    epoll_post_signal(epollfd_, event);
}
#endif
void EpollPoller::poll(int timeoutMs, ChannelList *activeChannels)
{
    // FIXME: epoll_wait blocks to timeoutMs. Which does not unblock when
    // io_uring is ready. Causing io_uring to have massive delay
    int numEvents = ::epoll_wait(epollfd_,
                                 &*events_.begin(),
                                 static_cast<int>(events_.size()),
                                 timeoutMs);
    int savedErrno = errno;
    // Timestamp now(Timestamp::now());
    if (numEvents > 0)
    {
        // LOG_TRACE << numEvents << " events happended";
        fillActiveChannels(numEvents, activeChannels);
        if (static_cast<size_t>(numEvents) == events_.size())
        {
            events_.resize(events_.size() * 2);
        }
    }
    else if (numEvents == 0)
    {
        // std::cout << "nothing happended" << std::endl;
    }
    else
    {
        // error happens, log uncommon ones
        if (savedErrno != EINTR)
        {
            errno = savedErrno;
            LOG_SYSERR << "EPollEpollPoller::poll()";
        }
    }

    pollIoUring();

    return;
}
void EpollPoller::fillActiveChannels(int numEvents,
                                     ChannelList *activeChannels) const
{
    assert(static_cast<size_t>(numEvents) <= events_.size());
    for (int i = 0; i < numEvents; ++i)
    {
#ifdef _WIN32
        if (events_[i].events == EPOLLEVENT)
        {
            eventCallback_(events_[i].data.u64);
            continue;
        }
#endif
        Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
#ifndef NDEBUG
        int fd = channel->fd();
        ChannelMap::const_iterator it = channels_.find(fd);
        assert(it != channels_.end());
        assert(it->second == channel);
#endif
        channel->setRevents(events_[i].events);
        activeChannels->push_back(channel);
    }
    // LOG_TRACE<<"active Channels num:"<<activeChannels->size();
}
void EpollPoller::updateChannel(Channel *channel)
{
    assertInLoopThread();
    assert(channel->fd() >= 0);

    const int index = channel->index();
    // LOG_TRACE << "fd = " << channel->fd()
    //  << " events = " << channel->events() << " index = " << index;
    if (index == kNew || index == kDeleted)
    {
// a new one, add with EPOLL_CTL_ADD
#ifndef NDEBUG
        int fd = channel->fd();
        if (index == kNew)
        {
            assert(channels_.find(fd) == channels_.end());
            channels_[fd] = channel;
        }
        else
        {  // index == kDeleted
            assert(channels_.find(fd) != channels_.end());
            assert(channels_[fd] == channel);
        }
#endif
        channel->setIndex(kAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else
    {
// update existing one with EPOLL_CTL_MOD/DEL
#ifndef NDEBUG
        int fd = channel->fd();
        (void)fd;
        assert(channels_.find(fd) != channels_.end());
        assert(channels_[fd] == channel);
#endif
        assert(index == kAdded);
        if (channel->isNoneEvent())
        {
            update(EPOLL_CTL_DEL, channel);
            channel->setIndex(kDeleted);
        }
        else
        {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}
void EpollPoller::removeChannel(Channel *channel)
{
    EpollPoller::assertInLoopThread();
#ifndef NDEBUG
    int fd = channel->fd();
    assert(channels_.find(fd) != channels_.end());
    assert(channels_[fd] == channel);
    size_t n = channels_.erase(fd);
    (void)n;
    assert(n == 1);
#endif
    assert(channel->isNoneEvent());
    int index = channel->index();
    assert(index == kAdded || index == kDeleted);
    if (index == kAdded)
    {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->setIndex(kNew);
}
void EpollPoller::update(int operation, Channel *channel)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = channel->events();
    event.data.ptr = channel;
    int fd = channel->fd();
    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if (operation == EPOLL_CTL_DEL)
        {
            // LOG_SYSERR << "epoll_ctl op =" << operationToString(operation) <<
            // " fd =" << fd;
        }
        else
        {
            //  LOG_SYSFATAL << "epoll_ctl op =" << operationToString(operation)
            //  << " fd =" << fd;
        }
    }
}

void EpollPoller::submitReadRequst(
    int fd,
    size_t size,
    std::function<void(MsgBuffer &&buffer)> callback,
    std::function<void()> errCallback)
{
    constexpr size_t blockSize = 1024;
    size_t remainingBytes = size;
    const size_t numBlocks = size / blockSize + (size % blockSize ? 1 : 0);
    IoData *ioData = new IoData;
    ioData->callback = std::move(callback);
    ioData->errCallback = std::move(errCallback);
    ioData->iovecs.resize(numBlocks);
    ioData->opType = OperationType::Read;

    for (size_t i = 0; remainingBytes; i++)
    {
        size_t bytesToRead = std::min(remainingBytes, blockSize);

        ioData->iovecs[i].iov_len = bytesToRead;
        void *buf;
        if (posix_memalign(&buf, blockSize, blockSize))
        {
            LOG_TRACE << "Failed to allocate memory for io_uring";
            abort();
        }
        ioData->iovecs[i].iov_base = buf;
        remainingBytes -= bytesToRead;
    }

    LOG_TRACE << "Submiting IO to io_uring, fd = " << fd;

    io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_readv(sqe, fd, ioData->iovecs.data(), numBlocks, 0);
    io_uring_sqe_set_data(sqe, ioData);
    io_uring_submit(&ring);
}

void EpollPoller::pollIoUring()
{
#ifdef __linux__
    if (haveIoUring_ == false)
        return;
    unsigned count;
    do
    {
        io_uring_cqe *cqes;
        count = io_uring_peek_batch_cqe(&ring,
                                        &cqes,
                                        4);  // XXX: is 4 the correct value?
        for (unsigned i = 0; i < count; i++)
        {
            LOG_TRACE << "io_uring command finished";
            auto cqe = cqes + i;
            IoData *ioData = (IoData *)io_uring_cqe_get_data(cqe);

            if (cqe->res < 0)
            {
                LOG_ERROR << "io_uring operation failed: "
                          << strerror(-cqe->res);
                ioData->errCallback();
            }
            else if (ioData->opType == OperationType::Read)
            {
                MsgBuffer buf;
                buf.ensureWritableBytes(ioData->iovecs.size() * 1024);
                for (const auto &vec : ioData->iovecs)
                {
                    buf.append((const char *)vec.iov_base, vec.iov_len);
                    free(vec.iov_base);
                }
                if (ioData->callback)
                    ioData->callback(std::move(buf));
            }
            io_uring_cqe_seen(&ring, &cqes[i]);
        }
    } while (count != 0);
#endif
}
#else
EpollPoller::EpollPoller(EventLoop *loop) : Poller(loop)
{
    assert(false);
}
EpollPoller::~EpollPoller()
{
}
void EpollPoller::poll(int, ChannelList *)
{
}
void EpollPoller::updateChannel(Channel *)
{
}
void EpollPoller::removeChannel(Channel *)
{
}

#endif
}  // namespace trantor
