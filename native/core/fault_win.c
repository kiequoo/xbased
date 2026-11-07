/*
 * hakuX native core — the core's faults on Windows: a vectored exception
 * handler hands access violations to nc_core_fault (MMIO, the dirty
 * tracking, a title's pager, a guest's own page fault) as Android's
 * SIGSEGV handler does, the exception's CONTEXT as the ucontext. Anything
 * else, and faults that aren't the core's, go on to whoever handles them
 * (structured exception handlers: the Vulkan driver's own probes among
 * them), then to the front end's report.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include "nc_winapi.h"  /* compat/linux: the vectored handlers over SIGSEGV */
#include <signal.h>
#endif

#include "native/core/core.h"

static NcCore *g_fault_core;

static LONG CALLBACK on_exception(PEXCEPTION_POINTERS ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    siginfo_t info;
    memset(&info, 0, sizeof(info));
#ifdef _WIN32
    if (g_fault_core && (er->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO || er->ExceptionCode == EXCEPTION_INT_OVERFLOW)) {
        /* a guest DIV/IDIV's #DE in translated code */
        info.si_signo = SIGFPE;
        return nc_core_fault(g_fault_core, SIGFPE, &info, ep->ContextRecord) ? EXCEPTION_CONTINUE_EXECUTION
                                                                             : EXCEPTION_CONTINUE_SEARCH;
    }
#endif
    if (!g_fault_core || er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    info.si_signo = SIGSEGV;
    info.si_addr = (void *)er->ExceptionInformation[1];
#ifdef _WIN32
    info.nc_write = er->ExceptionInformation[0] == 1;
#endif
    nc_cpu_fault_note_write(er->ExceptionInformation[0] == 1);
    return nc_core_fault(g_fault_core, SIGSEGV, &info, ep->ContextRecord) ? EXCEPTION_CONTINUE_EXECUTION
                                                                           : EXCEPTION_CONTINUE_SEARCH;
}

#ifdef _WIN32
/* A crash nobody handled: where (this executable's offsets, for the map or
 * llvm-symbolizer), and the frames under it, in the log. */
static LONG WINAPI on_crash(PEXCEPTION_POINTERS ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    HMODULE self = GetModuleHandleW(NULL);
    CONTEXT c = *ep->ContextRecord;
    nc_log("hakuX-native", "crash: exception %08lx at %p (exe+%llx), address %p, thread %lu",
           er->ExceptionCode, er->ExceptionAddress, (unsigned long long)((uintptr_t)er->ExceptionAddress - (uintptr_t)self),
           er->NumberParameters > 1 ? (void *)er->ExceptionInformation[1] : NULL, GetCurrentThreadId());
    for (int k = 0; k < 24 && c.Rip; k++) {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)c.Rip, &m);
        if (m) {
            GetModuleFileNameA(m, name, sizeof(name));
        }
        const char *base = strrchr(name, '\\');
        nc_log("hakuX-native", "crash:   #%d %s+%llx", k, base ? base + 1 : name,
               (unsigned long long)(c.Rip - (uintptr_t)m));
        DWORD64 img = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &img, NULL);
        if (!fe) {
            if (IsBadReadPtr((void *)c.Rsp, 8)) {
                break;
            }
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        } else {
            void *hd = NULL;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, c.Rip, fe, &c, &hd, &est, NULL);
        }
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

void nc_core_fault_install(NcCore *c)
{
    static void *handler;
    g_fault_core = c;
    if (!handler) {
        /* behind the translator's own (the interrupt poll), in front of the rest */
        handler = AddVectoredExceptionHandler(1, on_exception);
#ifdef _WIN32
        SetUnhandledExceptionFilter(on_crash);
#endif
    }
}
