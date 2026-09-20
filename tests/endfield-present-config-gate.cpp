#define WIN32_LEAN_AND_MEAN
#include "../src/present-config-gate.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

static bool Run(bool blocking)
{
    CRITICAL_SECTION lock; InitializeCriticalSection(&lock);
    HANDLE held=CreateEventW(nullptr,TRUE,FALSE,nullptr), input=CreateEventW(nullptr,TRUE,FALSE,nullptr), idle=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<bool> timeout{false}; std::atomic<unsigned> reloads{0};
    std::thread renderer([&]{EnterCriticalSection(&lock);SetEvent(held);assert(WaitForSingleObject(idle,3000)==WAIT_OBJECT_0);LeaveCriticalSection(&lock);});
    assert(WaitForSingleObject(held,3000)==WAIT_OBJECT_0);
    std::thread worker([&]{timeout=WaitForSingleObject(input,100)==WAIT_TIMEOUT;SetEvent(idle);});
    std::thread fg([&]{if(blocking){EnterCriticalSection(&lock);++reloads;LeaveCriticalSection(&lock);}else TryReloadPresentConfig(lock,[&]{++reloads;});SetEvent(input);});
    fg.join();worker.join();renderer.join();
    if(!blocking){assert(reloads==0);assert(TryReloadPresentConfig(lock,[&]{++reloads;}));assert(reloads==1);}
    CloseHandle(held);CloseHandle(input);CloseHandle(idle);DeleteCriticalSection(&lock);return timeout.load();
}

int main()
{
    assert(Run(true));
    for(int i=0;i<20;++i)assert(!Run(false));
    puts("nonblocking present-config gate test passed: 20/20");
}
