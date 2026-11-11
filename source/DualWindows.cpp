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
static BYTE* fusionModule=nullptr;
static wchar_t logPath[MAX_PATH];
using PresentFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
using ResetFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
static PresentFn realPresent;
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
    if(movieSnapshot) {movieSnapshot->Release();movieSnapshot=nullptr;}
    movieDrawn.store(false);
    lastFrameWasMovie.store(false);
    ReleaseSharedGui();
    for(int i=0;i<2;++i) { if(chains[i]) chains[i]->Release(); chains[i]=nullptr; chainW[i]=chainH[i]=0; }
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
            Log("Game window made borderless: %u x %u",m.right-m.left,m.bottom-m.top);
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
            i?L"RE Revelations 2 — Display 2 (v0.6.0)":L"RE Revelations 2 — Display 1 (v0.6.0)",   // same names as the dashboard
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
    uiReady.store(true,std::memory_order_release);
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    return 0;
}
static bool MakeWindows(IDirect3DDevice9* device) {
    if(uiReady.load(std::memory_order_acquire))return true;
    if(!uiThread) {
        D3DDEVICE_CREATION_PARAMETERS cp={}; if(FAILED(device->GetCreationParameters(&cp)))return false;
        gameWindow=cp.hFocusWindow;
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
    // Async only. Never synchronously call the UI thread from Direct3D hooks.
    if(views[0])PostMessageW(views[0],WM_APP+1,on,0);
    Log(on?"Output windows visible":"Original game view visible");
}
static bool EnsureChain(IDirect3DDevice9* d,int i,D3DFORMAT format) {
    UINT w=std::max(1u,uiWidth[i].load()),h=std::max(1u,uiHeight[i].load());
    if(chains[i]&&chainW[i]==w&&chainH[i]==h) return true;
    if(chains[i]) {chains[i]->Release();chains[i]=nullptr;}
    D3DPRESENT_PARAMETERS pp={}; pp.Windowed=TRUE; pp.hDeviceWindow=views[i];
    pp.BackBufferWidth=w; pp.BackBufferHeight=h; pp.BackBufferFormat=format;
    pp.BackBufferCount=1; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT hr=d->CreateAdditionalSwapChain(&pp,&chains[i]);
    static HRESULT lastError=S_OK;
    if(FAILED(hr)) { if(hr!=lastError) Log("CreateAdditionalSwapChain failed %08x",hr); lastError=hr; return false; }
    lastError=S_OK;
    chainW[i]=w; chainH[i]=h; return true;
}
static void ForceCoop();
static void KeyboardForMainOnly();
static void FocusFreeInput();
static void LogInputState();
static void DrawViews(IDirect3DDevice9* d) {
    ++frames;
    if(frames==1) {
        IDirect3DSurface9* initial=nullptr;
        if(SUCCEEDED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&initial))) {
            D3DSURFACE_DESC ds;initial->GetDesc(&ds);Log("Initial backbuffer %u x %u",ds.Width,ds.Height);initial->Release();
        }
    }
    if(!modeRoot&&!testMode&&frames==1) FindGameMode();
    if(!testMode&&frames==1) {makeBorderless=true; KeyboardForMainOnly();}   // AutoJoinPartner removed: patching exe+0x8b9507 crashed at startup (ERR09)
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
        if(uiMinimized[i].load()||!EnsureChain(d,i,desc.Format)) continue;
        IDirect3DSurface9* dest=nullptr; if(FAILED(chains[i]->GetBackBuffer(0,D3DBACKBUFFER_TYPE_MONO,&dest))) continue;
        d->ColorFill(dest,nullptr,D3DCOLOR_XRGB(0,0,0));
        RECT src=split?regions[i^swap]:RECT{0,0,(LONG)desc.Width,(LONG)desc.Height};
        int sw=src.right-src.left,sh=src.bottom-src.top;
        if(src.left<0||src.top<0||src.right>(LONG)desc.Width||src.bottom>(LONG)desc.Height||sw<=0||sh<=0||abs(sw*9-sh*16)>16){dest->Release();continue;}
        int w=chainW[i],h=w*9/16;
        if(h>(int)chainH[i]) {h=chainH[i]; w=h*16/9;}
        int x=(chainW[i]-w)/2,y=(chainH[i]-h)/2;
        RECT dst={x,y,x+w,y+h};
        HRESULT hr=d->StretchRect(frameSource,&src,dest,&dst,D3DTEXF_LINEAR);
        if(SUCCEEDED(hr)&&composite) DrawSharedGui(d,dest,dst);
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
        if(SUCCEEDED(hr)) hr=chains[i]->Present(nullptr,nullptr,nullptr,nullptr,0);
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
static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* d,const RECT* a,const RECT* b,HWND h,const RGNDATA* r) {
    bool outer=presentDepth++==0;
    if(outer) DrawViews(d);
    HRESULT hr=realPresent(d,a,b,h,r);
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
static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* pp) {
    KeepWindowed(pp);
    if(!testMode&&SameAsNow(d,pp)) {ReleaseChains(); Log("Device reset skipped: same windowed mode %u x %u",pp->BackBufferWidth,pp->BackBufferHeight); return D3D_OK;}
    ReleaseChains(); Log("Device reset: %u x %u",pp->BackBufferWidth,pp->BackBufferHeight);
    return realReset(d,pp);
}
static HRESULT STDMETHODCALLTYPE ObjectPresent(IDirect3DDevice9* d,const RECT* a,const RECT* b,HWND h,const RGNDATA* r) {
    bool outer=presentDepth++==0;
    if(outer) DrawViews(d);
    HRESULT hr=objectPresent(d,a,b,h,r);
    --presentDepth;
    if(outer) AttachDevice(d);
    return hr;
}
static HRESULT STDMETHODCALLTYPE ObjectReset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* pp) {
    ReleaseChains();Log("Object device reset %u x %u",pp->BackBufferWidth,pp->BackBufferHeight); KeepWindowed(pp);
    if(!testMode&&SameAsNow(d,pp)) {Log("Device reset skipped: same windowed mode"); AttachDevice(d); return D3D_OK;}
    HRESULT hr=objectReset(d,pp); if(FAILED(hr)) Log("Device reset failed %08x",hr); AttachDevice(d);return hr;
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
static BYTE* gameBase;
static bool KeyboardOwnsCoop(){return deviceDisplay[DevKeyboard]!=0&&CoopOn();}
extern "C" void __cdecl ChooseIdleMode(BYTE* self) {
    const bool keyboard=KeyboardOwnsCoop();
    *(int*)(self+0x15c4bc)=keyboard?1:0; *(int*)(self+0x15c4c0)=keyboard?2:0;
}
extern "C" void ModeIdleStub();
// fxsave: the game keeps float values in xmm registers here; the C code must not change them.
asm(".globl _ModeIdleStub\n_ModeIdleStub:\n pushal\n mov %esp,%ebp\n sub $512,%esp\n and $-16,%esp\n fxsave (%esp)\n"
    " push 4(%ebp)\n call _ChooseIdleMode\n add $4,%esp\n fxrstor (%esp)\n mov %ebp,%esp\n popal\n pop %esi\n ret\n");
using BuildInputFn=int(__fastcall*)(void*,void*,BYTE*);
static BuildInputFn realBuildInput=nullptr;
static CRITICAL_SECTION buildLock;   // the mode is global: never let two players' builds overlap (re-entrant)
static bool ReadPtr(uintptr_t at,uintptr_t& v);
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
static int __fastcall HookBuildInput(void* self,void* edx,BYTE* player) {
    uintptr_t input=0;
    if(!player||!KeyboardOwnsCoop()||!ReadPtr((uintptr_t)gameBase+0x117d120,input)) return realBuildInput(self,edx,player);
    const bool keyboard=IsKeyboardPlayer(player);
    LogPlayer(player,player[0x7924],keyboard);
    int* mode=(int*)(input+0x15c4bc);
    EnterCriticalSection(&buildLock);
    const int saved=*mode; *mode=keyboard?1:0;
    const int r=realBuildInput(self,edx,player);
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
    return *(int*)(player+0x7920)==0&&IsKeyboardPlayer(player);
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
        mode=(int*)(input+0x15c4bc); saved=*mode; *mode=IsKeyboardPlayer(player)?1:0;
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
    if(SUCCEEDED(hr)&&dev==keyboardDevice&&size==256&&data) ((BYTE*)data)[0x3b]=0;   // DIK_F1
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
    static DWORD last=0; static unsigned moved=0,prev[10]={~0u};
    BYTE* base=(BYTE*)GetModuleHandleW(nullptr);
    uintptr_t mouse=0,input=0,game=0,pad=BioPad(); int dxy[2]={0,0};
    if(ReadPtr((uintptr_t)base+0x11de940,mouse)&&ReadGame(mouse+0x2c,dxy,8)&&(dxy[0]||dxy[1])) moved=1;
    const DWORD now=GetTickCount(); if(now-last<1000) return; last=now;
    unsigned v[10]={0}; v[5]=moved; moved=0; v[8]=(unsigned)RegionXInput(0); v[9]=(unsigned)RegionXInput(1);
    if(ReadPtr((uintptr_t)base+0x117d120,input)) {ReadGame(input+0x15c4bc,&v[0],4);ReadGame(input+0x15c4c0,&v[1],4);}
    if(pad) {ReadGame(pad+0xba0,&v[2],4);ReadGame(pad+0xba4,&v[3],4);}
    if(ReadPtr((uintptr_t)base+0x117ae00,game)) ReadGame(game+0x7a8,&v[4],4);
    v[6]=GetForegroundWindow()==gameWindow; v[7]=CoopOn();
    if(!memcmp(v,prev,sizeof(v))) return; memcpy(prev,v,sizeof(v));
    char line[240]; snprintf(line,sizeof(line),"Input: mode=%u/%u mainPad=%d subPad=%d mainChar=%u mouseMoved=%u focus=%u coop=%u region0XInput=%d region1XInput=%d swap=%u",
        v[0],v[1],(int)v[2],(int)v[3],v[4],v[5],v[6],v[7],(int)v[8],(int)v[9],(unsigned)swapPlayers.load());
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
// The game creates its device in exclusive fullscreen (its FullScreen=ON); switching to windowed later
// in Reset sometimes failed and ended in "ERR09: Unsupported function". So it starts windowed instead.
using CreateDeviceFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
static CreateDeviceFn realCreateDevice;
static HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* api,UINT a,D3DDEVTYPE t,HWND w,DWORD f,D3DPRESENT_PARAMETERS* pp,IDirect3DDevice9** d) {
    if(pp&&!testMode) KeepWindowed(pp);
    const HRESULT hr=realCreateDevice(api,a,t,w,f,pp,d);
    Log("Game device created: result %08x",hr);
    return hr;
}
static DWORD WINAPI Initialize(void*) {
    SetProcessDPIAware();
    GetModuleFileNameW(selfModule,logPath,MAX_PATH);
    wchar_t* tail=wcsrchr(logPath,L'.'); if(tail) wcscpy_s(tail,MAX_PATH-(tail-logPath),L".log");
    wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);
    testMode=wcsstr(exe,L"DualWindowsTest.exe")!=nullptr;
    Log("DualWindows prototype 0.6.0 initializing");
    WNDCLASSW wc={};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=selfModule;wc.lpszClassName=L"Rev2DualProbe";
    RegisterClassW(&wc);
    HWND probe=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPED,0,0,32,32,nullptr,nullptr,selfModule,nullptr);
    IDirect3D9* api=Direct3DCreate9(D3D_SDK_VERSION); IDirect3DDevice9* d=nullptr;
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
        if(m==MH_OK && MH_CreateHook(vt[17],HookPresent,(void**)&realPresent)==MH_OK &&
            MH_CreateHook(vt[16],HookReset,(void**)&realReset)==MH_OK && MH_EnableHook(MH_ALL_HOOKS)==MH_OK)
            Log("D3D9 Present/Reset hooks ready");
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
        if(!wcsstr(exe,L"DualWindowsTest.exe")) ShowInputSetup();
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
    }
    fakeRegionPlayer[0]=fakeRegionPlayer[1]=0;
    swapPlayers=swap; return ok;
}
extern "C" __declspec(dllexport) void DualWindowsShowSetup() {wcscpy_s(logPath,L"DualWindowsSetupTest.log");ShowInputSetup();}
extern "C" __declspec(dllexport) UINT DualWindowsStats() {return successes;}
extern "C" __declspec(dllexport) UINT DualWindowsFrames() {return frames;}
extern "C" __declspec(dllexport) UINT DualWindowsPixels() {return pixelChecks;}
extern "C" __declspec(dllexport) void DualWindowsTestMovie() {if(testMode)movieDrawn.store(true);}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {selfModule=h;DisableThreadLibraryCalls(h);}
    return TRUE;
}









