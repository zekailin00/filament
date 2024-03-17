#pragma once

#include "tracy/Tracy.hpp"
#include "tracy/TracyC.h"
#include <stack>
#include <map>

#define SYSTRACE_ENABLE() 

#define SYSTRACE_DISABLE()

#define SYSTRACE_CONTEXT()

#define SYSTRACE_NAME(name) ZoneScopedNS(name, 5)

#define SYSTRACE_TEXT(name) TracyMessage(name, strlen(name))

#define SYSTRACE_FRAME_ID(frame) do {                       \
    FrameMark;                                              \
    std::string frameID = "Frame " + std::to_string(frame); \
    TracyMessage(frameID.c_str(),frameID.length());         \
} while(0)

#define SYSTRACE_NAME_BEGIN(name) do {          \
    TracyCZone(ctx, 1);                         \
    tracyCtxStack.push(ctx);                    \
} while(0)

#define SYSTRACE_NAME_END() do {                \
    TracyCZoneCtx ctx = tracyCtxStack.top();    \
    tracyCtxStack.pop();                        \
    TracyCZoneEnd(ctx);                         \
} while(0)

#define SYSTRACE_CALL() SYSTRACE_NAME(__FUNCTION__)

#define SYSTRACE_ASYNC_BEGIN(name, cookie)
#define SYSTRACE_ASYNC_END(name, cookie)


#define SYSTRACE_VALUE32(name, val)  TracyPlot(name, static_cast<int64_t>(val))
#define SYSTRACE_VALUE64(name, val)  TracyPlot(name, static_cast<int64_t>(val))


extern std::stack<TracyCZoneCtx> tracyCtxStack;
