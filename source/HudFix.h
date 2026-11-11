// Applies after the verified FusionFix GUI command builder emits its constants.
// The original HUD is laid out for 800x360; our player viewport is 16:9.
// Fit uniformly to width, and center vertically instead of shifting it off-screen.
#include <unordered_set>
#include <cmath>
static std::atomic<bool> hudFit{true};
static std::atomic<bool> sharedGuiDrawn{false};
static int GameMode();
using GuiFn=void (__fastcall*)(uintptr_t,int,uintptr_t);
static GuiFn realGui=nullptr;
static const unsigned HUD_CENTER=0xaaaaeeee,HUD_OFFSET=0xccccdddd,HUD_SUBTITLES=0xdddddddd;

static void __fastcall FitHud(uintptr_t object,int mode,uintptr_t context) {
    auto viewport=*(BYTE**)(context+4);
    int width=*(int*)(viewport+196)-*(int*)(viewport+188);
    int height=*(int*)(viewport+200)-*(int*)(viewport+192);
    int split=lastFrameWasMovie.load()?0:GameMode();
    bool player=false;
    RECT regions[2];bool ready[2];
    AcquireSRWLockShared(&layoutLock);
    for(int i=0;i<2;++i){regions[i]=playerRects[i];ready[i]=rectReady[i];}
    ReleaseSRWLockShared(&layoutLock);
    for(int i=0;i<2;++i)if(ready[i]&&abs(width-(regions[i].right-regions[i].left))<=1&&abs(height-(regions[i].bottom-regions[i].top))<=1)player=true;
    const bool layoutMode=(unsigned)mode==HUD_CENTER||(unsigned)mode==HUD_OFFSET||(unsigned)mode==HUD_SUBTITLES;
    const bool shared=split==1&&enabled.load()&&layoutMode&&
        width==static_cast<int>(sourceWidth)&&height==static_cast<int>(sourceHeight)&&
        (viewport[0x158]==8||static_cast<unsigned>(mode)==HUD_SUBTITLES);
    // uGUIPuzzle uses its own 1280x720 asset. Its base, finger and
    // target nodes must retain the same native coordinate system.
    const uintptr_t table=*(uintptr_t*)object;
    static const uintptr_t executable=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const bool chestPuzzle=split==1&&enabled.load()&&table==executable+0xfa0240;
    const bool nativePuzzle=chestPuzzle&&player&&layoutMode;
    // Both healing gauge classes (0139b998, 0139baf0) use a centered 1280x720 asset.
    const bool nativeHeal=split==1&&enabled.load()&&player&&layoutMode&&
        (table==executable+0xf9b998||table==executable+0xf9baf0);
    const bool adjust=split==1&&player&&layoutMode&&hudFit.load()&&!nativePuzzle&&!nativeHeal;
    // Mode 0 emits the native transform without FusionFix's old split offsets.
    const bool movieGui=lastFrameWasMovie.load()&&layoutMode;
    realGui(object,(adjust||shared||movieGui||nativePuzzle||nativeHeal)?0:mode,context);
    float* constants=*(float**)(viewport+0x1818);
    if(shared&&constants) {
        // Move the common 16:9 UI into the unused upper-right quarter.
        constants[0]*=0.5f;
        constants[1]*=0.5f;
        constants[2]=constants[2]*0.5f+0.5f;
        constants[3]=constants[3]*0.5f+0.5f;
        sharedGuiDrawn.store(true);
    }
    if(adjust&&constants&&width>0&&height>0) {
        const float fit=std::min(1.0f,(float(width)/height)/(800.0f/360.0f));
        if(std::isfinite(fit)&&fit>0) {
            constants[0]*=fit;
            constants[1]*=fit;
            constants[2]=(constants[2]+1.0f)*fit-1.0f;
            constants[3]=(constants[3]-1.0f)*fit+fit;
        }
    }
    if(split==1&&(table==executable+0xf915b0||table==executable+0xf91970)) {   // action icons: which screen draws which widget
        static thread_local std::unordered_set<uint64_t> drawn;
        const unsigned camera=*(BYTE*)(viewport+0x158);
        if(drawn.size()<64&&drawn.insert(((uint64_t)object<<8)|camera).second) {
            FILE* f=nullptr;_wfopen_s(&f,logPath,L"a");
            if(f){fprintf(f,"Action icon drawn: widget %08x, class %08x, camera %u, character %d\n",(unsigned)object,(unsigned)table,camera,*(int*)(object+0x2ac));fclose(f);}
        }
    }
    if(split==1) {
        static thread_local std::unordered_set<uint64_t> seen;
        if(nativePuzzle) {
            static std::atomic<bool> reported{false};
            if(!reported.exchange(true)) Log("Chest puzzle using native 16:9 dial transform");
        }
        const uint64_t key=((uint64_t)table<<32)^((uint64_t)(unsigned)width<<16)^(unsigned)height^((uint64_t)adjust<<63);
        if(seen.size()<256&&seen.insert(key).second) {
            FILE* f=nullptr;_wfopen_s(&f,logPath,L"a");
            if(f){fprintf(f,"HUD class=%08x viewport=%dx%d origin=%d,%d camera=%u mode=%08x fitted=%d constants=%g,%g,%g,%g\n",
                (unsigned)table,width,height,*(int*)(viewport+188),*(int*)(viewport+192),*(BYTE*)(viewport+0x158),(unsigned)mode,adjust,
                constants?constants[0]:0,constants?constants[1]:0,constants?constants[2]:0,constants?constants[3]:0);fclose(f);}
        }
    }
}
static void InstallHudFix() {
    auto target=fusionModule+0x35570;
    const BYTE expected[]={0x55,0x8b,0xec,0x83,0xec,0x30};
    if(memcmp(target,expected,sizeof(expected))!=0){Log("HUD builder signature mismatch; HUD patch skipped");return;}
    MH_STATUS result=MH_CreateHook(target,FitHud,(void**)&realGui);
    if(result==MH_OK)result=MH_EnableHook(target);
    Log("HUD fit hook status: %u",result);
}
