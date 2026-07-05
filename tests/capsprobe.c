/* Non-disruptive probe: what presentation intervals does this D3D8 HAL
 * support, and what fullscreen refresh rates does the adapter enumerate?
 * Needs no device (no mode switch), so it is safe to run unattended.
 * GetDeviceCaps fills D3DCAPS8.PresentationIntervals from the driver. */
#include <d3d8.h>
#include <stdio.h>

int main(void){
    IDirect3D8 *d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if(!d3d){ printf("no d3d\n"); return 2; }

    D3DDISPLAYMODE dm; d3d->lpVtbl->GetAdapterDisplayMode(d3d,0,&dm);
    printf("desktop: %ux%u @%uHz\n", dm.Width, dm.Height, dm.RefreshRate);

    D3DCAPS8 caps;
    HRESULT hr = d3d->lpVtbl->GetDeviceCaps(d3d,0,D3DDEVTYPE_HAL,&caps);
    printf("GetDeviceCaps(HAL) hr=0x%08lx\n", (unsigned long)hr);
    DWORD pi = caps.PresentationIntervals;
    printf("PresentationIntervals = 0x%08lx\n", (unsigned long)pi);
    printf("  IMMEDIATE : %s\n", (pi & D3DPRESENT_INTERVAL_IMMEDIATE)?"yes":"no");
    printf("  ONE       : %s\n", (pi & D3DPRESENT_INTERVAL_ONE)?"yes":"no");
    printf("  TWO       : %s\n", (pi & D3DPRESENT_INTERVAL_TWO)?"yes":"no");
    printf("  THREE     : %s\n", (pi & D3DPRESENT_INTERVAL_THREE)?"yes":"no");
    printf("  FOUR      : %s\n", (pi & D3DPRESENT_INTERVAL_FOUR)?"yes":"no");

    UINT n = d3d->lpVtbl->GetAdapterModeCount(d3d,0);
    printf("\n%u fullscreen modes; refresh rates at %ux%u:\n", n, dm.Width, dm.Height);
    for(UINT i=0;i<n;i++){
        D3DDISPLAYMODE m;
        if(SUCCEEDED(d3d->lpVtbl->EnumAdapterModes(d3d,0,i,&m)) &&
           m.Width==dm.Width && m.Height==dm.Height &&
           (m.Format==D3DFMT_X8R8G8B8||m.Format==D3DFMT_A8R8G8B8))
            printf("  %ux%u @%uHz\n", m.Width, m.Height, m.RefreshRate);
    }
    d3d->lpVtbl->Release(d3d);
    return 0;
}
