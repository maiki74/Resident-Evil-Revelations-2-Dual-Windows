// Startup controls dashboard + per-display controller routing.
// Game facts (decompiled rerev2.exe): sBioPad singleton at [0x15de5dc], 4 pad slots (XInput/DirectInput).
// Split-screen region v (0 = the character played solo, 1 = the one who joined, exe 0x428844) shows the
// character id [[0x157ae00]+0x8f8+v*4]; the player list [[0x1567eac]+0x20+id*4] gives its uPlayer
// (viewport setup exe 0x4ae570). Each uPlayer reads its own pad slot (+0x7924). sBioPad+0xba0 ("main pad")
// is NOT reliably the region-0 player's slot, so routing follows what each display shows instead.
#include <setupapi.h>
#include <hidsdi.h>
#include <cfgmgr32.h>

enum {DevKeyboard,DevPad1,DevPad2,DevCount};
// 0 = not used, 1 = display 1, 2 = display 2. Default = the game's own behaviour.
// The dashboard clears this every launch; the default only applies when it doesn't run (self-test).
static int deviceDisplay[DevCount]={1,1,2};

// Window index (0/1) that shows the main character. F1 still flips it, as before.
static int MainView() {return (deviceDisplay[DevKeyboard]==2?1:0)^(swapPlayers.load()?1:0);}
// A game pad slot is NOT the XInput number: each slot (sBioPad+0x3c+slot*0x2cc) takes the first free
// connected XInput number and keeps it at +0x10; +4 = in use, +7 = type (0 none, 2 DirectInput, else XInput).
static bool ReadGame(uintptr_t at,void* out,SIZE_T size){SIZE_T n=0;return ReadProcessMemory(GetCurrentProcess(),(void*)at,out,size,&n)&&n==size;}
static uintptr_t BioPad(){uintptr_t p=0;ReadGame((uintptr_t)GetModuleHandleW(nullptr)+0x11de5dc,&p,sizeof(p));return p;}
static int MainPadSlot() {
    if(testMode) return 0;
    int slot=0; const uintptr_t pad=BioPad();
    return pad&&ReadGame(pad+0xba0,&slot,sizeof(slot))&&slot>=0&&slot<4?slot:0;
}
static int SlotXInput(uintptr_t pad,int slot) {   // XInput number held by a game pad slot, or -1
    BYTE used=0,type=0; DWORD index=0; const uintptr_t at=pad+0x3c+slot*0x2cc;
    if(!ReadGame(at+4,&used,1)||!ReadGame(at+7,&type,1)||!ReadGame(at+0x10,&index,4)) return -1;
    return used&&type!=0&&type!=2&&index<4?(int)index:-1;
}
static int MainXInput() {   // XInput number the main character's slot reads (0 until the game has pads)
    if(testMode) return 0;
    const uintptr_t pad=BioPad(); const int x=pad?SlotXInput(pad,MainPadSlot()):-1;
    return x==1?1:0;
}
static int PartnerSlot() {   // game slot holding the partner's XInput number, or -1
    const uintptr_t pad=BioPad(); const int main=MainPadSlot(), other=MainXInput()==0?1:0;
    for(int s=0;pad&&s<4;++s) if(s!=main&&SlotXInput(pad,s)==other) return s;
    return -1;
}
// uPlayer shown in split region v, or 0 (none yet: menus, or region 1 in single player).
static uintptr_t fakeRegionPlayer[2]; static int fakeRegionX[2]={-2,-2};   // self-test only
static uintptr_t RegionPlayer(int v) {
    if(testMode) return fakeRegionPlayer[v];
    const uintptr_t base=(uintptr_t)GetModuleHandleW(nullptr); uintptr_t game=0,list=0,player=0; int id=-1;
    if(!ReadGame(base+0x117ae00,&game,sizeof(game))||!game||!ReadGame(game+0x8f8+v*4,&id,4)||id<0||id>=8) return 0;
    if(!ReadGame(base+0x1167eac,&list,sizeof(list))||!list||!ReadGame(list+0x20+id*4,&player,sizeof(player))) return 0;
    return player;
}
// uPlayer+0x7924 is a pad NUMBER, not a slot: the builder (0xa14bf3) gives it to the pad reader
// (0xb429b0 -> reader+0xc), which reads slot sBioPad+0xba0[number] (0xa967f0, 0xa96be9). The game fills
// +0xba0 with the slot that pressed Start on the title screen, so it is {1,0} when the controller did that.
static int PlayerXInput(int number,const int padNo[4],const int slotX[4]) {
    if(number<0||number>=4) return -1;
    const int slot=padNo[number];
    return slot>=0&&slot<4?slotX[slot]:-1;
}
// XInput number the region-v character reads: -2 nobody there, -1 its pad holds no XInput pad.
static int RegionXInput(int v) {
    if(testMode) return fakeRegionX[v];
    const uintptr_t p=RegionPlayer(v),pad=BioPad(); BYTE number=0xff; int padNo[4],slotX[4];
    if(!p) return -2;
    if(!pad||!ReadGame(p+0x7924,&number,1)||!ReadGame(pad+0xba0,padNo,sizeof(padNo))) return -1;
    for(int s=0;s<4;++s) slotX[s]=SlotXInput(pad,s);
    return PlayerXInput(number,padNo,slotX);
}
// Physical pads (0/1) that feed XInput number i. Window w shows region w^MainView(), so region v is on
// display (v^MainView())+1, and a display's controllers feed the XInput its character reads.
static int PadsFor(DWORD i,DWORD out[2]) {
    int view=-1; const int x0=RegionXInput(0);
    if(x0>=0) {
        int x1=RegionXInput(1); if(x1<0||x1==x0) x1=x0==0?1:0;   // solo: the other XInput is the partner's, so Start there joins co-op
        view=(int)i==x0?MainView():(int)i==x1?1-MainView():-1;
    } else {   // no character on screen yet (menus): the game's main pad slot
        const int main=MainXInput(),other=main==0?1:0;
        view=(int)i==main?MainView():(int)i==other?1-MainView():-1;
    }
    int n=0;
    if(view>=0) for(int p=0;p<2;++p) if(deviceDisplay[DevPad1+p]==view+1) out[n++]=p;
    return n;
}
struct PadState {DWORD packet;WORD buttons;BYTE lt,rt;SHORT lx,ly,rx,ry;};
static void Combine(PadState& a,const PadState& b) {
    a.packet+=b.packet; a.buttons|=b.buttons;
    a.lt=std::max(a.lt,b.lt); a.rt=std::max(a.rt,b.rt);
    auto len=[](long long x,long long y){return x*x+y*y;};
    if(len(b.lx,b.ly)>len(a.lx,a.ly)) {a.lx=b.lx;a.ly=b.ly;}
    if(len(b.rx,b.ry)>len(a.rx,a.ry)) {a.rx=b.rx;a.ry=b.ry;}
}
static bool CoopOn() {
    if(testMode) return false;
    uintptr_t game=0; int coop=0;
    return ReadGame((uintptr_t)GetModuleHandleW(nullptr)+0x117ae00,&game,sizeof(game))&&game&&
           ReadGame(game+0x8f0,&coop,sizeof(coop))&&coop==1;
}
static DWORD MergedState(PadFn real,DWORD i,void* s) {
    DWORD pads[2]; const int n=PadsFor(i,pads);
    bool any=false;
    for(int k=0;k<n;++k) {
        if(!any) {any=real(pads[k],s)==ERROR_SUCCESS;continue;}
        BYTE extra[64]={};   // GetStateEx may write past XINPUT_STATE
        if(real(pads[k],extra)==ERROR_SUCCESS) Combine(*(PadState*)s,*(PadState*)extra);
    }
    // XInput 0 and 1 always look connected (idle when their display has no controller): otherwise the
    // game fills its slots in a different order and the partner's slot ends up empty.
    if(!any&&i<2) {memset(s,0,sizeof(PadState)); any=true;}
    return any?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;
}
static DWORD WINAPI RouteGetState(DWORD i,void* s){return MergedState(realGetState,i,s);}
static DWORD WINAPI RouteGetStateEx(DWORD i,void* s){return MergedState(realGetStateEx,i,s);}
static DWORD WINAPI RouteSetState(DWORD i,void* v) {
    DWORD pads[2],result=ERROR_DEVICE_NOT_CONNECTED; const int n=PadsFor(i,pads);
    for(int k=0;k<n;++k) if(realSetState(pads[k],v)==ERROR_SUCCESS) result=ERROR_SUCCESS;
    return result;
}
static DWORD WINAPI RouteGetCaps(DWORD i,DWORD f,void* c) {
    DWORD pads[2]; const int n=PadsFor(i,pads);
    for(int k=0;k<n;++k) if(realGetCaps(pads[k],f,c)==ERROR_SUCCESS) return ERROR_SUCCESS;
    if(i<2) {memset(c,0,20); ((BYTE*)c)[0]=1; ((BYTE*)c)[1]=1; return ERROR_SUCCESS;}   // idle gamepad
    return ERROR_DEVICE_NOT_CONNECTED;
}

// ---------------- Dashboard (plain Win32 + GDI, shown before the game window exists) ----------------
namespace setup {
static float S=1; static int px(float v){return (int)(v*S+0.5f);}
static bool done=false, warn=false, connected[2]={true,true}, active[2]={false,false};
static wchar_t padName[2][64];
static int dragDevice=-1, hoverZone=-1; static POINT dragPoint, dragOffset;
static int lastLayout[DevCount]; static bool haveLast=false; static wchar_t lastText[160];
static HFONT fontTitle,fontText,fontSmall,fontBig,fontIcon,fontIconSmall,fontIconLabel;
static const COLORREF cBack=RGB(30,30,30),cText=RGB(236,236,236),cMuted=RGB(160,160,160),
    cLine=RGB(92,92,92),cChip=RGB(42,42,42),cAccent=RGB(84,145,235),cAccentBg=RGB(30,48,74),cDim=RGB(110,110,110);
// Zones: 0 = not used tray, 1 = display 1, 2 = display 2.
static RECT Zone(int z) {
    if(z==0) return {px(20),px(296),px(580),px(392)};
    const int x=z==1?20:310; return {px(x),px(56),px(x+270),px(256)};
}
static RECT StartButton(){return {px(450),px(408),px(580),px(444)};}
static RECT Chip(int dev) {
    const int zone=deviceDisplay[dev];
    auto width=[&](int d){return px(zone?(d==DevKeyboard?66:46):160);};
    const int h=px(zone?36:80),gap=px(zone?6:10);
    int before=0,total=-gap;
    for(int d=0;d<DevCount;++d) if(deviceDisplay[d]==zone) {if(d<dev) before+=width(d)+gap; total+=width(d)+gap;}
    RECT z=Zone(zone);
    const int w=width(dev), x=(z.left+z.right-total)/2+before;
    const int y=zone?z.bottom-px(10)-h:(z.top+z.bottom-h)/2;
    return {x,y,x+w,y+h};
}
static int ZoneAt(POINT p){for(int z=0;z<3;++z){RECT r=Zone(z);if(PtInRect(&r,p))return z;}return -1;}
static const wchar_t* Label(int dev){return dev==DevKeyboard?L"Mouse + keyboard":padName[dev-DevPad1];}
static void Round(HDC dc,RECT r,COLORREF fill,COLORREF line,int style=PS_SOLID,int width=1) {
    HPEN pen=CreatePen(style,width,line); HBRUSH brush=CreateSolidBrush(fill);
    auto op=SelectObject(dc,pen); auto ob=SelectObject(dc,brush);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,px(12),px(12));
    SelectObject(dc,op);SelectObject(dc,ob);DeleteObject(pen);DeleteObject(brush);
}
static void Text(HDC dc,const wchar_t* t,RECT r,HFONT f,COLORREF c,UINT flags=DT_CENTER|DT_VCENTER|DT_SINGLELINE) {
    SelectObject(dc,f);SetTextColor(dc,c);DrawTextW(dc,t,-1,&r,flags|DT_NOPREFIX);
}
static void DrawChip(HDC dc,int dev,RECT r,bool small) {
    const bool live=dev==DevKeyboard||connected[dev-DevPad1];
    const bool lit=dev!=DevKeyboard&&active[dev-DevPad1];   // this controller is being pressed right now
    Round(dc,r,lit?cAccentBg:cChip,lit?cAccent:live?RGB(110,110,110):cLine);
    const COLORREF ink=lit?cAccent:live?cText:cDim;
    RECT icon=r; if(!small) icon.bottom=r.top+px(38);
    if(dev==DevKeyboard) {
        // Drawn by hand: the font keyboard/mouse glyphs sit at different heights and look crooked side by side.
        const float u=small?0.8f:1.0f;
        const int kw=px(26*u),kh=px(16*u),mw=px(10*u),mh=px(16*u),gap=px(5*u);
        const int x=(icon.left+icon.right-(kw+gap+mw))/2, cy=(icon.top+icon.bottom)/2, top=cy-kh/2;
        HPEN pen=CreatePen(PS_SOLID,std::max(1,px(1.5f)),ink); HBRUSH fill=CreateSolidBrush(ink);
        auto op=SelectObject(dc,pen); auto ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
        RoundRect(dc,x,top,x+kw,top+kh,px(4*u),px(4*u));                       // keyboard body
        RoundRect(dc,x+kw+gap,cy-mh/2,x+kw+gap+mw,cy+mh/2,mw,mw);               // mouse body
        MoveToEx(dc,x+kw+gap+mw/2,cy-mh/2+px(2*u),nullptr); LineTo(dc,x+kw+gap+mw/2,cy-mh/2+px(6*u));
        for(int row=0;row<2;++row) for(int col=0;col<4;++col) {                 // keys
            RECT key={x+px((4+col*5.5f)*u),top+px((3.5f+row*4)*u),0,0};
            key.right=key.left+px(2.5f*u); key.bottom=key.top+px(2.5f*u); FillRect(dc,&key,fill);
        }
        RECT space={x+px(7*u),top+kh-px(4.5f*u),x+kw-px(7*u),top+kh-px(2.5f*u)}; FillRect(dc,&space,fill);
        SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(pen); DeleteObject(fill);
    } else Text(dc,L"",icon,small?fontIconSmall:fontIcon,ink);   // Segoe gamepad glyph
    if(!small) {
        RECT label=r; label.top=r.top+px(38); label.bottom=r.bottom-px(4);
        RECT inner=label; InflateRect(&inner,-px(6),0);
        Text(dc,live?Label(dev):L"Not connected",inner,fontSmall,lit?cAccent:live?cMuted:cDim,DT_CENTER|DT_WORDBREAK|DT_END_ELLIPSIS);   // long names wrap to 2 lines
    }
}
static void Paint(HWND h,HDC target) {
    RECT client; GetClientRect(h,&client);
    HDC dc=CreateCompatibleDC(target); HBITMAP bmp=CreateCompatibleBitmap(target,client.right,client.bottom);
    auto oldBmp=SelectObject(dc,bmp); SetBkMode(dc,TRANSPARENT);
    HBRUSH back=CreateSolidBrush(cBack); FillRect(dc,&client,back); DeleteObject(back);
    Text(dc,L"DualWindows: drag your controls onto a display",{px(20),px(16),px(580),px(44)},fontTitle,cText,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    for(int z=1;z<=2;++z) {
        RECT r=Zone(z); const bool over=dragDevice>=0&&hoverZone==z;
        Round(dc,r,over?cAccentBg:cBack,over?cAccent:cLine,PS_DASH);
        Text(dc,z==1?L"1":L"2",r,fontBig,cText);
        const int mid=(r.left+r.right)/2, top=r.top+px(10), bottom=r.top+px(36);
        Text(dc,L"",{mid-px(44),top,mid-px(24),bottom},fontIconLabel,cText);
        Text(dc,z==1?L"Display 1":L"Display 2",{mid-px(20),top,mid+px(60),bottom},fontText,cText,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }
    RECT trayLabel={px(20),px(270),px(300),px(292)};
    Text(dc,L"Not used",trayLabel,fontSmall,cMuted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const bool overTray=dragDevice>=0&&hoverZone==0;
    Round(dc,Zone(0),overTray?cAccentBg:cBack,overTray?cAccent:cLine);
    for(int d=0;d<DevCount;++d) if(d!=dragDevice) DrawChip(dc,d,Chip(d),deviceDisplay[d]!=0);
    bool placed=false; for(int d=0;d<DevCount;++d) placed|=deviceDisplay[d]!=0;
    Text(dc,warn?L"Drag at least one control onto a display first":haveLast&&!placed?lastText:L"Press a button on a controller to light up its icon",
         {px(20),px(408),px(440),px(444)},fontSmall,warn?RGB(240,180,80):cMuted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    RECT start=StartButton(); Round(dc,start,RGB(245,245,245),RGB(245,245,245));
    Text(dc,L"Start game",start,fontText,RGB(20,20,20));
    if(dragDevice>=0) {
        RECT r={dragPoint.x-dragOffset.x,dragPoint.y-dragOffset.y,0,0};
        r.right=r.left+px(160); r.bottom=r.top+px(80);
        DrawChip(dc,dragDevice,r,false);
    }
    BitBlt(target,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
    SelectObject(dc,oldBmp); DeleteObject(bmp); DeleteDC(dc);
}
// Real name of the pad in an XInput slot: its VID/PID (hidden XInputGetCapabilitiesEx, xinput1_4 #108)
// matched to the HID device Windows lists, whose product string is the controller's name.
static bool HidName(WORD vid,WORD pid,wchar_t* out,int count) {
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set=SetupDiGetClassDevsW(&hid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(set==INVALID_HANDLE_VALUE) return false;
    // A pad has several HID interfaces; its XInput one is often just "Controller (XBOX 360 For Windows)".
    bool found=false,specific=false; SP_DEVICE_INTERFACE_DATA iface={sizeof(iface)};
    for(DWORD i=0;!specific&&SetupDiEnumDeviceInterfaces(set,nullptr,&hid,i,&iface);++i) {
        BYTE buffer[1024]; auto detail=(SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buffer; detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,sizeof(buffer),nullptr,nullptr)) continue;
        HANDLE dev=CreateFileW(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(dev==INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a={sizeof(a)}; wchar_t name[64]={};
        if(HidD_GetAttributes(dev,&a)&&a.VendorID==vid&&a.ProductID==pid&&
           HidD_GetProductString(dev,name,sizeof(name))&&name[0]) {
            const bool generic=wcsncmp(name,L"Controller (",12)==0;
            if(!found||!generic) {wcsncpy_s(out,count,name,_TRUNCATE); found=true; specific=!generic;}
        }
        CloseHandle(dev);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
// Steam hides HID devices from the game, but not the USB device list: its "bus reported" name is the
// pad's real name (e.g. "8BitDo Ultimate 2C Wireless Controller"). Prefer names not starting "Controller (".
static bool UsbName(WORD vid,WORD pid,wchar_t* out,int count) {
    static const DEVPROPKEY busName={{0x540b947e,0x8b40,0x45bc,{0xa8,0xa2,0x6a,0x0b,0x89,0x4c,0xbd,0xa2}},4};
    wchar_t match[32]; swprintf_s(match,L"VID_%04X&PID_%04X",vid,pid);
    ULONG size=0; if(CM_Get_Device_ID_List_SizeW(&size,L"USB",CM_GETIDLIST_FILTER_ENUMERATOR|CM_GETIDLIST_FILTER_PRESENT)!=CR_SUCCESS||!size) return false;
    std::vector<wchar_t> ids(size);
    if(CM_Get_Device_ID_ListW(L"USB",ids.data(),size,CM_GETIDLIST_FILTER_ENUMERATOR|CM_GETIDLIST_FILTER_PRESENT)!=CR_SUCCESS) return false;
    bool found=false;
    for(wchar_t* id=ids.data();*id;id+=wcslen(id)+1) {
        if(!wcsstr(id,match)) continue;
        DEVINST node; if(CM_Locate_DevNodeW(&node,id,CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS) continue;
        DEVPROPTYPE type; wchar_t name[64]={}; ULONG bytes=sizeof(name);
        if(CM_Get_DevNode_PropertyW(node,&busName,&type,(BYTE*)name,&bytes,0)!=CR_SUCCESS||type!=DEVPROP_TYPE_STRING||!name[0]) continue;
        const bool generic=wcsncmp(name,L"Controller (",12)==0;
        if(!found||!generic) {wcsncpy_s(out,count,name,_TRUNCATE); found=true; if(!generic) return true;}
    }
    return found;
}
static bool namesKnown=false, steamSlot[2]={false,false};
// Steam Input hooks XInput inside the game, so every slot looks like a Steam virtual pad (VID 28DE).
// A private copy of xinput1_4.dll is not hooked and still sees the real pads; a Steam slot is matched
// to the real pad that is pressed at the same moment (or directly, when only one of each is left).
struct CapsEx {BYTE caps[20];WORD vid,pid,version,unknown1;DWORD unknown2;};
using CapsExFn=DWORD (WINAPI*)(DWORD,DWORD,DWORD,CapsEx*);
static CapsExFn realCapsEx; static PadFn realPadState;
static bool realOn[4],realPressed[4]; static DWORD realPressedAt[4]; static wchar_t realName[4][64];
static int slotReal[2]={-1,-1};
static void LoadPrivateXInput() {
    wchar_t system[MAX_PATH],copy[MAX_PATH];
    GetSystemDirectoryW(system,MAX_PATH); wcscat_s(system,L"\\xinput1_4.dll");
    GetTempPathW(MAX_PATH,copy); wcscat_s(copy,L"DualWindows_xinput1_4.dll");
    CopyFileW(system,copy,FALSE);   // may fail if another copy is loaded; the existing file is the same
    HMODULE m=LoadLibraryW(copy);
    Log(m?"Private XInput copy loaded":"Private XInput copy failed: error %u",GetLastError());
    if(!m) return;
    realCapsEx=(CapsExFn)GetProcAddress(m,MAKEINTRESOURCEA(108));
    realPadState=(PadFn)GetProcAddress(m,"XInputGetState");
}
static bool Pressed(const PadState& s){return s.buttons||s.lt>60||s.rt>60||abs(s.lx)>12000||abs(s.ly)>12000||abs(s.rx)>12000||abs(s.ry)>12000;}
static bool Bound(int r){return slotReal[0]==r||slotReal[1]==r;}
static void Bind(int slot,int r) {
    slotReal[slot]=r; wcscpy_s(padName[slot],realName[r]);
    Log("Steam slot %u is real controller %u",slot,r);
}
// One Steam slot and one real pad left over: they must be each other.
static void BindLeftover() {
    int slot=-1,pad=-1,slots=0,pads=0;
    for(int p=0;p<2;++p) if(steamSlot[p]&&connected[p]&&slotReal[p]<0) {slot=p;++slots;}
    for(int r=0;r<4;++r) if(realOn[r]&&!Bound(r)) {pad=r;++pads;}
    if(slots==1&&pads==1) Bind(slot,pad);
}
static const wchar_t* NameFor(bool known,const CapsEx& c,wchar_t* out) {
    if(known&&(UsbName(c.vid,c.pid,out,64)||HidName(c.vid,c.pid,out,64))) return out;
    return !known?L"Controller":c.vid==0x045e?(c.pid==0x028e?L"Xbox 360 Controller":L"Xbox Controller"):
        c.vid==0x054c?L"PlayStation Controller":c.vid==0x057e?L"Nintendo Controller":L"Controller";
}
// Right after the game starts, XInput may not have listed the pads yet: the timer retries until it has.
static void ReadPadNames() {
    static auto capsEx=(CapsExFn)GetProcAddress(LoadLibraryW(L"xinput1_4.dll"),MAKEINTRESOURCEA(108));
    static const bool fakeSteam=GetEnvironmentVariableW(L"DUALWINDOWS_FAKE_STEAM",nullptr,0)>0;   // test the Steam path off Steam
    static bool logged=false;
    namesKnown=true;
    for(int r=0;r<4;++r) {
        CapsEx c={}; const bool known=realCapsEx&&realCapsEx(1,r,0,&c)==ERROR_SUCCESS;
        if(known) {wchar_t name[64]; wcscpy_s(realName[r],NameFor(true,c,name));}
        if(known&&!logged) Log("Real controller %u: vendor/product %08x",r,(unsigned)c.vid<<16|c.pid);
    }
    for(int p=0;p<2;++p) {
        CapsEx c={};
        const DWORD result=capsEx?capsEx(1,p,0,&c):ERROR_PROC_NOT_FOUND;
        const bool known=result==ERROR_SUCCESS;
        if(!known&&connected[p]) {namesKnown=false; if(!logged) Log("Controller name lookup failed for slot %u: error %u",p,result);}
        if(known&&!logged) Log("Game controller slot %u: vendor/product %08x",p,(unsigned)c.vid<<16|c.pid);
        steamSlot[p]=steamSlot[p]||(known&&(c.vid==0x28de||fakeSteam));   // sticky: Steam may answer differently later
        if(steamSlot[p]) wcscpy_s(padName[p],slotReal[p]>=0?realName[slotReal[p]]:L"Press a button to show its name");
        else {wchar_t name[64]; wcscpy_s(padName[p],NameFor(known,c,name));}
    }
    logged=true;
    BindLeftover();
}
static bool PollPads() {   // returns true when something visible changed
    static auto get=(PadFn)GetProcAddress(LoadLibraryW(L"xinput1_3.dll"),"XInputGetState");
    bool changed=false,plugged=false;
    const DWORD now=GetTickCount();
    static int offTicks[4];
    for(int r=0;r<4;++r) {
        PadState s={}; const bool seen=realPadState&&realPadState(r,&s)==ERROR_SUCCESS, pressed=seen&&Pressed(s);
        offTicks[r]=seen?0:offTicks[r]+1;
        const bool on=seen||(realOn[r]&&offTicks[r]<10);   // ignore drop-outs shorter than 1 s
        if(on!=realOn[r]) {plugged=true; Log("Real controller %u on=%u",r,on);}
        if(pressed&&!realPressed[r]) realPressedAt[r]=now;
        realOn[r]=on; realPressed[r]=pressed;
    }
    for(int p=0;p<2;++p) {
        PadState s={}; const bool on=get&&get(p,&s)==ERROR_SUCCESS, pressed=on&&Pressed(s);
        if(on!=connected[p]) {plugged=true; Log("Game slot %u on=%u",p,on);}
        if(pressed&&!active[p]&&steamSlot[p]&&slotReal[p]<0) {
            int best=-1;   // the real pad that started being pressed most recently (within 0.5 s)
            for(int r=0;r<4;++r)
                if(realPressed[r]&&!Bound(r)&&now-realPressedAt[r]<500&&(best<0||realPressedAt[r]>realPressedAt[best])) best=r;
            if(best>=0) {Bind(p,best); BindLeftover(); changed=true;}
        }
        changed|=on!=connected[p]||pressed!=active[p];
        connected[p]=on; active[p]=pressed;
    }
    if(plugged) {
        for(int p=0;p<2;++p)   // keep a match while both its slot and its real pad are still there
            if(slotReal[p]>=0&&(!connected[p]||!realOn[slotReal[p]])) slotReal[p]=-1;
        ReadPadNames();
    }
    return changed;
}
// Last launch's layout (scripts\ResidentEvilRevelations2.DualWindows.ini). Not pre-placed on the
// dashboard (user asked for an empty one); Start with nothing placed reuses it.
static const wchar_t* const iniKeys[DevCount]={L"MouseKeyboard",L"Controller1",L"Controller2"};
static void IniPath(wchar_t* out){wcscpy_s(out,MAX_PATH,logPath); if(wchar_t* t=wcsrchr(out,L'.')) wcscpy_s(t,MAX_PATH-(t-out),L".ini");}
static void LoadLast() {
    wchar_t ini[MAX_PATH]; IniPath(ini); haveLast=false;
    for(int d=0;d<DevCount;++d) {lastLayout[d]=GetPrivateProfileIntW(L"Controls",iniKeys[d],0,ini); if(lastLayout[d]<0||lastLayout[d]>2) lastLayout[d]=0; haveLast|=lastLayout[d]!=0;}
    wcscpy_s(lastText,L"Start now = last setup:");
    for(int z=1;z<=2;++z) for(int d=0;d<DevCount;++d) if(lastLayout[d]==z) {
        wchar_t part[48]; swprintf_s(part,L" D%d %s",z,d==DevKeyboard?L"mouse+keyboard":d==DevPad1?L"controller 1":L"controller 2"); wcscat_s(lastText,part);}
}
static void SaveLayout() {
    wchar_t ini[MAX_PATH],v[4]; IniPath(ini);
    for(int d=0;d<DevCount;++d) {swprintf_s(v,L"%d",deviceDisplay[d]); WritePrivateProfileStringW(L"Controls",iniKeys[d],v,ini);}
}
static void TryStart(HWND h) {
    bool any=false; for(int d=0;d<DevCount;++d) any|=deviceDisplay[d]!=0;
    if(!any&&haveLast) {for(int d=0;d<DevCount;++d) deviceDisplay[d]=lastLayout[d]; Log("Input setup: reused last saved layout"); any=true;}
    if(any) {SaveLayout(); done=true; return;}
    warn=true; InvalidateRect(h,nullptr,FALSE);
}
static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); Paint(h,dc); EndPaint(h,&ps); return 0;}
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        static int tick=0;
        bool redraw=PollPads();
        if(!namesKnown&&++tick%5==0) {ReadPadNames(); redraw=true;}
        if(redraw) InvalidateRect(h,nullptr,FALSE);
        return 0;}
    case WM_LBUTTONDOWN: {
        POINT p={(short)LOWORD(l),(short)HIWORD(l)};
        RECT start=StartButton(); if(PtInRect(&start,p)) {TryStart(h); return 0;}
        for(int d=0;d<DevCount;++d) {RECT r=Chip(d); if(PtInRect(&r,p)) {
            dragDevice=d; dragPoint=p; dragOffset={px(80),px(40)}; hoverZone=ZoneAt(p);
            SetCapture(h); InvalidateRect(h,nullptr,FALSE); break;}}
        return 0;}
    case WM_MOUSEMOVE:
        if(dragDevice>=0) {dragPoint={(short)LOWORD(l),(short)HIWORD(l)}; hoverZone=ZoneAt(dragPoint); InvalidateRect(h,nullptr,FALSE);}
        return 0;
    case WM_LBUTTONUP:
        if(dragDevice>=0) {
            const int z=ZoneAt({(short)LOWORD(l),(short)HIWORD(l)});
            if(z>=0) deviceDisplay[dragDevice]=z;
            if(z>0) warn=false;
            dragDevice=-1; ReleaseCapture(); InvalidateRect(h,nullptr,FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if(w==VK_RETURN||w==VK_SPACE) TryStart(h);
        else if(w==VK_ESCAPE) PostMessageW(h,WM_CLOSE,0,0);
        return 0;
    case WM_CLOSE:   // closing the window means "don't play": quit before the game opens
        Log("Controls window closed: game not started");
        TerminateProcess(GetCurrentProcess(),0);
        return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
static HFONT Font(float size,int weight,const wchar_t* face) {
    return CreateFontW(-px(size),0,0,0,weight,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,face);
}
}

// Blocks until the player presses Start game. Runs before the game creates its window.
static void ShowInputSetup() {
    using namespace setup;
    for(int& d:deviceDisplay) d=0;   // every launch starts with all controls in "Not used"
    LoadLast();
    auto oldContext=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    S=GetDpiForSystem()/96.0f;
    // Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on Windows 10: same code points.
    const wchar_t* iconFace=L"Segoe Fluent Icons";
    {HDC screen=GetDC(nullptr); HFONT probe=Font(20,400,iconFace); auto old=SelectObject(screen,probe);
     wchar_t face[64]={}; GetTextFaceW(screen,64,face); if(_wcsicmp(face,iconFace)) iconFace=L"Segoe MDL2 Assets";
     SelectObject(screen,old); DeleteObject(probe); ReleaseDC(nullptr,screen);}
    fontTitle=Font(16,600,L"Segoe UI"); fontText=Font(14,400,L"Segoe UI"); fontSmall=Font(12,400,L"Segoe UI");
    fontBig=Font(100,600,L"Segoe UI"); fontIcon=Font(28,400,iconFace); fontIconSmall=Font(20,400,iconFace);
    fontIconLabel=Font(15,400,iconFace);
    LoadPrivateXInput(); PollPads(); ReadPadNames();
    WNDCLASSW wc={}; wc.lpfnWndProc=Proc; wc.hInstance=selfModule; wc.lpszClassName=L"Rev2DualWindowsSetup";
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW); RegisterClassW(&wc);
    const DWORD style=WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    RECT r={0,0,px(600),px(464)}; AdjustWindowRect(&r,style,FALSE);
    MONITORINFO mi={sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&mi);
    const int w=r.right-r.left,hgt=r.bottom-r.top;
    HWND h=CreateWindowExW(0,wc.lpszClassName,L"RE Revelations 2 — DualWindows controls",style,
        (mi.rcWork.left+mi.rcWork.right-w)/2,(mi.rcWork.top+mi.rcWork.bottom-hgt)/2,w,hgt,nullptr,nullptr,selfModule,nullptr);
    if(h) {
        BOOL dark=TRUE;   // dark title bar (DWMWA_USE_IMMERSIVE_DARK_MODE); harmless if unsupported
        using DwmSet=HRESULT (WINAPI*)(HWND,DWORD,LPCVOID,DWORD);
        if(auto set=(DwmSet)GetProcAddress(LoadLibraryW(L"dwmapi.dll"),"DwmSetWindowAttribute")) set(h,20,&dark,sizeof(dark));
        ShowWindow(h,SW_SHOW); SetForegroundWindow(h); SetTimer(h,1,100,nullptr);
        MSG msg; done=false;
        while(!done&&GetMessageW(&msg,nullptr,0,0)>0) {TranslateMessage(&msg);DispatchMessageW(&msg);}
        DestroyWindow(h);
    }
    for(HFONT f:{fontTitle,fontText,fontSmall,fontBig,fontIcon,fontIconSmall,fontIconLabel}) DeleteObject(f);
    SetThreadDpiAwarenessContext(oldContext);
    Log("Input setup: mouse+keyboard=%u, controller 1=%u",deviceDisplay[DevKeyboard],deviceDisplay[DevPad1]);
    Log("Input setup: controller 2=%u",deviceDisplay[DevPad2]);
    for(int z=1;z<=2;++z) for(int d=0;d<DevCount;++d) if(deviceDisplay[d]==z)
        Log(d==DevKeyboard?"Window Display %u: mouse + keyboard":"Window Display %u: controller %u",z,d);
}

// Self-check of the routing rules (DualWindowsTest.exe). testMode => main pad slot is 0.
static bool InputRoutingSelfTest() {
    const int saved[DevCount]={deviceDisplay[0],deviceDisplay[1],deviceDisplay[2]}; const bool swap=swapPlayers.load();
    auto pads=[](DWORD i){DWORD o[2]={9,9};int n=PadsFor(i,o);return n*100+(n>0?o[0]*10:0)+(n>1?o[1]:0);};
    bool ok=true;
    deviceDisplay[0]=1;deviceDisplay[1]=1;deviceDisplay[2]=2;               // default: same as the game
    ok&=pads(0)==100&&pads(1)==110&&pads(2)==0&&MainView()==0;
    swapPlayers=true; ok&=pads(0)==110&&pads(1)==100&&MainView()==1;        // F1 still swaps
    swapPlayers=false;
    deviceDisplay[0]=2;deviceDisplay[1]=1;deviceDisplay[2]=0;               // mouse+keyboard on display 2
    ok&=MainView()==1&&pads(0)==0&&pads(1)==100;                          // controller 1 plays the partner
    swapPlayers=true; ok&=MainView()==0&&pads(0)==100&&pads(1)==0;        // after F1: controller 1 plays the main character
    swapPlayers=false; ok&=MainView()==1&&pads(0)==0&&pads(1)==100;       // F1 again: back
    deviceDisplay[0]=1;deviceDisplay[1]=1;deviceDisplay[2]=1;               // both pads on display 1
    ok&=pads(0)==201&&pads(1)==0;
    // Your setup: Display 1 = controller 1, Display 2 = mouse + keyboard. Routing follows what each display
    // shows (region XInput), not sBioPad's main pad. Both orders of the game's XInput numbers are checked.
    deviceDisplay[0]=2;deviceDisplay[1]=1;deviceDisplay[2]=0;
    for(int x0=0;x0<2;++x0) {
        const DWORD solo=x0, join=1-x0;                                     // region 0 = solo character
        fakeRegionX[0]=x0; fakeRegionX[1]=-2;                               // single player: no region 1
        ok&=pads(solo)==0&&pads(join)==100;                                 // controller feeds the join XInput: Start joins
        fakeRegionX[1]=x0; ok&=pads(solo)==0&&pads(join)==100;              // same, if region 1 still shows the solo character
        fakeRegionX[1]=1-x0;                                                // co-op: joiner on region 1
        for(int round=0;round<4;++round) {                                 // F1 back and forth
            swapPlayers=round&1;
            const DWORD onDisplay1=swapPlayers.load()?solo:join;           // region (1^swap) is on display 1
            ok&=pads(onDisplay1)==100&&pads(1-onDisplay1)==0;
        }
        swapPlayers=false;
    }
    // Real runs from the game log. Slots take XInput 0 and 1 in order (both always look connected).
    // Each uPlayer+0x7924 is a pad number; the slot is sBioPad+0xba0[number] (0xa967f0).
    {
        const int slotX[4]={0,1,-1,-1};
        const int working[4]={0,1,-1,-1};      // run 42094031: mainPad=0 subPad=1 (keyboard pressed Start on the title)
        const int title[4]={1,-1,-1,-1};        // run 43748390: mainPad=1 subPad=-1 (controller pressed Start on the title)
        const int joined[4]={1,0,-1,-1};        // run 42620281: mainPad=1 subPad=0 after the join
        ok&=PlayerXInput(0,working,slotX)==0&&PlayerXInput(1,working,slotX)==1;
        ok&=PlayerXInput(0,title,slotX)==1&&PlayerXInput(1,title,slotX)==-1;   // solo character reads XInput 1
        ok&=PlayerXInput(0,joined,slotX)==1&&PlayerXInput(1,joined,slotX)==0;
        ok&=PlayerXInput(9,joined,slotX)==-1;
        // Your dashboard, last run's state: the solo character (pad number 0) reads XInput 1.
        deviceDisplay[0]=2;deviceDisplay[1]=1;deviceDisplay[2]=0; swapPlayers=false;
        fakeRegionX[0]=PlayerXInput(0,title,slotX); fakeRegionX[1]=-2;
        ok&=pads(1)==0&&pads(0)==100;           // controller feeds the free XInput 0, not the solo character: Start joins
        // After the join the joiner (pad number 1) is on region 1 and reads XInput 0.
        fakeRegionX[0]=PlayerXInput(0,joined,slotX); fakeRegionX[1]=PlayerXInput(1,joined,slotX);
        for(int round=0;round<6;++round) {      // before F1, after F1, and repeated swaps
            swapPlayers=round&1;
            const int onDisplay1=swapPlayers.load()?fakeRegionX[0]:fakeRegionX[1];   // window 0 shows region 0^MainView()
            ok&=pads(onDisplay1)==100&&pads(1-onDisplay1)==0;                       // controller on exactly that character
        }
        swapPlayers=false;
    }
    // Both dashboard layouts, co-op, through F1 rounds: window w shows region w^MainView(); the keyboard
    // plays region (swap?1:0) (IsKeyboardPlayer). Each device must stay on the window it was dropped on.
    for(int layout=0;layout<2;++layout) {
        const int kb=layout?1:2, pad=layout?2:1;                              // A: D1 pad, D2 keyboard. B: the reverse.
        deviceDisplay[0]=kb;deviceDisplay[1]=pad;deviceDisplay[2]=0;
        for(int x0=0;x0<2;++x0) {
            fakeRegionX[0]=x0; fakeRegionX[1]=1-x0;
            for(int round=0;round<4;++round) {
                swapPlayers=round&1;
                const int kbWindow=(swapPlayers.load()?1:0)^MainView();
                const int padRegion=(pad-1)^MainView();                       // region shown on the pad's window
                ok&=kbWindow==kb-1&&pads(fakeRegionX[padRegion])==100&&pads(fakeRegionX[1-padRegion])==0;
            }
        }
        swapPlayers=false;
    }
    fakeRegionX[0]=fakeRegionX[1]=-2;
    PadState a={1,0x1000,10,0,100,0,0,0},b={2,0x0020,50,7,-3000,4000,5,5};
    Combine(a,b); ok&=a.packet==3&&a.buttons==0x1020&&a.lt==50&&a.rt==7&&a.lx==-3000&&a.ly==4000&&a.rx==5;
    for(int d=0;d<DevCount;++d) deviceDisplay[d]=saved[d]; swapPlayers=swap;
    return ok;
}
