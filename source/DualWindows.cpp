#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <atomic>
#include "minhook/include/MinHook.h"
// clang/MinGW (zig build) won't implicitly convert function pointers to LPVOID like MSVC does.
template<class F> static MH_STATUS MH_CreateHook(LPVOID target,F* detour,LPVOID* original){return MH_CreateHook(target,(LPVOID)detour,original);}

// One game/device, two additional presentation surfaces. No input emulation.
static HMODULE selfModule;
static HWND gameWindow, views[2];
static IDirect3DSwapChain9* chains[2];
static UINT chainW[2], chainH[2];
static std::atomic<bool> enabled{true};
static bool manualSplit=false, testMode=false;
static std::atomic<bool> uiReady{false};
static std::atomic<bool> makeBorderless{false};
static std::atomic<UINT> uiWidth[2],uiHeight[2];
static std::atomic<bool> uiMinimized[2];
static DWORD uiThreadId=0;
static HANDLE uiThread=nullptr;
static SRWLOCK layoutLock=SRWLOCK_INIT;
static RECT playerRects[2];
static bool rectReady[2]={false,false};
static bool shown=false, oldF8=false, oldF9=false;
static LONG started=0;
static uintptr_t modeRoot=0;
static UINT frames=0, successes=0, lastWidth=0, lastHeight=0;
static UINT pixelChecks=0;
static UINT sourceWidth=960,sourceHeight=1080;
// What each player window shows: back-buffer rectangle src drawn into client rectangle dst (menu cursor mapping).
struct ViewMap {RECT src,dst; bool valid;};
static ViewMap viewMap[2];
static SRWLOCK viewMapLock=SRWLOCK_INIT;
static std::atomic<bool> sensitivityPanelOpen{false};
static RECT lastGameClip{},panelClip{}; static bool haveGameClip=false,panelClipped=false;   // game's ClipCursor rect / F3 panel's
static void ReleaseSensitivityPanel(); static int SensitivityPanelWindow();
static bool DrawSensitivityPanel(IDirect3DDevice9* d,IDirect3DSurface9* dest,const RECT& picture,int window,bool active);
static RECT PanelRect(const RECT& picture,float s); static float PanelScale(const RECT& picture);   // F3 panel: the mouse + keyboard player's devices go to the panel
static BYTE* fusionModule=nullptr;
static wchar_t logPath[MAX_PATH];
using PresentFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
using ResetFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
static PresentFn realPresent;
// Player windows are presented with d3d9's own swap-chain Present, hooked here at startup before any overlay
// (Steam, Discord) hooks it. All three freeze dumps (2026-11-12) had the render thread inside overlay code that
// had hooked a player-window Present, waiting for the Direct3D lock held by a game loading thread stuck in the
// NVIDIA driver (CreateTexture). The overlays still draw on the game itself; they just never touch these windows.
using ChainPresentFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3DSwapChain9*,const RECT*,const RECT*,HWND,const RGNDATA*,DWORD);
static ChainPresentFn rawChainPresent;
static HRESULT STDMETHODCALLTYPE HookChainPresent(IDirect3DSwapChain9* s,const RECT* a,const RECT* b,HWND h,const RGNDATA* r,DWORD f) {
    return rawChainPresent(s,a,b,h,r,f);   // everyone else: unchanged
}
// DrawViews (and the swap chains it owns) runs on one thread at a time: the game presents from its render
// thread AND, on WM_PAINT, from its main thread (exe+0x8ca8f0).
static SRWLOCK drawLock=SRWLOCK_INIT;
static ResetFn realReset;
static PresentFn objectPresent;
static ResetFn objectReset;
static void* presentTarget;
static void* resetTarget;
static thread_local int presentDepth=0;
static void AttachDevice(IDirect3DDevice9* d);
static IDirect3DSurface9* movieSnapshot=nullptr;

static void Log(const char* text, unsigned a=0, unsigned b=0) {
    FILE* f=nullptr; _wfopen_s(&f,logPath,L"a");
    if(f) { fprintf(f,"%llu: ",GetTickCount64()); fprintf(f,text,a,b); fputc('\n',f); fclose(f); }
}
#include "Diagnostics.h"
// A Direct3D object still referenced after we release it makes the next device Reset fail (D3DERR_INVALIDCALL).
template<class T> static void ReleaseNow(T*& p,const char* what) {
    if(!p) return; const ULONG left=p->Release(); p=nullptr;
    if(left) LogF("%s still referenced %lu time(s) after release: a device Reset now fails",what,left);
}
static int GameMode();
// F1: swap which character each display's controllers drive, and swap the two output windows.
static std::atomic<bool> swapPlayers{false};
using PadFn=DWORD (WINAPI*)(DWORD,void*);
using CapsFn=DWORD (WINAPI*)(DWORD,DWORD,void*);
static PadFn realGetState,realGetStateEx,realSetState;
static CapsFn realGetCaps;
#include "InputSetup.h"
static void CreateInputSwapHooks() {
    HMODULE x=LoadLibraryW(L"xinput1_3.dll");
    if(!x){Log("xinput1_3.dll not found; F1 swaps windows only");return;}
    struct {LPCSTR name;LPVOID detour;LPVOID* real;} hooks[]={
        {"XInputGetState",(LPVOID)RouteGetState,(LPVOID*)&realGetState},
        {MAKEINTRESOURCEA(100),(LPVOID)RouteGetStateEx,(LPVOID*)&realGetStateEx}, // hidden GetStateEx (guide button)
        {"XInputSetState",(LPVOID)RouteSetState,(LPVOID*)&realSetState},
        {"XInputGetCapabilities",(LPVOID)RouteGetCaps,(LPVOID*)&realGetCaps}};
    unsigned ok=0;
    for(auto& h:hooks) if(FARPROC f=GetProcAddress(x,h.name)) ok+=MH_CreateHook((LPVOID)f,h.detour,h.real)==MH_OK;
    Log("XInput routing hooks created: %u of 4",ok);
}
#include "MovieFrames.h"
#include "HudFix.h"
#include "SharedGui.h"

// Healing gauge: two GUI classes lay out their ring for the staggered split screen
// ("if split && !flag: x=-97/-210, scale 1.4"). v0.4.8 only patched the first
// (vtable 0139b998 at 0x8e99cc); the second (0139baf0 at 0x8e9e9e) stayed split,
// which is why the fix only worked some of the time. je -> jmp on both.
static void SetCenteredHealLayout(bool centered) {
    BYTE* exe=reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    struct Site {uintptr_t rva;BYTE prefix[7];BYTE suffix[8];} sites[]={
        {0x4e99cc,{0xe8,0xff,0xb9,0x06,0,0x84,0xc0},{0x58,0x80,0xbb,0xb1,0x02,0,0,0}},
        {0x4e9e9e,{0xe8,0x2d,0xb5,0x06,0,0x84,0xc0},{0x6f,0x80,0xbf,0xb1,0x02,0,0,0}}};
    for(auto& s:sites) {
        BYTE* branch=exe+s.rva+sizeof(s.prefix);
        if(memcmp(exe+s.rva,s.prefix,sizeof(s.prefix))||memcmp(branch+1,s.suffix,sizeof(s.suffix))||
           (*branch!=0x74&&*branch!=0xeb)) {Log("Healing gauge layout signature did not match at %x",s.rva);continue;}
        const BYTE opcode=centered?0xeb:0x74;
        if(*branch==opcode) continue;
        DWORD protection=0;
        if(!VirtualProtect(branch,1,PAGE_EXECUTE_READWRITE,&protection)) continue;
        *branch=opcode;
        VirtualProtect(branch,1,protection,&protection);
        FlushInstructionCache(GetCurrentProcess(),branch,1);
        Log(centered?"Centered healing gauge layout enabled at %x":"Original healing gauge layout restored at %x",s.rva);
    }
}

static void SetCenteredPuzzleLayout(bool centered) {
    // uGUIPuzzle::Update selects a translated, 1.5x graphic for the
    // original staggered split-screen. Use the native centered branch
    // when DualWindows presents separate views.
    auto base=reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto code=base+0x51053b;
    constexpr BYTE prefix[]={0x83,0xb9,0xf0,0x08,0,0,0x01};
    constexpr BYTE suffix[]={0x67,0x83,0xbe,0xac,0x02,0,0,0};
    BYTE* branch=code+sizeof(prefix);
    if(memcmp(code,prefix,sizeof(prefix))||memcmp(branch+1,suffix,sizeof(suffix))||
       (*branch!=0x75&&*branch!=0xeb)) {
        Log("Chest puzzle layout signature did not match");
        return;
    }
    const BYTE opcode=centered?0xeb:0x75;
    if(*branch==opcode) return;
    DWORD protection=0;
    if(!VirtualProtect(branch,1,PAGE_EXECUTE_READWRITE,&protection)) return;
    *branch=opcode;
    VirtualProtect(branch,1,protection,&protection);
    FlushInstructionCache(GetCurrentProcess(),branch,1);
    Log(centered?"Centered chest puzzle layout enabled":"Original chest puzzle layout restored");
}
static void SetFusionDimensions(bool split) {
    if(!fusionModule)return;
    // Keep the physical canvas at its normal 16:9 resolution. Only the
    // FusionFix HUD/aspect calculations see the narrower per-player layout.
    *(UINT*)(fusionModule+0x1099c8)=split?sourceWidth/2:sourceWidth;
    *(UINT*)(fusionModule+0x1099cc)=sourceHeight;
}
static void __stdcall SetHalf(void* out,int32_t* rect,int player) {
    SetFusionDimensions(true);
    // 1280x720 virtual canvas: two 640x360 views in its left half.
    // Capture the converter's actual output; NEVER assume backbuffer halves.
    rect[0]=0;rect[1]=player*360;rect[2]=640;rect[3]=(player+1)*360;
    using Convert=void (__stdcall*)(void*,int32_t*);
    auto convert=(Convert)((BYTE*)GetModuleHandleW(nullptr)+0xac310);
    convert(out,rect);
    RECT result=*(RECT*)out;
    AcquireSRWLockExclusive(&layoutLock);
    playerRects[player]=result;rectReady[player]=true;
    ReleaseSRWLockExclusive(&layoutLock);
    Log(player?"Player 2 converted origin: %u,%u":"Player 1 converted origin: %u,%u",result.left,result.top);
    Log(player?"Player 2 converted extent: %u,%u":"Player 1 converted extent: %u,%u",result.right,result.bottom);
}
static void __stdcall TopHalf(void* out,int32_t* rect){SetHalf(out,rect,0);}
static void __stdcall BottomHalf(void* out,int32_t* rect){SetHalf(out,rect,1);}
static void ConfigureFusion() {
    auto mod=(BYTE*)GetModuleHandleW(L"ResidentEvilRevelations2.FusionFix.asi");
    if(!mod||mod[0x1742b]!=0x76||mod[0x1748b]!=0x76)return;
    BYTE* exe=(BYTE*)GetModuleHandleW(nullptr);
    BYTE* sites[]={exe+0xae6e8,exe+0xae732};
    void* replacements[]={(void*)TopHalf,(void*)BottomHalf};
    for(auto site:sites) {
        if(site[0]!=0xe8)return;
        BYTE* target=site+5+*(int32_t*)(site+1);
        if(target<mod||target>=mod+0x120000)return;
    }
    fusionModule=mod;
    InstallHudFix();
    InstallMovieDetection();
    SetCenteredPuzzleLayout(enabled.load());
    SetCenteredHealLayout(enabled.load());
    for(int i=0;i<2;++i) {DWORD old;VirtualProtect(sites[i],5,PAGE_EXECUTE_READWRITE,&old);
        *(int32_t*)(sites[i]+1)=(BYTE*)replacements[i]-(sites[i]+5);
        VirtualProtect(sites[i],5,old,&old);FlushInstructionCache(GetCurrentProcess(),sites[i],5);
    }
    Log("v0.2 native viewport capture installed; FusionFix disk file unchanged");
}
static void ReleaseChains() {
    AcquireSRWLockExclusive(&drawLock);   // never free a swap chain another thread is presenting
    ReleaseNow(movieSnapshot,"Movie copy");
    movieDrawn.store(false);
    lastFrameWasMovie.store(false);
    ReleaseSharedGui();
    ReleaseSensitivityPanel();
    for(int i=0;i<2;++i) { ReleaseNow(chains[i],i?"Player 2 swap chain":"Player 1 swap chain"); chainW[i]=chainH[i]=0; }
    ReleaseSRWLockExclusive(&drawLock);
}
static int GameMode() {
    if(testMode || manualSplit) return 1;
    // ReadProcessMemory instead of __try: same fault safety, also builds with clang/MinGW.
    uintptr_t p=0; int mode=-1; SIZE_T n=0; HANDLE self=GetCurrentProcess();
    if(modeRoot&&ReadProcessMemory(self,(void*)modeRoot,&p,sizeof(p),&n)&&p&&
       ReadProcessMemory(self,(void*)(p+0x8f4),&mode,sizeof(mode),&n)) return mode;
    return -1;
}
static void FindGameMode() {
    auto base=(BYTE*)GetModuleHandleW(nullptr);
    auto nt=(IMAGE_NT_HEADERS*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec=IMAGE_FIRST_SECTION(nt);
    // Read-only signature from the public FusionFix game-mode discovery.
    const BYTE p[]={0x8b,0x0d,0,0,0,0,0x55,0xe8,0,0,0,0,0x39,0x86,0xe0,0x0c,0,0};
    const char mask[]="xx????xx????xxxxxx";
    int matches=0; uintptr_t found=0;
    for(unsigned s=0;s<nt->FileHeader.NumberOfSections;++s) {
        if(!(sec[s].Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE* code=base+sec[s].VirtualAddress;
        for(unsigned j=0;j+sizeof(p)<=sec[s].Misc.VirtualSize;++j) {
            bool ok=true;
            for(unsigned k=0;k<sizeof(p);++k) if(mask[k]=='x'&&code[j+k]!=p[k]) {ok=false;break;}
            if(ok) {found=*(uintptr_t*)(code+j+2);++matches;}
        }
    }
    if(matches==1) { modeRoot=found; Log("Game-mode signature found at %08x",(unsigned)found); }
    else Log("Game-mode signature matches: %u; F9 manual fallback available",matches);
}
static int ViewIndex(HWND h) {return h==views[1]?1:0;}
static constexpr WPARAM FillMonitorCommand=0x1100;
static constexpr WPARAM FirstSizeCommand=0x1110;
static constexpr WPARAM NextSizeCommand=0x1120;
static constexpr WPARAM NextMonitorCommand=0x1130;
static constexpr int outputWidths[]={1280,1600,1920,2560,3840};
static RECT restoredWindow[2];
static bool monitorFilled[2];
static size_t nextSize[2];

static void ToggleMonitorFill(HWND window) {
    const int i=ViewIndex(window);
    if(!monitorFilled[i]) {
        MONITORINFO monitor={sizeof(monitor)};
        if(!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)) return;
        if(IsZoomed(window)||IsIconic(window)) ShowWindow(window,SW_SHOWNOACTIVATE);
        GetWindowRect(window,&restoredWindow[i]);
        const RECT& r=monitor.rcMonitor;
        SetWindowLongPtrW(window,GWL_STYLE,WS_POPUP|WS_VISIBLE);
        SetWindowPos(window,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,
                     SWP_NOACTIVATE|SWP_NOZORDER|SWP_FRAMECHANGED);
    } else {
        const RECT& r=restoredWindow[i];
        SetWindowLongPtrW(window,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE);
        SetWindowPos(window,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,
                     SWP_NOACTIVATE|SWP_NOZORDER|SWP_FRAMECHANGED);
    }
    monitorFilled[i]=!monitorFilled[i];
}

static BOOL CALLBACK CollectMonitor(HMONITOR monitor,HDC,LPRECT,LPARAM data) {
    reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor);
    return TRUE;
}

static void MoveToNextMonitor(HWND window) {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr,nullptr,CollectMonitor,reinterpret_cast<LPARAM>(&monitors));
    if(monitors.size()<2) return;
    const auto current=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
    const auto index=std::find(monitors.begin(),monitors.end(),current)-monitors.begin();
    MONITORINFO target={sizeof(target)};
    if(!GetMonitorInfoW(monitors[(index+1)%monitors.size()],&target)) return;
    const bool wasFilled=monitorFilled[ViewIndex(window)];
    if(wasFilled) ToggleMonitorFill(window);
    SetWindowPos(window,nullptr,target.rcWork.left,target.rcWork.top,0,0,
                 SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
    if(wasFilled) ToggleMonitorFill(window);
}

static LRESULT CALLBACK ViewProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    // The game reads mouse + keyboard only while its own window is foreground (exe+0xa91f3a), and the
    // player windows cover it, so a click on a player window gives focus back to the game window.
    if(msg==WM_MOUSEACTIVATE) {if(IsWindow(gameWindow)&&GetForegroundWindow()!=gameWindow) SetForegroundWindow(gameWindow); return MA_NOACTIVATE;}
    if(msg==WM_SYSCOMMAND) {
        if(w==NextMonitorCommand) {MoveToNextMonitor(h);return 0;}
        if(w==NextSizeCommand) w=FirstSizeCommand+nextSize[ViewIndex(h)];
        if(w==FillMonitorCommand) {ToggleMonitorFill(h);return 0;}
        if(w>=FirstSizeCommand&&w<FirstSizeCommand+sizeof(outputWidths)/sizeof(outputWidths[0])) {
            if(monitorFilled[ViewIndex(h)]) ToggleMonitorFill(h);
            if(IsZoomed(h)||IsIconic(h)) ShowWindow(h,SW_SHOWNOACTIVATE);
            const int width=outputWidths[w-FirstSizeCommand];
            nextSize[ViewIndex(h)]=(w-FirstSizeCommand+1)%(sizeof(outputWidths)/sizeof(outputWidths[0]));
            RECT outer={0,0,width,width*9/16};
            AdjustWindowRectExForDpi(&outer,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_NOACTIVATE,GetDpiForWindow(h));
            SetWindowPos(h,nullptr,0,0,outer.right-outer.left,outer.bottom-outer.top,
                         SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
            return 0;
        }
    }
    if(msg==WM_CLOSE) {enabled=false;ShowWindow(views[0],SW_HIDE);ShowWindow(views[1],SW_HIDE);return 0;}
    if(msg==WM_ERASEBKGND) return 1;
    if(msg==WM_DISPLAYCHANGE&&h==views[0]) {LogF("Display change: %ux%u, monitors %d",LOWORD(l),HIWORD(l),GetSystemMetrics(SM_CMONITORS));}
    if(msg==WM_DPICHANGED&&!monitorFilled[ViewIndex(h)]) {
        const auto& r=*reinterpret_cast<const RECT*>(l);
        SetWindowPos(h,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_NOZORDER);
        return 0;
    }
    if(msg==WM_SIZE) {int i=ViewIndex(h);uiWidth[i]=LOWORD(l);uiHeight[i]=HIWORD(l);uiMinimized[i]=w==SIZE_MINIMIZED;return 0;}
    if(msg==WM_APP+1) {for(auto v:views)if(v)ShowWindow(v,w?SW_SHOWNOACTIVATE:SW_HIDE);return 0;}
    if(msg==WM_APP+2&&IsWindow(gameWindow)) {   // game window: no frame, covering its monitor
        MONITORINFO mi={sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(gameWindow,MONITOR_DEFAULTTONEAREST),&mi);
        const RECT& m=mi.rcMonitor; RECT now; GetWindowRect(gameWindow,&now);
        const LONG_PTR style=GetWindowLongPtrW(gameWindow,GWL_STYLE);
        if((style&(WS_CAPTION|WS_THICKFRAME))||!EqualRect(&now,&m)) {
            SetWindowLongPtrW(gameWindow,GWL_STYLE,(style&~(WS_OVERLAPPEDWINDOW|WS_MAXIMIZE))|WS_POPUP|WS_VISIBLE);
            SetWindowPos(gameWindow,nullptr,m.left,m.top,m.right-m.left,m.bottom-m.top,
                SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED|SWP_ASYNCWINDOWPOS);
            Log("Game window made borderless: %u x %u",m.right-m.left,m.bottom-m.top); LogWindow("Game window",gameWindow);
        }
        return 0;
    }
    if(msg==WM_TIMER&&!IsWindow(gameWindow)) {for(auto v:views)if(v)DestroyWindow(v);PostQuitMessage(0);return 0;}
    if(msg==WM_SIZING) {
        RECT* r=(RECT*)l; RECT frame={0,0,0,0};
        AdjustWindowRectEx(&frame,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_NOACTIVATE);
        int fw=frame.right-frame.left,fh=frame.bottom-frame.top;
        int cw=std::max(320L,r->right-r->left-fw), ch=(cw*9+8)/16;
        if(w==WMSZ_TOP || w==WMSZ_TOPLEFT || w==WMSZ_TOPRIGHT) r->top=r->bottom-ch-fh;
        else r->bottom=r->top+ch+fh;
        return TRUE;
    }
    return DefWindowProcW(h,msg,w,l);
}
// Monitors in Windows' own numbering (\\.\DISPLAY1, 2, ...), so the "Display 1" window opens on
// Windows display 1: EnumDisplayMonitors order is not guaranteed.
static BOOL CALLBACK MonitorProc(HMONITOR m,HDC,LPRECT,LPARAM p) {
    MONITORINFOEXW info={}; info.cbSize=sizeof(info); GetMonitorInfoW(m,&info);
    const wchar_t* n=info.szDevice+wcslen(info.szDevice); while(n>info.szDevice&&iswdigit(n[-1])) --n;
    auto& list=*(std::vector<std::pair<int,RECT>>*)p; list.push_back({_wtoi(n),info.rcWork});
    std::sort(list.begin(),list.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    return TRUE;
}
static DWORD WINAPI WindowLoop(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc={}; wc.lpfnWndProc=ViewProc; wc.hInstance=selfModule;
    wc.lpszClassName=L"Rev2DualWindowsV2"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&wc);
    std::vector<std::pair<int,RECT>> monitors; EnumDisplayMonitors(nullptr,nullptr,MonitorProc,(LPARAM)&monitors);
    if(monitors.empty()) return 1;
    for(int i=0;i<2;++i) {
        RECT area=monitors[monitors.size()>1?i:0].second;
        int available=area.right-area.left;
        int width=std::min(1280,monitors.size()>1?available-32:available/2-24);
        width=std::max(320,std::min(width,(int)(area.bottom-area.top-100)*16/9));width=width/16*16;
        RECT r={0,0,width,width*9/16}; AdjustWindowRectEx(&r,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_NOACTIVATE);
        int x=area.left+12+(monitors.size()>1?0:i*available/2);
        views[i]=CreateWindowExW(WS_EX_NOACTIVATE,L"Rev2DualWindowsV2",
            i?L"RE Revelations 2 — Display 2 (v0.6.3)":L"RE Revelations 2 — Display 1 (v0.6.3)",   // same names as the dashboard
            WS_OVERLAPPEDWINDOW,x,area.top+30,r.right-r.left,r.bottom-r.top,gameWindow,nullptr,selfModule,nullptr);
        if(!views[i])return 2;
        if(HMENU menu=GetSystemMenu(views[i],FALSE)) {
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,MF_STRING,FillMonitorCommand,L"Fill monitor / restore");
            for(size_t j=0;j<sizeof(outputWidths)/sizeof(outputWidths[0]);++j) {
                wchar_t label[48];
                swprintf_s(label,L"Output %d × %d",outputWidths[j],outputWidths[j]*9/16);
                AppendMenuW(menu,MF_STRING,FirstSizeCommand+j,label);
            }
        }
        RECT client;GetClientRect(views[i],&client);uiWidth[i]=client.right;uiHeight[i]=client.bottom;
    }
    SetTimer(views[0],1,1000,nullptr);
    Log("Output window thread ready: %u",GetCurrentThreadId());
    LogWindow("Display 1 window",views[0]); LogWindow("Display 2 window",views[1]); Stage("output windows ready");
    uiReady.store(true,std::memory_order_release);
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    return 0;
}
static bool MakeWindows(IDirect3DDevice9* device) {
    if(uiReady.load(std::memory_order_acquire))return true;
    if(!uiThread) {
        D3DDEVICE_CREATION_PARAMETERS cp={}; if(FAILED(device->GetCreationParameters(&cp)))return false;
        gameWindow=cp.hFocusWindow;
        LogWindow("Game window",gameWindow); Stage("creating output windows");
        uiThread=CreateThread(nullptr,0,WindowLoop,nullptr,0,&uiThreadId);
        Log("Render thread: %u; game window thread: %u",GetCurrentThreadId(),GetWindowThreadProcessId(gameWindow,nullptr));
    }
    return false;
}
// Writes a 32-bit render target to a BMP next to the log (F5 diagnostic).
static void SaveSurface(IDirect3DDevice9* d,IDirect3DSurface9* s,const wchar_t* name) {
    D3DSURFACE_DESC ds; s->GetDesc(&ds);
    if(ds.Format!=D3DFMT_X8R8G8B8&&ds.Format!=D3DFMT_A8R8G8B8){Log("Capture skipped: format %u",ds.Format);return;}
    IDirect3DSurface9* rb=nullptr;
    if(FAILED(d->CreateOffscreenPlainSurface(ds.Width,ds.Height,ds.Format,D3DPOOL_SYSTEMMEM,&rb,nullptr)))return;
    D3DLOCKED_RECT lr;
    if(SUCCEEDED(d->GetRenderTargetData(s,rb))&&SUCCEEDED(rb->LockRect(&lr,nullptr,D3DLOCK_READONLY))) {
        wchar_t path[MAX_PATH]; wcscpy_s(path,logPath);
        if(wchar_t* slash=wcsrchr(path,L'\\')) wcscpy_s(slash+1,MAX_PATH-(slash+1-path),name);
        FILE* f=nullptr; _wfopen_s(&f,path,L"wb");
        if(f) {
            BITMAPINFOHEADER ih={sizeof(ih),(LONG)ds.Width,-(LONG)ds.Height,1,32,BI_RGB};
            BITMAPFILEHEADER fh={0x4d42};
            fh.bfOffBits=sizeof(fh)+sizeof(ih); fh.bfSize=fh.bfOffBits+ds.Width*4*ds.Height;
            fwrite(&fh,sizeof(fh),1,f); fwrite(&ih,sizeof(ih),1,f);
            for(UINT y=0;y<ds.Height;++y) fwrite((BYTE*)lr.pBits+y*lr.Pitch,4,ds.Width,f);
            fclose(f);
        }
        rb->UnlockRect();
    } else Log("Capture readback failed");
    rb->Release();
}
static void SetShown(bool on) {
    if(on==shown)return;shown=on;
    if(!on) {AcquireSRWLockExclusive(&viewMapLock); viewMap[0].valid=viewMap[1].valid=false; ReleaseSRWLockExclusive(&viewMapLock);}
    // Async only. Never synchronously call the UI thread from Direct3D hooks.
    if(views[0])PostMessageW(views[0],WM_APP+1,on,0);
    Log(on?"Output windows visible":"Original game view visible");
}
static D3DFORMAT chainFormat[2];
static UINT chainCreates=0;
static BOOL CALLBACK LargestMonitor(HMONITOR,HDC,LPRECT r,LPARAM p) {
    auto& s=*(SIZE*)p; s.cx=std::max(s.cx,r->right-r->left); s.cy=std::max(s.cy,r->bottom-r->top); return TRUE;
}
// Each player window gets ONE swap chain, big enough for any size the window takes (largest monitor, and at
// least 4096x2304, above the 3840x2160 output preset). Resizing, filling, moving or minimizing only changes the rectangle that
// is drawn and presented (D3DSWAPEFFECT_COPY + source/destination rect), so no Direct3D object is destroyed or
// created while the game runs. Recreating the chains on every resize (Ctrl+1/2, Ctrl+Alt+1/2, restore) preceded
// every loading-screen freeze and an NVIDIA driver heap-corruption crash (dumps 2026-11-12).
// w,h: the window's current client size, the area to draw.
static bool EnsureChain(IDirect3DDevice9* d,int i,D3DFORMAT format,UINT& w,UINT& h) {
    w=std::max(1u,uiWidth[i].load()); h=std::max(1u,uiHeight[i].load());
    if(chains[i]&&chainFormat[i]==format&&w<=chainW[i]&&h<=chainH[i]) return true;
    SIZE big={4096,2304};   // headroom: a 3840-wide preset window can get a client slightly wider than 3840
    EnumDisplayMonitors(nullptr,nullptr,LargestMonitor,(LPARAM)&big);
    const UINT cw=std::max<UINT>(w,big.cx), ch=std::max<UINT>(h,big.cy);
    if(chains[i]) {chains[i]->Release();chains[i]=nullptr;}
    D3DPRESENT_PARAMETERS pp={}; pp.Windowed=TRUE; pp.hDeviceWindow=views[i];
    pp.BackBufferWidth=cw; pp.BackBufferHeight=ch; pp.BackBufferFormat=format;
    pp.BackBufferCount=1; pp.SwapEffect=D3DSWAPEFFECT_COPY;
    pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT hr=d->CreateAdditionalSwapChain(&pp,&chains[i]);
    static HRESULT lastError=S_OK;
    if(FAILED(hr)) { if(hr!=lastError) LogF("CreateAdditionalSwapChain %d failed %08lx (window monitor %p)",i,hr,(void*)MonitorFromWindow(views[i],MONITOR_DEFAULTTONULL)); lastError=hr; return false; }
    LogF("Swap chain %d created: %ux%u (window %ux%u, %p on monitor %p)",i,cw,ch,w,h,(void*)views[i],(void*)MonitorFromWindow(views[i],MONITOR_DEFAULTTONULL));
    lastError=S_OK;
    chainW[i]=cw; chainH[i]=ch; chainFormat[i]=format; ++chainCreates; return true;
}
static void ForceCoop();
static void KeyboardForMainOnly();
static void SkipPaintPresent();
static void FocusFreeInput();
static void LogInputState();
static void CursorFollowsPlayerWindows();
static void PerPlayerActionGates();
static void LoadSensitivity();
static void ToggleSensitivityPanel();
static BYTE* gameBase;
static void DrawViews(IDirect3DDevice9* d) {
    ++frames;
    if(frames==1) {
        Stage("first frame");
        IDirect3DSurface9* initial=nullptr;
        if(SUCCEEDED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&initial))) {
            D3DSURFACE_DESC ds;initial->GetDesc(&ds);Log("Initial backbuffer %u x %u",ds.Width,ds.Height);initial->Release();
        }
    }
    if(!modeRoot&&!testMode&&frames==1) FindGameMode();
    if(!testMode&&frames==1) {   // AutoJoinPartner removed: patching exe+0x8b9507 crashed at startup (ERR09)
        makeBorderless=true; KeyboardForMainOnly(); SkipPaintPresent();
        if(!gameBase) gameBase=(BYTE*)GetModuleHandleW(nullptr);
        CursorFollowsPlayerWindows(); PerPlayerActionGates(); LoadSensitivity();
    }
    if(!testMode) {FocusFreeInput(); LogInputState();}
    if(!MakeWindows(d))return;
    if(makeBorderless.exchange(false)&&views[0]) PostMessageW(views[0],WM_APP+2,0,0);
    DWORD fgpid=0; GetWindowThreadProcessId(GetForegroundWindow(),&fgpid);
    const bool active=fgpid==GetCurrentProcessId();
    static int gameFocused=-1; const int focusedNow=GetForegroundWindow()==gameWindow;
    if(focusedNow!=gameFocused) {gameFocused=focusedNow; Log("Game window focused (mouse + keyboard live): %u",focusedNow);}
    SHORT key8=active?GetAsyncKeyState(VK_F8):0,key9=active?GetAsyncKeyState(VK_F9):0;
    const bool f8=key8&0x8000,f9=key9&0x8000;
    if((key8&1)||(f8&&!oldF8)) enabled=!enabled.load();
    if((key9&1)||(f9&&!oldF9)) {manualSplit=!manualSplit;Log("Manual split: %u",manualSplit);}
    oldF8=f8; oldF9=f9;
    if(active&&(GetAsyncKeyState(VK_F10)&1)){hudFit=!hudFit.load();Log("HUD fitting enabled: %u",hudFit.load());}
    const bool control=active&&(GetAsyncKeyState(VK_CONTROL)&0x8000);
    const bool shift=(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0;
    const bool alt=(GetAsyncKeyState(VK_MENU)&0x8000)!=0;
    for(int i=0;i<2;++i) {
        const SHORT key=GetAsyncKeyState('1'+i);
        if(control&&(key&1)&&!(shift&&alt)) {
            const WPARAM command=shift?NextSizeCommand:alt?NextMonitorCommand:FillMonitorCommand;
            PostMessageW(views[i],WM_SYSCOMMAND,command,0);
        }
    }
    // Single player has one character (shown on both displays): a swap there would only move the controller
    // off the free pad it needs to press Start and join, so F1 swaps only in co-op.
    if(active&&(GetAsyncKeyState(VK_F1)&1)) {
        if(CoopOn()||testMode) {swapPlayers=!swapPlayers.load();Log("Players swapped: %u",swapPlayers.load());}
        else Log("F1 ignored: co-op is not on yet (one character)");
    }
    if(active&&(GetAsyncKeyState(VK_F2)&1)) ForceCoop();
    if(active&&!testMode&&(GetAsyncKeyState(VK_F3)&1)) {LogF("F3 pressed: sensitivity panel %s",sensitivityPanelOpen.load()?"closing":"opening"); ToggleSensitivityPanel();}
    const int swap=MainView();
    const bool capture=active&&(GetAsyncKeyState(VK_F5)&1);
    int mode=GameMode();
    if(active&&(GetAsyncKeyState(VK_F7)&1)) {
        forceSharedFrame=!forceSharedFrame;
        Log(forceSharedFrame?"Manual shared frame enabled":"Manual shared frame disabled");
    }
    const bool detectedMovie=movieDrawn.exchange(false);
    const bool movie=(mode==1&&detectedMovie)||forceSharedFrame;
    if(lastFrameWasMovie.exchange(movie)!=movie) Log(movie?"Shared movie started":"Shared movie ended");
    const bool split=mode==1&&!movie;
    const bool hasSharedGui=sharedGuiDrawn.exchange(false);
    static int lastMode=-99;
    if(mode!=lastMode) {Log("Game mode: %u",mode);lastMode=mode;}
    IDirect3DSurface9* source=nullptr;
    if(FAILED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&source))) return;
    D3DSURFACE_DESC desc; source->GetDesc(&desc);
    sourceWidth=desc.Width;sourceHeight=desc.Height;
    if(!testMode&&!fusionModule)ConfigureFusion();
    static bool puzzleCentered=true;
    if(fusionModule&&puzzleCentered!=enabled.load()) {
        puzzleCentered=enabled.load();
        SetCenteredPuzzleLayout(puzzleCentered);
        SetCenteredHealLayout(puzzleCentered);
    }
    SetFusionDimensions(split);
    if(!enabled.load()) {SetShown(false);source->Release();return;}
    if(lastWidth!=desc.Width||lastHeight!=desc.Height) {
        if(movieSnapshot) {movieSnapshot->Release();movieSnapshot=nullptr;}
        lastWidth=desc.Width; lastHeight=desc.Height; Log("Source backbuffer %u x %u",lastWidth,lastHeight);
    }
    // Each native player rectangle must itself be 16:9; the full canvas stays standard.
    if(desc.Width*9!=desc.Height*16) {SetShown(false);source->Release();return;}
    RECT regions[2];bool ready[2];
    AcquireSRWLockShared(&layoutLock);
    for(int i=0;i<2;++i){regions[i]=playerRects[i];ready[i]=rectReady[i];}
    ReleaseSRWLockShared(&layoutLock);
    if(testMode){regions[0]={0,0,(LONG)desc.Width/2,(LONG)desc.Height/2};regions[1]={0,(LONG)desc.Height/2,(LONG)desc.Width/2,(LONG)desc.Height};ready[0]=ready[1]=true;}
    if(split&&(!ready[0]||!ready[1])){SetShown(false);source->Release();return;}
    const bool composite=split&&(hasSharedGui||testMode)&&CaptureSharedGui(d,source,desc);
    IDirect3DSurface9* frameSource=source;
    if(movie) {
        if(!movieSnapshot)
            d->CreateRenderTarget(desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&movieSnapshot,nullptr);
        // Some overlays draw into the current backbuffer during swap-chain Present.
        // Freeze the movie source before either player window presents it.
        if(movieSnapshot&&SUCCEEDED(d->StretchRect(source,nullptr,movieSnapshot,nullptr,D3DTEXF_NONE)))
            frameSource=movieSnapshot;
    }
    SetShown(true);
    for(int i=0;i<2;++i) {
        UINT cw=0,ch=0;
        if(uiMinimized[i].load()||!EnsureChain(d,i,desc.Format,cw,ch)) continue;
        const RECT area={0,0,(LONG)cw,(LONG)ch};   // the window's client area inside the larger back buffer
        IDirect3DSurface9* dest=nullptr; if(FAILED(chains[i]->GetBackBuffer(0,D3DBACKBUFFER_TYPE_MONO,&dest))) continue;
        d->ColorFill(dest,&area,D3DCOLOR_XRGB(0,0,0));
        RECT src=split?regions[i^swap]:RECT{0,0,(LONG)desc.Width,(LONG)desc.Height};
        int sw=src.right-src.left,sh=src.bottom-src.top;
        if(src.left<0||src.top<0||src.right>(LONG)desc.Width||src.bottom>(LONG)desc.Height||sw<=0||sh<=0||abs(sw*9-sh*16)>16){dest->Release();continue;}
        int w=cw,h=w*9/16;
        if(h>(int)ch) {h=ch; w=h*16/9;}
        int x=(cw-w)/2,y=(ch-h)/2;
        RECT dst={x,y,x+w,y+h};
        HRESULT hr=d->StretchRect(frameSource,&src,dest,&dst,D3DTEXF_LINEAR);
        if(SUCCEEDED(hr)) {AcquireSRWLockExclusive(&viewMapLock); viewMap[i]={src,dst,true}; ReleaseSRWLockExclusive(&viewMapLock);}
        if(SUCCEEDED(hr)&&composite) DrawSharedGui(d,dest,dst);
        if(SUCCEEDED(hr)&&sensitivityPanelOpen.load()&&i==SensitivityPanelWindow()) DrawSensitivityPanel(d,dest,dst,i,active);
        if(testMode&&SUCCEEDED(hr)&&!movie&&(pixelChecks&3)==3&&!(pixelChecks&0x10)) {
            // F3 panel: open it, read back BOTH player windows' swap chains: panel body colour must be in the
            // mouse + keyboard window's picture and must not be in the other one.
            static bool opened=false; static UINT seen=0;
            if(!opened) {opened=true; ToggleSensitivityPanel(); if(i==SensitivityPanelWindow()) DrawSensitivityPanel(d,dest,dst,i,active);}
            const float ps=PanelScale(dst); const RECT pr=PanelRect(dst,ps);
            IDirect3DSurface9* readback=nullptr; DWORD px=0;
            if(SUCCEEDED(d->CreateOffscreenPlainSurface(chainW[i],chainH[i],desc.Format,D3DPOOL_SYSTEMMEM,&readback,nullptr))) {
                D3DLOCKED_RECT lock;
                if(SUCCEEDED(d->GetRenderTargetData(dest,readback))&&SUCCEEDED(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY))) {
                    px=*(DWORD*)((BYTE*)lock.pBits+(pr.top+(LONG)(92*ps))*lock.Pitch+(pr.left+(LONG)(8*ps))*4)&0xffffff; readback->UnlockRect();
                }
                readback->Release();
            }
            const bool panelHere=px==0x1c2026,want=i==SensitivityPanelWindow();
            if(panelHere&&want) SaveSurface(d,dest,L"panel-test-player-window.bmp");   // the exact image that window presents
            seen|=(panelHere==want?1u:0u)<<i;
            if(panelHere!=want) LogF("Panel pixel test: window %d pixel %06lx (panel expected %u)",i,px,want);
            if(seen==3) {pixelChecks|=0x10; ToggleSensitivityPanel(); Log("Panel pixel test: drawn in the mouse + keyboard window only");}
        }
        const UINT checkBit=1u<<(i+(movie?2:0));
        if(testMode&&frames>20&&!(pixelChecks&checkBit)&&SUCCEEDED(hr)) {
            IDirect3DSurface9* readback=nullptr;
            if(SUCCEEDED(d->CreateOffscreenPlainSurface(chainW[i],chainH[i],desc.Format,D3DPOOL_SYSTEMMEM,&readback,nullptr))) {
                if(SUCCEEDED(d->GetRenderTargetData(dest,readback))) {
                    D3DLOCKED_RECT lock;
                    if(SUCCEEDED(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY))) {
                        auto pixel=[&](int px,int py){return *(DWORD*)((BYTE*)lock.pBits+py*lock.Pitch+px*4)&0xffffff;};
                        DWORD expected=i?0x9b4119:0x0f5a91;
                        const bool correct=movie
                            ? pixel(x+w/10,y+h/10)==0x801080&&pixel(x+w*9/10,y+h*9/10)==0x108010
                            : pixel(x+w/10,y+h/2)==expected&&pixel(x+w/2,y+h/2)==0xffffff&&
                              pixel(x+w*9/10,y+h/2)==expected&&pixel(x+w/2,y+h*4/5)==0xffff00;
                        if(correct) pixelChecks|=checkBit;
                        readback->UnlockRect();
                    }
                }
                readback->Release();
            }
        }
        if(capture) SaveSurface(d,dest,i?L"capture-player2.bmp":L"capture-player1.bmp");
        dest->Release();
        renderSpot=i?"player 2 window Present":"player 1 window Present";
        if(SUCCEEDED(hr)) hr=rawChainPresent?rawChainPresent(chains[i],&area,&area,nullptr,nullptr,0):chains[i]->Present(&area,&area,nullptr,nullptr,0);
        renderSpot="DrawViews";
        if(SUCCEEDED(hr)) ++successes;
        else if(frames%120==0) Log("Player %u presentation failed %08x",i+1,hr);
    }
    if(capture) {
        // F5 diagnostic: also save the raw game frame and the shared-UI layer.
        SaveSurface(d,source,L"capture-game.bmp");
        IDirect3DSurface9* layer=nullptr;
        if(sharedLayer&&SUCCEEDED(sharedLayer->GetSurfaceLevel(0,&layer))) {SaveSurface(d,layer,L"capture-shared.bmp");layer->Release();}
        Log("F5 capture saved (composite=%u)",composite);
    }
    source->Release();
    if(successes==2) Log("Both player surfaces presented successfully");
}
// Never waits: if another thread is already drawing the player windows, this Present just skips them.
static void DrawViewsOnce(IDirect3DDevice9* d) {
    renderThreadId=GetCurrentThreadId();
    if(!TryAcquireSRWLockExclusive(&drawLock)) {renderSpot="game Present (player windows busy on another thread)"; return;}
    renderSpot="DrawViews"; DrawViews(d); ReleaseSRWLockExclusive(&drawLock);
    renderSpot="game Present";
}
static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* d,const RECT* a,const RECT* b,HWND h,const RGNDATA* r) {
    bool outer=presentDepth++==0;
    if(outer) DrawViewsOnce(d);
    HRESULT hr=realPresent(d,a,b,h,r);
    if(outer) renderSpot="game frame (outside Present)";
    --presentDepth;
    if(outer) AttachDevice(d);
    return hr;
}
// Exclusive fullscreen (game setting FullScreen=ON without FusionFix borderless) forbids the extra
// swap chains the player windows need, so the game is kept in a window the size of its resolution.
static void KeepWindowed(D3DPRESENT_PARAMETERS* pp) {
    Log("Game asked for windowed=%u, refresh %u",pp->Windowed,pp->FullScreen_RefreshRateInHz);
    if(testMode) return;
    makeBorderless=true;   // always borderless, whatever the game's FullScreen setting
    if(pp->Windowed) return;
    pp->Windowed=TRUE; pp->FullScreen_RefreshRateInHz=0;
    Log("Exclusive fullscreen changed to windowed so the player windows can be drawn");
}
// The game (FullScreen=ON) resets the device when its window is first activated, asking for the same
// windowed mode it already has. If that comes late (window unfocused at start, intro video playing) the
// reset fails with D3DERR_INVALIDCALL -> ERR09. When nothing changes and the device is not lost, skip it.
static bool SameAsNow(IDirect3DDevice9* d,const D3DPRESENT_PARAMETERS* pp) {
    if(d->TestCooperativeLevel()!=D3D_OK||!pp->Windowed||!pp->BackBufferWidth||!pp->BackBufferHeight) return false;
    IDirect3DSwapChain9* sc=nullptr; D3DPRESENT_PARAMETERS cur{};
    if(FAILED(d->GetSwapChain(0,&sc))) return false;
    const bool ok=SUCCEEDED(sc->GetPresentParameters(&cur)); sc->Release();
    return ok&&cur.Windowed&&cur.BackBufferWidth==pp->BackBufferWidth&&cur.BackBufferHeight==pp->BackBufferHeight&&
        (pp->BackBufferFormat==D3DFMT_UNKNOWN||cur.BackBufferFormat==pp->BackBufferFormat)&&cur.BackBufferCount==std::max(pp->BackBufferCount,1u)&&
        cur.MultiSampleType==pp->MultiSampleType&&cur.SwapEffect==pp->SwapEffect&&cur.EnableAutoDepthStencil==pp->EnableAutoDepthStencil&&
        (!pp->EnableAutoDepthStencil||cur.AutoDepthStencilFormat==pp->AutoDepthStencilFormat)&&cur.PresentationInterval==pp->PresentationInterval&&
        (!pp->hDeviceWindow||cur.hDeviceWindow==pp->hDeviceWindow);
}
static const char* ModuleOf(const void* p,char (&name)[MAX_PATH]) {
    HMODULE m=nullptr; name[0]=0;
    if(p&&GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)p,&m)) GetModuleFileNameA(m,name,MAX_PATH);
    const char* s=strrchr(name,'\\'); return s?s+1:name[0]?name:"(no module)";
}
// D3D9 Reset returns D3DERR_INVALIDCALL when an object in D3DPOOL_DEFAULT (extra swap chain, render target, state
// block...) is still alive, or a parameter is invalid. A hook another program put on Reset after ours (an overlay)
// is skipped by the per-device Reset, so it could not free its own objects: the "Reset entry" line shows it.
static void LogResetFailure(IDirect3DDevice9* d,const D3DPRESENT_PARAMETERS* pp,HRESULT hr,const void* called) {
    LogF("RESET FAILED %08lx: windowed %d, %ux%u, format %u, buffers %u, multisample %u/%lu, swap effect %u, window %p, "
         "auto depth %d (format %u), flags %08lx, refresh %u, interval %08x; device state now %08lx",hr,pp->Windowed,
         pp->BackBufferWidth,pp->BackBufferHeight,pp->BackBufferFormat,pp->BackBufferCount,pp->MultiSampleType,pp->MultiSampleQuality,
         pp->SwapEffect,(void*)pp->hDeviceWindow,pp->EnableAutoDepthStencil,pp->AutoDepthStencilFormat,pp->Flags,
         pp->FullScreen_RefreshRateInHz,pp->PresentationInterval,d->TestCooperativeLevel());
    char a[MAX_PATH],b[MAX_PATH]; const BYTE* entry=(const BYTE*)resetTarget;
    const void* to=entry&&entry[0]==0xe9?entry+5+*(const int32_t*)(entry+1):nullptr;
    LogF("Reset path: called %s; Reset entry %s %s",called==(void*)realReset?"d3d9 through the hooks placed before ours":ModuleOf(called,a),to?"jumps to":"is",to?ModuleOf(to,b):"not hooked");
}
static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* pp) {
    KeepWindowed(pp);
    if(!testMode&&SameAsNow(d,pp)) {ReleaseChains(); Log("Device reset skipped: same windowed mode %u x %u",pp->BackBufferWidth,pp->BackBufferHeight); return D3D_OK;}
    ReleaseChains(); Log("Device reset: %u x %u",pp->BackBufferWidth,pp->BackBufferHeight);
    Stage("device Reset"); const HRESULT hr=realReset(d,pp); LogF("Device reset result %08lx",hr); stage="device reset done";
    if(FAILED(hr)) LogResetFailure(d,pp,hr,(void*)realReset);
    return hr;
}
static HRESULT STDMETHODCALLTYPE ObjectPresent(IDirect3DDevice9* d,const RECT* a,const RECT* b,HWND h,const RGNDATA* r) {
    bool outer=presentDepth++==0;
    if(outer) DrawViewsOnce(d);
    HRESULT hr=objectPresent(d,a,b,h,r);
    if(outer) renderSpot="game frame (outside Present)";
    --presentDepth;
    if(outer) AttachDevice(d);
    return hr;
}
static HRESULT STDMETHODCALLTYPE ObjectReset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* pp) {
    ReleaseChains();Log("Object device reset %u x %u",pp->BackBufferWidth,pp->BackBufferHeight); KeepWindowed(pp);
    if(!testMode&&SameAsNow(d,pp)) {Log("Device reset skipped: same windowed mode"); AttachDevice(d); return D3D_OK;}
    Stage("device Reset"); HRESULT hr=objectReset(d,pp); LogF("Device reset result %08lx",hr); stage="device reset done";
    if(FAILED(hr)) LogResetFailure(d,pp,hr,(void*)objectReset);
    AttachDevice(d);return hr;
}
static void AttachDevice(IDirect3DDevice9* d) {
    void** vt=*(void***)d;
    if(vt[17]==(void*)ObjectPresent)return;
    objectPresent=vt[17]==presentTarget?realPresent:(PresentFn)vt[17];
    objectReset=vt[16]==resetTarget?realReset:(ResetFn)vt[16];
    DWORD old=0;
    if(!VirtualProtect(vt+16,2*sizeof(void*),PAGE_READWRITE,&old))return;
    vt[17]=(void*)ObjectPresent;vt[16]=(void*)ObjectReset;
    VirtualProtect(vt+16,2*sizeof(void*),old,&old);
    Log("Attached per-device presentation table");
}
// ---- Co-op: mouse + keyboard drive ONE character, the controller drives the other (decompiled) ----
// The game has ONE global input mode ([[0x157d120]+0x15c4bc]: 1 = mouse + keyboard, 0 = controller).
// Each player's actions are built once per frame by exe+0x6152d0 (virtual method 7 of the per-player input
// object, vtable exe+0xfc9260; thiscall, argument = uPlayer, ret 4). Inside it, ~25 mode checks decide
// keyboard/mouse path vs controller path; the controller path reads the player's OWN pad slot
// (input+0x858, slot = uPlayer+0x7924, read through sBioPad at exe+0x742680). In co-op the mode is the same
// for both players, so mouse + keyboard drove both (mode 1) or nobody (mode 0).
// Fix: hook exe+0x6152d0 and set the mode for the duration of each player's build only: 1 for the player
// whose pad slot is the main slot (sBioPad+0xba0, the player on the mouse + keyboard display), 0 for the
// other. exe+0x9886a7 (the per-frame mode chooser forcing controller in co-op) jumps to ChooseIdleMode so
// menus keep mouse + keyboard. Replaces the earlier 22 single-check patches.
static bool KeyboardOwnsCoop(){return deviceDisplay[DevKeyboard]!=0&&CoopOn();}
// ---- One display with mouse + keyboard AND a controller: its player switches freely (2026-11-12) ----
// The hooks below set the game's single global mode per player, and the mouse + keyboard player always got
// mode 1. A controller placed on the same display already fed that character's pad slot (PadsFor), but with
// mode 1 the game never read it. Now that display's last-used device decides: a NEW controller press (button,
// trigger, or stick past setup::Pressed's drift threshold) switches it to the controller; a new key, mouse
// button, wheel or mouse movement switches it back. Only new presses switch, so a held stick or trigger never
// makes it flicker. The other display is never affected.
static CRITICAL_SECTION buildLock;   // the mode is global: never let two players' builds overlap (re-entrant)
static std::atomic<bool> keyboardDisplayOnPad{false};
static unsigned PadsWithKeyboard() {   // bit p set = controller p is on the mouse + keyboard display
    unsigned m=0;
    if(deviceDisplay[DevKeyboard]) for(int p=0;p<2;++p) if(deviceDisplay[DevPad1+p]==deviceDisplay[DevKeyboard]) m|=1u<<p;
    return m;
}
static bool NextOnPad(bool onPad,bool padPress,bool keyboardPress) {return padPress!=keyboardPress?padPress:onPad;}   // both at once: keep
static bool KeyboardInUse() {return !keyboardDisplayOnPad.load();}
static bool ReadPtr(uintptr_t at,uintptr_t& v);
static void SenseKeyboardDisplayDevice() {   // game thread, under buildLock
    const unsigned pads=PadsWithKeyboard();
    if(!pads||!realGetState) {keyboardDisplayOnPad=false; return;}
    static bool padWas[2]; static DWORD keysWas[8],buttonsWas;
    bool padPress=false,keyPress=false;
    for(int p=0;p<2;++p) if(pads>>p&1) {
        PadState st={}; const bool down=realGetState(p,&st)==ERROR_SUCCESS&&setup::Pressed(st);
        padPress|=down&&!padWas[p]; padWas[p]=down;
    }
    uintptr_t keyboard=0,mouse=0; DWORD keys[8],buttons=0; int move[3];
    if(ReadPtr((uintptr_t)gameBase+0x11de918,keyboard)&&ReadGame(keyboard+0x120,keys,sizeof(keys))) {   // key bits the game's own chooser reads (exe+0x588637)
        for(int k=0;k<8;++k) keyPress|=(keys[k]&~keysWas[k])!=0;
        memcpy(keysWas,keys,sizeof(keys));
    }
    if(ReadPtr((uintptr_t)gameBase+0x11de940,mouse)) {
        if(ReadGame(mouse+0x2c,move,sizeof(move))) keyPress|=abs(move[0])+abs(move[1])>=2||move[2]!=0;   // dx, dy, wheel (as exe+0x588664)
        if(ReadGame(mouse+0x38,&buttons,4)) {keyPress|=(buttons&~buttonsWas)!=0; buttonsWas=buttons;}   // +0x38 now, +0x3c before (exe+0x8276b0)
    }
    const bool onPad=NextOnPad(keyboardDisplayOnPad.load(),padPress,keyPress);
    if(onPad!=keyboardDisplayOnPad.load()) {
        keyboardDisplayOnPad=onPad;
        static unsigned logged=0;
        if(++logged<=200) LogF("Display %d player now uses %s",deviceDisplay[DevKeyboard],onPad?"its controller":"mouse + keyboard");
    }
}
extern "C" void __cdecl ChooseIdleMode(BYTE* self) {
    const bool coop=KeyboardOwnsCoop();
    if(coop) {EnterCriticalSection(&buildLock); SenseKeyboardDisplayDevice(); LeaveCriticalSection(&buildLock);}
    const bool keyboard=coop&&KeyboardInUse();   // menus follow the mouse + keyboard display's current device
    *(int*)(self+0x15c4bc)=keyboard?1:0; *(int*)(self+0x15c4c0)=keyboard?2:0;
}
extern "C" void ModeIdleStub();
// fxsave: the game keeps float values in xmm registers here; the C code must not change them.
asm(".globl _ModeIdleStub\n_ModeIdleStub:\n pushal\n mov %esp,%ebp\n sub $512,%esp\n and $-16,%esp\n fxsave (%esp)\n"
    " push 4(%ebp)\n call _ChooseIdleMode\n add $4,%esp\n fxrstor (%esp)\n mov %ebp,%esp\n popal\n pop %esi\n ret\n");
using BuildInputFn=int(__fastcall*)(void*,void*,BYTE*);
static BuildInputFn realBuildInput=nullptr;
// The mouse + keyboard player is the character shown on the mouse + keyboard display. Window w shows region
// w^MainView(), and MainView() puts region 0 on the keyboard's display, so that is region 0, or region 1
// after F1. PadsFor() routes each controller to the character on its display the same way.
static bool IsKeyboardPlayer(BYTE* player) {
    if(const uintptr_t kb=RegionPlayer(swapPlayers.load()?1:0)) return (uintptr_t)player==kb;
    return (player[0x7924]==0)!=swapPlayers.load();   // regions not set yet: pad number 0 = first player
}
static void LogPlayer(BYTE* player,unsigned number,unsigned keyboard) {
    static BYTE* seen[4]; static unsigned last[4];
    unsigned k=0; while(k<4&&seen[k]&&seen[k]!=player) ++k; if(k==4) return;
    const int region=(uintptr_t)player==RegionPlayer(0)?0:(uintptr_t)player==RegionPlayer(1)?1:-1;
    const unsigned now=number|keyboard<<8|(unsigned)player[0x790c]<<16|(unsigned)(region+1)<<24;
    if(seen[k]==player&&last[k]==now) return; seen[k]=player; last[k]=now;
    Log("Co-op player %u: pad number %u",k,number);
    Log("  -> region %d, XInput %d",region,region<0?-1:RegionXInput(region));
    Log("  -> display %d",region<0?-1:(region^MainView())+1);
    Log("  -> mouse+keyboard %u (player index %u)",keyboard,player[0x790c]);
}
// ---- Mouse camera sensitivity (F3 panel, decompiled 2026-11-13) ----
// Mouse look exists only inside this builder (exe+0x6155c2): with mode 1 it turns the mouse movement of this
// frame ([mouse+0x2c] dx, [mouse+0x30] dy, ints) into the right-stick direction and strength that camera and
// aiming use. Menus take the Windows cursor position instead (exe+0x8278a8), the controller player never has
// mode 1. Scaling dx/dy for this one build therefore changes camera and aim only, exactly once per frame.
static std::atomic<float> mouseSensitivity{1.0f};
static float ClampSensitivity(float s) {return s==s?std::clamp(s,0.1f,3.0f):1.0f;}   // NaN -> default
static int ScaleDelta(int d,float s,float& rest) {const float v=d*s+rest; const int out=(int)v; rest=v-out; return out;}   // keeps fractions: slow moves still turn
struct MouseScale {
    int* d=nullptr; int saved[2]={0,0},scaled[2]={0,0};
    MouseScale(BYTE* player,int mode) {
        const float s=sensitivityPanelOpen.load()?0.0f:mouseSensitivity.load(); uintptr_t mouse=0;   // panel open: camera holds still
        if(s==1.0f||mode!=1||!player||*(int*)(player+0x7920)!=0||!ReadPtr((uintptr_t)gameBase+0x11de940,mouse)) return;
        static float rest[2];
        d=(int*)(mouse+0x2c); saved[0]=d[0]; saved[1]=d[1];
        d[0]=scaled[0]=ScaleDelta(saved[0],s,rest[0]); d[1]=scaled[1]=ScaleDelta(saved[1],s,rest[1]);
    }
    ~MouseScale() {if(d) for(int k=0;k<2;++k) if(d[k]==scaled[k]) d[k]=saved[k];}   // a newer mouse read is left alone
};
static void LogAim(BYTE* player) {   // diagnostics: per character, when its aim flag ([uPlayer+0x78e8] bit 2) changes
    static BYTE* seen[4]; static unsigned last[4],lines=0;
    const unsigned aim=*(DWORD*)(player+0x78e8)>>2&1;
    for(int k=0;k<4;++k) {
        if(seen[k]&&seen[k]!=player) continue;
        if(seen[k]==player&&last[k]==aim) return;
        seen[k]=player; last[k]=aim;
        if(++lines<=400) LogF("Aim: character %u (%s) aim flag %u",player[0x790c],IsKeyboardPlayer(player)?"mouse + keyboard display":"controller display",aim);
        return;
    }
}
static int __fastcall HookBuildInput(void* self,void* edx,BYTE* player) {
    uintptr_t input=0;
    const bool haveInput=ReadPtr((uintptr_t)gameBase+0x117d120,input);
    if(!player||!KeyboardOwnsCoop()||!haveInput) {MouseScale s(player,haveInput?*(int*)(input+0x15c4bc):0); return realBuildInput(self,edx,player);}
    const bool keyboard=IsKeyboardPlayer(player);
    LogPlayer(player,player[0x7924],keyboard); LogAim(player);
    int* mode=(int*)(input+0x15c4bc);
    EnterCriticalSection(&buildLock);
    if(keyboard) SenseKeyboardDisplayDevice();
    const int saved=*mode; *mode=keyboard&&KeyboardInUse()?1:0;
    int r;
    {MouseScale s(player,*mode); r=realBuildInput(self,edx,player);}
    *mode=saved;
    LeaveCriticalSection(&buildLock);
    return r;
}
// exe+0x614ec0 answers "does this player use mouse + keyboard": human (uPlayer+0x7920 == 0) && mode == 1.
// Its mode compare uses a register (cmp [eax+0x15c4bc],edx), which my earlier patch missed: that is why the
// keyboard kept driving the partner. It is also called outside the builder (exe+0x3220c0, turn speed).
// In co-op with mouse + keyboard assigned it now answers for the player itself.
using UsesKeyboardFn=bool(__fastcall*)(void*,void*,BYTE*);
static UsesKeyboardFn realUsesKeyboard=nullptr;
static bool __fastcall HookUsesKeyboard(void* self,void* edx,BYTE* player) {
    if(!player||!KeyboardOwnsCoop()) return realUsesKeyboard(self,edx,player);
    return *(int*)(player+0x7920)==0&&IsKeyboardPlayer(player)&&KeyboardInUse();
}
// ---- Interactions and button prompts use the same global mode, outside the builder (decompiled) ----
// exe+0x40edc0 is the action check behind pickups, doors and other context actions. It gets a character id
// (2nd argument; uPlayer = player list +0x20+id*4, exe+0x6e6dc0) and, for a human, tests the KEYBOARD key
// (exe+0x40f019) when the global mode is 1, otherwise that character's own pad reader (set to its pad
// number at exe+0x40c70f). In co-op the global mode stays 1 outside the builder, so every character's
// action read the keyboard: the keyboard picked up items for the controller character, the pad never did.
// exe+0x49ae40 builds the floating "press X" prompt for the player whose screen it is on (4th argument,
// uses that player's camera +0x4780) and picks keyboard or pad icons from the same mode.
// exe+0x4f6e50/90/d0 are HUD button hints for character id [widget+0x2ac].
// Fix: run each of them with the mode of its own character, like the builder hook.
static BYTE* PlayerById(int id) {
    uintptr_t list=0,p=0;
    if(id<0||id>=8||!ReadPtr((uintptr_t)gameBase+0x1167eac,list)||!ReadGame(list+0x20+id*4,&p,sizeof(p))) return nullptr;
    return (BYTE*)p;
}
struct PlayerMode {   // sets the global mode for one on-screen human player while it lives
    int* mode=nullptr; int saved=0;
    explicit PlayerMode(BYTE* player) {
        uintptr_t input=0;
        if(!player||!KeyboardOwnsCoop()||((uintptr_t)player!=RegionPlayer(0)&&(uintptr_t)player!=RegionPlayer(1))||
           *(int*)(player+0x7920)!=0||!ReadPtr((uintptr_t)gameBase+0x117d120,input)) return;
        EnterCriticalSection(&buildLock);
        mode=(int*)(input+0x15c4bc); saved=*mode; *mode=IsKeyboardPlayer(player)&&KeyboardInUse()?1:0;
    }
    ~PlayerMode() {if(mode) {*mode=saved; LeaveCriticalSection(&buildLock);}}
    int Used() const {return mode?*mode:-1;}
};
using ActionCheckFn=bool(__fastcall*)(void*,void*,void*,int);
static ActionCheckFn realActionCheck=nullptr;
static bool __fastcall HookActionCheck(void* self,void* edx,void* input,int id) {
    PlayerMode m(PlayerById(id));
    const bool hit=realActionCheck(self,edx,input,id);
    static bool was[8];   // diagnostics: which character acted, with which device (logged when it starts)
    if(id>=0&&id<8&&hit!=was[id]) {
        was[id]=hit;
        if(hit) Log(m.Used()==1?"Action: character %u, device mouse + keyboard":m.Used()==0?"Action: character %u, device controller":"Action: character %u, game's own mode",id);
    }
    return hit;
}
// The prompt code identifies its player only through an object that shares the player's camera (+0x4780),
// which is not always the uPlayer itself: match by pointer first, then by camera.
static BYTE* OnScreenPlayer(uintptr_t p,int* region=nullptr) {
    if(region) *region=-1;
    if(!p) return nullptr;
    uintptr_t cam=0; const bool hasCam=ReadGame(p+0x4780,&cam,sizeof(cam))&&cam;
    for(int v=0;v<2;++v) {
        const uintptr_t rp=RegionPlayer(v); uintptr_t rc=0;
        if(rp&&(rp==p||(hasCam&&ReadGame(rp+0x4780,&rc,sizeof(rc))&&rc==cam))) {if(region) *region=v; return (BYTE*)rp;}
    }
    return nullptr;
}
static void LogPromptOwner(const char* what,uintptr_t object,int region,int mode) {   // diagnostics, once per object
    static uintptr_t seen[16]; static int seenRegion[16],seenMode[16];
    for(int k=0;k<16;++k) {
        if(seen[k]==object) {if(seenRegion[k]==region&&seenMode[k]==mode) return; seenRegion[k]=region; seenMode[k]=mode; break;}
        if(!seen[k]) {seen[k]=object; seenRegion[k]=region; seenMode[k]=mode; break;}
        if(k==15) return;
    }
    Log(what,(unsigned)object,(unsigned)region);
    Log("  -> mode used %d (1 keyboard icons, 0 controller icons, -1 unchanged)",(unsigned)mode);
}
using PromptFn=int(__fastcall*)(void*,void*,void*,void*,void*,BYTE*,float);
static PromptFn realPrompt=nullptr;
static int __fastcall HookPrompt(void* self,void* edx,void* a,void* b,void* c,BYTE* player,float f) {
    int region; PlayerMode m(OnScreenPlayer((uintptr_t)player,&region));
    if(KeyboardOwnsCoop()) LogPromptOwner("Camera curve: player object %08x, region %d",(uintptr_t)player,region,m.Used());
    return realPrompt(self,edx,a,b,c,player,f);
}
// exe+0x4a4260 (vtable 0x12644dc, thiscall, two floats, ret 8) is the per-frame update of one prompt system.
// Its owner is [this+0x90] (exe+0x4a42aa); it passes that owner down to exe+0x49ae40. Running the whole
// update with the owner's mode also covers icon work done in its other helpers.
using PromptUpdateFn=int(__fastcall*)(BYTE*,void*,float,float);
static PromptUpdateFn realPromptUpdate=nullptr;
static int __fastcall HookPromptUpdate(BYTE* self,void* edx,float a,float b) {
    uintptr_t owner=0; int region=-1;
    BYTE* player=ReadGame((uintptr_t)self+0x90,&owner,sizeof(owner))?OnScreenPlayer(owner,&region):nullptr;
    PlayerMode m(player);
    if(KeyboardOwnsCoop()) LogPromptOwner("Camera curve system: owner %08x, region %d",owner,region,m.Used());
    return realPromptUpdate(self,edx,a,b);
}
using HintFn=int(__fastcall*)(BYTE*);
static HintFn realHint[3];
template<int N> static int __fastcall HookHint(BYTE* widget) {
    PlayerMode m(PlayerById(*(int*)(widget+0x2ac))); return realHint[N](widget);
}
// ---- The visible action button (ladder, door, item...) is drawn by uGUIActionIcon / uGUIActionIcon2 ----
// (class names from the game's type info; vtables exe+0xf915b0 / exe+0xf91970, method 8 = per-frame update,
// thiscall, no arguments, plain ret). Inside that update, with the GLOBAL mode:
//  - exe+0x48e0f0 picks the icon frame: 6.0 = keyboard key cap when mode is 1, else the pad type's frame;
//  - exe+0x48dd40 / exe+0x48f490: when mode is 1, exe+0x48f230 / exe+0x4901c0 write the keyboard key names
//    from the key config ([keyboard+0xa44+key*8]) into the icon's text: that is the "F";
//  - exe+0x48fe70 picks the message id (0xf4252/0xf4253 keyboard, 0xf424x pad).
// Each widget belongs to character id [widget+0x2ac] (exe+0x48ff8a). Running the update with that
// character's mode makes each screen show its own device. (0x49ae40/0x4a4260 above turned out to be the
// camera-curve choice, cCameraCurve, not the prompt.)
using IconUpdateFn=int(__fastcall*)(BYTE*);
static IconUpdateFn realIconUpdate[2];
template<int N> static int __fastcall HookIconUpdate(BYTE* widget) {
    int id=-1; ReadGame((uintptr_t)widget+0x2ac,&id,sizeof(id));
    PlayerMode m(PlayerById(id));
    if(KeyboardOwnsCoop()) {   // diagnostics: one line per widget, again when its mode changes
        static BYTE* seen[8]; static int seenMode[8];
        for(int k=0;k<8;++k) {
            if(seen[k]==widget) {if(seenMode[k]==m.Used()) break; seenMode[k]=m.Used();}
            else if(seen[k]) continue; else {seen[k]=widget; seenMode[k]=m.Used();}
            int region=-1; OnScreenPlayer((uintptr_t)PlayerById(id),&region);
            Log(N?"Action icon 2: widget %08x, character %d":"Action icon 1: widget %08x, character %d",(unsigned)(uintptr_t)widget,(unsigned)id);
            Log("  -> region %d, display %d",(unsigned)region,(unsigned)(region<0?-1:(region^MainView())+1));
            Log("  -> mode used %d (1 keyboard icon, 0 controller icon, -1 unchanged)",(unsigned)m.Used());
            break;
        }
    }
    return realIconUpdate[N](widget);
}
static void PerPlayerInteractions() {
    struct Site {DWORD rva; const char* bytes; int n; void* hook; void** real; const char* name;};
    const Site sites[]={
        {0x0edc0,"\x56\x8b\xf1\x8b\x4e\x0c\xc6\x46\x09\x00",10,(void*)HookActionCheck,(void**)&realActionCheck,"action check"},
        {0x48e3d0,"\x55\x8b\xec\x83\xe4\xf0\x83\xec\x48\x56",10,(void*)HookIconUpdate<0>,(void**)&realIconUpdate[0],"action icon 1"},
        {0x48f750,"\x56\x8b\xf1\x8b\x86\xcc\x02\x00\x00\x85",10,(void*)HookIconUpdate<1>,(void**)&realIconUpdate[1],"action icon 2"},
        {0x09ae40,"\x55\x8b\xec\x83\xe4\xf0\x81\xec\xd8\x00\x00\x00",12,(void*)HookPrompt,(void**)&realPrompt,"camera curve"},
        {0x0a4260,"\x55\x8b\xec\x83\xe4\xf0\x83\xec\x68",9,(void*)HookPromptUpdate,(void**)&realPromptUpdate,"camera curve update"},
        {0x4f6e50,"\x56\x6a\x00\xff\xb1\xac\x02\x00\x00",9,(void*)HookHint<0>,(void**)&realHint[0],"HUD hint 1"},
        {0x4f6e90,"\x56\x6a\x00\xff\xb1\xac\x02\x00\x00",9,(void*)HookHint<1>,(void**)&realHint[1],"HUD hint 2"},
        {0x4f6ed0,"\x56\x6a\x00\xff\xb1\xac\x02\x00\x00",9,(void*)HookHint<2>,(void**)&realHint[2],"HUD hint 3"},
    };
    unsigned on=0;
    for(const Site& s:sites) {
        BYTE* at=gameBase+s.rva;
        if(memcmp(at,s.bytes,s.n)) {Log(s.name);Log("  -> game code differs, not hooked");continue;}
        if(MH_CreateHook(at,s.hook,s.real)!=MH_OK||MH_EnableHook(at)!=MH_OK) {Log(s.name);Log("  -> hook failed");continue;}
        ++on;
    }
    Log("Per-player interactions and prompts: %u of 8 hooks on",on);
}
static bool WriteCode(BYTE* at,const BYTE* bytes,size_t n) {
    DWORD old; if(!VirtualProtect(at,n,PAGE_EXECUTE_READWRITE,&old)) return false;
    memcpy(at,bytes,n); VirtualProtect(at,n,old,&old); FlushInstructionCache(GetCurrentProcess(),at,n); return true;
}
static void Jump(BYTE* at,void* to,BYTE opcode,size_t total) {   // call/jmp rel32, rest NOPs
    BYTE code[16]; memset(code,0x90,total); code[0]=opcode; *(int32_t*)(code+1)=(int32_t)((BYTE*)to-(at+5)); WriteCode(at,code,total);
}
// ---- Partner's inventory took the mouse away (decompiled 2026-11-12) ----
// Every frame the mouse update (exe+0x679490) calls exe+0x679600, which decides if the mouse is captured
// (mouse+0x29) or shows the menu cursor (cursor object mouse+0x144). It asks exe+0x2e6a70 for "the human
// player" = the FIRST human in the player list (uPlayer+0x7920 == 0), then exe+0x301f70(player) = "that
// player has a menu open" (+0x7448/+0x744c, e.g. the inventory). In co-op both characters are human, so when
// the first one is the controller player, opening its inventory released the mouse and showed the cursor.
// Then the input builder (exe+0x615bd3) zeroes mouse look while the cursor is shown (exe+0x679450): the mouse
// player lost the camera. exe+0xd2d0 / exe+0xd740 (keyboard key checks) block keys the same way.
// Fix: at those 3 call sites only, "the human player" is the mouse + keyboard player (the character on the
// mouse + keyboard display, follows F1). Its own menus still release the mouse as before.
static uintptr_t KeyboardRegionPlayer() {return RegionPlayer(swapPlayers.load()?1:0);}
using FirstHumanFn=BYTE* (__fastcall*)(void*);
static FirstHumanFn firstHuman;
static BYTE* __fastcall KeyboardHuman(void* list) {
    if(KeyboardOwnsCoop()) if(const uintptr_t kb=KeyboardRegionPlayer()) if(*(int*)(kb+0x7920)==0) return (BYTE*)kb;
    return firstHuman(list);
}
static BYTE* __fastcall CaptureHuman(void* list) {   // mouse-capture site: same answer, logs when the choice matters
    BYTE* used=KeyboardHuman(list); BYTE* first=firstHuman(list);
    using MenuFn=bool (__fastcall*)(BYTE*); const auto menuOpen=(MenuFn)(gameBase+0x301f70);
    const unsigned state=(used&&menuOpen(used))|(first&&menuOpen(first))<<1|(used!=first)<<2;
    static unsigned last=~0u;
    if(state!=last) {
        last=state;
        LogF("Mouse capture follows player %p (menu open %u); game's first human %p (menu open %u)",
             (void*)used,state&1,(void*)first,(state>>1)&1);
    }
    return used;
}
static void MenuChecksForKeyboardPlayer() {
    firstHuman=(FirstHumanFn)(gameBase+0x2e6a70);
    const struct {DWORD rva; void* to;} sites[]={{0x67966a,(void*)CaptureHuman},{0xd31e,(void*)KeyboardHuman},{0xd78e,(void*)KeyboardHuman}};
    unsigned on=0;
    for(auto& s:sites) {
        BYTE* at=gameBase+s.rva;
        if(at[0]!=0xe8||at+5+*(int32_t*)(at+1)!=(BYTE*)firstHuman) {LogF("Menu check at exe+%x: game code differs, not patched",s.rva);continue;}
        Jump(at,s.to,0xe8,5); ++on;
    }
    LogF("Menu checks follow the mouse + keyboard player: %u of 3 sites",on);
}
// ---- Mouse + keyboard leaked into the controller player's inventory (decompiled 2026-11-12) ----
// Every menu (uBioGUI: inventory uGUIInventoryCampaign, pause menu uGUICommonMenu...) has an owner: virtual
// method 54 (exe+0x48e230) = [menu+0x2ac] in co-op, the character it belongs to (the pause menu gets whoever
// paused, exe+0x4ba036). Menus read their buttons through methods 56/57 (exe+0x568590 pressed, exe+0x568560
// repeat) -> the GUI manager [0x15de768] exe+0x554db0 / exe+0x554c60, which return
//   the OWNER's controller (sBioPad exe+0x696970 / exe+0x696830, slot = sBioPad+0xba0[owner])
//   | mouse buttons (left = confirm, right = cancel; not mode-gated)
//   | keyboard menu keys (key config 0x32..0x3c) when the GLOBAL mode is 1.
// The controller part follows the owner; the mouse and keyboard parts don't. With mouse + keyboard live, the
// keyboard player's keys and clicks also drove the controller player's inventory. Inside the menu's update
// (base update exe+0x568eb0 -> state functions, inventory table exe+0xf9c930) the inventory also selects items
// under the mouse cursor when the global sub-mode is 2 (exe+0x4f5d5c, exe+0x4f63d4), and ignores its own
// confirm while the left mouse button is held (exe+0x5556b0, exe+0x4f65dc).
// Fix: a per-player menu owned by the player who is NOT on mouse + keyboard right now gets only its owner's
// controller buttons (exactly the game's own controller part), and its update runs with the global mode and
// sub-mode set to controller and "mouse button held" off. The mouse + keyboard player's own menus, the mouse
// capture fix above and every other menu are unchanged.
static const struct {DWORD vt; const char* name;} playerMenus[]={   // vtables (exe+rva), from the game's type info
    {0xf9ccc8,"inventory"},{0xf9d5e8,"key items"},{0xf9d408,"item pickup"},{0xf9ae20,"weapon select"},
    {0xf9afb0,"weapon shortcut"},{0xf9e0f0,"map"},{0xf95070,"pause menu"},{0xf96858,"weapon upgrade"},
    {0xf91c80,"appraise popup"},{0xfa0240,"puzzle"}};
// -1: not an on-screen human player (game's own input), 0: mouse + keyboard (+ that display's controller), 1: controller only
static int MenuPadOnly(BYTE* player) {
    if(!player||((uintptr_t)player!=RegionPlayer(0)&&(uintptr_t)player!=RegionPlayer(1))||*(int*)(player+0x7920)!=0) return -1;
    return IsKeyboardPlayer(player)&&KeyboardInUse()?0:1;
}
static int MenuRoute(BYTE* gui,const char*& name,int& owner) {
    name=nullptr; owner=-1;
    if(!gui||!KeyboardOwnsCoop()) return -1;
    const uintptr_t vt=*(uintptr_t*)gui-(uintptr_t)gameBase;
    for(auto& m:playerMenus) if(vt==m.vt) name=m.name;
    if(!name) return -1;
    owner=((int(__fastcall*)(BYTE*,void*))(*(void***)gui)[54])(gui,nullptr);
    BYTE* player=PlayerById(owner); const int route=MenuPadOnly(player);
    static BYTE* seen[16]; static int seenRoute[16]; static unsigned lines=0;   // diagnostics: per menu, when its route changes (F1 too)
    for(int k=0;k<16;++k) {
        if(seen[k]&&seen[k]!=gui) continue;
        if(seen[k]==gui&&seenRoute[k]==route) break;
        seen[k]=gui; seenRoute[k]=route;
        int region=-1; OnScreenPlayer((uintptr_t)player,&region);
        if(++lines<=300) LogF("Menu: %s %p of character %d (display %d, F1 %u) -> %s, XInput %d",name,(void*)gui,owner,
            region<0?-1:(region^MainView())+1,swapPlayers.load()?1:0,route==1?"controller only":route==0?"mouse + keyboard and that display's controller":"game's own input",
            region<0?-1:RegionXInput(region));
        break;
    }
    return route;
}
static void LogMenuInput(const char* what,const char* name,int owner,int bits) {   // diagnostics, bounded
    static unsigned lines=0; static int last=0;
    if(bits==last||++lines>300) return; last=bits;
    LogF("Menu: %s %08x, %s of character %d",what,bits,name,owner);
}
using MenuInputFn=int(__fastcall*)(BYTE*,void*);
static MenuInputFn realMenuInput[2];
template<int Repeat> static int __fastcall HookMenuInput(BYTE* gui,void* edx) {
    const int all=realMenuInput[Repeat](gui,edx);
    const char* name; int owner;
    if(MenuRoute(gui,name,owner)!=1||!gui[0x2b9]) return all;   // [gui+0x2b9] off: the game returns 0 itself
    void* pads=*(void**)(gameBase+0x11de5dc); const BYTE* manager=*(BYTE**)(gameBase+0x11de768);
    using PadBitsFn=int(__fastcall*)(void*,void*,int,int);
    const int own=pads&&manager&&manager[0x16c4]?((PadBitsFn)(gameBase+(Repeat?0x696830:0x696970)))(pads,nullptr,owner,0):0;
    if(all&~own) LogMenuInput("blocked mouse/keyboard buttons",name,owner,all&~own);
    else if(own&&!Repeat) LogMenuInput("controller buttons",name,owner,own);
    return own;
}
static thread_local const char* padOnlyMenu=nullptr;   // set while a controller-only menu updates
static thread_local int padOnlyOwner=-1;
using MenuUpdateFn=int(__fastcall*)(BYTE*,void*);
static MenuUpdateFn realMenuUpdate;
static int __fastcall HookMenuUpdate(BYTE* gui,void* edx) {
    const char* name; int owner; uintptr_t input=0;
    if(MenuRoute(gui,name,owner)!=1||!ReadPtr((uintptr_t)gameBase+0x117d120,input)) return realMenuUpdate(gui,edx);
    EnterCriticalSection(&buildLock);
    int* mode=(int*)(input+0x15c4bc); const int m=mode[0],sub=mode[1];   // +0x15c4bc mode, +0x15c4c0 sub-mode
    mode[0]=mode[1]=0;
    const char* outer=padOnlyMenu; const int outerOwner=padOnlyOwner; padOnlyMenu=name; padOnlyOwner=owner;
    const int r=realMenuUpdate(gui,edx);
    padOnlyMenu=outer; padOnlyOwner=outerOwner; mode[0]=m; mode[1]=sub;
    LeaveCriticalSection(&buildLock);
    return r;
}
using MouseHeldFn=bool(__cdecl*)();
static MouseHeldFn realMouseHeld;
static bool __cdecl HookMouseHeld() {
    const bool held=realMouseHeld();
    if(!padOnlyMenu) return held;
    if(held) LogMenuInput("blocked mouse button held",padOnlyMenu,padOnlyOwner,1);
    return false;
}
static void PerPlayerMenus() {
    const struct {DWORD rva; const char* bytes; int n; void* hook; void** real; const char* name;} sites[]={
        {0x568590,"\x80\xb9\xb9\x02\x00\x00\x00\xc6\x81\xb8\x02\x00\x00\x01",14,(void*)HookMenuInput<0>,(void**)&realMenuInput[0],"menu buttons pressed"},
        {0x568560,"\x80\xb9\xb9\x02\x00\x00\x00\xc6\x81\xb8\x02\x00\x00\x01",14,(void*)HookMenuInput<1>,(void**)&realMenuInput[1],"menu buttons repeat"},
        {0x568eb0,"\xa1\xf8\xe6\x5d\x01\x53\x80\x78\x34\x00",10,(void*)HookMenuUpdate,(void**)&realMenuUpdate,"menu update"},
        {0x5556b0,"\xa1\x20\xd1\x57\x01\x83\x78\x50\x00",9,(void*)HookMouseHeld,(void**)&realMouseHeld,"menu mouse button"},
    };
    static const char padBits[]="\x80\xb9\xc0\x0b\x00\x00\x00\x8b\x44\x24\x04\x8b\x94\x81";   // sBioPad pressed / repeat readers
    if(memcmp(gameBase+0x696970,padBits,14)||memcmp(gameBase+0x696830,padBits,14)) {Log("Per-player menus: game code differs, not hooked");return;}
    unsigned on=0;
    for(auto& s:sites) {
        BYTE* at=gameBase+s.rva;
        if(memcmp(at,s.bytes,s.n)) {LogF("Per-player menus: %s: game code differs, not hooked",s.name);continue;}
        if(MH_CreateHook(at,s.hook,s.real)!=MH_OK||MH_EnableHook(at)!=MH_OK) {LogF("Per-player menus: %s: hook failed",s.name);continue;}
        ++on;
    }
    LogF("Per-player menus (only the owner's devices): %u of 4 hooks on",on);
}
// ---- Menu cursor reached only part of the inventory / key-binding list (decompiled 2026-11-13) ----
// The mouse update (exe+0x8273e0) places the menu cursor from the WINDOWS cursor: exe+0x8277a0 calls
// GetCursorPos + ScreenToClient(game window) (exe+0x8278be), then cursor = (client - viewport.left) * W /
// viewport width (exe+0x8278e8; viewport = [[0x15e0388]+0xa4], W = [+0xb8]). exe+0x82745d does the same to
// decide "cursor outside the game picture" (shows the Windows arrow via exe+0x876820). The game window is the
// hidden one behind the player windows (log: 3840x1100 at -960,-540), not the window the player looks at, so
// only the part of the screen that overlaps it reached the menus and the top/left of full-screen menus (the
// first key bindings) was off-screen. Fix: both points come from the player window instead: the cursor's spot in
// the mouse + keyboard player's window (or the window under it outside co-op) -> the back-buffer pixel that
// window shows there (clamped to its picture) -> the client point the game's own formula turns into that pixel.
// exe+0x82768b (ClipCursor) is left as the game does it, except while the F3 panel is open (cursor free).
static POINT WindowToBackBuffer(const RECT& src,const RECT& dst,POINT p) {
    const LONG dw=std::max(1L,dst.right-dst.left),dh=std::max(1L,dst.bottom-dst.top);
    p.x=std::clamp(p.x,dst.left,dst.left+dw-1); p.y=std::clamp(p.y,dst.top,dst.top+dh-1);
    return {src.left+(LONG)((int64_t)(p.x-dst.left)*(src.right-src.left)/dw),src.top+(LONG)((int64_t)(p.y-dst.top)*(src.bottom-src.top)/dh)};
}
static LONG GameClientFor(LONG pixel,LONG from,LONG to,UINT backBuffer) {return from+(LONG)((int64_t)pixel*(to-from)/(LONG)std::max(1u,backBuffer));}
static int CursorWindow(POINT screen) {
    if(KeyboardOwnsCoop()) return (swapPlayers.load()?1:0)^MainView();   // window showing the mouse + keyboard player (follows F1)
    for(int i=0;i<2;++i) {RECT r; if(views[i]&&IsWindowVisible(views[i])&&GetWindowRect(views[i],&r)&&PtInRect(&r,screen)) return i;}
    return deviceDisplay[DevKeyboard]?deviceDisplay[DevKeyboard]-1:0;
}
static bool CursorOnBackBuffer(POINT screen,POINT& pixel,bool& inside,int& window) {
    if(testMode||!uiReady.load()||!enabled.load()) return false;
    window=CursorWindow(screen); if(window<0||window>1||!views[window]||IsIconic(views[window])) return false;
    AcquireSRWLockShared(&viewMapLock); const ViewMap m=viewMap[window]; ReleaseSRWLockShared(&viewMapLock);
    POINT p=screen; if(!m.valid||!ScreenToClient(views[window],&p)) return false;
    inside=PtInRect(&m.dst,p)!=0; pixel=WindowToBackBuffer(m.src,m.dst,p); return true;
}
static void LogCursor(const char* what,int window,bool inside,POINT pixel) {   // diagnostics: on change, bounded
    static int lastWindow=-2,lastInside=-1; static unsigned lines=0; static DWORD lastTick=0;
    const DWORD now=GetTickCount();
    if((window==lastWindow&&(int)inside==lastInside&&now-lastTick<3000)||++lines>400) return;
    lastWindow=window; lastInside=inside; lastTick=now;
    LogF("Menu cursor (%s): window %d (Display %d), over its picture %u, frame pixel %ld,%ld",what,window,window+1,inside?1:0,pixel.x,pixel.y);
}
static BOOL WINAPI CursorOverPicture(HWND h,LPPOINT p) {   // exe+0x82745d
    const POINT screen=*p; const BOOL ok=ScreenToClient(h,p); POINT pixel; bool inside; int w; RECT c;
    if(ok&&CursorOnBackBuffer(screen,pixel,inside,w)&&GetClientRect(h,&c)) {
        *p=inside?POINT{(c.left+c.right)/2,(c.top+c.bottom)/2}:POINT{c.left-1,c.top-1};
        LogCursor("over picture",w,inside,pixel);
    }
    return ok;
}
static BOOL WINAPI CursorPosition(HWND h,LPPOINT p) {   // exe+0x8278be
    const POINT screen=*p; const BOOL ok=ScreenToClient(h,p); POINT pixel; bool inside; int w;
    if(!ok||!CursorOnBackBuffer(screen,pixel,inside,w)) return ok;
    RECT vp={}; uintptr_t render=0;   // the viewport the game divides by (exe+0x8277a9: render object, else globals)
    const bool have=ReadPtr((uintptr_t)gameBase+0x11e0388,render)?ReadGame(render+0xa4,&vp,sizeof(vp)):ReadGame((uintptr_t)gameBase+0x11ddff0,&vp,sizeof(vp));
    if(!have||vp.right<=vp.left||vp.bottom<=vp.top) return ok;
    p->x=GameClientFor(pixel.x,vp.left,vp.right,sourceWidth); p->y=GameClientFor(pixel.y,vp.top,vp.bottom,sourceHeight);
    return ok;
}
static BOOL WINAPI GameClipCursor(const RECT* r) {   // F3 panel open: cursor stays in the mouse + keyboard player's picture
    haveGameClip=r!=nullptr; if(r) lastGameClip=*r;
    return ClipCursor(sensitivityPanelOpen.load()&&panelClipped?&panelClip:r);
}   // exe+0x82768b
static void CursorFollowsPlayerWindows() {
    const struct {DWORD rva; DWORD iat; void* to; const char* name;} sites[]={
        {0x82745d,0xe3f348,(void*)CursorOverPicture,"cursor over picture"},{0x8278be,0xe3f348,(void*)CursorPosition,"cursor position"},
        {0x82768b,0xe3f374,(void*)GameClipCursor,"cursor clip"}};
    unsigned on=0;
    for(auto& s:sites) {
        BYTE* at=gameBase+s.rva;
        if(at[0]!=0xff||at[1]!=0x15||*(DWORD*)(at+2)!=(DWORD)(uintptr_t)(gameBase+s.iat)) {LogF("Menu cursor: %s: game code differs, not patched",s.name);continue;}
        Jump(at,s.to,0xe8,6); ++on;
    }
    LogF("Menu cursor follows the player windows: %u of 3 sites",on);
}
// ---- One player's aiming blocked the other player's inventory (decompiled 2026-11-13) ----
// The input builder runs once per player (exe+0x6152d0, cPadEx). Its inventory-button handler exe+0x617b40
// (keys 0x33/0x34/0x35 or the pad's inventory button -> exe+0x711ca0 tab, [cPadEx+0x2e] open request) first
// checks the FIRST human character in the player list (exe+0x617b56 -> exe+0x2e6a70): its state [+0xc]&7 must be
// 1 or 2 and its flag [+0x78e8] bit 2 must be clear; only then "can open inventory" (exe+0x303980) for the player
// being built. Bit 2 is the flag that also blocks a player's own quick heal / weapon switch while aiming
// (exe+0x61791d, exe+0x617726). In co-op the first human is always character 0 (Barry / Claire): while it aims,
// character 1's inventory (and healing from it) is refused; character 1 aiming never blocks character 0. That is
// the reported pattern, whatever the devices. Fix: that one call returns the player being built (edi there)
// when it is human, so each player's own aim/state decides. Single player: same character, no change.
// Not changed: the pause menu opener (exe+0x556b30) has the same first-human check (pause is refused while
// character 0 aims); the quick heal button (exe+0x617670 -> exe+0x311cb0) checks only the healer's own state.
using FirstHumanList=BYTE* (__fastcall*)(void*);
static BYTE* GatePlayer(BYTE* player,BYTE* first) {return player&&*(int*)(player+0x7920)==0?player:first;}
static bool InventoryGateBlocks(BYTE* p) {if(!p) return true; const int s=*(int*)(p+0xc)&7; return (s!=1&&s!=2)||(*(DWORD*)(p+0x78e8)>>2&1);}
extern "C" BYTE* __cdecl InventoryGatePlayer(void* list,BYTE* player) {
    BYTE* first=((FirstHumanList)(gameBase+0x2e6a70))(list); BYTE* used=GatePlayer(player,first);
    if(used!=first&&used&&first) {   // diagnostics: when the old check would have decided differently
        static unsigned lastState=~0u,lines=0;
        const unsigned state=InventoryGateBlocks(first)|InventoryGateBlocks(used)<<1|(unsigned)used[0x790c]<<2;
        if(state!=lastState&&(state&1)!=((state>>1)&1)&&++lines<=300)
            LogF("Inventory button of character %u: decided by its own state (blocked %u); character %u's state would have %s it",
                 used[0x790c],(state>>1)&1,first[0x790c],state&1?"BLOCKED":"allowed");
        lastState=state;
    }
    return used;
}
extern "C" void InventoryGateStub();
asm(".globl _InventoryGateStub\n_InventoryGateStub:\n push %ecx\n push %edx\n push %edi\n push %ecx\n call _InventoryGatePlayer\n add $8,%esp\n pop %edx\n pop %ecx\n ret\n");
using HealRequestFn=int(__fastcall*)(BYTE*,void*,int,int);
static HealRequestFn realHealRequest;
static int __fastcall HookHealRequest(BYTE* player,void* edx,int kind,int flag) {   // diagnostics only: quick heal/item button reached the game
    static unsigned lines=0;
    if(++lines<=300) {
        BYTE* other=(BYTE*)RegionPlayer(0); if(other==player) other=(BYTE*)RegionPlayer(1);
        LogF("Quick heal/item button: character %u, kind %d, own aim %u, other character aim %d",player[0x790c],kind,
             *(DWORD*)(player+0x78e8)>>2&1,other&&other!=player?(int)(*(DWORD*)(other+0x78e8)>>2&1):-1);
    }
    return realHealRequest(player,edx,kind,flag);
}
static void PerPlayerActionGates() {
    BYTE* at=gameBase+0x617b56;
    if(at[0]!=0xe8||at+5+*(int32_t*)(at+1)!=gameBase+0x2e6a70) Log("Inventory gate: game code differs, not patched");
    else {Jump(at,(void*)InventoryGateStub,0xe8,5); Log("Inventory button uses each player's own aim/state: on");}
    BYTE* heal=gameBase+0x311cb0;
    if(memcmp(heal,"\x56\x8b\xf1\x83\xbe\x08\x1a\x00\x00\x00",10)) Log("Quick heal log: game code differs, not hooked");
    else if(MH_CreateHook(heal,HookHealRequest,(void**)&realHealRequest)!=MH_OK||MH_EnableHook(heal)!=MH_OK) Log("Quick heal log: hook failed");
}
// ---- F3: mouse camera sensitivity panel ----
// Drawn INTO the mouse + keyboard player's picture: into that window's own swap chain, right before it is
// presented, so it is seen wherever that picture is seen (monitor, filled window, capture or streaming).
// Why: the earlier panel was a separate desktop window. The log of 2026-11-13 shows it shown, painted, foreground
// and not covered (2694,454, centre of the filled Display 1 window on \\.\DISPLAY21), yet it was not seen in-game.
// No extra window and no focus change: the game stays the foreground window.
// Mouse: while open the cursor is clipped to that window's picture and the panel draws its own pointer (the game
// hides the system cursor over its picture). Drag or click the slider, click -0.01 / +0.01 / Reset / Close.
// Keyboard: Left/Right or A/D 0.01 (hold to repeat), R reset, Esc or F3 close. While open the game gets no mouse
// or keyboard input (HookDeviceState) and the camera holds still (scale 0 in the input builder). On close the
// game's own cursor clip is put back. Saved as [Mouse] MouseCameraSensitivity in scripts\...DualWindows.ini.
static const int PanelW=440,PanelH=176;
struct PanelHit {RECT track,less,more,reset,close;};
static struct {
    IDirect3DSurface9* surface=nullptr; D3DFORMAT format=D3DFMT_UNKNOWN; int w=0,h=0;   // D3DPOOL_DEFAULT, lockable
    HDC dc=nullptr; HBITMAP bitmap=nullptr; HGDIOBJ oldBitmap=nullptr; DWORD* bits=nullptr; int bw=0,bh=0;
    HFONT font=nullptr,big=nullptr; float fontScale=0;
    float shownValue=-1; int shownHover=-2,shownDown=-2; bool dirty=true;
    bool dragging=false,wasDown=false; DWORD heldSince=0,lastStep=0;
    unsigned framesDrawn=0,framesMissed=0; bool loggedPlacement=false;
} panel;
static void SaveSensitivity() {
    wchar_t ini[MAX_PATH],v[16]; setup::IniPath(ini); swprintf_s(v,L"%.2f",mouseSensitivity.load());
    WritePrivateProfileStringW(L"Mouse",L"MouseCameraSensitivity",v,ini);
}
static void LoadSensitivity() {
    wchar_t ini[MAX_PATH],v[32]; setup::IniPath(ini); GetPrivateProfileStringW(L"Mouse",L"MouseCameraSensitivity",L"1.0",v,32,ini);
    mouseSensitivity=ClampSensitivity((float)_wtof(v)); LogF("Mouse camera sensitivity: %.2fx (F3 to change)",mouseSensitivity.load());
}
static float StepSensitivity(float s,int steps) {return ClampSensitivity(std::round(s*100.0f+steps)/100.0f);}   // exact 0.01 grid, no float drift
static int SensitivityPanelWindow() {   // the window showing the mouse + keyboard player (follows F1 in co-op)
    if(KeyboardOwnsCoop()) return (swapPlayers.load()?1:0)^MainView();
    return deviceDisplay[DevKeyboard]==2?1:0;
}
// Panel layout in panel pixels (scaled down only when the window is too small for 440x176).
static PanelHit PanelLayout(float s) {
    auto R=[s](int l,int t,int r,int b){return RECT{(LONG)(l*s),(LONG)(t*s),(LONG)(r*s),(LONG)(b*s)};};
    return {R(24,64,416,84),R(16,100,100,132),R(108,100,192,132),R(200,100,316,132),R(324,100,424,132)};
}
static float PanelScale(const RECT& picture) {
    const float s=std::min({1.0f,(picture.right-picture.left-16)/(float)PanelW,(picture.bottom-picture.top-16)/(float)PanelH});
    return std::max(0.3f,s);
}
static RECT PanelRect(const RECT& picture,float s) {   // centred in the picture, in client pixels
    const int w=(int)(PanelW*s),h=(int)(PanelH*s),x=(picture.left+picture.right-w)/2,y=(picture.top+picture.bottom-h)/2;
    return {x,y,x+w,y+h};
}
static float TrackValue(const RECT& track,LONG x) {
    const float t=std::clamp((x-track.left)/(float)std::max(1L,track.right-track.left),0.0f,1.0f);
    return ClampSensitivity(std::round((0.1f+t*2.9f)*100.0f)/100.0f);
}
static void ReleaseSensitivityPanel() {   // device reset / shutdown: D3DPOOL_DEFAULT surface must go
    if(panel.surface) {panel.surface->Release(); panel.surface=nullptr;}
}
static void PaintPanel(float s,int hover,int down) {   // GDI into a 32-bit DIB, then copied into the D3D surface
    const int w=(int)(PanelW*s),h=(int)(PanelH*s);
    if(!panel.dc||panel.bw!=w||panel.bh!=h) {
        if(panel.dc) {SelectObject(panel.dc,panel.oldBitmap); DeleteObject(panel.bitmap); DeleteDC(panel.dc); panel.dc=nullptr;}
        BITMAPINFO bi={}; bi.bmiHeader={sizeof(BITMAPINFOHEADER),w,-h,1,32,BI_RGB};
        panel.dc=CreateCompatibleDC(nullptr); panel.bitmap=CreateDIBSection(panel.dc,&bi,DIB_RGB_COLORS,(void**)&panel.bits,nullptr,0);
        if(!panel.dc||!panel.bitmap) {Log("Sensitivity panel: GDI bitmap not created"); return;}
        panel.oldBitmap=SelectObject(panel.dc,panel.bitmap); panel.bw=w; panel.bh=h;
    }
    if(panel.fontScale!=s) {
        if(panel.font) DeleteObject(panel.font); if(panel.big) DeleteObject(panel.big);
        panel.font=CreateFontW(-(int)(15*s),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        panel.big=CreateFontW(-(int)(21*s),0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        panel.fontScale=s;
    }
    HDC dc=panel.dc; const PanelHit hit=PanelLayout(s); const float v=mouseSensitivity.load();
    auto fill=[dc](RECT r,COLORREF c){HBRUSH b=CreateSolidBrush(c); FillRect(dc,&r,b); DeleteObject(b);};
    auto text=[dc](RECT r,const wchar_t* t,COLORREF c,UINT f){SetTextColor(dc,c); DrawTextW(dc,t,-1,&r,f|DT_NOPREFIX);};
    SetBkMode(dc,TRANSPARENT);
    fill({0,0,w,h},RGB(235,235,235)); fill({(LONG)(2*s),(LONG)(2*s),w-(LONG)(2*s),h-(LONG)(2*s)},RGB(28,32,38));   // light border, dark body
    wchar_t t[96];
    SelectObject(dc,panel.big); swprintf_s(t,L"Mouse camera sensitivity: %.2fx",v);
    text({(LONG)(16*s),(LONG)(12*s),w-(LONG)(16*s),(LONG)(40*s)},t,RGB(255,255,255),DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    SelectObject(dc,panel.font);
    text({(LONG)(16*s),(LONG)(40*s),w-(LONG)(16*s),(LONG)(60*s)},L"0.10x",RGB(160,170,180),DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    text({(LONG)(16*s),(LONG)(40*s),w-(LONG)(16*s),(LONG)(60*s)},L"3.00x",RGB(160,170,180),DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    RECT tr=hit.track; fill({tr.left,tr.top+(tr.bottom-tr.top)/2-(LONG)(3*s),tr.right,tr.top+(tr.bottom-tr.top)/2+(LONG)(3*s)},RGB(90,100,112));
    const LONG kx=tr.left+(LONG)((v-0.1f)/2.9f*(tr.right-tr.left));
    fill({tr.left,tr.top+(tr.bottom-tr.top)/2-(LONG)(3*s),kx,tr.top+(tr.bottom-tr.top)/2+(LONG)(3*s)},RGB(70,150,230));   // filled part
    const LONG one=tr.left+(LONG)((1.0f-0.1f)/2.9f*(tr.right-tr.left)); fill({one-1,tr.top+(LONG)(2*s),one+1,tr.bottom-(LONG)(2*s)},RGB(160,170,180));   // 1.00x mark
    fill({kx-(LONG)(7*s),tr.top-(LONG)(4*s),kx+(LONG)(7*s),tr.bottom+(LONG)(4*s)},hover==0||down==0?RGB(255,255,255):RGB(210,220,232));   // thumb
    const RECT buttons[]={hit.less,hit.more,hit.reset,hit.close}; const wchar_t* labels[]={L"- 0.01",L"+ 0.01",L"Reset 1.00x",L"Close (F3)"};
    for(int b=0;b<4;++b) {
        const COLORREF face=down==b+1?RGB(40,110,190):hover==b+1?RGB(78,90,104):RGB(56,64,74);
        fill(buttons[b],RGB(150,160,172)); fill({buttons[b].left+1,buttons[b].top+1,buttons[b].right-1,buttons[b].bottom-1},face);
        text(buttons[b],labels[b],RGB(255,255,255),DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    }
    text({(LONG)(16*s),(LONG)(140*s),w-(LONG)(16*s),(LONG)(166*s)},L"Drag/click.  Left/Right or A/D: 0.01.  R: reset.  Esc/F3: close.",RGB(170,180,190),DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    GdiFlush();
    for(int i=0;i<w*h;++i) panel.bits[i]|=0xff000000;   // opaque for A8R8G8B8 back buffers
}
static bool UploadPanel(IDirect3DDevice9* d,D3DFORMAT format) {
    if(panel.surface&&(panel.format!=format||panel.w!=panel.bw||panel.h!=panel.bh)) ReleaseSensitivityPanel();
    if(!panel.surface) {
        const HRESULT hr=d->CreateOffscreenPlainSurface(panel.bw,panel.bh,format,D3DPOOL_DEFAULT,&panel.surface,nullptr);
        if(FAILED(hr)) {LogF("Sensitivity panel: surface %dx%d format %d not created (%08lx)",panel.bw,panel.bh,(int)format,hr); panel.surface=nullptr; return false;}
        panel.format=format; panel.w=panel.bw; panel.h=panel.bh;
    }
    D3DLOCKED_RECT lock; const HRESULT hr=panel.surface->LockRect(&lock,nullptr,0);
    if(FAILED(hr)) {LogF("Sensitivity panel: surface lock failed (%08lx)",hr); return false;}
    for(int y=0;y<panel.h;++y) memcpy((BYTE*)lock.pBits+y*lock.Pitch,panel.bits+y*panel.w,panel.w*4);
    panel.surface->UnlockRect(); return true;
}
static void DrawPointer(IDirect3DDevice9* d,IDirect3DSurface9* dest,const RECT& bounds,POINT p) {   // arrow, black outline
    for(int row=0;row<18;++row) {
        const int width=row<13?row+1:std::max(1,13-(row-13)*3);
        RECT outline={p.x-1,p.y+row-1,p.x+width+1,p.y+row+1},inner={p.x,p.y+row,p.x+width-(row<13?1:0),p.y+row+1};
        RECT o,i; if(IntersectRect(&o,&outline,&bounds)) d->ColorFill(dest,&o,D3DCOLOR_XRGB(0,0,0));
        if(inner.right>inner.left&&IntersectRect(&i,&inner,&bounds)) d->ColorFill(dest,&i,D3DCOLOR_XRGB(255,255,255));
    }
}
static void ClosePanel(const char* how) {
    if(!sensitivityPanelOpen.exchange(false)) return;
    panel.dragging=false; SaveSensitivity();
    if(panelClipped) {panelClipped=false; ClipCursor(haveGameClip?&lastGameClip:nullptr);}   // the game's own capture again
    LogF("Sensitivity panel closed (%s): %.2fx saved; drawn in %u frames, %u frames could not be drawn",how,mouseSensitivity.load(),panel.framesDrawn,panel.framesMissed);
}
static void ToggleSensitivityPanel() {
    if(sensitivityPanelOpen.load()) {ClosePanel("F3"); return;}
    for(int k:{VK_LEFT,VK_RIGHT,(int)'A',(int)'D',(int)'R',VK_ESCAPE,VK_LBUTTON}) GetAsyncKeyState(k);   // drop presses made before opening
    panel.dirty=true; panel.dragging=false; panel.wasDown=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0; panel.heldSince=0;
    panel.framesDrawn=panel.framesMissed=0; panel.loggedPlacement=false;
    sensitivityPanelOpen=true;
    const int w=SensitivityPanelWindow();
    LogF("Sensitivity panel opened: %.2fx, drawn into player window %d (Display %d)%s",mouseSensitivity.load(),w,w+1,
         views[w]&&IsWindowVisible(views[w])&&!IsIconic(views[w])?"":" - that window is hidden or minimized");
}
// Render thread, once per frame per player window, after the game picture is in `dest`. Returns true if drawn.
static bool DrawSensitivityPanel(IDirect3DDevice9* d,IDirect3DSurface9* dest,const RECT& picture,int window,bool active) {
    const float s=PanelScale(picture); const RECT where=PanelRect(picture,s); const PanelHit hit0=PanelLayout(s);
    auto at=[&](RECT r){OffsetRect(&r,where.left,where.top); return r;};
    // --- mouse (client pixels of this window; the swap chain area is the client area 1:1) ---
    POINT screen{},client{}; bool haveMouse=false;
    if(!testMode&&views[window]&&GetCursorPos(&screen)) {client=screen; haveMouse=ScreenToClient(views[window],&client)!=0;}
    if(haveMouse&&active) {   // hold the cursor inside this player's picture while the panel is open
        RECT c=picture; POINT tl{c.left,c.top},br{c.right,c.bottom}; ClientToScreen(views[window],&tl); ClientToScreen(views[window],&br);
        panelClip={tl.x,tl.y,br.x,br.y}; panelClipped=true; ClipCursor(&panelClip);
    }
    int hover=-1; const bool down=active&&(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
    const RECT track=at(hit0.track);
    if(haveMouse) {
        RECT trackArea={track.left-(LONG)(8*s),track.top-(LONG)(8*s),track.right+(LONG)(8*s),track.bottom+(LONG)(8*s)};
        if(PtInRect(&trackArea,client)) hover=0;
        const RECT b[]={at(hit0.less),at(hit0.more),at(hit0.reset),at(hit0.close)};
        for(int k=0;k<4;++k) if(PtInRect(&b[k],client)) hover=k+1;
    }
    if(down&&!panel.wasDown) {   // press
        if(hover==0) panel.dragging=true;
        else if(hover>=1) {
            const float v=mouseSensitivity.load();
            if(hover==1) mouseSensitivity=StepSensitivity(v,-1); else if(hover==2) mouseSensitivity=StepSensitivity(v,1);
            else if(hover==3) mouseSensitivity=1.0f;
            if(hover==4) {panel.wasDown=down; ClosePanel("Close button"); return false;}
            SaveSensitivity(); LogF("Sensitivity panel: button %d clicked -> %.2fx",hover,mouseSensitivity.load());
        }
    }
    if(panel.dragging&&haveMouse) {const float v=TrackValue(track,client.x); if(v!=mouseSensitivity.load()) mouseSensitivity=v;}
    if(!down&&panel.dragging) {panel.dragging=false; SaveSensitivity(); LogF("Sensitivity panel: slider set to %.2fx",mouseSensitivity.load());}
    panel.wasDown=down;
    // --- keyboard ---
    if(active&&!testMode) {
        if(GetAsyncKeyState(VK_ESCAPE)&1) {ClosePanel("Esc"); return false;}
        if(GetAsyncKeyState('R')&1) {mouseSensitivity=1.0f; SaveSensitivity();}
        auto held=[](int k){return (GetAsyncKeyState(k)&0x8000)!=0;};
        const int dir=(held(VK_RIGHT)||held('D'))-(held(VK_LEFT)||held('A')); const DWORD now=GetTickCount();
        if(!dir) panel.heldSince=0;
        else if(!panel.heldSince||(now-panel.heldSince>=400&&now-panel.lastStep>=40)) {
            if(!panel.heldSince) panel.heldSince=now;
            panel.lastStep=now; mouseSensitivity=StepSensitivity(mouseSensitivity.load(),dir); SaveSensitivity();
        }
    }
    // --- draw ---
    const float v=mouseSensitivity.load(); const int pressed=down?hover:-1;
    if(panel.dirty||v!=panel.shownValue||hover!=panel.shownHover||pressed!=panel.shownDown||panel.bw!=where.right-where.left||panel.bh!=where.bottom-where.top) {
        PaintPanel(s,hover,pressed); panel.shownValue=v; panel.shownHover=hover; panel.shownDown=pressed; panel.dirty=true;
    }
    D3DSURFACE_DESC dd; dest->GetDesc(&dd);
    if(!panel.bits||((panel.dirty||!panel.surface||panel.format!=dd.Format)&&!UploadPanel(d,dd.Format))) {++panel.framesMissed; return false;}
    panel.dirty=false;
    RECT to=where; const HRESULT hr=d->StretchRect(panel.surface,nullptr,dest,&to,D3DTEXF_NONE);
    if(FAILED(hr)) {if(panel.framesMissed++<3) LogF("Sensitivity panel: copy into player window %d failed (%08lx)",window,hr); return false;}
    if(haveMouse) DrawPointer(d,dest,picture,client);
    if(!panel.loggedPlacement) {
        panel.loggedPlacement=true; RECT wr{}; if(views[window]) GetWindowRect(views[window],&wr);
        LogF("Sensitivity panel drawn into player window %d (Display %d, screen %ld,%ld %ldx%ld): panel %ld,%ld %ldx%ld inside picture %ld,%ld %ldx%ld (fully inside %u), format %d, mouse %s at %ld,%ld",
             window,window+1,wr.left,wr.top,wr.right-wr.left,wr.bottom-wr.top,where.left,where.top,where.right-where.left,where.bottom-where.top,
             picture.left,picture.top,picture.right-picture.left,picture.bottom-picture.top,
             where.left>=picture.left&&where.top>=picture.top&&where.right<=picture.right&&where.bottom<=picture.bottom,(int)dd.Format,
             haveMouse?"held in the picture":"not read",client.x,client.y);
    }
    ++panel.framesDrawn; return true;
}
static void KeyboardForMainOnly() {
    if(!deviceDisplay[DevKeyboard]) {Log("Mouse + keyboard not assigned: co-op input unchanged");return;}
    gameBase=(BYTE*)GetModuleHandleW(nullptr);
    BYTE* idle=gameBase+0x5886a7; BYTE* build=gameBase+0x6152d0; BYTE* uses=gameBase+0x614ec0;
    static const BYTE idleBytes[]={0xc7,0x86,0xbc,0xc4,0x15,0x00,0,0,0,0,0xc7,0x86,0xc0,0xc4,0x15,0x00,0,0,0,0,0x5e,0xc3};
    static const BYTE usesBytes[]={0x8b,0x44,0x24,0x04,0x32,0xc9,0x85,0xc0};
    static const BYTE buildBytes[]={0x55,0x8b,0xec,0x83,0xe4,0xf0,0x81,0xec,0xc8,0x00,0x00,0x00,0x8b,0x45,0x08};
    if(memcmp(idle,idleBytes,sizeof(idleBytes))||memcmp(build,buildBytes,sizeof(buildBytes))||
       memcmp(uses,usesBytes,sizeof(usesBytes))) {Log("Co-op mouse + keyboard: game code differs, not patched");return;}
    InitializeCriticalSection(&buildLock);
    if(MH_CreateHook(build,HookBuildInput,(void**)&realBuildInput)!=MH_OK||MH_EnableHook(build)!=MH_OK||
       MH_CreateHook(uses,HookUsesKeyboard,(void**)&realUsesKeyboard)!=MH_OK||MH_EnableHook(uses)!=MH_OK) {Log("Co-op mouse + keyboard: hook failed");return;}
    Jump(idle,(void*)ModeIdleStub,0xe9,5);
    Log("Co-op mouse + keyboard: per-player input mode hook on");
    PerPlayerInteractions();
    MenuChecksForKeyboardPlayer();
    PerPlayerMenus();
}
// ---- WM_PAINT freeze (decompiled + hang dump, 2026-11-12) ----
// The game window procedure answers WM_PAINT (switch case at exe+0x874095) with exe+0x8ca8f0, which calls
// device->Present on the MAIN thread while the render thread presents every frame. Moving, filling or
// minimizing the player windows (Ctrl+1/2, Ctrl+Alt+1/2, Alt+Tab) invalidates the game window; the main thread
// handles that WM_PAINT when it pumps messages, i.e. during loading screens. The hang dump showed the main
// thread there, inside DrawViews waiting for the Direct3D device lock, while the render thread was inside a
// player-window Present that never returned. While the player windows are shown they cover the game window
// and the render thread repaints it every frame, so this extra main-thread Present is skipped (the game still
// validates the window with BeginPaint/EndPaint, so no WM_PAINT storm). With F8 (windows off) it runs as before.
using RepaintFn=void (__fastcall*)(void*,void*);
static RepaintFn realRepaint;
static void __fastcall HookRepaint(void* self,void* edx) {
    if(uiReady.load()&&enabled.load()) {
        static bool logged=false; if(!logged) {logged=true; Log("WM_PAINT present on the main thread skipped (player windows shown)");}
        return;
    }
    realRepaint(self,edx);
}
static void SkipPaintPresent() {
    BYTE* at=(BYTE*)GetModuleHandleW(nullptr)+0x8ca8f0;
    static const BYTE bytes[]={0x56,0x8b,0xf1,0x8b,0x8e,0x98,0x00,0x00,0x00,0x85,0xc9};
    if(memcmp(at,bytes,sizeof(bytes))) {Log("WM_PAINT present: game code differs, not hooked");return;}
    if(MH_CreateHook(at,HookRepaint,(void**)&realRepaint)!=MH_OK||MH_EnableHook(at)!=MH_OK) {Log("WM_PAINT present: hook failed");return;}
    Log("WM_PAINT present hook on");
}
// ---- Mouse + keyboard keep working when the game window loses focus (decompiled) ----
// The window-active flag [[0x15df9cc]+0x2551] is cleared on deactivate (exe+0x876415). While it is 0:
//  exe+0x826afd skips the keyboard DirectInput read, exe+0x82769d skips the mouse DirectInput read,
//  exe+0x876426 marks the pad slots (exe+0x814e10). The keyboard device is also created
//  EXCLUSIVE|FOREGROUND (push 0x15 at exe+0x826711), so it cannot be acquired without focus.
// Cursor clipping (exe+0x8274ba) still follows the real focus, so the cursor is never trapped.
static bool ReadPtr(uintptr_t at,uintptr_t& v);
// F1 is the mod's swap key. The game reads keys only from this DirectInput keyboard (GetDeviceState, 256
// bytes, exe+0x826f15; its GetAsyncKeyState table exe+0x100d654 holds mouse buttons only), so clearing
// DIK_F1 there hides F1 from the game. The mod itself still sees it through GetAsyncKeyState.
using DeviceStateFn=HRESULT(WINAPI*)(void*,DWORD,void*);
static DeviceStateFn realDeviceState=nullptr;
static void* keyboardDevice=nullptr;
static HRESULT WINAPI HookDeviceState(void* dev,DWORD size,void* data) {
    const HRESULT hr=realDeviceState(dev,size,data);
    if(FAILED(hr)||!data) return hr;
    if(dev==keyboardDevice&&size==256) {
        ((BYTE*)data)[0x3b]=((BYTE*)data)[0x3d]=0;   // DIK_F1, DIK_F3 (mod keys)
        if(sensitivityPanelOpen.load()) memset(data,0,size);   // F3 panel open: keys go to the panel only
    } else if(sensitivityPanelOpen.load()&&(size==16||size==20)) memset(data,0,size);   // DIMOUSESTATE(2): no camera turn while using the panel
    return hr;
}
static void FocusFreeInput() {
    BYTE* base=(BYTE*)GetModuleHandleW(nullptr);
    static int state=0;   // 0 = patch code, 1 = waiting for keyboard device, 2 = done, -1 = gave up
    if(state==0) {
        struct {DWORD at; BYTE n; const char* was; const char* now;} p[]={
            {0x826afd,2,"\x74\x0f","\x90\x90"},{0x82769d,2,"\x74\x11","\x90\x90"},
            {0x826711,2,"\x6a\x15","\x6a\x0a"},{0x876426,5,"\xe8\xe5\xe9\xf9\xff","\x90\x90\x90\x90\x90"}};
        for(auto& x:p) if(memcmp(base+x.at,x.was,x.n)) {Log("Focus-free input: game code differs at %08x, not patched",x.at);state=-1;return;}
        for(auto& x:p) WriteCode(base+x.at,(const BYTE*)x.now,x.n);
        state=1;
    }
    if(state!=1) return;
    uintptr_t kbd=0,dev=0,app=0; HWND hwnd=nullptr;
    if(!ReadPtr((uintptr_t)base+0x11de918,kbd)||!ReadPtr(kbd+0xa08,dev)||!ReadPtr((uintptr_t)base+0x11df9cc,app)||
       !ReadGame(app+0x12c,&hwnd,sizeof(hwnd))||!hwnd) return;   // keyboard not created yet: try next frame
    void** vt=*(void***)dev;   // IDirectInputDevice8: [7] Acquire, [8] Unacquire, [13] SetCooperativeLevel
    ((HRESULT(WINAPI*)(uintptr_t))vt[8])(dev);
    const HRESULT hr=((HRESULT(WINAPI*)(uintptr_t,HWND,DWORD))vt[13])(dev,hwnd,0x0a);   // DISCL_NONEXCLUSIVE|DISCL_BACKGROUND
    Log("Focus-free input: mouse + keyboard read without focus, keyboard background result %08x",(unsigned)hr);
    keyboardDevice=(void*)dev;
    const bool f1=MH_CreateHook(vt[9],(void*)HookDeviceState,(void**)&realDeviceState)==MH_OK&&MH_EnableHook(vt[9])==MH_OK;
    Log("F1 hidden from the game: %u",f1);
    state=2;
}
// Once a second, logs what the game's input actually holds (only when it changes).
static void LogInputState() {
    static DWORD last=0; static unsigned moved=0,prev[11]={~0u};
    BYTE* base=(BYTE*)GetModuleHandleW(nullptr);
    uintptr_t mouse=0,input=0,game=0,pad=BioPad(); int dxy[2]={0,0};
    if(ReadPtr((uintptr_t)base+0x11de940,mouse)&&ReadGame(mouse+0x2c,dxy,8)&&(dxy[0]||dxy[1])) moved=1;
    const DWORD now=GetTickCount(); if(now-last<1000) return; last=now;
    unsigned v[11]={0}; v[5]=moved; if(mouse) ReadGame(mouse+0x29,&v[10],1);   // 1 = mouse captured (no menu cursor)
    moved=0; v[8]=(unsigned)RegionXInput(0); v[9]=(unsigned)RegionXInput(1);
    if(ReadPtr((uintptr_t)base+0x117d120,input)) {ReadGame(input+0x15c4bc,&v[0],4);ReadGame(input+0x15c4c0,&v[1],4);}
    if(pad) {ReadGame(pad+0xba0,&v[2],4);ReadGame(pad+0xba4,&v[3],4);}
    if(ReadPtr((uintptr_t)base+0x117ae00,game)) ReadGame(game+0x7a8,&v[4],4);
    v[6]=GetForegroundWindow()==gameWindow; v[7]=CoopOn();
    // Pad slots, connected controllers and the join request, logged when they change (controller-only co-op).
    {
        static char lastSlots[200];
        char slots[200]; int n=snprintf(slots,sizeof(slots),"Pad slots:");
        for(int s=0;s<4;++s) {
            BYTE used=0,type=0; DWORD index=0; const uintptr_t at=pad+0x3c+s*0x2cc;
            if(!pad||!ReadGame(at+4,&used,1)||!ReadGame(at+7,&type,1)||!ReadGame(at+0x10,&index,4)||!used||!type) n+=snprintf(slots+n,sizeof(slots)-n," %d=none",s);
            else if(type==2) n+=snprintf(slots+n,sizeof(slots)-n," %d=DirectInput",s);
            else n+=snprintf(slots+n,sizeof(slots)-n," %d=XInput%lu",s,index);
        }
        static unsigned connected=0,tick=0; PadState st;   // empty XInput numbers are slow to ask: every 5 s
        if(tick++%5==0) {connected=0; for(DWORD p=0;p<4&&realGetState;++p) if(realGetState(p,&st)==ERROR_SUCCESS) connected|=1u<<p;}
        uintptr_t area=0; int join=-1; if(ReadPtr((uintptr_t)base+0x11de720,area)) ReadGame(area+0x3898,&join,4);
        n+=snprintf(slots+n,sizeof(slots)-n,"; controllers on XInput %x; join pad %d",connected,join);
        if(strcmp(slots,lastSlots)) {
            strcpy(lastSlots,slots); LogF("%s",slots);
            if(connected&0xc) Log("  -> a controller on XInput 2 or 3 is connected: the dashboard only places XInput 0 and 1");
        }
    }
    if(!memcmp(v,prev,sizeof(v))) return; memcpy(prev,v,sizeof(v));
    char line[260]; snprintf(line,sizeof(line),"Input: mode=%u/%u mainPad=%d subPad=%d mainChar=%u mouseMoved=%u focus=%u coop=%u region0XInput=%d region1XInput=%d swap=%u mouseCaptured=%u",
        v[0],v[1],(int)v[2],(int)v[3],v[4],v[5],v[6],v[7],(int)v[8],(int)v[9],(unsigned)swapPlayers.load(),v[10]);
    Log(line);
}
// F2 does what the game's co-op menu does once Player 2 is picked (exe+0x8b952b and exe+0xa908a3):
// both players present (+0x7ac), the partner's pad (+0x3898), then join request type 1 (exe+0x436340).
// The game's own loop (exe+0x4287b0) then joins the partner if that character is free (+0x7b4 == 1).
static bool ReadPtr(uintptr_t at,uintptr_t& v){SIZE_T n=0;v=0;return ReadProcessMemory(GetCurrentProcess(),(void*)at,&v,sizeof(v),&n)&&v;}
static void ForceCoop() {
    BYTE* base=(BYTE*)GetModuleHandleW(nullptr);
    uintptr_t game,area;
    if(!ReadPtr((uintptr_t)base+0x117ae00,game)||!ReadPtr((uintptr_t)base+0x11de720,area)) {Log("F2: game not ready");return;}
    BYTE* getter=base+0x4d690;   // returns the join-request object, or 0 outside a level
    if(getter[0]!=0xa1||memcmp(getter+5,"\x56\x8b\xb0\x2c\x38\x00\x00",7)) {Log("F2: game code differs");return;}
    if(*(int*)(game+0x8f0)==1) {Log("F2: co-op is already on");return;}
    const int partner=(*(int*)(game+0x7a8))^1, state=*(int*)(game+0x7b4+4*(partner&7)), pad=PartnerSlot();
    if(pad<0) {Log("F2: no game pad slot holds the partner's controller yet (main slot %u)",MainPadSlot());return;}
    auto object=(BYTE*)((void*(__fastcall*)(void*))getter)(nullptr);
    if(!object) {Log("F2: no level loaded yet");return;}
    *(DWORD*)(game+0x7ac)|=3;
    *(int*)(area+0x3898)=pad;
    *(int*)(object+0x6e0)=pad; *(int*)(object+0x6e4)=0; *(int*)(object+0x6dc)=1;
    Log("F2: co-op join requested for pad %u (partner state %u)",pad,state);
}
// ---- ERR09 at startup with a resolution taller than the desktop (decompiled 2026-11-12) ----
// The game sizes its window from config.ini [DISPLAY] Resolution (exe+0x875bb1: "WxH", default 1280x720, clamped
// to 640..8192 x 480..8192), then takes the back buffer size from that window's client area (GetClientRect at
// exe+0x8c9801 -> render+0xb8/0xbc -> CreateDevice). Windows limits a window to the desktop, so 3840x2160 on two
// 1920x1080 monitors became a 3840x1100 device. The game then queued a mode change to 3840x2160 (exe+0x8c9b4d),
// applied only once its window is active (exe+0x87551a -> exe+0x8cad80), a few seconds into startup. That real
// Reset returned D3DERR_INVALIDCALL and the game's check exe+0x8c5690 showed "ERR09 : Unsupported function".
// Now the game gets the size it gave its window, so the device starts at the configured resolution and the startup
// mode change has nothing to change (skipped as "same windowed mode", like every 1920x1080 start).
static void GameResolution(const char* text,LONG& w,LONG& h) {   // the game's parsing: strtok(" _-a-zA-Z") + atoi
    auto cut=[](char c){return c==' '||c=='_'||c=='-'||(c>='a'&&c<='z')||(c>='A'&&c<='Z');};
    LONG v[2]={1280,720}; int n=0;
    for(const char* p=text;p&&*p&&n<2;) {
        if(cut(*p)) {++p;continue;}
        v[n++]=atoi(p); while(*p&&!cut(*p)) ++p;
    }
    w=std::clamp<LONG>(v[0],640,8192); h=std::clamp<LONG>(v[1],480,8192);
}
static void** gameClientRectSlot;   // the game's own import slot, so any other hook on GetClientRect still runs
static BOOL WINAPI DeviceClientRect(HWND window,RECT* r) {
    if(!((BOOL (WINAPI*)(HWND,RECT*))*gameClientRectSlot)(window,r)) return FALSE;
    const char* ini=(const char*)(gameBase+0x113e514);   // config.ini path inside the game's config object (exe+0x113e510)
    const size_t n=strnlen(ini,MAX_PATH);
    if(n<10||n>=MAX_PATH||_stricmp(ini+n-10,"config.ini")) {Log("Game config path not found: back buffer follows the window");return TRUE;}
    char text[64]={}; GetPrivateProfileStringA("DISPLAY","Resolution","1280x720",text,sizeof(text),ini);
    LONG w,h; GameResolution(text,w,h);
    LogF("Game back buffer: window client %ldx%ld, configured resolution %ldx%ld",r->right-r->left,r->bottom-r->top,w,h);
    if(r->right-r->left!=w||r->bottom-r->top!=h) {
        *r={0,0,w,h};
        Log("Back buffer uses the configured resolution (Windows had limited the window): no startup device reset needed");
    }
    return TRUE;
}
// Diagnostics: every failed Direct3D call the game checks. exe+0x8c5690 (thiscall: result, context; ret 8) turns
// D3DERR_INVALIDCALL into "ERR09 : Unsupported function" and other results into their own error dialogs.
#ifdef _MSC_VER
#include <intrin.h>
#define CallerAddress() _ReturnAddress()
#else
#define CallerAddress() __builtin_return_address(0)
#endif
using CheckD3DFn=int (__fastcall*)(void*,void*,HRESULT,const char*);
static CheckD3DFn realCheckD3D;
static int __fastcall HookCheckD3D(void* self,void* edx,HRESULT hr,const char* where) {
    static int logged=0;
    if(FAILED(hr)&&logged<50) {++logged;
        LogF("GAME DIRECT3D ERROR %08lx checked at exe+%x (stage: %s); the game shows its error dialog now%s",hr,
             (unsigned)((BYTE*)CallerAddress()-gameBase),stage.load(),hr==D3DERR_INVALIDCALL?" (ERR09)":"");}
    return realCheckD3D(self,edx,hr,where);
}
static void GameStartupFixes() {
    gameBase=(BYTE*)GetModuleHandleW(nullptr);
    BYTE* at=gameBase+0x8c9801; void** slot=(void**)(gameBase+0xe3f36c);
    if(at[0]==0xff&&at[1]==0x15&&*(void***)(at+2)==slot) {gameClientRectSlot=slot; Jump(at,(void*)DeviceClientRect,0xe8,6); Log("Back buffer size from the configured resolution: on");}
    else Log("Back buffer size: game code differs, not patched");
    BYTE* check=gameBase+0x8c5690;
    if(!memcmp(check,"\x56\x8b\x74\x24\x08\x85\xf6",7)&&MH_CreateHook(check,HookCheckD3D,(void**)&realCheckD3D)==MH_OK) Log("Game Direct3D error log: on");
    else Log("Game Direct3D error log: game code differs, not hooked");
}
// The game creates its device in exclusive fullscreen (its FullScreen=ON); switching to windowed later
// in Reset sometimes failed and ended in "ERR09: Unsupported function". So it starts windowed instead.
using CreateDeviceFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
static CreateDeviceFn realCreateDevice;
static HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* api,UINT a,D3DDEVTYPE t,HWND w,DWORD f,D3DPRESENT_PARAMETERS* pp,IDirect3DDevice9** d) {
    if(pp&&!testMode) KeepWindowed(pp);
    if(pp) LogF("Game CreateDevice: adapter %u (monitor %p), flags %08lx, %ux%u, device window %p",a,(void*)api->GetAdapterMonitor(a),f,
                pp->BackBufferWidth,pp->BackBufferHeight,(void*)pp->hDeviceWindow);
    if(w) LogWindow("Game focus window",w);
    Stage("game CreateDevice");
    const HRESULT hr=realCreateDevice(api,a,t,w,f,pp,d);
    Log("Game device created: result %08x",hr); Stage("game device created");
    return hr;
}
static DWORD WINAPI Initialize(void*) {
    SetProcessDPIAware();
    GetModuleFileNameW(selfModule,logPath,MAX_PATH);
    wchar_t* tail=wcsrchr(logPath,L'.'); if(tail) wcscpy_s(tail,MAX_PATH-(tail-logPath),L".log");
    wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);
    testMode=wcsstr(exe,L"DualWindowsTest.exe")!=nullptr;
    Log("DualWindows prototype 0.6.3 initializing");
    WNDCLASSW wc={};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=selfModule;wc.lpszClassName=L"Rev2DualProbe";
    RegisterClassW(&wc);
    HWND probe=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPED,0,0,32,32,nullptr,nullptr,selfModule,nullptr);
    IDirect3D9* api=Direct3DCreate9(D3D_SDK_VERSION); IDirect3DDevice9* d=nullptr;
    LogDisplays(api);
    D3DPRESENT_PARAMETERS pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=probe;
    pp.BackBufferWidth=32;pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
    HRESULT hr=api?api->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,probe,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&d):E_FAIL;
    if(SUCCEEDED(hr)) {
        void** vt=*(void***)d;
        presentTarget=vt[17];resetTarget=vt[16];
        MH_STATUS m=MH_Initialize();
        if(m==MH_OK) CreateMovieDrawHooks(vt);
        if(m==MH_OK) CreateInputSwapHooks();
        if(m==MH_OK&&!testMode&&MH_CreateHook((*(void***)api)[16],HookCreateDevice,(void**)&realCreateDevice)!=MH_OK) Log("CreateDevice hook failed");
        if(m==MH_OK&&!testMode) GameStartupFixes();   // before the game's render setup (it waits for this thread)
        IDirect3DSwapChain9* probeChain=nullptr;
        if(m==MH_OK&&SUCCEEDED(d->GetSwapChain(0,&probeChain))) {
            BYTE* fn=(BYTE*)(*(void***)probeChain)[3];
            LogF("Swap-chain Present at %p starts %02x %02x %02x (e9 = already hooked by someone)",(void*)fn,fn[0],fn[1],fn[2]);
            if(MH_CreateHook(fn,HookChainPresent,(void**)&rawChainPresent)!=MH_OK) {rawChainPresent=nullptr; Log("Swap-chain Present hook failed");}
            probeChain->Release();
        }
        if(m==MH_OK && MH_CreateHook(vt[17],HookPresent,(void**)&realPresent)==MH_OK &&
            MH_CreateHook(vt[16],HookReset,(void**)&realReset)==MH_OK && MH_EnableHook(MH_ALL_HOOKS)==MH_OK)
            {Log("D3D9 Present/Reset hooks ready"); stage="hooks ready, waiting for the game's device";}
        else Log("Hook initialization failed");
        d->Release();
    } else Log("D3D9 probe failed %08x",hr);
    if(api) api->Release(); DestroyWindow(probe);
    return 0;
}
extern "C" __declspec(dllexport) void InitializeASI() {
    if(InterlockedCompareExchange(&started,1,0)==0) {
        wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);
        GetModuleFileNameW(selfModule,logPath,MAX_PATH);   // so the dashboard can log too
        if(wchar_t* tail=wcsrchr(logPath,L'.')) wcscpy_s(tail,MAX_PATH-(tail-logPath),L".log");
        // Controls dashboard first: the game waits here, before it creates its window.
        if(!wcsstr(exe,L"DualWindowsTest.exe")) {StartWatchdog(); Stage("dashboard"); ShowInputSetup(); Stage("dashboard closed");}
        // Hooks must exist before the game makes its device: on a single screen its exclusive fullscreen
        // device otherwise makes the probe fail (D3DERR_DEVICELOST) and the mod never starts.
        // Waiting is capped in case this runs under the loader lock (the thread could not start yet).
        HANDLE t=CreateThread(nullptr,0,Initialize,nullptr,0,nullptr);
        if(t) {if(WaitForSingleObject(t,3000)!=WAIT_OBJECT_0) Log("Hook setup still running after 3 s; game continues"); CloseHandle(t);}}
}
extern "C" __declspec(dllexport) BOOL DualWindowsInputSelfTest() {
    testMode=true; bool ok=InputRoutingSelfTest();
    // Keyboard player follows F1 (testMode: main pad slot is 0). Two fake players: slot 0 (main), slot 1 (partner).
    static BYTE mainP[0x7930],partnerP[0x7930]; mainP[0x7924]=0; partnerP[0x7924]=1; const bool swap=swapPlayers.load();
    for(int round=0;round<4;++round) {   // swap repeatedly: keyboard always on exactly one player
        swapPlayers=round&1;
        ok&=IsKeyboardPlayer(mainP)==!swapPlayers.load()&&IsKeyboardPlayer(partnerP)==swapPlayers.load();
    }
    // Real failing case: the region-0 (solo) character sits on slot 1, not the "main pad" slot 0.
    // The keyboard must still follow the character on the region, before and after F1.
    mainP[0x7924]=1; partnerP[0x7924]=0;
    fakeRegionPlayer[0]=(uintptr_t)mainP; fakeRegionPlayer[1]=(uintptr_t)partnerP;
    for(int round=0;round<4;++round) {
        swapPlayers=round&1;
        ok&=IsKeyboardPlayer(mainP)==!swapPlayers.load()&&IsKeyboardPlayer(partnerP)==swapPlayers.load();
        // Menu checks (inventory -> mouse cursor) must use this same player, never the other one.
        ok&=KeyboardRegionPlayer()==(uintptr_t)(swapPlayers.load()?partnerP:mainP);
    }
    // Device switching on one display: a new press of one device switches, both at once keep the current one.
    ok&=NextOnPad(false,true,false)&&!NextOnPad(true,false,true)&&NextOnPad(true,true,true)&&!NextOnPad(false,true,true)&&
        NextOnPad(true,false,false)&&!NextOnPad(false,false,false);
    // Dashboard configs A-E (pad index 0 = physical, 1 = Parsec unless noted). Each display's controllers must feed
    // exactly the character on that display (menus, solo + join, co-op, F1 rounds); mouse + keyboard stays on its display.
    const int savedDisplay[DevCount]={deviceDisplay[0],deviceDisplay[1],deviceDisplay[2]};
    struct Config {int kb,p1,p2; unsigned withKeyboard;} configs[]={
        {1,2,0,0},   // A: D1 mouse+keyboard, D2 physical
        {1,1,2,1},   // B: D1 mouse+keyboard + physical, D2 Parsec
        {0,1,2,0},   // C: D1 physical, D2 Parsec
        {0,2,1,0},   // D: D1 Parsec, D2 physical
        {1,2,1,2}};  // E: D1 mouse+keyboard + Parsec, D2 physical
    auto pads=[](DWORD i){DWORD o[2]={9,9};int n=PadsFor(i,o);return n*100+(n>0?o[0]*10:0)+(n>1?o[1]:0);};
    auto expect=[](int display){int n=0,code=0; for(int p=0;p<2;++p) if(deviceDisplay[DevPad1+p]==display) {code+=n?p:p*10; ++n;} return n*100+code;};
    for(const Config& c:configs) {
        deviceDisplay[DevKeyboard]=c.kb; deviceDisplay[DevPad1]=c.p1; deviceDisplay[DevPad2]=c.p2; swapPlayers=false;
        ok&=PadsWithKeyboard()==c.withKeyboard;
        fakeRegionX[0]=fakeRegionX[1]=-2;                                   // menus: main slot (XInput 0) = display MainView()+1
        ok&=pads(0)==expect(MainView()+1)&&pads(1)==expect(2-MainView());
        for(int x0=0;x0<2;++x0) {
            fakeRegionX[0]=x0; fakeRegionX[1]=-2;                           // solo: the other display's Start reaches the free XInput
            ok&=pads(x0)==expect(MainView()+1)&&pads(1-x0)==expect(2-MainView());
            fakeRegionX[1]=1-x0;                                            // co-op
            for(int round=0;round<4;++round) {
                swapPlayers=round&1;
                const int region1=MainView();                               // region on window 0 = display 1
                ok&=pads(fakeRegionX[region1])==expect(1)&&pads(fakeRegionX[1-region1])==expect(2);
                if(c.kb) {
                    ok&=((swapPlayers.load()?1:0)^MainView())==c.kb-1;   // keyboard player stays on its display
                    fakeRegionPlayer[0]=(uintptr_t)mainP; fakeRegionPlayer[1]=(uintptr_t)partnerP;
                    BYTE* kbP=swapPlayers.load()?partnerP:mainP; BYTE* other=kbP==mainP?partnerP:mainP;
                    for(int onPad=0;onPad<2;++onPad) {                      // the keyboard player's mode follows its device, the other's never
                        keyboardDisplayOnPad=onPad!=0;
                        ok&=(IsKeyboardPlayer(kbP)&&KeyboardInUse())==!onPad&&!(IsKeyboardPlayer(other)&&KeyboardInUse());
                        // Menus: the controller player's inventory never takes mouse + keyboard; the keyboard player's
                        // takes it only while that display is on mouse + keyboard (its controller always works).
                        ok&=MenuPadOnly(kbP)==onPad&&MenuPadOnly(other)==1;
                    }
                    static BYTE offScreen[0x7930];                          // not on a region / AI partner: game's own input
                    ok&=MenuPadOnly(offScreen)==-1&&MenuPadOnly(nullptr)==-1;
                    *(int*)(other+0x7920)=1; ok&=MenuPadOnly(other)==-1; *(int*)(other+0x7920)=0;
                    keyboardDisplayOnPad=false; fakeRegionPlayer[0]=fakeRegionPlayer[1]=0;
                }
            }
            swapPlayers=false;
        }
    }
    fakeRegionX[0]=fakeRegionX[1]=-2;
    for(int d=0;d<DevCount;++d) deviceDisplay[d]=savedDisplay[d];
    fakeRegionPlayer[0]=fakeRegionPlayer[1]=0;
    // ERR09: the back buffer gets the configured resolution, read exactly like the game reads it (exe+0x875bb1).
    const struct {const char* text; LONG w,h;} res[]={{"3840x2160",3840,2160},{"1920x1080",1920,1080},{"1280 X 720",1280,720},
        {"",1280,720},{"2560x",2560,720},{"100x100",640,480},{"9000x9000",8192,8192}};
    for(auto& r:res) {LONG w=0,h=0; GameResolution(r.text,w,h); if(w!=r.w||h!=r.h) {ok=false; LogF("Resolution \"%s\" read as %ldx%ld",r.text,w,h);}}
    // Menu cursor: every point of a player window's picture reaches its back-buffer pixel; corners reach the
    // region's corners, points outside the picture stay on its edge (never in the other player's region).
    {
        const RECT bottom={0,1080,1920,2160},window={0,0,1280,720},letterbox={0,90,1280,810};
        auto at=[](const RECT& s,const RECT& d,LONG x,LONG y){return WindowToBackBuffer(s,d,{x,y});};
        POINT a=at(bottom,window,0,0),b=at(bottom,window,1279,719),c=at(bottom,window,640,360),o=at(bottom,window,-500,5000),t=at(bottom,letterbox,100,0);
        const bool corners=a.x==0&&a.y==1080&&b.x==1918&&b.y==2158&&c.x==960&&c.y==1620&&o.x==0&&o.y==2158&&t.y==1080;
        bool stays=true;
        for(LONG x=-50;x<1400;x+=37) for(LONG y=-50;y<800;y+=29) {POINT p=at(bottom,window,x,y); stays&=p.x>=0&&p.x<1920&&p.y>=1080&&p.y<2160;}
        bool round=true;   // through the game's own formula (exe+0x8278e8): (client - vp.left) * W / vp width, W = frame width
        const RECT vps[]={{0,0,3840,2160},{0,0,3840,1100},{10,20,1930,1100}};
        for(auto& vp:vps) for(LONG px:{0L,1L,960L,1919L,3839L}) {
            const LONG cx=GameClientFor(px,vp.left,vp.right,3840),cy=GameClientFor(px*9/16,vp.top,vp.bottom,2160);
            const LONG gx=(LONG)((int64_t)(cx-vp.left)*3840/(vp.right-vp.left)),gy=(LONG)((int64_t)(cy-vp.top)*2160/(vp.bottom-vp.top));
            round&=abs(gx-px)<=3&&abs(gy-px*9/16)<=3;
        }
        if(!corners||!stays||!round) {ok=false; LogF("Menu cursor mapping failed: corners %u, stays %u, round trip %u",corners,stays,round);}
    }
    // Mouse camera sensitivity: exact at 1.0x, fractions kept (0.1x of 3 per frame = 30 over 100 frames), clamped.
    {
        float rest=0; int sum=0; for(int k=0;k<100;++k) sum+=ScaleDelta(3,0.1f,rest);
        float r1=0; const bool same=ScaleDelta(7,1.0f,r1)==7&&ScaleDelta(-5,1.0f,r1)==-5&&r1==0.0f;
        float r2=0; const bool twice=ScaleDelta(-5,2.0f,r2)==-10;
        const bool clamp=ClampSensitivity(0.0f)==0.1f&&ClampSensitivity(5.0f)==3.0f&&ClampSensitivity(0.0f/0.0f)==1.0f&&ClampSensitivity(1.5f)==1.5f;
        float k=1.0f; for(int i=0;i<100;++i) k=StepSensitivity(k,1);   // F3: 100 presses of 0.01 land exactly on 2.00
        const bool steps=k==2.0f&&StepSensitivity(1.0f,-1)==0.99f&&StepSensitivity(0.1f,-1)==0.1f&&StepSensitivity(3.0f,1)==3.0f;
        if(!steps) {ok=false; LogF("F3 step failed: 100 steps from 1.00 gave %.4f",k);}
        if(sum<29||sum>30||!same||!twice||!clamp) {ok=false; LogF("Sensitivity scaling failed: sum %d, same %u, twice %u, clamp %u",sum,same,twice,clamp);}
    }
    // Inventory button: decided by the player being built when human, never by the other character.
    {
        static BYTE human[0x7930],ai[0x7930],first[0x7930]; *(int*)(ai+0x7920)=1;
        if(GatePlayer(human,first)!=human||GatePlayer(ai,first)!=first||GatePlayer(nullptr,first)!=first) {ok=false; Log("Inventory gate choice failed");}
    }
    swapPlayers=swap; return ok;
}
extern "C" __declspec(dllexport) void DualWindowsShowSetup() {wcscpy_s(logPath,L"DualWindowsSetupTest.log");ShowInputSetup();}
extern "C" __declspec(dllexport) UINT DualWindowsStats() {return successes;}
extern "C" __declspec(dllexport) UINT DualWindowsChainCreates() {return chainCreates;}
extern "C" __declspec(dllexport) UINT DualWindowsFrames() {return frames;}
extern "C" __declspec(dllexport) UINT DualWindowsPixels() {return pixelChecks;}
extern "C" __declspec(dllexport) void DualWindowsTestMovie() {if(testMode)movieDrawn.store(true);}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {selfModule=h;DisableThreadLibraryCalls(h);}
    return TRUE;
}









