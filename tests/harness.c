/* Isolated mechanics test for the H2SA d3d8 proxy + widescreen plugin.
 * Mimics what RenderD3D.dll does: create a window, Direct3DCreate8, then
 * CreateDevice with EXCLUSIVE-FULLSCREEN parameters (the configuration
 * that fails on CrossOver). If the proxy + plugin work, the plugin rewrites
 * the parameters to windowed and CreateDevice succeeds. Also issues a
 * SetTransform(PROJECTION) to confirm the FOV path runs.
 *
 * Loads the proxy explicitly from the path in argv[1] so we exercise the
 * game-directory copy (its DllMain then scans <dir>/scripts/*.asi). */
#include <d3d8.h>
#include <stdio.h>

static const char *fmtname(D3DFORMAT f){
    switch(f){case D3DFMT_UNKNOWN:return "UNKNOWN";
    case D3DFMT_X8R8G8B8:return "X8R8G8B8";case D3DFMT_A8R8G8B8:return "A8R8G8B8";
    case D3DFMT_R5G6B5:return "R5G6B5";default:return "other";}
}

int main(int argc, char **argv){
    const char *dllpath = argc>1?argv[1]:"d3d8.dll";
    HMODULE h = LoadLibraryA(dllpath);
    if(!h){ printf("FAIL: LoadLibrary(%s) err=%lu\n", dllpath, GetLastError()); return 2; }
    printf("proxy loaded at %p\n", (void*)h);

    typedef IDirect3D8* (WINAPI *dc_t)(UINT);
    dc_t Create = (dc_t)(void*)GetProcAddress(h, "Direct3DCreate8");
    if(!Create){ printf("FAIL: no Direct3DCreate8 export\n"); return 2; }

    IDirect3D8 *d3d = Create(D3D_SDK_VERSION);
    if(!d3d){ printf("FAIL: Direct3DCreate8 returned NULL\n"); return 2; }
    printf("IDirect3D8 = %p\n", (void*)d3d);

    D3DDISPLAYMODE dm; d3d->lpVtbl->GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &dm);
    printf("desktop mode %ux%u fmt=%s\n", dm.Width, dm.Height, fmtname(dm.Format));

    WNDCLASSA wc; ZeroMemory(&wc,sizeof(wc));
    wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(NULL);
    wc.lpszClassName="H2SATestWnd"; RegisterClassA(&wc);
    HWND wnd=CreateWindowExA(0,"H2SATestWnd","h2sa",WS_POPUP,0,0,800,600,
        NULL,NULL,wc.hInstance,NULL);
    printf("window = %p\n", (void*)wnd);

    /* exclusive fullscreen request, like the stock game */
    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp,sizeof(pp));
    pp.BackBufferWidth=800; pp.BackBufferHeight=600;
    pp.BackBufferFormat=D3DFMT_X8R8G8B8; pp.BackBufferCount=1;
    pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=wnd;
    pp.Windowed=FALSE; pp.EnableAutoDepthStencil=TRUE;
    pp.AutoDepthStencilFormat=D3DFMT_D16;
    pp.FullScreen_RefreshRateInHz=D3DPRESENT_RATE_DEFAULT;

    IDirect3DDevice8 *dev=NULL;
    HRESULT hr=d3d->lpVtbl->CreateDevice(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,
        wnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev);
    printf("CreateDevice hr=0x%08lx dev=%p  (after fixup: %ux%u windowed=%d fmt=%s)\n",
        (unsigned long)hr,(void*)dev,pp.BackBufferWidth,pp.BackBufferHeight,
        pp.Windowed,fmtname(pp.BackBufferFormat));
    if(FAILED(hr)||!dev){ printf("RESULT: CreateDevice FAILED\n"); return 1; }

    /* perspective projection: _34=1,_44=0. Feed a 4:3-authored matrix and
     * check the plugin scales _11 for the (wide) backbuffer aspect. */
    D3DMATRIX m; ZeroMemory(&m,sizeof(m));
    float w=1.3f, hh=1.7f;              /* arbitrary xScale/yScale */
    m._11=w; m._22=hh; m._33=1.0f; m._34=1.0f;
    printf("proj _11 before = %.5f\n", m._11);
    dev->lpVtbl->SetTransform(dev,D3DTS_PROJECTION,&m);
    /* Read back what the device stored to see the applied value. */
    D3DMATRIX got; dev->lpVtbl->GetTransform(dev,D3DTS_PROJECTION,&got);
    printf("proj _11 stored  = %.5f (bb %ux%u aspect %.3f)\n",
        got._11, pp.BackBufferWidth, pp.BackBufferHeight,
        (double)pp.BackBufferWidth/pp.BackBufferHeight);

    printf("RESULT: OK\n");
    dev->lpVtbl->Release(dev);
    d3d->lpVtbl->Release(d3d);
    return 0;
}
