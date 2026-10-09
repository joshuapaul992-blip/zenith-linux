#pragma once
#include <exec/nodes.h>
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; UBYTE lh_Type, l_pad; };
struct MinList { struct MinNode *mlh_Head, *mlh_Tail, *mlh_TailPred; };
#define NEWLIST(l) do { (l)->mlh_TailPred = (void *)(l); (l)->mlh_Tail = 0; (l)->mlh_Head = (void *)&(l)->mlh_Tail; } while (0)
#define ADDTAIL(l, n) ((void)(l), (void)(n))
#define REMOVE(n) ((void)(n))
#define ForeachNodeSafe(l, n, nn) for ((n) = (void *)(l)->mlh_Head; ((nn) = (void *)((struct MinNode *)(n))->mln_Succ); (n) = (nn))
