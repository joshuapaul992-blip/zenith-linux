#pragma once
#include <exec/io.h>
struct timeval;
struct aros_timeval { ULONG tv_secs, tv_micro; };
struct timerequest { struct IORequest tr_node; struct aros_timeval tr_time; };
#define TR_ADDREQUEST 9
