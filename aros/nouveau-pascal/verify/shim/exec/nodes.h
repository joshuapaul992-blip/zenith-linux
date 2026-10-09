#pragma once
#include <exec/types.h>
struct Node { struct Node *ln_Succ, *ln_Pred; UBYTE ln_Type; BYTE ln_Pri; char *ln_Name; };
struct MinNode { struct MinNode *mln_Succ, *mln_Pred; };
#define NT_MSGPORT 4
#define NT_REPLYMSG 7
#define NT_INTERRUPT 2
