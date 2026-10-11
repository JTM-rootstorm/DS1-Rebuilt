// dxvk-native smoke test: SDL3 window, D3D9 device through DXVK, a few
// hundred frames of clear + a fixed-function triangle. The triangle uses
// FVF + lighting off + diffuse vertex colour, i.e. the same fixed-function
// path a D3D7-on-D3D9 layer will lean on.
//
//   d3d9_smoke [frames]

#include <windows.h>
#include <d3d9.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

struct Vertex {
  float x, y, z, rhw;
  DWORD color;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

}  // namespace

int main(int argc, char** argv) {
  int frames = argc > 1 ? std::atoi(argv[1]) : 300;

  // dxvk-native throws from Direct3DCreate9 unless a WSI backend is named.
  setenv("DXVK_WSI_DRIVER", "SDL3", 0);

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window =
      SDL_CreateWindow("ds1 d3d9 smoke", 1024, 768, SDL_WINDOW_VULKAN);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 1;
  }

  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    std::fprintf(stderr, "Direct3DCreate9 failed\n");
    return 1;
  }
  D3DADAPTER_IDENTIFIER9 id{};
  d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
  std::printf("adapter: %s (%s)\n", id.Description, id.Driver);

  // dxvk-native takes the SDL_Window* in place of an HWND.
  HWND hwnd = reinterpret_cast<HWND>(window);
  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 1024;
  pp.BackBufferHeight = 768;
  pp.hDeviceWindow = hwnd;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

  IDirect3DDevice9* dev = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                 &dev);
  if (FAILED(hr)) {
    std::fprintf(stderr, "CreateDevice failed: %08lx\n", (unsigned long)hr);
    return 1;
  }

  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetFVF(kFvf);

  bool quit = false;
  for (int f = 0; f < frames && !quit; f++) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
      if (ev.type == SDL_EVENT_QUIT) quit = true;

    float t = f / 60.0f;
    D3DCOLOR clear = D3DCOLOR_XRGB(20, 24, int(40 + 30 * std::sin(t)));
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clear, 1.0f, 0);
    dev->BeginScene();
    float cx = 512, cy = 384, r = 250;
    Vertex tri[3];
    for (int i = 0; i < 3; i++) {
      float a = t + i * 2.0943951f;
      tri[i] = {cx + r * std::cos(a), cy + r * std::sin(a), 0.5f, 1.0f,
                i == 0 ? 0xFFFF4040u : i == 1 ? 0xFF40FF40u : 0xFF4040FFu};
    }
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
    dev->EndScene();
    if (FAILED(dev->Present(nullptr, nullptr, nullptr, nullptr))) {
      std::fprintf(stderr, "Present failed at frame %d\n", f);
      return 1;
    }
  }
  std::printf("presented %d frames OK\n", frames);

  dev->Release();
  d3d->Release();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
