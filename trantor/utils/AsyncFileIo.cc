#include <trantor/utils/AsyncFileIo.h>
#include <trantor/net/inner/poller/EpollPoller.h>
#include <trantor/utils/Logger.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>


off_t get_file_size(int fd) {
    struct stat st;

    if(fstat(fd, &st) < 0) {
        perror("fstat");
        return -1;
    }

    if (S_ISBLK(st.st_mode)) {
        unsigned long long bytes;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
        perror("ioctl");
            return -1;
            
    }
        return bytes;
        
    } else if (S_ISREG(st.st_mode))
        return st.st_size;

    return -1;
    
}

namespace trantor
{
void asyncFileRead(EventLoop* loop, const std::string& path, std::function<void(MsgBuffer&&)> callback)
{
    auto poller = dynamic_cast<trantor::EpollPoller*>(loop->getPoller());
    int fd = open(path.c_str(), O_RDONLY);
    auto size = get_file_size(fd);
    LOG_TRACE << "File size: " << size << " bytes";
    poller->submitReadRequst(fd, size, callback, [](){});
}
}
