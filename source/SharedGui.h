#include <d3dcompiler.h>

static IDirect3DTexture9* sharedLayer=nullptr;
static IDirect3DPixelShader9* sharedShader=nullptr;
static D3DSURFACE_DESC sharedDesc{};

static void ReleaseSharedGui() {
    ReleaseNow(sharedLayer,"Shared menu layer");
    if(sharedShader) sharedShader->Release();
    sharedShader=nullptr;
    sharedDesc={};
}

static bool CaptureSharedGui(IDirect3DDevice9* device,IDirect3DSurface9* source,
                             const D3DSURFACE_DESC& desc) {
    if(!sharedShader) {
        // The unused part of the backbuffer is cleared to black. Reconstruct
        // coverage from the UI color since the backbuffer has no reliable alpha.
        constexpr char shader[]=R"(
            sampler2D layer : register(s0);
            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                float3 rgb=tex2D(layer,uv).rgb;
                float coverage=saturate(2*max(rgb.r,max(rgb.g,rgb.b)));
                return float4(rgb,coverage);
            }
        )";
        ID3DBlob* code=nullptr;
        HRESULT hr=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,
                              "main","ps_2_0",0,0,&code,nullptr);
        if(FAILED(hr)) return false;
        hr=device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),&sharedShader);
        code->Release();
        if(FAILED(hr)) return false;
    }
    if(sharedLayer&&(sharedDesc.Width!=desc.Width||sharedDesc.Height!=desc.Height||sharedDesc.Format!=desc.Format)) {
        sharedLayer->Release();
        sharedLayer=nullptr;
    }
    if(!sharedLayer) {
        if(FAILED(device->CreateTexture(desc.Width/2,desc.Height/2,1,D3DUSAGE_RENDERTARGET,
                                        desc.Format,D3DPOOL_DEFAULT,&sharedLayer,nullptr))) return false;
        sharedDesc=desc;
    }
    IDirect3DSurface9* surface=nullptr;
    if(FAILED(sharedLayer->GetSurfaceLevel(0,&surface))) return false;
    RECT rect={LONG(desc.Width/2),0,LONG(desc.Width),LONG(desc.Height/2)};
    HRESULT hr=device->StretchRect(source,&rect,surface,nullptr,D3DTEXF_NONE);
    surface->Release();
    return SUCCEEDED(hr);
}

static bool DrawSharedGui(IDirect3DDevice9* device,IDirect3DSurface9* dest,const RECT& rect) {
    IDirect3DStateBlock9* state=nullptr;
    IDirect3DSurface9* target=nullptr;
    IDirect3DSurface9* depth=nullptr;
    D3DVIEWPORT9 viewport{};
    if(FAILED(device->CreateStateBlock(D3DSBT_ALL,&state))) return false;
    device->GetRenderTarget(0,&target);
    device->GetDepthStencilSurface(&depth);
    device->GetViewport(&viewport);
    if(!target||FAILED(device->BeginScene())) {
        if(target) target->Release();
        if(depth) depth->Release();
        state->Release();
        return false;
    }
    device->SetDepthStencilSurface(nullptr);
    HRESULT hr=device->SetRenderTarget(0,dest);
    if(SUCCEEDED(hr)) {
        device->SetVertexShader(nullptr);
        device->SetPixelShader(sharedShader);
        device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
        device->SetTexture(0,sharedLayer);
        device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
        device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
        device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
        device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
        device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
        device->SetRenderState(D3DRS_ZENABLE,FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE,FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
        device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
        device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        struct Vertex {float x,y,z,rhw,u,v;};
        const float l=rect.left-0.5f,r=rect.right-0.5f,t=rect.top-0.5f,b=rect.bottom-0.5f;
        const Vertex quad[]={{l,t,0,1,0,0},{r,t,0,1,1,0},{l,b,0,1,0,1},{r,b,0,1,1,1}};
        hr=device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex));
    }
    device->EndScene();
    device->SetRenderTarget(0,target);
    device->SetDepthStencilSurface(depth);
    state->Apply();
    device->SetViewport(&viewport);
    target->Release();
    if(depth) depth->Release();
    state->Release();
    return SUCCEEDED(hr);
}
