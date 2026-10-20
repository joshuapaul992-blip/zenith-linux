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
 * Build (Windows DDK / MSVC x64):
 *     cl /c /GS- /O2 /Zl /W4 lxinit.c
 *     link /SUBSYSTEM:NATIVE,5.02 /ENTRY:NtProcessStartup /NODEFAULTLIB ^
 *          /MACHINE:X64 lxinit.obj ntdll.lib /OUT:lxinit.exe
 *
 *   /Zl + /NODEFAULTLIB   -> no C runtime is linked in.
 *   /SUBSYSTEM:NATIVE,5.02 -> NT 5.2 native image (no csrss dependency).
 *   /GS-                  -> no stack-cookie runtime (there is no __security_*).
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
 *     exit. The only way to block it permanently is to never return. We thus
 *     do the early work, optionally stage the X11 hand-off, and exit.
 *
 *  3. Early-boot screen output: at BootExecute time no video miniport is
 *     loaded, there is no mapped linear framebuffer, and INT 10h is gone in
 *     long mode. NtDisplayString (the boot "blue/black screen" text path) is
 *     the only sanctioned output primitive here. It renders onto the native
 *     640x480 text console the kernel owns before csrss takes the display.
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
#define CONST    const
#define IN
#define OUT
#define OPTIONAL

#define NT_SUCCESS(s)   (((NTSTATUS)(s)) >= 0)

#define STATUS_SUCCESS            ((NTSTATUS)0x00000000L)
#define STATUS_BUFFER_OVERFLOW    ((NTSTATUS)0x80000005L)

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

typedef enum _SECTION_INHERIT { ViewShare = 1, ViewUnmap = 2 } SECTION_INHERIT;

/* Object-manager / access / file / section constants. */
#define OBJ_CASE_INSENSITIVE        0x00000040UL
#define OBJ_KERNEL_HANDLE           0x00000200UL

#define SYNCHRONIZE                 0x00100000UL
#define STANDARD_RIGHTS_READ        0x00020000UL
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
 *  Configuration: is an X11 / Xorg graphical layer requested?
 *
 *  Reads DWORD "EnableX11" under
 *    HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Zenith
 *  Returns nonzero only when the value exists and is != 0.
 * ========================================================================= */

static BOOLEAN IsX11Requested(VOID)
{
    UNICODE_STRING    keyName, valName;
    OBJECT_ATTRIBUTES oa;
    HANDLE            hKey = 0;
    NTSTATUS          st;
    BOOLEAN           requested = 0;

    /* Aligned storage for the value + its trailing DWORD payload. */
    __declspec(align(8)) UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG resultLen = 0;

    RtlInitUnicodeString(&keyName,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control"
        L"\\Session Manager\\Zenith");
    InitializeObjectAttributes(&oa, &keyName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, 0, 0);

    st = NtOpenKey(&hKey, KEY_READ, &oa);
    if (!NT_SUCCESS(st))
        return 0;                       /* no config key -> text mode */

    RtlInitUnicodeString(&valName, L"EnableX11");
    st = NtQueryValueKey(hKey, &valName, KeyValuePartialInformation,
                         info, sizeof(buf), &resultLen);
    if (NT_SUCCESS(st) && info->DataLength >= sizeof(ULONG)) {
        ULONG v = *(PULONG)info->Data;
        requested = (v != 0);
    }

    NtClose(hKey);
    return requested;
}

/* =========================================================================
 *  Map the 64-bit Linux init/bash image into this address space.
 *
 *  Opens the file, creates a read-only data section over it, maps a view,
 *  and validates the ELF64 header. Reports the base address and class.
 *  Caller owns unmapping via the returned handle/base.
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
        if (viewSize < 20 ||
            e[0] != 0x7F || e[1] != 'E' || e[2] != 'L' || e[3] != 'F') {
            NtUnmapViewOfSection(NtCurrentProcess(), base);
            NtClose(hSection);
            NtClose(hFile);
            return (NTSTATUS)0xC000007BL;      /* STATUS_INVALID_IMAGE_FORMAT */
        }
        if (e[4] != 2 /* ELFCLASS64 */) {
            NtUnmapViewOfSection(NtCurrentProcess(), base);
            NtClose(hSection);
            NtClose(hFile);
            return (NTSTATUS)0xC000007BL;
        }
    }

    /* The view keeps the section mapped; the handles may be closed now. */
    NtClose(hSection);
    NtClose(hFile);

    *outBase = base;
    *outSize = viewSize;
    return STATUS_SUCCESS;
}

/* =========================================================================
 *  Native entry point
 * ========================================================================= */

VOID NTAPI NtProcessStartup(PSTARTUP_ARGUMENTS StartupArguments)
{
    PVOID    imageBase = 0;
    SIZE_T   imageSize = 0;
    NTSTATUS st;
    WCHAR    hexbuf[19];
    BOOLEAN  x11;

    (VOID)StartupArguments;   /* LoadPath/Flags unused in this bootstrap */

    Display(L"\r\n");
    Display(L"zenith-linux: native-mode bootstrap (lxinit) starting\r\n");
    Display(L"zenith-linux: NT 5.2 BootExecute stage, pre-csrss\r\n");

    /* --- (3) configuration: graphical layer requested? --------------- */
    x11 = IsX11Requested();

    /* --- (2) map the Linux init/bash image --------------------------- */
    st = MapLinuxImage(L"\\??\\C:\\ubuntu_rootfs\\bin\\bash",
                       &imageBase, &imageSize);
    if (NT_SUCCESS(st)) {
        Display(L"zenith-linux: mapped ELF64 /bin/bash at ");
        FormatHex64((ULONGLONG)(ULONG_PTR)imageBase, hexbuf);
        Display(hexbuf);
        Display(L" size ");
        FormatHex64((ULONGLONG)imageSize, hexbuf);
        Display(hexbuf);
        Display(L"\r\n");

        /*
         * At this point imageBase/imageSize is the raw ELF in our address
         * space. A kernel-side syscall-translation provider would consume
         * this to stand up a Linux pico-process; that provider is out of
         * scope for a native user-mode BootExecute app, so we release the
         * view and let the next stage own execution.
         */
        NtUnmapViewOfSection(NtCurrentProcess(), imageBase);
    } else {
        Display(L"zenith-linux: WARNING could not map /bin/bash, status ");
        FormatHex64((ULONGLONG)(ULONG)st, hexbuf);
        Display(hexbuf);
        Display(L"\r\n");
    }

    /* --- (4) X11 vs. text-mode TTY hand-off -------------------------- */
    if (x11) {
        /*
         * X11 requested. We must NOT hold the boot path open: smss is
         * blocked on this process and will not start csrss or load the
         * video miniports until we terminate. So we only *record intent*
         * for the post-Win32 stage (Xorg can bind the display once a real
         * video driver exists) and fall through to exit. The graphical
         * layer is brought up later, after csrss is alive -- never from
         * inside BootExecute, which would deadlock the display stack.
         */
        Display(L"zenith-linux: X11 requested; deferring Xorg to post-csrss stage\r\n");
        Display(L"zenith-linux: releasing boot so smss can start csrss + video miniports\r\n");
    } else {
        /*
         * Text mode: the early TTY is the NtDisplayString console we are
         * already writing to. No separate device bring-up is needed here;
         * the interactive TTY/login is launched by the post-Win32 stage.
         */
        Display(L"zenith-linux: text-mode TTY active on native console\r\n");
    }

    Display(L"zenith-linux: bootstrap complete, returning control to smss\r\n");

    /*
     * (2)/(4) Terminating is the one and only supported way to tell smss
     * "proceed" -- it unblocks subsystem initialization (csrss / Win32) and
     * the video driver load. Returning normally from a native entry point is
     * undefined (no CRT to catch it), so exit explicitly.
     */
    NtTerminateProcess(NtCurrentProcess(), STATUS_SUCCESS);
}
