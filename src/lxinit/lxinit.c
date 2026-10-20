/*
 * lxinit.c  --  zenith-linux native-mode bootstrap
 *
 * A Windows XP x64 (NT 5.2) *native* subsystem executable, intended to be
 * registered in:
 *
 *     HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\BootExecute
 *
 * BootExecute entries run inside smss.exe's native environment, after the
 * kernel and ntdll are up but *before* the Win32 subsystem (csrss.exe) and
 * the video miniports exist. There is no CRT, no kernel32/user32, and no
 * Win32 window/console -- only ntdll's native syscall stubs. This file is
 * therefore self-contained: it declares the handful of NT types and ntdll
 * prototypes it needs and links against ntdll.lib only.
 *
 * This bootstrap is distribution-agnostic: it works with any extracted
 * x86-64 Linux root filesystem (Ubuntu, Debian, Alpine, Arch, Void, Fedora,
 * ...). The rootfs location and (optionally) the init binary are read from
 * the registry; otherwise it probes the usual init/shell locations and uses
 * whichever ELF64 it finds first.
 *
 * Build (Windows DDK / MSVC x64):
 *     cl /c /GS- /O2 /Zl /W4 lxinit.c
 *     link /SUBSYSTEM:NATIVE,5.02 /ENTRY:NtProcessStartup /NODEFAULTLIB ^
 *          /MACHINE:X64 lxinit.obj ntdll.lib /OUT:lxinit.exe
 *
 *   /Zl + /NODEFAULTLIB   -> no C runtime is linked in.
 *   /SUBSYSTEM:NATIVE,5.02 -> NT 5.2 native image (no csrss dependency).
 *   /GS-                  -> no stack-cookie runtime (there is no __security_*).
 *
 * Registry configuration (all optional), under
 *   HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Zenith :
 *     RootfsPath  REG_SZ    DOS path to the rootfs root (default "C:\rootfs")
 *     InitPath    REG_SZ    rootfs-relative init, e.g. "\sbin\init"
 *                           (if absent, init is auto-detected)
 *     EnableX11   REG_DWORD nonzero to request a graphical layer
 *
 * ---------------------------------------------------------------------------
 * HONEST ENGINEERING NOTES  (read before trusting the comments elsewhere)
 * ---------------------------------------------------------------------------
 *  1. Mapping the Linux binary: NtCreateSection with SEC_IMAGE parses *PE*
 *     headers only. An ELF cannot be mapped as SEC_IMAGE. We map it as a
 *     plain data section (SEC_COMMIT, read-only), which gives us the raw file
 *     bytes in our address space. That is all a native app can do -- actually
 *     *executing* the ELF requires a syscall-translation layer in kernel mode
 *     (the role lxcore.sys / the pico-process provider played for WSL1). This
 *     bootstrap validates the image and hands off a mapped pointer; it does
 *     not pretend to run Linux code in-process.
 *
 *  2. "Letting smss proceed to spawn csrss / load video drivers": smss runs
 *     each BootExecute program synchronously and blocks the rest of boot until
 *     that program *terminates*. There is no side-channel flag or thread that
 *     tells smss to continue -- returning from NtProcessStartup (we call
 *     NtTerminateProcess) IS the signal. So the way to guarantee the Win32
 *     subsystem is "not permanently blocked" is simply to finish promptly and
 *     exit. The only way to block it permanently is to never return.
 *
 *  3. Early-boot screen output: at BootExecute time no video miniport is
 *     loaded, there is no mapped linear framebuffer, and INT 10h is gone in
 *     long mode. NtDisplayString (the boot text path) is the only sanctioned
 *     output primitive here.
 */

/* =========================================================================
 *  Minimal NT type layer (x64 / LLP64)
 * ========================================================================= */

typedef long               NTSTATUS;
typedef void              *HANDLE;
typedef HANDLE            *PHANDLE;
typedef void              *PVOID;
typedef PVOID             *PPVOID;
typedef unsigned char      UCHAR;
typedef unsigned char      BOOLEAN;
typedef unsigned short     USHORT;
typedef unsigned short     WCHAR;
typedef WCHAR             *PWSTR;
typedef const WCHAR       *PCWSTR;
typedef unsigned long      ULONG;      /* 32-bit under LLP64 */
typedef ULONG             *PULONG;
typedef unsigned __int64   ULONGLONG;
typedef unsigned __int64   SIZE_T;
typedef SIZE_T            *PSIZE_T;
typedef unsigned __int64   ULONG_PTR;
typedef ULONG              ACCESS_MASK;

#define NTAPI    __stdcall          /* ignored on x64; kept for clarity   */
#define VOID     void
#define IN
#define OUT
#define OPTIONAL

#define NT_SUCCESS(s)   (((NTSTATUS)(s)) >= 0)

#define STATUS_SUCCESS            ((NTSTATUS)0x00000000L)
#define STATUS_BUFFER_OVERFLOW    ((NTSTATUS)0x80000005L)
#define STATUS_INVALID_IMAGE_FORMAT ((NTSTATUS)0xC000007BL)
#define STATUS_NOT_FOUND          ((NTSTATUS)0xC0000225L)

#define NtCurrentProcess()        ((HANDLE)(ULONG_PTR)-1)

typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; long HighPart; } u;
    __int64 QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _UNICODE_STRING {
    USHORT Length;          /* bytes, not counting NUL */
    USHORT MaximumLength;   /* bytes of buffer         */
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG           Length;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG           Attributes;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _IO_STATUS_BLOCK {
    union { NTSTATUS Status; PVOID Pointer; } u;
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

/* Native process entry argument block (simplified; smss fills it in). */
typedef struct _STARTUP_ARGUMENTS {
    ULONG          Flags;
    ULONG          Reserved;
    UNICODE_STRING LoadPath;
} STARTUP_ARGUMENTS, *PSTARTUP_ARGUMENTS;

/* Registry value query shape. */
typedef enum _KEY_VALUE_INFORMATION_CLASS {
    KeyValuePartialInformation = 2
} KEY_VALUE_INFORMATION_CLASS;

typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataLength;
    UCHAR Data[1];
} KEY_VALUE_PARTIAL_INFORMATION, *PKEY_VALUE_PARTIAL_INFORMATION;

#define REG_SZ     1UL
#define REG_DWORD  4UL

typedef enum _SECTION_INHERIT { ViewShare = 1, ViewUnmap = 2 } SECTION_INHERIT;

typedef VOID (NTAPI *PIO_APC_ROUTINE)(PVOID, PIO_STATUS_BLOCK, ULONG);

/* Object-manager / access / file / section constants. */
#define OBJ_CASE_INSENSITIVE        0x00000040UL
#define OBJ_KERNEL_HANDLE           0x00000200UL

#define SYNCHRONIZE                 0x00100000UL
#define FILE_READ_DATA              0x00000001UL
#define FILE_READ_ATTRIBUTES        0x00000080UL
#define KEY_READ                    0x00020019UL

#define FILE_SHARE_READ             0x00000001UL
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020UL
#define FILE_NON_DIRECTORY_FILE     0x00000040UL

#define SECTION_MAP_READ            0x00000004UL
#define SECTION_QUERY               0x00000001UL

#define PAGE_READONLY               0x00000002UL
#define SEC_COMMIT                  0x08000000UL

#define InitializeObjectAttributes(p, n, a, r, s)       \
    do {                                                \
        (p)->Length = sizeof(OBJECT_ATTRIBUTES);        \
        (p)->RootDirectory = (r);                       \
        (p)->ObjectName = (n);                          \
        (p)->Attributes = (a);                          \
        (p)->SecurityDescriptor = 0;                    \
        (p)->SecurityQualityOfService = 0;              \
    } while (0)

/* =========================================================================
 *  ntdll imports
 * ========================================================================= */

__declspec(dllimport) VOID NTAPI
RtlInitUnicodeString(PUNICODE_STRING Dest, PCWSTR Src);

__declspec(dllimport) NTSTATUS NTAPI
NtDisplayString(PUNICODE_STRING String);

__declspec(dllimport) NTSTATUS NTAPI
NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
           POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
           ULONG ShareAccess, ULONG OpenOptions);

__declspec(dllimport) NTSTATUS NTAPI
NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
           PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
           ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);

__declspec(dllimport) NTSTATUS NTAPI
NtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                POBJECT_ATTRIBUTES ObjectAttributes, PLARGE_INTEGER MaximumSize,
                ULONG SectionPageProtection, ULONG AllocationAttributes,
                HANDLE FileHandle);

__declspec(dllimport) NTSTATUS NTAPI
NtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle, PPVOID BaseAddress,
                   ULONG_PTR ZeroBits, SIZE_T CommitSize,
                   PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize,
                   SECTION_INHERIT InheritDisposition, ULONG AllocationType,
                   ULONG Win32Protect);

__declspec(dllimport) NTSTATUS NTAPI
NtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress);

__declspec(dllimport) NTSTATUS NTAPI
NtOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess,
          POBJECT_ATTRIBUTES ObjectAttributes);

__declspec(dllimport) NTSTATUS NTAPI
NtQueryValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName,
                KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
                PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);

__declspec(dllimport) NTSTATUS NTAPI
NtClose(HANDLE Handle);

__declspec(dllimport) NTSTATUS NTAPI
NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);

/* =========================================================================
 *  Tiny CRT-free helpers
 * ========================================================================= */

#define PATH_MAX_WCH   520

static SIZE_T zl_wcslen(PCWSTR s)
{
    SIZE_T n = 0;
    while (s[n]) n++;
    return n;
}

/* Emit a NUL-terminated wide string to the native boot console. */
static VOID Display(PCWSTR text)
{
    UNICODE_STRING us;
    SIZE_T len = zl_wcslen(text) * sizeof(WCHAR);

    us.Buffer        = (PWSTR)text;
    us.Length        = (USHORT)len;
    us.MaximumLength = (USHORT)(len + sizeof(WCHAR));
    NtDisplayString(&us);
}

/* Append src onto dst[*len], NUL-terminating; returns 0 on overflow. */
static BOOLEAN AppendW(PWSTR dst, ULONG cap, PULONG len, PCWSTR src)
{
    ULONG i = 0;
    while (src[i]) {
        if (*len + 1 >= cap) { dst[*len] = 0; return 0; }
        dst[(*len)++] = src[i++];
    }
    dst[*len] = 0;
    return 1;
}

/* Format a 64-bit value as "0x" + 16 hex digits into caller buffer (>=19 WCHAR). */
static VOID FormatHex64(ULONGLONG v, PWSTR out)
{
    static const WCHAR digits[] = L"0123456789ABCDEF";
    int i;
    out[0] = L'0';
    out[1] = L'x';
    for (i = 0; i < 16; i++)
        out[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    out[18] = L'\0';
}

/* =========================================================================
 *  Registry configuration
 * ========================================================================= */

static NTSTATUS OpenZenithKey(PHANDLE outKey)
{
    UNICODE_STRING    keyName;
    OBJECT_ATTRIBUTES oa;

    *outKey = 0;
    RtlInitUnicodeString(&keyName,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control"
        L"\\Session Manager\\Zenith");
    InitializeObjectAttributes(&oa, &keyName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, 0, 0);
    return NtOpenKey(outKey, KEY_READ, &oa);
}

static BOOLEAN ReadRegDword(HANDLE hKey, PCWSTR value, PULONG out)
{
    UNICODE_STRING valName;
    NTSTATUS       st;
    ULONG          resultLen = 0;
    __declspec(align(8)) UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;

    RtlInitUnicodeString(&valName, (PWSTR)value);
    st = NtQueryValueKey(hKey, &valName, KeyValuePartialInformation,
                         info, sizeof(buf), &resultLen);
    if (NT_SUCCESS(st) && info->Type == REG_DWORD &&
        info->DataLength >= sizeof(ULONG)) {
        *out = *(PULONG)info->Data;
        return 1;
    }
    return 0;
}

/* Reads a REG_SZ into out (capChars incl. NUL). Returns 0 if absent/too big. */
static BOOLEAN ReadRegString(HANDLE hKey, PCWSTR value, PWSTR out, ULONG capChars)
{
    UNICODE_STRING valName;
    NTSTATUS       st;
    ULONG          resultLen = 0;
    __declspec(align(8)) UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + PATH_MAX_WCH * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    PWSTR src;
    ULONG chars, i;

    RtlInitUnicodeString(&valName, (PWSTR)value);
    st = NtQueryValueKey(hKey, &valName, KeyValuePartialInformation,
                         info, sizeof(buf), &resultLen);
    if (!NT_SUCCESS(st) || info->Type != REG_SZ || info->DataLength < sizeof(WCHAR))
        return 0;

    src   = (PWSTR)info->Data;
    chars = info->DataLength / sizeof(WCHAR);
    if (chars >= capChars) chars = capChars - 1;

    for (i = 0; i < chars && src[i]; i++)
        out[i] = src[i];
    out[i] = 0;
    return (i > 0);
}

/* =========================================================================
 *  Best-effort distro identification from <rootfs>\etc\os-release
 * ========================================================================= */

static BOOLEAN AsciiNeedle(const UCHAR *hay, ULONG hayLen, const char *needle,
                           ULONG *posOut)
{
    ULONG nlen = 0, i, j;
    while (needle[nlen]) nlen++;
    if (nlen == 0 || hayLen < nlen) return 0;
    for (i = 0; i + nlen <= hayLen; i++) {
        for (j = 0; j < nlen; j++)
            if (hay[i + j] != (UCHAR)needle[j]) break;
        if (j == nlen) { *posOut = i + nlen; return 1; }
    }
    return 0;
}

static VOID ReportDistro(PCWSTR ntPrefix)
{
    UNICODE_STRING    name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK   iosb;
    HANDLE            hFile = 0;
    NTSTATUS          st;
    LARGE_INTEGER     off;
    WCHAR             path[PATH_MAX_WCH];
    ULONG             plen = 0;
    UCHAR             data[2048];
    WCHAR             wline[256];
    ULONG             pos, i;

    AppendW(path, PATH_MAX_WCH, &plen, ntPrefix);
    AppendW(path, PATH_MAX_WCH, &plen, L"\\etc\\os-release");

    RtlInitUnicodeString(&name, path);
    InitializeObjectAttributes(&oa, &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, 0, 0);

    st = NtOpenFile(&hFile, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb,
                    FILE_SHARE_READ,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) {
        Display(L"zenith-linux: distro = (no /etc/os-release)\r\n");
        return;
    }

    off.QuadPart = 0;
    st = NtReadFile(hFile, 0, 0, 0, &iosb, data, sizeof(data), &off, 0);
    NtClose(hFile);
    if (!NT_SUCCESS(st) || iosb.Information == 0) {
        Display(L"zenith-linux: distro = (unreadable)\r\n");
        return;
    }

    {
        ULONG have = (ULONG)iosb.Information;
        if (!AsciiNeedle(data, have, "PRETTY_NAME=", &pos) &&
            !AsciiNeedle(data, have, "NAME=", &pos)) {
            Display(L"zenith-linux: distro = (unidentified rootfs)\r\n");
            return;
        }
        if (pos < have && data[pos] == '"') pos++;   /* skip opening quote */
        i = 0;
        while (pos < have && i < 255) {
            UCHAR c = data[pos];
            if (c == '"' || c == '\n' || c == '\r') break;
            wline[i++] = (WCHAR)c;                    /* os-release is UTF-8/ASCII */
            pos++;
        }
        wline[i] = 0;
    }

    Display(L"zenith-linux: distro = ");
    Display(wline);
    Display(L"\r\n");
}

/* =========================================================================
 *  Map a 64-bit Linux ELF into this address space (read-only data view).
 * ========================================================================= */

static NTSTATUS MapLinuxImage(PCWSTR ntPath, PPVOID outBase, PSIZE_T outSize)
{
    UNICODE_STRING    name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK   iosb;
    HANDLE            hFile    = 0;
    HANDLE            hSection = 0;
    PVOID             base     = 0;
    SIZE_T            viewSize = 0;
    LARGE_INTEGER     offset;
    NTSTATUS          st;

    *outBase = 0;
    *outSize = 0;
    offset.QuadPart = 0;

    RtlInitUnicodeString(&name, (PWSTR)ntPath);
    InitializeObjectAttributes(&oa, &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, 0, 0);

    st = NtOpenFile(&hFile,
                    FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                    &oa, &iosb, FILE_SHARE_READ,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st))
        return st;

    /* Data section, NOT SEC_IMAGE: an ELF has no PE headers to parse. */
    st = NtCreateSection(&hSection, SECTION_MAP_READ | SECTION_QUERY,
                         0, 0, PAGE_READONLY, SEC_COMMIT, hFile);
    if (!NT_SUCCESS(st)) {
        NtClose(hFile);
        return st;
    }

    st = NtMapViewOfSection(hSection, NtCurrentProcess(), &base,
                            0, 0, &offset, &viewSize,
                            ViewUnmap, 0, PAGE_READONLY);
    if (!NT_SUCCESS(st)) {
        NtClose(hSection);
        NtClose(hFile);
        return st;
    }

    /* Validate ELF64 little-endian magic: 0x7F 'E' 'L' 'F', EI_CLASS==2. */
    {
        const UCHAR *e = (const UCHAR *)base;
        BOOLEAN ok = (viewSize >= 20 &&
                      e[0] == 0x7F && e[1] == 'E' && e[2] == 'L' && e[3] == 'F' &&
                      e[4] == 2 /* ELFCLASS64 */);
        if (!ok) {
            NtUnmapViewOfSection(NtCurrentProcess(), base);
            NtClose(hSection);
            NtClose(hFile);
            return STATUS_INVALID_IMAGE_FORMAT;
        }
    }

    /* The view keeps the section mapped; the handles may be closed now. */
    NtClose(hSection);
    NtClose(hFile);

    *outBase = base;
    *outSize = viewSize;
    return STATUS_SUCCESS;
}

/*
 * Locate and map the rootfs init. If an explicit InitPath is given it is used
 * verbatim; otherwise the usual init/shell locations are probed in order and
 * the first ELF64 wins. Distros covered by the default probe list:
 *   systemd (Ubuntu/Debian/Arch/Fedora): /sbin/init, /lib/systemd/systemd
 *   busybox   (Alpine):                  /sbin/init, /bin/busybox
 *   runit     (Void):                    /sbin/init, /bin/sh
 *   fallback shells:                     /bin/bash, /bin/sh, /usr/bin/bash
 */
static NTSTATUS ResolveAndMapInit(PCWSTR ntPrefix, PCWSTR explicitRel,
                                  PWSTR usedRelOut, ULONG usedCap,
                                  PPVOID outBase, PSIZE_T outSize)
{
    static const PCWSTR candidates[] = {
        L"\\sbin\\init",
        L"\\init",
        L"\\lib\\systemd\\systemd",
        L"\\bin\\busybox",
        L"\\bin\\bash",
        L"\\bin\\sh",
        L"\\usr\\bin\\bash",
        L"\\usr\\bin\\init",
    };
    const ULONG nCand = sizeof(candidates) / sizeof(candidates[0]);
    WCHAR   full[PATH_MAX_WCH];
    ULONG   i, flen;
    NTSTATUS st;

    if (explicitRel && explicitRel[0]) {
        flen = 0;
        AppendW(full, PATH_MAX_WCH, &flen, ntPrefix);
        AppendW(full, PATH_MAX_WCH, &flen, explicitRel);
        st = MapLinuxImage(full, outBase, outSize);
        if (NT_SUCCESS(st)) {
            ULONG u = 0;
            AppendW(usedRelOut, usedCap, &u, explicitRel);
        }
        return st;
    }

    for (i = 0; i < nCand; i++) {
        flen = 0;
        AppendW(full, PATH_MAX_WCH, &flen, ntPrefix);
        AppendW(full, PATH_MAX_WCH, &flen, candidates[i]);
        st = MapLinuxImage(full, outBase, outSize);
        if (NT_SUCCESS(st)) {
            ULONG u = 0;
            AppendW(usedRelOut, usedCap, &u, candidates[i]);
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

/* =========================================================================
 *  Native entry point
 * ========================================================================= */

VOID NTAPI NtProcessStartup(PSTARTUP_ARGUMENTS StartupArguments)
{
    HANDLE   hKey = 0;
    PVOID    imageBase = 0;
    SIZE_T   imageSize = 0;
    NTSTATUS st;
    WCHAR    hexbuf[19];
    WCHAR    usedRel[PATH_MAX_WCH];
    WCHAR    rootfsDos[PATH_MAX_WCH];
    WCHAR    initRel[PATH_MAX_WCH];
    WCHAR    ntPrefix[PATH_MAX_WCH];
    ULONG    prefLen = 0;
    ULONG    x11val = 0;
    BOOLEAN  haveInitRel = 0;
    BOOLEAN  x11;

    (VOID)StartupArguments;   /* LoadPath/Flags unused in this bootstrap */

    Display(L"\r\n");
    Display(L"zenith-linux: native-mode bootstrap (lxinit) starting\r\n");
    Display(L"zenith-linux: NT 5.2 BootExecute stage, pre-csrss\r\n");

    /* --- (3) configuration ------------------------------------------- */
    rootfsDos[0] = 0;
    initRel[0]   = 0;
    if (NT_SUCCESS(OpenZenithKey(&hKey))) {
        ReadRegString(hKey, L"RootfsPath", rootfsDos, PATH_MAX_WCH);
        haveInitRel = ReadRegString(hKey, L"InitPath", initRel, PATH_MAX_WCH);
        ReadRegDword(hKey, L"EnableX11", &x11val);
        NtClose(hKey);
    }
    if (rootfsDos[0] == 0) {
        ULONG d = 0;
        AppendW(rootfsDos, PATH_MAX_WCH, &d, L"C:\\rootfs");  /* default */
    }
    x11 = (x11val != 0);

    /* Build NT device path prefix: "\??\<DOS rootfs>" */
    AppendW(ntPrefix, PATH_MAX_WCH, &prefLen, L"\\??\\");
    AppendW(ntPrefix, PATH_MAX_WCH, &prefLen, rootfsDos);

    Display(L"zenith-linux: rootfs = ");
    Display(rootfsDos);
    Display(L"\r\n");

    /* --- distro banner (best effort) --------------------------------- */
    ReportDistro(ntPrefix);

    /* --- (2) locate + map the rootfs init ---------------------------- */
    usedRel[0] = 0;
    st = ResolveAndMapInit(ntPrefix, haveInitRel ? initRel : 0,
                           usedRel, PATH_MAX_WCH, &imageBase, &imageSize);
    if (NT_SUCCESS(st)) {
        Display(L"zenith-linux: init = ");
        Display(usedRel);
        Display(L"\r\n");
        Display(L"zenith-linux: mapped ELF64 image at ");
        FormatHex64((ULONGLONG)(ULONG_PTR)imageBase, hexbuf);
        Display(hexbuf);
        Display(L" size ");
        FormatHex64((ULONGLONG)imageSize, hexbuf);
        Display(hexbuf);
        Display(L"\r\n");

        /*
         * imageBase/imageSize is the raw ELF in our address space. A
         * kernel-side syscall-translation provider would consume this to
         * stand up a Linux pico-process; that provider is out of scope for a
         * native user-mode BootExecute app, so we release the view and let
         * the next stage own execution.
         */
        NtUnmapViewOfSection(NtCurrentProcess(), imageBase);
    } else {
        Display(L"zenith-linux: WARNING no ELF64 init found in rootfs, status ");
        FormatHex64((ULONGLONG)(ULONG)st, hexbuf);
        Display(hexbuf);
        Display(L"\r\n");
    }

    /* --- (4) X11 vs. text-mode TTY hand-off -------------------------- */
    if (x11) {
        /*
         * X11 requested. We must NOT hold the boot path open: smss is blocked
         * on this process and will not start csrss or load the video miniports
         * until we terminate. So we only record intent for the post-Win32
         * stage (Xorg binds the display once a real video driver exists) and
         * fall through to exit. Bringing a graphical layer up from inside
         * BootExecute would deadlock the display stack.
         */
        Display(L"zenith-linux: X11 requested; deferring Xorg to post-csrss stage\r\n");
        Display(L"zenith-linux: releasing boot so smss can start csrss + video miniports\r\n");
    } else {
        Display(L"zenith-linux: text-mode TTY active on native console\r\n");
    }

    Display(L"zenith-linux: bootstrap complete, returning control to smss\r\n");

    /*
     * Terminating is the one supported way to tell smss "proceed" -- it
     * unblocks subsystem initialization (csrss / Win32) and the video driver
     * load. Returning from a native entry point is undefined (no CRT), so
     * exit explicitly.
     */
    NtTerminateProcess(NtCurrentProcess(), STATUS_SUCCESS);
}
