#include <trantor/utils/AsyncFileIo.h>
#include "trantor/net/EventLoop.h"
#include <trantor/utils/Logger.h>

using namespace trantor;

int main()
{
    trantor::EventLoop loop;
    Logger::setLogLevel(Logger::LogLevel::kTrace);
    trantor::asyncFileRead(&loop, "compile_commands.json", [](MsgBuffer&& buffer){
        LOG_INFO << "File read done. Size = " << buffer.readableBytes();
    });

    loop.loop();
}
