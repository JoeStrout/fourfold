#include "base/fd.h"

#include <unistd.h>

namespace fourfold {

void Fd::reset(int fd) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
}

}  // namespace fourfold
