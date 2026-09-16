// Copyright 2016, Tao An.  All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Tao An

#include "AresResolver.h"
#include <trantor/net/Channel.h>
#include <ares.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <netdb.h>
#include <arpa/inet.h>  // inet_ntop
#include <netinet/in.h>
#include <poll.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <limits>

using namespace trantor;
using namespace std::placeholders;

namespace
{
double getSeconds(struct timeval* tv)
{
    if (tv)
        return double(tv->tv_sec) + double(tv->tv_usec) / 1000000.0;
    else
        return -1.0;
}

const char* getSocketType(int type)
{
    if (type == SOCK_DGRAM)
        return "UDP";
    else if (type == SOCK_STREAM)
        return "TCP";
    else
        return "Unknown";
}

}  // namespace

bool Resolver::isCAresUsed()
{
    return true;
}

AresResolver::LibraryInitializer::LibraryInitializer()
{
    ares_library_init(ARES_LIB_INIT_ALL);

    hints_ = new ares_addrinfo_hints;
    hints_->ai_flags = 0;
    hints_->ai_family = AF_INET;
    hints_->ai_socktype = 0;
    hints_->ai_protocol = 0;
}
AresResolver::LibraryInitializer::~LibraryInitializer()
{
    ares_library_cleanup();
    delete hints_;
}

AresResolver::LibraryInitializer AresResolver::libraryInitializer_;

std::shared_ptr<Resolver> Resolver::newResolver(trantor::EventLoop* loop,
                                                size_t timeout)
{
    return std::make_shared<AresResolver>(loop, timeout);
}

AresResolver::AresResolver(EventLoop* loop, size_t timeout)
    : loop_(loop), timeout_(timeout)
{
    if (!loop)
    {
        loop_ = getLoop();
    }
    loopValid_ = std::make_shared<bool>(true);
    loop_->runOnQuit([loopValid = loopValid_]() { *loopValid = false; });
}
void AresResolver::init()
{
    if (!ctx_)
    {
        struct ares_options options{};
        int optmask = ARES_OPT_FLAGS;
        // Keep the resolver socket warm, but retain c-ares' normal response
        // validation and retry behavior.  NOCHECKRESP is intended for DNS
        // diagnostic clients: it prevents SERVFAIL/REFUSED/NOTIMP responses
        // from being requeued with backoff.  IGNTC likewise suppresses the
        // normal TCP fallback for truncated UDP replies.
        options.flags = ARES_FLAG_STAYOPEN;
        optmask |= ARES_OPT_SOCK_STATE_CB;
        options.sock_state_cb = &AresResolver::ares_sock_statecallback_;
        options.sock_state_cb_data = this;
        // Resolver::newResolver() documents this value as the DNS timeout in
        // seconds.  Let c-ares use it as the initial per-server attempt
        // timeout; c-ares applies its own retry backoff after that attempt.
        // A value of zero leaves the c-ares/system default untouched.
        if (timeout_ > 0)
        {
            optmask |= ARES_OPT_TIMEOUT;
            options.timeout =
                timeout_ > static_cast<size_t>(std::numeric_limits<int>::max())
                    ? std::numeric_limits<int>::max()
                    : static_cast<int>(timeout_);
        }
        // optmask |= ARES_OPT_LOOKUPS;
        // options.lookups = lookups;

        int status = ares_init_options(&ctx_, &options, optmask);
        if (status != ARES_SUCCESS)
        {
            assert(0);
        }
        ares_set_socket_callback(ctx_,
                                 &AresResolver::ares_sock_createcallback_,
                                 this);
    }
}
AresResolver::~AresResolver()
{
    destroying_ = true;
    if (ctx_)
        ares_destroy(ctx_);
}

void AresResolver::resolveInLoop(const std::string& hostname,
                                 const ResolverResultsCallback& cb)
{
    loop_->assertInLoopThread();
#ifdef _WIN32
    if (hostname == "localhost")
    {
        const static std::vector<trantor::InetAddress> localhost_{
            trantor::InetAddress{"127.0.0.1", 0}};
        cb(localhost_);
        return;
    }
#endif
    init();
    pendingQueries_.push_back(PendingQuery{hostname, cb});
    dispatchPending();
}

void AresResolver::dispatchPending()
{
    loop_->assertInLoopThread();
    while (activeQueries_ < kMaxActiveQueries && !pendingQueries_.empty())
    {
        PendingQuery query = std::move(pendingQueries_.front());
        pendingQueries_.pop_front();
        QueryData* queryData =
            new QueryData(this, query.callback_, query.hostname_);
        ++activeQueries_;
        ares_getaddrinfo(ctx_,
                         query.hostname_.c_str(),
                         NULL,
                         libraryInitializer_.hints_,
                         &AresResolver::ares_hostcallback_,
                         queryData);
    }
    updateTimer();
}

void AresResolver::scheduleDispatch()
{
    if (destroying_ || dispatchScheduled_ || pendingQueries_.empty())
        return;
    dispatchScheduled_ = true;
    loop_->queueInLoop([thisPtr = shared_from_this()]() {
        thisPtr->dispatchScheduled_ = false;
        thisPtr->dispatchPending();
    });
}

bool AresResolver::retryTransientQuery(int status, QueryData* query)
{
    if (status != ARES_ETIMEOUT && status != ARES_ESERVFAIL &&
        status != ARES_ECONNREFUSED && status != ARES_EREFUSED)
        return false;
    if (destroying_ || timeout_ == 0)
        return false;

    const auto now = std::chrono::steady_clock::now();
    const auto deadline = query->started_ + std::chrono::seconds(timeout_);
    if (now + query->retryDelay_ >= deadline)
        return false;

    const auto delay = query->retryDelay_;
    query->retryDelay_ = std::min(query->retryDelay_ * 2,
                                  std::chrono::milliseconds(2000));
    LOG_TRACE << "Transient DNS failure for " << query->hostname_ << ": "
              << ares_strerror(status) << "; retrying in " << delay.count()
              << "ms";
    loop_->runAfter(
        std::chrono::duration<double>(delay),
        [thisPtr = shared_from_this(), query]() {
            ares_getaddrinfo(thisPtr->ctx_,
                             query->hostname_.c_str(),
                             NULL,
                             libraryInitializer_.hints_,
                             &AresResolver::ares_hostcallback_,
                             query);
            thisPtr->updateTimer();
        });
    return true;
}

void AresResolver::onSocketEvent(int sockfd)
{
    const auto it = channels_.find(sockfd);
    if (it == channels_.end())
        return;

    const int revents = it->second->revents();
    const bool readable =
        (revents & (POLLIN | POLLPRI | POLLHUP | POLLERR | POLLNVAL)) != 0;
    const bool writable = (revents & POLLOUT) != 0;
    ares_process_fd(ctx_,
                    readable ? sockfd : ARES_SOCKET_BAD,
                    writable ? sockfd : ARES_SOCKET_BAD);
    updateTimer();
}

void AresResolver::onTimer()
{
    if (!timerActive_)
        return;
    timerActive_ = false;
    timerId_ = 0;
    ares_process_fd(ctx_, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
    updateTimer();
}

void AresResolver::updateTimer()
{
    struct timeval tv;
    struct timeval* tvp = ares_timeout(ctx_, NULL, &tv);
    const double timeout = getSeconds(tvp);

    if (timeout < 0)
    {
        if (timerActive_)
            loop_->invalidateTimer(timerId_);
        timerActive_ = false;
        timerId_ = 0;
        return;
    }

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(timeout));

    // Keeping an already earlier timer is safe: it will merely ask c-ares for
    // its next deadline again.  Only replace a timer when c-ares now needs to
    // be serviced sooner.  This avoids leaving stale late timers without
    // creating one canceled timer per DNS packet.
    if (timerActive_ && timerDeadline_ <= deadline)
        return;

    if (timerActive_)
        loop_->invalidateTimer(timerId_);
    timerDeadline_ = deadline;
    timerId_ = loop_->runAfter(
        timeout, std::bind(&AresResolver::onTimer, shared_from_this()));
    timerActive_ = true;
}

void AresResolver::onQueryResult(int status,
                                 int timeouts,
                                 struct ares_addrinfo* result,
                                 const std::string& hostname,
                                 const ResolverResultsCallback& callback)
{
    assert(activeQueries_ > 0);
    --activeQueries_;
    if (status == ARES_SUCCESS)
        LOG_TRACE << "DNS result for " << hostname << ": success";
    else
        LOG_WARN << "DNS failure for " << hostname << ": "
                 << ares_strerror(status) << " (status=" << status
                 << ", timeouts=" << timeouts << ')';
    auto inets_ptr = std::make_shared<std::vector<trantor::InetAddress>>();
    if (result)
    {
        auto pptr = (struct ares_addrinfo_node*)result->nodes;
        for (; pptr != NULL; pptr = pptr->ai_next)
        {
            trantor::InetAddress inet;
            if (pptr->ai_family == AF_INET)
            {
                struct sockaddr_in* addr4 = (struct sockaddr_in*)pptr->ai_addr;
                inets_ptr->emplace_back(trantor::InetAddress{*addr4});
            }
            else if (pptr->ai_family == AF_INET6)
            {
                struct sockaddr_in6* addr6 =
                    (struct sockaddr_in6*)pptr->ai_addr;
                inets_ptr->emplace_back(trantor::InetAddress{*addr6});
            }
            else
            {
                // TODO: Handle unknown family?
            }
        }
        ares_freeaddrinfo(result);
    }
    const bool resolved = !inets_ptr->empty();
    if (!resolved)
    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        InetAddress inet(addr);
        inets_ptr->emplace_back(std::move(inet));
    }
    // c-ares already retries transient resolver failures.  Do not turn its
    // final timeout/SERVFAIL/etc. into a cached 0.0.0.0 entry: callers still
    // receive the API's unspecified-address sentinel, but a later lookup is
    // allowed to recover.
    if (status == ARES_SUCCESS && resolved)
    {
        std::lock_guard<std::mutex> lock(globalMutex());
        auto& addrItem = globalCache()[hostname];
        addrItem.first = inets_ptr;
        addrItem.second = trantor::Date::date();
    }
    scheduleDispatch();
    callback(*inets_ptr);
}

void AresResolver::onSockCreate(int sockfd, int type)
{
    (void)type;
    loop_->assertInLoopThread();
    assert(channels_.find(sockfd) == channels_.end());
    Channel* channel = new Channel(loop_, sockfd);
    channel->setEventCallback(
        std::bind(&AresResolver::onSocketEvent, this, sockfd));
    channels_[sockfd].reset(channel);
}

void AresResolver::onSockStateChange(int sockfd, bool read, bool write)
{
    loop_->assertInLoopThread();
    ChannelList::iterator it = channels_.find(sockfd);
    if (it == channels_.end())
        return;

    if (read || write)
    {
        if (read && !it->second->isReading())
            it->second->enableReading();
        else if (!read && it->second->isReading())
            it->second->disableReading();

        if (write && !it->second->isWriting())
            it->second->enableWriting();
        else if (!write && it->second->isWriting())
            it->second->disableWriting();
    }
    else if (*loopValid_)
    {
        // c-ares can report the socket closed from inside this Channel's own
        // event callback.  Remove it from the poller and map immediately so
        // the descriptor can be reused, but defer deleting the Channel until
        // handleEventSafely() has returned.
        it->second->disableAll();
        it->second->remove();
        Channel* retired = it->second.release();
        channels_.erase(it);
        loop_->queueInLoop([retired]() { delete retired; });
    }
}

void AresResolver::ares_hostcallback_(void* data,
                                      int status,
                                      int timeouts,
                                      struct ares_addrinfo* hostent)
{
    QueryData* query = static_cast<QueryData*>(data);

    if (query->owner_->retryTransientQuery(status, query))
    {
        if (hostent)
            ares_freeaddrinfo(hostent);
        return;
    }

    query->owner_->onQueryResult(status,
                                 timeouts,
                                 hostent,
                                 query->hostname_,
                                 query->callback_);
    delete query;
}

#ifdef _WIN32
int AresResolver::ares_sock_createcallback_(SOCKET sockfd, int type, void* data)
#else
int AresResolver::ares_sock_createcallback_(int sockfd, int type, void* data)
#endif
{
    LOG_TRACE << "sockfd=" << sockfd << " type=" << getSocketType(type);
    static_cast<AresResolver*>(data)->onSockCreate(sockfd, type);
    return 0;
}

void AresResolver::ares_sock_statecallback_(void* data,
#ifdef _WIN32
                                            SOCKET sockfd,
#else
                                            int sockfd,
#endif
                                            int read,
                                            int write)
{
    LOG_TRACE << "sockfd=" << sockfd << " read=" << read << " write=" << write;
    static_cast<AresResolver*>(data)->onSockStateChange(sockfd, read, write);
}
