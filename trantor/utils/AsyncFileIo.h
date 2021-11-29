#pragma once

#include <trantor/utils/MsgBuffer.h>
#include <trantor/net/EventLoop.h>

namespace trantor
{
void asyncFileRead(EventLoop* loop, const std::string& path, std::function<void(MsgBuffer&&)> callback);
}
