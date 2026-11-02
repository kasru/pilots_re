#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#pragma once

#include <windows.h>
#include <stdio.h>
#ifdef _WIN32
#include <intrin.h>
#include <crtdbg.h>
#endif
#include <stdlib.h>
#include <string.h>

/* Заголовок определяет много static-функций, которые используются не в каждом
   трансляционном единице; при /W4 /WX это ошибка C4505. Подавляем здесь же —
   и в debug, и в release: канал лога скомпилирован в обеих конфигурациях,
   в release он просто выключен по умолчанию (ветка #else внизу). */
#pragma warning(disable: 4505)

/*
 * Лог движка: файл + OutputDebugString.
 *   _DEBUG  — включён всегда, файл <имя образа>_debug.log рядом с ним;
 *   release — выключен по умолчанию; включает его ключ -log [файл], см.
 *             DbgScanLogSwitch ниже.
 * View OutputDebugString output with:
 *   - Visual Studio Output window (Debug -> Windows -> Output)
 *   - DebugView (Sysinternals) - https://learn.microsoft.com/en-us/sysinternals/downloads/debugview
 *   - WinDbg
 *
 * Канал (файл, критическая секция вокруг записи, флаг «включён») определяется
 * в data.c и объявлен здесь — общий для обеих конфигураций.
 */
extern HANDLE g_hDebugLogFile;
extern CRITICAL_SECTION g_DebugLogCS;
extern int g_DbgLogOn;

/* ---- Ключ -log [файл] ---------------------------------------------------- */

/* Следующий токен командной строки. Токен в кавычках может содержать пробелы,
   и кавычки в него не входят. Возвращает 0, когда строка исчерпана, и 1, когда
   токен прочитан — *len может быть 0 для пустого "" (поэтому вызывающий не
   должен считать 0 признаком «ничего не найдено»). Тот же разбор, что
   SdlNextToken в sdl_backend.c: ключ -log читается в DbgLogInit, то есть раньше,
   чем туда дойдёт SdlBackendParseCmdLine, и дублировать ради одного ключа
   общий токенизатор не стоило. */
static int DbgNextToken(const char **pp, const char **ptok, int *len)
{
    const char *p = *pp;
    const char *tok;

    while (*p == ' ' || *p == '\t')
        ++p;
    if (!*p)
        return 0;
    if (*p == '"')
    {
        ++p;
        tok = p;
        while (*p && *p != '"')
            ++p;
        *len = (int)(p - tok);
        if (*p == '"')
            ++p;
    }
    else
    {
        tok = p;
        while (*p && *p != ' ' && *p != '\t')
            ++p;
        *len = (int)(p - tok);
    }
    *ptok = tok;
    *pp = p;
    return 1;
}

/* Весь токен — «-name» (или «/name»), без учёта регистра, и ничего больше. */
static int DbgArgIs(const char *tok, int len, const char *name)
{
    int i;

    for (i = 0; name[i]; ++i)
    {
        char a = (i + 1 < len) ? tok[i + 1] : 0;   /* +1 пропускает ведущий - или / */
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (a != name[i])
            return 0;
    }
    return len == i + 1;
}

/* Токен похож на ключ, а не на значение. «-...» — ключом бывает всегда; «/...»
   — ключом бывает, только если за слэшем идёт одно из имён, которые вообще
   бывают ключами (тот же список, что в SdlBackendParseCmdLine: если там
   появится новый ключ, добавить его и сюда). Без этого списка выбор был бы
   ложным в обе стороны: запретить любой «/...» — и «-log /tmp/журнал.log» на
   Linux/macOS молча превращается в «-log без имени» (абсолютный posix-путь
   начинается со слэша, файл уходит не туда); разрешить любой — и
   «-log /fullscreen» съедает ключ вместо того, чтобы его пропустить. */
static int DbgLooksLikeSwitch(const char *tok, int len)
{
    static const char *const names[] = { "fullscreen", "datapath", "log" };
    size_t i;

    if (len > 1 && tok[0] == '-')
        return 1;
    if (len < 2 || tok[0] != '/')
        return 0;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    {
        if (DbgArgIs(tok, len, names[i]))
            return 1;
    }
    return 0;
}

/* Ключ -log [файл]. Возвращает:
 *   0 — ключа нет (в release лог не пишется вовсе, в debug это ничего не меняет);
 *   1 — ключ есть, имени файла нет: пишем как обычно, <имя образа>_debug.log;
 *   2 — ключ есть с именем файла: пишем в него.
 * Имя берётся, только если следующий токен не похож на ключ (см.
 * DbgLooksLikeSwitch): иначе «-log -fullscreen» создал бы файл с именем
 * «-fullscreen», а ключ остался бы неразобранным. Относительный путь
 * разрешается от каталога запуска, а не от каталога данных: -log читается в
 * DbgLogInit, то есть раньше, чем -datapath может сменить рабочий каталог. */
static int DbgScanLogSwitch(const char *cmdLine, const char **val, int *len)
{
    const char *p = cmdLine;
    const char *tok;
    int tlen;

    *val = NULL;
    *len = 0;
    if (!p)
        return 0;
    while (DbgNextToken(&p, &tok, &tlen))
    {
        const char *look;
        const char *v;
        int vlen;

        if (tlen <= 1 || (tok[0] != '-' && tok[0] != '/'))
            continue;
        if (!DbgArgIs(tok, tlen, "log"))
            continue;
        look = p;
        if (DbgNextToken(&look, &v, &vlen) && vlen > 0 && !DbgLooksLikeSwitch(v, vlen))
        {
            *val = v;
            *len = vlen;
            return 2;
        }
        return 1;
    }
    return 0;
}

#ifdef _DEBUG

#ifdef _WIN32
/*
 * Custom invalid parameter handler for MSVC debug CRT.
 * The original Watcom runtime simply returned -1/errno on invalid handles
 * (e.g. read(-1, ...) or close(-1)). MSVC debug CRT triggers an assertion
 * dialog instead. This handler suppresses the popup and lets the CRT function
 * return an error code, matching Watcom behavior.
 */
static void __cdecl DbgInvalidParameterHandler(
    const wchar_t *expression,
    const wchar_t *function,
    const wchar_t *file,
    unsigned int line,
    uintptr_t pReserved)
{
    /* Silently ignore — the CRT function will return an error value */
    (void)expression; (void)function; (void)file; (void)line; (void)pReserved;
}
#endif /* _WIN32 */
#endif /* _DEBUG: обработчик параметров нужен только debug-ветке CRT */

/* Открыть лог. cmdLine — командная строка, полученная WinMain: в ней ищется
   ключ -log (DbgScanLogSwitch). В debug лог включён всегда, -log там может
   только сменить файл; в release без ключа функция выходит, не создав ни
   файла, ни строки — ровно то, что и обещает «в release лог не пишется».
   Зовётся раньше SdlBackendParseCmdLine, поэтому строки про -datapath тоже
   попадают в лог. */
static void DbgLogInit(const char *cmdLine)
{
    char logPath[MAX_PATH];
    const char *val;
    int vlen;
    int which;

    g_hDebugLogFile = INVALID_HANDLE_VALUE;
    g_DbgLogOn = 0;
    InitializeCriticalSection(&g_DebugLogCS);

#if defined(_DEBUG) && defined(_WIN32)
    /* Suppress MSVC debug CRT assertion popups for invalid I/O handles.
     * Original Watcom CRT returned -1 on invalid handles; MSVC debug CRT
     * fires assertions. This makes _read/_write/_close/_lseek behave like
     * Watcom: return -1 and set errno. */
    _set_invalid_parameter_handler(DbgInvalidParameterHandler);
    _CrtSetReportMode(_CRT_ASSERT, 0);  /* Disable assert popups */
#endif

    which = DbgScanLogSwitch(cmdLine, &val, &vlen);
#ifndef _DEBUG
    if (!which)
        return;                         /* release без -log: лога нет */
#endif

    if (which == 2 && vlen < (int)sizeof(logPath))
    {
        /* -log <файл>: пишем в указанный файл. Относительный путь — от каталога
           запуска (см. DbgScanLogSwitch). Кавычки снял токенизатор, пробелы в
           имени пути этим токеном и собираются. */
        memcpy(logPath, val, vlen);
        logPath[vlen] = '\0';
    }
    else
    {
        /* Path of the running image: real GetModuleFileNameA on Windows, /proc/self/exe
           (formatted as a Win path) via win32_shim on Linux; CreateFileA below converts
           '\' -> '/' there, so the resulting log sits next to the executable. */
        GetModuleFileNameA(NULL, logPath, MAX_PATH);
        /* Replace .exe with _debug.log */
        {
            char *dot = strrchr(logPath, '.');
            if (dot) *dot = 0;
        }
        strcat(logPath, "_debug.log");
    }

    g_hDebugLogFile = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    g_DbgLogOn = 1;
    /* Текст не менять: scripts/deploy_switch.sh ищет эту строку в .nro. */
    OutputDebugStringA("[PILOTS] Debug logging started\n");
}

static void DbgLogClose(void)
{
    if (g_hDebugLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hDebugLogFile);
        g_hDebugLogFile = INVALID_HANDLE_VALUE;
    }
    DeleteCriticalSection(&g_DebugLogCS);
}

static void DbgPrintf(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    DWORD written;

    /* Первым делом — выключено ли: в release без -log здесь выход до всякой
       работы, поэтому цена выключенного лога — один вызов и один переход.
       Разбор формата идёт только когда лог действительно пишется. */
    if (!g_DbgLogOn)
        return;

    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);

    /* OutputDebugString - visible in debugger/DebugView */
    OutputDebugStringA(buf);

    /* Also write to log file */
    EnterCriticalSection(&g_DebugLogCS);
    if (g_hDebugLogFile != INVALID_HANDLE_VALUE) {
        WriteFile(g_hDebugLogFile, buf, lstrlenA(buf), &written, NULL);
        FlushFileBuffers(g_hDebugLogFile);
    }
    LeaveCriticalSection(&g_DebugLogCS);
}

/* Log function entry */
#define DBG_ENTER(func) DbgPrintf("[PILOTS] ENTER: %s\n", func)
#define DBG_EXIT(func)  DbgPrintf("[PILOTS] EXIT:  %s\n", func)

/* Log a message.  ##__VA_ARGS__: swallows the comma for the no-arg form - MSVC's
   preprocessor did this implicitly, GCC needs the ## (both accept it). */
#define DBG_LOG(msg, ...) DbgPrintf("[PILOTS] " msg "\n", ##__VA_ARGS__)
#define DBG_MSG(msg) DbgPrintf("[PILOTS] " msg "\n")

/* Log an error */
#define DBG_ERR(msg, ...) DbgPrintf("[PILOTS] ERROR: " msg "\n", ##__VA_ARGS__)

/* Дамп регистров и стека на крахе — только debug-ветка: в release его нет и
   с -log тоже, поэтому падения по-прежнему воспроизводятся на debug-сборке
   (см. AGENTS.md). Сам лог при этом в release с -log уже пишется. */
#ifdef _DEBUG

#ifdef _WIN32
/* ===== SEH Exception Handler ===== */

static const char* ExceptionCodeToString(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:       return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:  return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:             return "BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:  return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:   return "FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:     return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:     return "FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:  return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:           return "FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:        return "FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:          return "FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:    return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:          return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:     return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:           return "INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:    return "INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:       return "PRIVILEGED_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:            return "SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:         return "STACK_OVERFLOW";
    default:                               return "UNKNOWN";
    }
}

static LONG WINAPI DebugExceptionFilter(EXCEPTION_POINTERS *pExcept)
{
    EXCEPTION_RECORD *rec = pExcept->ExceptionRecord;
    CONTEXT *ctx = pExcept->ContextRecord;
    DWORD *sp;
    int i;

#if defined(_M_IX86)
    DbgPrintf("[PILOTS] *** EXCEPTION: 0x%08X (%s) at address 0x%08X ***\n",
        rec->ExceptionCode,
        ExceptionCodeToString(rec->ExceptionCode),
        (unsigned int)rec->ExceptionAddress);

    DbgPrintf("[PILOTS] Registers: EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
        ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    DbgPrintf("[PILOTS]            ESI=0x%08X EDI=0x%08X EBP=0x%08X ESP=0x%08X\n",
        ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
    DbgPrintf("[PILOTS]            EIP=0x%08X EFLAGS=0x%08X\n",
        ctx->Eip, ctx->EFlags);

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        DbgPrintf("[PILOTS] Access violation: %s address 0x%08X\n",
            rec->ExceptionInformation[0] ? "WRITE to" : "READ from",
            (unsigned int)rec->ExceptionInformation[1]);
    }

    /* Dump raw stack contents around ESP */
    sp = (DWORD *)ctx->Esp;
    DbgPrintf("[PILOTS] Raw stack (ESP=0x%08X):\n", ctx->Esp);
    for (i = 0; i < 32; i += 4) {
        DbgPrintf("[PILOTS]   +%02X: %08X %08X %08X %08X\n",
            i*4, sp[i], sp[i+1], sp[i+2], sp[i+3]);
    }

    /* Walk stack frames via EBP chain */
    {
        DWORD *ebp = (DWORD *)ctx->Ebp;
        int frame = 0;
        DbgPrintf("[PILOTS] EBP chain:\n");
        while (ebp && frame < 20) {
            if ((unsigned int)ebp < 0x10000 || (unsigned int)ebp > 0x7FFFFFFF)
                break;
            DbgPrintf("[PILOTS]   F%d: EBP=0x%08X Ret=0x%08X\n", frame, (unsigned int)ebp, ebp[1]);
            ebp = (DWORD *)ebp[0];
            frame++;
        }
    }
#else /* !_M_IX86: x64 - full-width registers, no EBP chain ---------------- */
    DbgPrintf("[PILOTS] *** EXCEPTION: 0x%08X (%s) at address 0x%p ***\n",
        rec->ExceptionCode,
        ExceptionCodeToString(rec->ExceptionCode),
        rec->ExceptionAddress);

    DbgPrintf("[PILOTS] Registers: RAX=0x%016I64X RBX=0x%016I64X RCX=0x%016I64X RDX=0x%016I64X\n",
        ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    DbgPrintf("[PILOTS]            RSI=0x%016I64X RDI=0x%016I64X RBP=0x%016I64X RSP=0x%016I64X\n",
        ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
    DbgPrintf("[PILOTS]            R8 =0x%016I64X R9 =0x%016I64X R10=0x%016I64X R11=0x%016I64X\n",
        ctx->R8, ctx->R9, ctx->R10, ctx->R11);
    DbgPrintf("[PILOTS]            R12=0x%016I64X R13=0x%016I64X R14=0x%016I64X R15=0x%016I64X\n",
        ctx->R12, ctx->R13, ctx->R14, ctx->R15);
    DbgPrintf("[PILOTS]            RIP=0x%016I64X EFLAGS=0x%08X\n",
        ctx->Rip, ctx->EFlags);

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        DbgPrintf("[PILOTS] Access violation: %s address 0x%p\n",
            rec->ExceptionInformation[0] ? "WRITE to" : "READ from",
            (void *)rec->ExceptionInformation[1]);
    }

    /* Raw stack around RSP. MSVC x64 unwinds via RUNTIME_FUNCTION records,
       not [rbp] links, so the i386 EBP-chain walk has no x64 counterpart;
       the raw dump plus RIP is enough to identify the faulting frame. */
    sp = (DWORD *)ctx->Rsp;
    DbgPrintf("[PILOTS] Raw stack (RSP=0x%016I64X):\n", ctx->Rsp);
    for (i = 0; i < 32; i += 4) {
        DbgPrintf("[PILOTS]   +%02X: %08X %08X %08X %08X\n",
            i*4, sp[i], sp[i+1], sp[i+2], sp[i+3]);
    }
#endif /* _M_IX86 */

    /* Flush the log before we crash */
    DbgLogClose();

    return EXCEPTION_CONTINUE_SEARCH;
}

static void DbgInstallExceptionHandler(void)
{
    SetUnhandledExceptionFilter(DebugExceptionFilter);
    DBG_MSG("Unhandled exception filter installed");
}

#else /* !_WIN32: POSIX signal handlers + backtrace instead of SEH */

#include <signal.h>
#include <unistd.h>

/* backtrace() lives in glibc's execinfo.h, and that header is a glibc-ism:
   newlib - which is what the Switch builds against - does not ship it, so the
   _DEBUG crash handler below simply did not compile there. Probing for the
   header instead of testing __NEWLIB__ keeps this right on musl and on the
   older toolchains too. */
#ifdef __has_include
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define PILOTS_HAVE_BACKTRACE 1
#  endif
#else
#  include <execinfo.h>           /* compilers without __has_include: assume glibc */
#  define PILOTS_HAVE_BACKTRACE 1
#endif

/* Async-signal-safe-ish crash dump: raw write() + backtrace_symbols_fd only
   (DbgPrintf uses the log-file CS and could deadlock inside a handler). */
static void DbgSignalHandler(int sig)
{
    ssize_t wr;
    static const char hdr[] = "\n[PILOTS] *** FATAL SIGNAL - stack trace ***\n";

    wr = write(2, hdr, sizeof hdr - 1);
    (void)wr;
#ifdef PILOTS_HAVE_BACKTRACE
    {
        void *frames[32];
        int n = backtrace(frames, 32);
        backtrace_symbols_fd(frames, n, 2);
    }
#else
    /* Nothing to unwind to here. The log file is written and flushed line by
       line (DbgPrintf), so whatever it already holds is the trace of how far
       the run got - and on the Switch stderr goes nowhere, so that log is all
       there is. Do NOT "fix" this by logging from here: EnterCriticalSection
       inside a handler is the deadlock the comment above is about. */
    static const char nb[] = "\n[PILOTS] (no backtrace on this platform)\n";
    wr = write(2, nb, sizeof nb - 1);
    (void)wr;
#endif
    _exit(128 + sig);
}

static void DbgInstallExceptionHandler(void)
{
    signal(SIGSEGV, DbgSignalHandler);
    signal(SIGBUS, DbgSignalHandler);
    signal(SIGFPE, DbgSignalHandler);
    signal(SIGILL, DbgSignalHandler);
    signal(SIGABRT, DbgSignalHandler);
#ifdef PILOTS_HAVE_BACKTRACE
    DBG_MSG("POSIX signal handlers installed (backtrace on crash)");
#else
    DBG_MSG("POSIX signal handlers installed (no backtrace; the log tail is the trace)");
#endif
}

#endif /* _WIN32 */

#else /* !_DEBUG: дампа нет, но сам лог живой — см. DbgPrintf выше */

/* Единственное, что в release остаётся no-op: крах-дамп регистров/стека.
   DBG_* и DbgPrintf в release скомпилированы и включаются ключом -log. */
#define DbgInstallExceptionHandler() ((void)0)

#endif /* _DEBUG */

#endif /* DEBUG_LOG_H */
