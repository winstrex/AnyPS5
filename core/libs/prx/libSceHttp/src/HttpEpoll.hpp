#ifndef CORE_LIBS_PRX_LIBSCEHTTP_SRC_HTTPEPOLL_HPP
#define CORE_LIBS_PRX_LIBSCEHTTP_SRC_HTTPEPOLL_HPP

#include <atomic>

struct HttpEpoll {
    bool aborted = false;
    std::atomic<int> waiters{0};
};

#endif
