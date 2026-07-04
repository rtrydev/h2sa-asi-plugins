/* Probe raw EXCLUSIVE-FULLSCREEN capability of the D3D8 stack directly
 * (builtin d3d8, no proxy). Enumerate the adapter's fullscreen modes and try
 * CreateDevice(Windowed=FALSE) at each, plus the game's requested sizes. */
#include <d3d8.h>
#include <stdio.h>

static const char *fmtname(D3DFORMAT f){
    switch(f){case D3DFMT_X8R8G8B8:return "X8R8G8B8";case D3DFMT_A8R8G8B8:return "A8R8G8B8";
    case D3DFMT_R5G6B5:return "R5G6B5";case D3DFMT_X1R5G5B5:return "X1R5G5B5";
    default:return "?";}
}

static IDirect3D8 *d3d;
static HWND wnd;

static int try_fs(int w,int h,D3DFORMAT fmt){
    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp,sizeof(pp));
    pp.BackBufferWidth=w; pp.BackBufferHeight=h; pp.BackBufferFormat=fmt;
    pp.BackBufferCount=1; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=wnd; pp.Windowed=FALSE;
    pp.EnableAutoDepthStencil=TRUE; pp.AutoDepthStencilFormat=D3DFMT_D16;
    pp.FullScreen_RefreshRateInHz=D3DPRESENT_RATE_DEFAULT;
    IDirect3DDevice8 *dev=NULL;
    HRESULT hr=d3d->lpVtbl->CreateDevice(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,
        wnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev);
    printf("  FULLSCREEN %dx%d %-9s -> hr=0x%08lx %s\n",w,h,fmtname(fmt),
        (unsigned long)hr, (SUCCEEDED(hr)&&dev)?"OK":"FAIL");
    if(SUCCEEDED(hr)&&dev){ dev->lpVtbl->Release(dev); return 1; }
    return 0;
}

int main(void){
    d3d=Direct3DCreate8(D3D_SDK_VERSION);
    if(!d3d){ printf("no d3d\n"); return 2; }
    D3DDISPLAYMODE dm; d3d->lpVtbl->GetAdapterDisplayMode(d3d,0,&dm);
    printf("desktop mode: %ux%u %s\n\n", dm.Width,dm.Height,fmtname(dm.Format));

    UINT n=d3d->lpVtbl->GetAdapterModeCount(d3d,0);
    printf("adapter reports %u fullscreen modes:\n", n);
    D3DDISPLAYMODE modes[256]; UINT nm=0;
    for(UINT i=0;i<n && nm<256;i++){
        D3DDISPLAYMODE m; if(SUCCEEDED(d3d->lpVtbl->EnumAdapterModes(d3d,0,i,&m))){
            printf("  [%2u] %ux%u %s @%u\n",i,m.Width,m.Height,fmtname(m.Format),m.RefreshRate);
            modes[nm++]=m;
        }
    }
    WNDCLASSA wc; ZeroMemory(&wc,sizeof(wc)); wc.lpfnWndProc=DefWindowProcA;
    wc.hInstance=GetModuleHandleA(NULL); wc.lpszClassName="fsprobe";
    RegisterClassA(&wc);
    wnd=CreateWindowExA(0,"fsprobe","fs",WS_POPUP,0,0,640,480,0,0,wc.hInstance,0);

    printf("\ntry exclusive fullscreen at the desktop mode:\n");
    try_fs(dm.Width,dm.Height,dm.Format);
    printf("\ntry exclusive fullscreen at each enumerated 32-bit mode:\n");
    int anyok=0;
    for(UINT i=0;i<nm;i++)
        if(modes[i].Format==D3DFMT_X8R8G8B8||modes[i].Format==D3DFMT_A8R8G8B8)
            anyok |= try_fs(modes[i].Width,modes[i].Height,modes[i].Format);
    printf("\ntry the game's typical requests:\n");
    try_fs(800,600,D3DFMT_X8R8G8B8);
    try_fs(1280,1024,D3DFMT_X8R8G8B8);
    try_fs(1920,1080,D3DFMT_X8R8G8B8);
    printf("\nSUMMARY: exclusive fullscreen %s on this stack\n",
        anyok?"WORKS at some enumerated mode":"FAILS at every mode tried");
    return 0;
}
