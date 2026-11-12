// Freeze diagnostics: startup stages, monitors/adapters, and a hang watchdog.
// The watchdog never sends messages to game windows: it only polls IsHungAppWindow once a second.
// When a window of this process stops responding it logs the stuck thread's stack (return addresses
// as module+offset), the last stage reached, and writes one minidump next to the log.
#include <cstdarg>
static std::atomic<const char*> stage{"starting"};       // last startup/reset step reached
static std::atomic<const char*> renderSpot{"none"};      // where the render thread is inside Present
static std::atomic<DWORD> renderThreadId{0};

static void LogF(const char* fmt,...) {
    FILE* f=nullptr; _wfopen_s(&f,logPath,L"a"); if(!f) return;
    fprintf(f,"%llu: [t%lu] ",GetTickCount64(),GetCurrentThreadId());
    va_list a; va_start(a,fmt); vfprintf(f,fmt,a); va_end(a); fputc('\n',f); fclose(f);
}
static void Stage(const char* s) {stage=s; LogF("Stage: %s",s);}

static BOOL CALLBACK LogMonitorProc(HMONITOR m,HDC,LPRECT,LPARAM) {
    MONITORINFOEXW mi={}; mi.cbSize=sizeof(mi); GetMonitorInfoW(m,&mi);
    using DpiFn=HRESULT (WINAPI*)(HMONITOR,int,UINT*,UINT*);
    static auto dpiFor=(DpiFn)GetProcAddress(LoadLibraryW(L"shcore.dll"),"GetDpiForMonitor");
    UINT dx=0,dy=0; if(dpiFor) dpiFor(m,0,&dx,&dy);
    const RECT& r=mi.rcMonitor;
    LogF("Monitor %p %ls: %ld,%ld %ldx%ld%s, dpi %u",(void*)m,mi.szDevice,r.left,r.top,r.right-r.left,r.bottom-r.top,
         (mi.dwFlags&MONITORINFOF_PRIMARY)?" primary":"",dx);
    return TRUE;
}
static void LogDisplays(IDirect3D9* api) {
    LogF("Monitors: %d (thread DPI awareness %d)",GetSystemMetrics(SM_CMONITORS),
         (int)GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()));
    EnumDisplayMonitors(nullptr,nullptr,LogMonitorProc,0);
    if(!api) return;
    for(UINT i=0;i<api->GetAdapterCount();++i) {
        D3DADAPTER_IDENTIFIER9 id={}; api->GetAdapterIdentifier(i,0,&id);
        LogF("D3D adapter %u: %s on %s, monitor %p",i,id.Description,id.DeviceName,(void*)api->GetAdapterMonitor(i));
    }
}
static void LogWindow(const char* what,HWND h) {
    RECT r={}; GetWindowRect(h,&r);
    LogF("%s %p: thread %lu, monitor %p, %ld,%ld %ldx%ld, style %08lx",what,(void*)h,GetWindowThreadProcessId(h,nullptr),
         (void*)MonitorFromWindow(h,MONITOR_DEFAULTTONULL),r.left,r.top,r.right-r.left,r.bottom-r.top,(DWORD)GetWindowLongPtrW(h,GWL_STYLE));
}

// ---- watchdog ----
static bool LooksLikeReturn(uintptr_t a) {
    MEMORY_BASIC_INFORMATION m;
    if(a<0x10000||!VirtualQuery((void*)a,&m,sizeof(m))||m.State!=MEM_COMMIT) return false;
    if(!(m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return false;
    if(a-(uintptr_t)m.BaseAddress<7) return false;
    const BYTE* p=(const BYTE*)a;   // preceded by a call instruction (E8 rel32, or FF /2 forms)
    return p[-5]==0xE8||p[-2]==0xFF||p[-3]==0xFF||p[-6]==0xFF||p[-7]==0xFF;
}
static void Where(uintptr_t a,char* out,size_t n) {
    HMODULE mod=nullptr; wchar_t path[MAX_PATH]=L"?";
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)a,&mod))
        GetModuleFileNameW(mod,path,MAX_PATH);
    const wchar_t* name=wcsrchr(path,L'\\'); name=name?name+1:path;
    snprintf(out,n,"%08x %ls+0x%x",(unsigned)a,name,(unsigned)(a-(uintptr_t)mod));
}
static DWORD stackWords[8192];
// Copies the thread's stack while it is suspended (no locks, no heap), then logs return addresses.
static void LogThreadStack(DWORD tid,const char* who) {
    if(!tid||tid==GetCurrentThreadId()) return;
    HANDLE t=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT,FALSE,tid);
    if(!t) {LogF("Stack of %s thread %lu: cannot open",who,tid);return;}
    CONTEXT c={}; c.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER; size_t n=0; bool got=false;
    if(SuspendThread(t)!=(DWORD)-1) {
        got=GetThreadContext(t,&c)!=0;
        MEMORY_BASIC_INFORMATION m;
        if(got&&VirtualQuery((void*)c.Esp,&m,sizeof(m))&&m.State==MEM_COMMIT) {
            n=std::min<size_t>(8192,((uintptr_t)m.BaseAddress+m.RegionSize-c.Esp)/4);
            memcpy(stackWords,(void*)c.Esp,n*4);
        }
        ResumeThread(t);
    }
    CloseHandle(t);
    if(!got) {LogF("Stack of %s thread %lu: no context",who,tid);return;}
    char at[300]; Where(c.Eip,at,sizeof(at));
    LogF("Stack of %s thread %lu: eip %s, esp %08lx, eax %08lx",who,tid,at,c.Esp,c.Eax);
    for(size_t i=0,shown=0;i<n&&shown<60;++i) if(LooksLikeReturn(stackWords[i])) {
        Where(stackWords[i],at,sizeof(at)); LogF("  esp+%04x: %s",(unsigned)(i*4),at); ++shown;
    }
}
using DumpFn=BOOL (WINAPI*)(HANDLE,DWORD,HANDLE,int,void*,void*,void*);
static DumpFn writeDump;
static void WriteHangDump(int number) {
    if(!writeDump) {LogF("Hang dump: dbghelp not available");return;}
    wchar_t path[MAX_PATH]; wcscpy_s(path,logPath);
    if(wchar_t* dot=wcsrchr(path,L'.')) swprintf_s(dot,MAX_PATH-(dot-path),L".hang%d.dmp",number);
    HANDLE f=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) {LogF("Hang dump: cannot create file");return;}
    // MiniDumpNormal | WithUnloadedModules | WithThreadInfo: every thread's stack, small file.
    const BOOL ok=writeDump(GetCurrentProcess(),GetCurrentProcessId(),f,0x1020,nullptr,nullptr,nullptr);
    CloseHandle(f); LogF("Hang dump %ls: %s",path,ok?"written":"failed");
}
struct HungSearch {HWND window; int windows;};
static BOOL CALLBACK FindHungWindow(HWND h,LPARAM p) {
    DWORD pid=0; GetWindowThreadProcessId(h,&pid);
    if(pid!=GetCurrentProcessId()||!IsWindowVisible(h)) return TRUE;
    auto& s=*(HungSearch*)p; ++s.windows;
    if(!s.window&&IsHungAppWindow(h)) s.window=h;
    return TRUE;
}
static DWORD WINAPI Watchdog(void*) {
    writeDump=(DumpFn)GetProcAddress(LoadLibraryW(L"dbghelp.dll"),"MiniDumpWriteDump");   // load now, not while hung
    int dumps=0; bool reported=false; int monitors=GetSystemMetrics(SM_CMONITORS);
    for(;;) {
        Sleep(1000);
        const int nowMonitors=GetSystemMetrics(SM_CMONITORS);
        if(nowMonitors!=monitors) {LogF("Monitor count changed: %d -> %d",monitors,nowMonitors); monitors=nowMonitors;}
        HungSearch s={}; EnumWindows(FindHungWindow,(LPARAM)&s);
        if(!s.window) {if(reported) LogF("Watchdog: windows respond again"); reported=false; continue;}
        if(reported) continue;
        reported=true;
        const DWORD hungThread=GetWindowThreadProcessId(s.window,nullptr);
        wchar_t cls[64]=L""; GetClassNameW(s.window,cls,64);
        LogF("WATCHDOG: window %p (%ls, thread %lu) not responding. stage=%s, render=%s, frames=%u, monitors=%d",
             (void*)s.window,cls,hungThread,stage.load(),renderSpot.load(),frames,monitors);
        LogF("Threads: game window %p thread %lu, render %lu, output windows %lu, foreground %p",(void*)gameWindow,
             gameWindow?GetWindowThreadProcessId(gameWindow,nullptr):0,renderThreadId.load(),uiThreadId,(void*)GetForegroundWindow());
        LogThreadStack(hungThread,"hung window");
        if(renderThreadId&&renderThreadId!=hungThread) LogThreadStack(renderThreadId,"render");
        if(uiThreadId&&uiThreadId!=hungThread) LogThreadStack(uiThreadId,"output window");
        if(dumps<2) WriteHangDump(++dumps);
    }
}
static void StartWatchdog() {if(HANDLE t=CreateThread(nullptr,0,Watchdog,nullptr,0,nullptr)) CloseHandle(t);}
