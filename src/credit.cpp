#include "credit.h"

#include "log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>

namespace sitcom {
namespace {

constexpr wchar_t kCreditTitle[] = L"Sitcom mod by jbg0801 2026";
constexpr wchar_t kCreditDedication[] =
    L"This mod is dedicated to my amazing mum. I lost her during the creation of this mod,\n"
    L"and I'll never get to tell her about most of the projects I've worked on.\n"
    L"I miss you mum. I love you, sleep well.";

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);

PresentFn g_orig_present = nullptr;
void** g_present_slot = nullptr;
PresentFn g_present_backup = nullptr;

std::atomic<bool> g_want_credit{false};
std::atomic<bool> g_hooked{false};
std::once_flag g_hook_once;
std::mutex g_draw_mu;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Texture2D* g_tex = nullptr;
IDXGISwapChain* g_bound_swap = nullptr;
DXGI_FORMAT g_tex_format = DXGI_FORMAT_UNKNOWN;
int g_tex_w = 0;
int g_tex_h = 0;
bool g_logged_draw = false;
bool g_logged_fail = false;

HWND FindGameWindow() {
  struct Ctx {
    DWORD pid;
    HWND best;
    int best_area;
  } ctx{GetCurrentProcessId(), nullptr, 0};
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c->pid || !IsWindowVisible(hwnd)) {
          return TRUE;
        }
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) {
          return TRUE;
        }
        const int area = (rc.right - rc.left) * (rc.bottom - rc.top);
        if (area > c->best_area) {
          c->best_area = area;
          c->best = hwnd;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.best;
}

void ReleaseDeviceObjects() {
  if (g_tex) {
    g_tex->Release();
    g_tex = nullptr;
  }
  g_tex_w = g_tex_h = 0;
  g_tex_format = DXGI_FORMAT_UNKNOWN;
  g_bound_swap = nullptr;
  g_device = nullptr;
  g_ctx = nullptr;
}

bool RenderTextBitmap(std::vector<uint32_t>* out_bgra, int* out_w, int* out_h) {
  HDC screen = GetDC(nullptr);
  HDC hdc = CreateCompatibleDC(screen);
  // ANTIALIASED (not ClearType) — ClearType overhang is often clipped under Wine/Proton.
  // Fixed wide canvas: Wine GetTextExtent can under-measure, which was cutting "2026".
  HFONT title_font =
      CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                  DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HFONT body_font =
      CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                  DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  const int pad_x = 20;
  const int pad_y = 12;
  const int gap = 8;
  const int canvas_w = 680;

  HGDIOBJ old_font = title_font ? SelectObject(hdc, title_font) : nullptr;
  SIZE title_sz{};
  GetTextExtentPoint32W(hdc, kCreditTitle, static_cast<int>(wcslen(kCreditTitle)), &title_sz);

  RECT body_measure{0, 0, canvas_w - pad_x * 2, 0};
  if (body_font) {
    SelectObject(hdc, body_font);
  }
  DrawTextW(hdc, kCreditDedication, -1, &body_measure,
            DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_NOCLIP);
  const int body_h = body_measure.bottom - body_measure.top;
  const int w = canvas_w;
  const int h = pad_y + title_sz.cy + gap + body_h + pad_y + 8;

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!bmp || !bits) {
    if (old_font) {
      SelectObject(hdc, old_font);
    }
    if (title_font) {
      DeleteObject(title_font);
    }
    if (body_font) {
      DeleteObject(body_font);
    }
    DeleteDC(hdc);
    return false;
  }
  HGDIOBJ old_bmp = SelectObject(hdc, bmp);
  RECT fill{0, 0, w, h};
  HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
  FillRect(hdc, &fill, brush);
  DeleteObject(brush);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(255, 255, 255));

  RECT title_rc{pad_x, pad_y, w - pad_x, pad_y + title_sz.cy};
  if (title_font) {
    SelectObject(hdc, title_font);
  }
  DrawTextW(hdc, kCreditTitle, -1, &title_rc,
            DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);

  RECT body_rc{pad_x, pad_y + title_sz.cy + gap, w - pad_x, h - pad_y};
  if (body_font) {
    SelectObject(hdc, body_font);
  }
  DrawTextW(hdc, kCreditDedication, -1, &body_rc,
            DT_WORDBREAK | DT_RIGHT | DT_NOPREFIX | DT_NOCLIP);

  if (old_font) {
    SelectObject(hdc, old_font);
  }
  if (title_font) {
    DeleteObject(title_font);
  }
  if (body_font) {
    DeleteObject(body_font);
  }

  // Crop to ink + pad so the blit isn't an oversized black slab, but never clip glyphs.
  const auto* src = static_cast<const uint32_t*>(bits);
  int min_x = w;
  int min_y = h;
  int max_x = 0;
  int max_y = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint32_t p = src[y * w + x] & 0x00FFFFFFu;
      if (p != 0) {
        if (x < min_x) {
          min_x = x;
        }
        if (y < min_y) {
          min_y = y;
        }
        if (x > max_x) {
          max_x = x;
        }
        if (y > max_y) {
          max_y = y;
        }
      }
    }
  }
  if (max_x < min_x) {
    SelectObject(hdc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
    return false;
  }
  const int crop_pad = 10;
  min_x = (min_x > crop_pad) ? (min_x - crop_pad) : 0;
  min_y = (min_y > crop_pad) ? (min_y - crop_pad) : 0;
  max_x = (max_x + crop_pad < w) ? (max_x + crop_pad) : (w - 1);
  max_y = (max_y + crop_pad < h) ? (max_y + crop_pad) : (h - 1);
  const int cw = max_x - min_x + 1;
  const int ch = max_y - min_y + 1;

  out_bgra->resize(static_cast<size_t>(cw * ch));
  for (int y = 0; y < ch; ++y) {
    for (int x = 0; x < cw; ++x) {
      const uint32_t p = src[(min_y + y) * w + (min_x + x)];
      (*out_bgra)[static_cast<size_t>(y * cw + x)] = (p & 0x00FFFFFFu) | 0xFF000000u;
    }
  }

  SelectObject(hdc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(hdc);
  *out_w = cw;
  *out_h = ch;
  return true;
}

void BgraToRgba(std::vector<uint32_t>* px) {
  for (uint32_t& p : *px) {
    const uint32_t a = p & 0xFF000000u;
    const uint32_t r = (p >> 16) & 0xFFu;
    const uint32_t g = (p >> 8) & 0xFFu;
    const uint32_t b = p & 0xFFu;
    p = a | (b << 16) | (g << 8) | r;
  }
}

bool EnsureCreditTexture(ID3D11Device* device, DXGI_FORMAT format) {
  if (g_tex && g_tex_format == format) {
    return true;
  }
  if (g_tex) {
    g_tex->Release();
    g_tex = nullptr;
  }

  std::vector<uint32_t> pixels;
  int w = 0;
  int h = 0;
  if (!RenderTextBitmap(&pixels, &w, &h)) {
    return false;
  }
  if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
    BgraToRgba(&pixels);
  } else if (format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
    // Fall back to BGRA upload; copy may fail if mismatched — caller logs.
  }

  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA init{};
  init.pSysMem = pixels.data();
  init.SysMemPitch = static_cast<UINT>(w * 4);
  if (FAILED(device->CreateTexture2D(&td, &init, &g_tex))) {
    // Some backbuffers are typeless — retry as UNORM BGRA.
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
      // pixels already swapped to RGBA; rebuild BGRA
      pixels.clear();
      if (!RenderTextBitmap(&pixels, &w, &h)) {
        return false;
      }
    }
    if (FAILED(device->CreateTexture2D(&td, &init, &g_tex))) {
      return false;
    }
    g_tex_format = DXGI_FORMAT_B8G8R8A8_UNORM;
  } else {
    g_tex_format = format;
  }
  g_tex_w = w;
  g_tex_h = h;
  return true;
}

bool DrawCreditD3D(IDXGISwapChain* swap) {
  std::lock_guard<std::mutex> lock(g_draw_mu);

  ID3D11Device* device = nullptr;
  if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) ||
      !device) {
    if (!g_logged_fail) {
      LogWrite("credit: swapchain is not D3D11 (cannot draw)");
      g_logged_fail = true;
    }
    return false;
  }
  ID3D11DeviceContext* ctx = nullptr;
  device->GetImmediateContext(&ctx);
  if (!ctx) {
    device->Release();
    return false;
  }

  if (g_bound_swap != swap || g_device != device) {
    ReleaseDeviceObjects();
    g_device = device;
    g_ctx = ctx;
    g_bound_swap = swap;
    g_device->AddRef();
    g_ctx->AddRef();
  }
  device->Release();
  ctx->Release();

  if (!g_device || !g_ctx) {
    return false;
  }

  ID3D11Texture2D* back = nullptr;
  if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) ||
      !back) {
    return false;
  }
  D3D11_TEXTURE2D_DESC back_desc{};
  back->GetDesc(&back_desc);

  DXGI_FORMAT copy_fmt = back_desc.Format;
  if (copy_fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS) {
    copy_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  } else if (copy_fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
    copy_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
  }

  if (!EnsureCreditTexture(g_device, copy_fmt)) {
    back->Release();
    if (!g_logged_fail) {
      LogWrite("credit: failed to create credit texture");
      g_logged_fail = true;
    }
    return false;
  }

  const UINT bw = back_desc.Width;
  const UINT bh = back_desc.Height;
  if (bw < 64 || bh < 64) {
    back->Release();
    return false;
  }

  const UINT tw = static_cast<UINT>(g_tex_w);
  const UINT th = static_cast<UINT>(g_tex_h);
  // Top-right; keep clear of the display edge (Proton overlays / overscan).
  const UINT margin_x = (bw > 80) ? (bw / 28) : 24;
  const UINT margin_y = (bh > 40) ? bh / 36 : 8;
  UINT dstx = (bw > tw + margin_x) ? (bw - tw - margin_x) : 0;
  UINT dsty = margin_y;

  D3D11_BOX box{};
  box.left = 0;
  box.top = 0;
  box.front = 0;
  box.right = tw;
  box.bottom = th;
  box.back = 1;

  // Blit opaque credit bar onto the backbuffer — no shaders / d3dcompiler needed.
  g_ctx->CopySubresourceRegion(back, 0, dstx, dsty, 0, g_tex, 0, &box);
  back->Release();

  if (!g_logged_draw) {
    LogWrite("credit: drew title credit via D3D11 Present blit");
    g_logged_draw = true;
  }
  return true;
}

HRESULT __stdcall HookedPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
  if (g_want_credit.load(std::memory_order_relaxed) && swap) {
    DrawCreditD3D(swap);
  }
  return g_orig_present ? g_orig_present(swap, sync, flags) : S_OK;
}

bool PatchPresentVtable(IDXGISwapChain* swap) {
  if (!swap) {
    return false;
  }
  void** vtable = *reinterpret_cast<void***>(swap);
  void** slot = &vtable[8];  // IDXGISwapChain::Present
  if (g_hooked.load() && g_present_slot == slot) {
    return true;
  }

  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
    return false;
  }
  g_present_slot = slot;
  g_present_backup = reinterpret_cast<PresentFn>(vtable[8]);
  g_orig_present = g_present_backup;
  *slot = reinterpret_cast<void*>(&HookedPresent);
  VirtualProtect(slot, sizeof(void*), old, &old);
  g_hooked.store(true);
  LogWrite("credit: IDXGISwapChain::Present hooked (D3D11 blit)");
  return true;
}

bool InstallPresentHook() {
  HWND hwnd = FindGameWindow();
  if (!hwnd) {
    hwnd = CreateWindowExW(0, L"STATIC", L"sitcom_dummy", WS_OVERLAPPED, 0, 0, 100, 100, nullptr,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
  }
  if (!hwnd) {
    return false;
  }

  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 1;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.Width = 2;
  sd.BufferDesc.Height = 2;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain* swap = nullptr;
  const HRESULT hr =
      D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
                                    D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx);
  if (FAILED(hr) || !swap) {
    LogWrite("credit: dummy D3D11 device failed (Present hook skipped)");
    return false;
  }

  const bool ok = PatchPresentVtable(swap);
  swap->Release();
  ctx->Release();
  dev->Release();
  return ok;
}

}  // namespace

void CreditEnsureHook() {
  std::call_once(g_hook_once, []() {
    for (int i = 0; i < 50 && !g_hooked.load(); ++i) {
      if (InstallPresentHook()) {
        break;
      }
      Sleep(100);
    }
    if (g_hooked.load()) {
      LogWrite("credit: Present hook installed");
    } else {
      LogWrite("credit: Present hook never installed");
    }
  });
}

void CreditUpdate(bool on_title_screen) {
  g_want_credit.store(on_title_screen, std::memory_order_relaxed);
}

void CreditShutdown() {
  g_want_credit.store(false);
  {
    std::lock_guard<std::mutex> lock(g_draw_mu);
    if (g_device) {
      g_device->Release();
    }
    if (g_ctx) {
      g_ctx->Release();
    }
    ReleaseDeviceObjects();
  }
  if (g_hooked.load() && g_present_slot && g_present_backup) {
    DWORD old = 0;
    if (VirtualProtect(g_present_slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
      *g_present_slot = reinterpret_cast<void*>(g_present_backup);
      VirtualProtect(g_present_slot, sizeof(void*), old, &old);
    }
  }
  g_hooked.store(false);
}

}  // namespace sitcom
