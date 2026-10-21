# Makefile for lxinit.exe -- native NT subsystem binary (x64, NT 5.2).
# Use with NMAKE from an x64 MSVC/DDK command prompt:  nmake
# Clean:  nmake clean

OUT     = lxinit.exe
OBJ     = lxinit.obj
SRC     = lxinit.c

CC      = cl
LINK    = link
CFLAGS  = /c /GS- /Gs1000000 /O2 /Zl /W4 /nologo
LFLAGS  = /nologo /SUBSYSTEM:NATIVE,5.02 /ENTRY:NtProcessStartup \
          /NODEFAULTLIB /MACHINE:X64
LIBS    = ntdll.lib

all: $(OUT)

$(OBJ): $(SRC)
	$(CC) $(CFLAGS) $(SRC)

$(OUT): $(OBJ)
	$(LINK) $(LFLAGS) $(OBJ) $(LIBS) /OUT:$(OUT)

clean:
	-del /q $(OBJ) $(OUT) 2>nul

.PHONY: all clean
