#pragma once
#include <exec/lists.h>
struct SignalSemaphore { struct Node ss_Link; WORD ss_NestCount; struct MinList ss_WaitQueue; };
