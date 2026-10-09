#pragma once
#include <exec/semaphores.h>
#include <exec/ports.h>
struct Task;
void Disable(void); void Enable(void); void Forbid(void); void Permit(void);
void InitSemaphore(struct SignalSemaphore *); void ObtainSemaphore(struct SignalSemaphore *);
void ReleaseSemaphore(struct SignalSemaphore *); ULONG AttemptSemaphore(struct SignalSemaphore *);
struct Task *FindTask(const char *); void Signal(struct Task *, ULONG); ULONG Wait(ULONG);
APTR AllocVec(IPTR, ULONG); void FreeVec(APTR);
#define SIGB_SINGLE 4
#define SIGF_SINGLE (1UL << SIGB_SINGLE)
