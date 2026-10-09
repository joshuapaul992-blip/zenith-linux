#pragma once
#include <exec/ports.h>
struct IORequest { struct Message io_Message; void *io_Device, *io_Unit; UWORD io_Command; UBYTE io_Flags; BYTE io_Error; };
