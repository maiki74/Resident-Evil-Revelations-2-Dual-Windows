#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cmath>
#include <vector>
struct Vertex {float x,y,z,rhw;DWORD color;};
// Second thread presenting at the same time, like the game's WM_PAINT Present on its main thread.
static IDirect3DDevice9* shared; static volatile LONG stopPainter=0, painterPresents=0;
static DWORD WINAPI Painter(void*) {while(!stopPainter){shared->Present(0,0,0,0);InterlockedIncrement(&painterPresents);Sleep(1);}return 0;}
static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l) {if(m==WM_CLOSE){PostQuitMessage(0);return 0;}return DefWindowProcW(h,m,w,l);}
int main() {
    SetProcessDPIAware();
    HMODULE mod=LoadLibraryW(L"ResidentEvilRevelations2.DualWindows.asi");
    if(!mod){puts("FAIL load");return 1;}
    auto init=(void(*)())GetProcAddress(mod,"InitializeASI");
    auto stats=(UINT(*)())GetProcAddress(mod,"DualWindowsStats");
    auto calls=(UINT(*)())GetProcAddress(mod,"DualWindowsFrames");
    auto pixels=(UINT(*)())GetProcAddress(mod,"DualWindowsPixels");
    auto markMovie=(void(*)())GetProcAddress(mod,"DualWindowsTestMovie");
    auto creates=(UINT(*)())GetProcAddress(mod,"DualWindowsChainCreates");
    auto inputTest=(BOOL(*)())GetProcAddress(mod,"DualWindowsInputSelfTest");
    auto showSetup=(void(*)())GetProcAddress(mod,"DualWindowsShowSetup");
    if(__argc>1&&showSetup){showSetup();return 0;}   // DualWindowsTest.exe setup: dashboard only
    const bool inputOK=inputTest&&inputTest();
    printf("Input routing self-test: %s\n",inputOK?"OK":"FAIL");
    if(!init||!stats||!markMovie)return 2;init();Sleep(1000);
    WNDCLASSW wc={};wc.hInstance=GetModuleHandle(nullptr);wc.lpfnWndProc=Proc;wc.lpszClassName=L"DualTestSource";
    RegisterClassW(&wc);HWND hwnd=CreateWindowW(wc.lpszClassName,L"DualWindows - teste de renderizacao",WS_OVERLAPPEDWINDOW,20,20,640,720,0,0,wc.hInstance,0);ShowWindow(hwnd,SW_SHOW);ShowWindow(hwnd,SW_SHOW);
    IDirect3D9* api=Direct3DCreate9(D3D_SDK_VERSION); IDirect3DDevice9* d=nullptr;
    D3DPRESENT_PARAMETERS pp={};pp.Windowed=1;pp.hDeviceWindow=hwnd;pp.BackBufferWidth=2560;pp.BackBufferHeight=1440;
    pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    if(FAILED(api->CreateDevice(0,D3DDEVTYPE_HAL,hwnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,&pp,&d)))return 3;
    UINT createsBefore=0,recreatedWhileResizing=0;
    shared=d; HANDLE painter=nullptr; bool concurrentOK=false; UINT beforeConcurrent=0;
    bool quit=false; bool resetOK=false; bool rejectedBadAspect=false;UINT beforeBad=0;
    HWND output=nullptr;
    bool presetOK=false,fillOK=false,restoreOK=false;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    for(int frame=0;frame<600&&!quit;++frame) {
        MSG msg;while(PeekMessage(&msg,0,0,0,PM_REMOVE)){if(msg.message==WM_QUIT)quit=true;TranslateMessage(&msg);DispatchMessage(&msg);}
        // Resizing, filling and restoring must never recreate a swap chain (only device resets may).
        if(frame==35||frame==185) createsBefore=creates();
        if(frame==175||frame==285) recreatedWhileResizing+=creates()-createsBefore;
        if(frame==40) {
            output=FindWindowW(L"Rev2DualWindowsV2",L"RE Revelations 2 \u2014 Display 1 (v0.6.3)");
            if(output) PostMessageW(output,WM_SYSCOMMAND,0x1120,0);
        }
        if(frame==60&&output) {
            RECT client{};
            GetClientRect(output,&client);
            presetOK=client.right==1280&&client.bottom==720;
            PostMessageW(output,WM_SYSCOMMAND,0x1100,0);
        }
        if(frame==80&&output) {
            MONITORINFO monitor={sizeof(monitor)};
            RECT bounds{};
            GetMonitorInfoW(MonitorFromWindow(output,MONITOR_DEFAULTTONEAREST),&monitor);
            GetWindowRect(output,&bounds);
            fillOK=EqualRect(&bounds,&monitor.rcMonitor)&&!(GetWindowLongPtrW(output,GWL_STYLE)&WS_CAPTION);
            PostMessageW(output,WM_SYSCOMMAND,0x1100,0);
        }
        if(frame==100&&output) {
            RECT client{};
            GetClientRect(output,&client);
            restoreOK=client.right==1280&&client.bottom==720&&(GetWindowLongPtrW(output,GWL_STYLE)&WS_CAPTION);
        }
        if(frame==110&&output) PostMessageW(output,WM_SYSCOMMAND,0x1120,0);
        if(frame==130&&output) {RECT r{};GetClientRect(output,&r);presetOK=presetOK&&r.right==1600&&r.bottom==900;}
        // Concurrent presents while both windows are resized, filled and restored (the freeze pattern).
        if(frame==190) {beforeConcurrent=stats(); painter=CreateThread(nullptr,0,Painter,nullptr,0,nullptr);}
        if(frame>190&&frame<280&&frame%6==0) for(HWND v=FindWindowW(L"Rev2DualWindowsV2",nullptr);v;v=FindWindowExW(nullptr,v,L"Rev2DualWindowsV2",nullptr))
            PostMessageW(v,WM_SYSCOMMAND,frame%12?0x1120:0x1100,0);
        if(frame==280) {stopPainter=1; concurrentOK=painter&&WaitForSingleObject(painter,3000)==WAIT_OBJECT_0&&painterPresents>50&&stats()>beforeConcurrent+100;
            for(HWND v=FindWindowW(L"Rev2DualWindowsV2",nullptr);v;v=FindWindowExW(nullptr,v,L"Rev2DualWindowsV2",nullptr)) {
                MONITORINFO m={sizeof(m)}; RECT b; GetMonitorInfoW(MonitorFromWindow(v,MONITOR_DEFAULTTONEAREST),&m); GetWindowRect(v,&b);
                if(EqualRect(&b,&m.rcMonitor)) PostMessageW(v,WM_SYSCOMMAND,0x1100,0);}}   // leave no window filled
        if(frame==180) {resetOK=SUCCEEDED(d->Reset(&pp));if(!resetOK){puts("FAIL reset");return 4;}}
        if(frame==300) {beforeBad=stats();pp.BackBufferWidth=1280;pp.BackBufferHeight=1440;if(FAILED(d->Reset(&pp)))return 6;}
        if(frame==360) {rejectedBadAspect=stats()==beforeBad;pp.BackBufferWidth=2560;pp.BackBufferHeight=1440;if(FAILED(d->Reset(&pp)))return 7;}
        d->Clear(0,0,D3DCLEAR_TARGET,D3DCOLOR_XRGB(0,0,0),1,0);
        D3DRECT top={0,0,1280,720},bottom={0,720,1280,1440};
        d->Clear(1,&top,D3DCLEAR_TARGET,D3DCOLOR_XRGB(15,90,145),1,0);
        d->Clear(1,&bottom,D3DCLEAR_TARGET,D3DCOLOR_XRGB(155,65,25),1,0);
        if(pp.BackBufferWidth==2560) {D3DRECT subtitle={1792,540,2048,612}; d->Clear(1,&subtitle,D3DCLEAR_TARGET,D3DCOLOR_XRGB(255,255,0),1,0);}
        d->BeginScene();d->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);d->SetRenderState(D3DRS_LIGHTING,FALSE);d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
        for(int i=0;i<2;++i) {
            std::vector<Vertex> circle;circle.push_back({640,360.0f+i*720,0,1,0xffffffff});
            for(int s=0;s<=96;++s){float a=s*6.2831853f/96;circle.push_back({640+180*cosf(a),360.0f+i*720+180*sinf(a),0,1,0xffffffff});}
            d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN,96,circle.data(),sizeof(Vertex));
        }
        d->EndScene();
        if(frame>=140&&frame<160) {
            d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff801080,1,0);
            D3DRECT corner={1280,720,2560,1440};
            d->Clear(1,&corner,D3DCLEAR_TARGET,0xff108010,1,0);
            markMovie();
        }
        HRESULT hr=d->Present(0,0,0,0);if(frame%120==0)printf("frame=%d present=%08x hooks=%u success=%u iconic=%d\n",frame,hr,calls(),stats(),IsIconic(hwnd));Sleep(16);
    }
    UINT result=stats();bool passed=inputOK&&result>360&&resetOK&&rejectedBadAspect&&pixels()==0x1f&&presetOK&&fillOK&&restoreOK&&concurrentOK&&creates&&recreatedWhileResizing==0;
    printf("Swap chains recreated while resizing: %u (expected 0)\n",recreatedWhileResizing);
    printf("Concurrent presents while resizing: %s (%ld from the second thread)\n",concurrentOK?"OK":"FAIL",painterPresents);
    printf("Resolution preset=%s, monitor fill=%s, restore=%s\n",presetOK?"OK":"FAIL",fillOK?"OK":"FAIL",restoreOK?"OK":"FAIL");
    printf("%s: %u player frames, Reset=%s, invalid-aspect rejection=%s, gameplay + movie pixel checks=%x (expected 1f)\n",passed?"PASS":"FAIL",result,resetOK?"OK":"FAIL",rejectedBadAspect?"OK":"FAIL",pixels());
    // All extra surfaces belong to this process and are released by process teardown.
    return passed?0:5;
}








