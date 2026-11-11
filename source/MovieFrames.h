static std::atomic<bool> movieDrawn{false};
static std::atomic<bool> lastFrameWasMovie{false};
static bool forceSharedFrame=false;
using DrawFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT);
using IndexedDrawFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
static DrawFn realDraw=nullptr;
static IndexedDrawFn realIndexedDraw=nullptr;

static bool IsMovieShader(IDirect3DDevice9* device) {
    IDirect3DPixelShader9* shader=nullptr;
    if(FAILED(device->GetPixelShader(&shader))||!shader) return false;
    // Hold the last shader so a recycled COM address cannot reuse its classification.
    static IDirect3DPixelShader9* cached=nullptr;
    static bool isMovie=false;
    if(shader==cached) {
        shader->Release();
        return isMovie;
    }
    if(cached) cached->Release();
    cached=shader;
    isMovie=false;
    UINT length=0;
    if(FAILED(shader->GetFunction(nullptr,&length))||!length) return false;
    std::vector<BYTE> bytes(length);
    if(FAILED(shader->GetFunction(bytes.data(),&length))) return false;
    uint32_t crc=0xffffffff;
    for(UINT i=0;i<length;++i) {
        crc^=bytes[i];
        for(int bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1)?0xedb88320:0);
    }
    // WMV YUV decoder fingerprint documented by FusionFix's Shaders module.
    isMovie=(~crc)==0x9d190fc7;
    if(isMovie) {
        static bool reported=false;
        if(!reported) {reported=true;Log("WMV decoder shader matched");}
    }
    return isMovie;
}

static HRESULT STDMETHODCALLTYPE DrawScreenQuad(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,
                                                  UINT start,UINT count) {
    const bool movie=IsMovieShader(device);
    HRESULT hr=device->DrawPrimitive(type,start,count);
    if(movie&&SUCCEEDED(hr)) movieDrawn.store(true);
    return hr;
}

static HRESULT STDMETHODCALLTYPE ObserveDraw(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,
                                                UINT start,UINT count) {
    const bool movie=count<=4&&!movieDrawn.load()&&IsMovieShader(device);
    HRESULT hr=realDraw(device,type,start,count);
    if(movie&&SUCCEEDED(hr)) movieDrawn.store(true);
    return hr;
}

static HRESULT STDMETHODCALLTYPE ObserveIndexedDraw(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,
                                                       INT base,UINT min,UINT vertices,UINT start,UINT count) {
    const bool movie=count<=4&&!movieDrawn.load()&&IsMovieShader(device);
    HRESULT hr=realIndexedDraw(device,type,base,min,vertices,start,count);
    if(movie&&SUCCEEDED(hr)) movieDrawn.store(true);
    return hr;
}

static void CreateMovieDrawHooks(void** table) {
    const MH_STATUS draw=MH_CreateHook(table[81],ObserveDraw,(void**)&realDraw);
    const MH_STATUS indexed=MH_CreateHook(table[82],ObserveIndexedDraw,(void**)&realIndexedDraw);
    Log("Movie draw hook status (0 = OK): %u, %u",draw,indexed);
}

static void InstallMovieDetection() {
    const BYTE signature[]={0x57,0xff,0x90,0x44,0x01,0,0,0x8b,0x9c,0x24,0x60,0x0e,0,0};
    auto base=(BYTE*)GetModuleHandleW(nullptr);
    auto nt=(IMAGE_NT_HEADERS*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto section=IMAGE_FIRST_SECTION(nt);
    BYTE* call=nullptr;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        if(!(section[i].Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE* code=base+section[i].VirtualAddress;
        for(unsigned offset=0;offset+sizeof(signature)<=section[i].Misc.VirtualSize;++offset) {
            if(code[offset]!=signature[0]||memcmp(code+offset,signature,sizeof(signature))) continue;
            if(call) {Log("Movie screen-quad signature is ambiguous");return;}
            call=code+offset+1;
        }
    }
    if(!call) {Log("Movie screen-quad signature not found");return;}
    DWORD protection=0;
    if(!VirtualProtect(call,6,PAGE_EXECUTE_READWRITE,&protection)) return;
    // Preserve the existing device push and DrawPrimitive's stdcall arguments.
    const uint32_t displacement=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&DrawScreenQuad)-
                                                        reinterpret_cast<uintptr_t>(call+5));
    call[0]=0xe8;
    memcpy(call+1,&displacement,sizeof(displacement));
    call[5]=0x90;
    VirtualProtect(call,6,protection,&protection);
    FlushInstructionCache(GetCurrentProcess(),call,6);
    Log("Movie screen-quad detection installed");
}
