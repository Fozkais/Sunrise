#include "mission_editor_window.h"

#include <Windows.h>

#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <string>

#include "../../../client/hooks/graphics/renderer/renderer.h"
#include "mission_editor_view.h"

// The window makes its own device; the game's is never shared with it.
#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window,
                                                             UINT message,
                                                             WPARAM word,
                                                             LPARAM value);

namespace sunrise::server::ui::mission_editor::window {
namespace {

namespace renderer = client::hooks::graphics::renderer;

constexpr const wchar_t* kClassName = L"SunriseMissionEditor";
constexpr const wchar_t* kTitle = L"Sunrise - Mission editor";
constexpr float kFontPixels = 17.0F;

/** The thread and its window are alive. */
std::atomic_bool g_open{false};
/** The window should close. */
std::atomic_bool g_stop{false};
/** A shutdown holds the interface lock; nothing may wait on it any more. */
std::atomic_bool g_abandon{false};
std::atomic<UINT> g_resizeWidth{0};
std::atomic<UINT> g_resizeHeight{0};
std::atomic<HWND> g_window{nullptr};
/**
 * Set by a caller that wants the open window in front. The window thread raises it itself: the
 * caller holds the interface lock, and raising the window from there sends it messages its
 * procedure answers under that same lock, which deadlocks the game.
 */
std::atomic_bool g_raise{false};
HANDLE g_thread{};

// Owned by the window thread while it runs, then by whoever tears the window down.
ImGuiContext* g_imgui{};
ID3D11Device* g_device{};
ID3D11DeviceContext* g_context{};
IDXGISwapChain* g_swapChain{};
ID3D11RenderTargetView* g_target{};
std::string g_font{};
std::string g_boldFont{};
/**
 * The window thread holds the interface lock. A call made under it, such as the backend's
 * ReleaseCapture, can send a message straight back into window_proc on this thread; that nested
 * call must use the lock already held, or it waits on itself and the game waits behind it.
 */
thread_local bool t_holding{};

[[nodiscard]] HINSTANCE module_instance() noexcept {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&g_open),
                       &module);
    return module;
}

void release_target() noexcept {
    if (g_target != nullptr) {
        g_target->Release();
        g_target = nullptr;
    }
}

[[nodiscard]] bool create_target() noexcept {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || back == nullptr) {
        return false;
    }
    const HRESULT created = g_device->CreateRenderTargetView(back, nullptr, &g_target);
    back->Release();
    return SUCCEEDED(created);
}

void release_device() noexcept {
    release_target();
    if (g_swapChain != nullptr) {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    if (g_context != nullptr) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
}

[[nodiscard]] bool create_device(HWND window) noexcept {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr,
                                             D3D_DRIVER_TYPE_HARDWARE,
                                             nullptr,
                                             0,
                                             levels,
                                             2,
                                             D3D11_SDK_VERSION,
                                             &description,
                                             &g_swapChain,
                                             &g_device,
                                             &level,
                                             &g_context))) {
        return false;
    }
    return create_target();
}

/** Reads one Windows font file, so the window reads like a desktop tool. */
void load_font(const wchar_t* name, std::string& output) noexcept {
    if (!output.empty()) {
        return;
    }
    std::wstring path(MAX_PATH, L'\0');
    const UINT length = GetWindowsDirectoryW(path.data(), MAX_PATH);
    path.resize(length);
    path += L"\\Fonts\\";
    path += name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0 && size.QuadPart < 8 << 20) {
        try {
            output.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            if (ReadFile(file, output.data(), static_cast<DWORD>(output.size()), &read, nullptr)
                    == FALSE
                || read != output.size()) {
                output.clear();
            }
        } catch (...) {
            output.clear();
        }
    }
    CloseHandle(file);
}

[[nodiscard]] constexpr ImVec4 rgb(int red, int green, int blue, float alpha = 1.0F) noexcept {
    return {static_cast<float>(red) / 255.0F, static_cast<float>(green) / 255.0F,
            static_cast<float>(blue) / 255.0F, alpha};
}

/** A dark slate theme with one blue accent, roomy spacing and rounded controls. */
void apply_theme(float scale) noexcept {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle{};
    style.WindowPadding = {16.0F, 14.0F};
    style.FramePadding = {10.0F, 6.0F};
    style.ItemSpacing = {10.0F, 8.0F};
    style.ItemInnerSpacing = {6.0F, 6.0F};
    style.CellPadding = {8.0F, 4.0F};
    style.IndentSpacing = 20.0F;
    style.ScrollbarSize = 12.0F;
    style.GrabMinSize = 12.0F;
    style.WindowRounding = 0.0F;
    style.ChildRounding = 8.0F;
    style.FrameRounding = 6.0F;
    style.PopupRounding = 8.0F;
    style.ScrollbarRounding = 6.0F;
    style.GrabRounding = 6.0F;
    style.TabRounding = 6.0F;
    style.WindowBorderSize = 0.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.SeparatorTextBorderSize = 1.0F;
    style.SeparatorTextPadding = {0.0F, 6.0F};
    style.SelectableTextAlign = {0.0F, 0.5F};
    ImVec4* colors = style.Colors;
    const ImVec4 text = rgb(226, 230, 238);
    const ImVec4 dim = rgb(141, 149, 168);
    const ImVec4 base = rgb(17, 19, 24);
    const ImVec4 panel = rgb(23, 26, 33);
    const ImVec4 raised = rgb(32, 36, 45);
    const ImVec4 hover = rgb(43, 49, 62);
    const ImVec4 line = rgb(45, 50, 62);
    const ImVec4 accent = rgb(110, 168, 254);
    const ImVec4 accentDeep = rgb(31, 51, 80);
    const ImVec4 accentMid = rgb(43, 86, 148);
    colors[ImGuiCol_Text] = text;
    colors[ImGuiCol_TextDisabled] = dim;
    colors[ImGuiCol_WindowBg] = base;
    colors[ImGuiCol_ChildBg] = panel;
    colors[ImGuiCol_PopupBg] = rgb(24, 27, 35, 0.98F);
    colors[ImGuiCol_Border] = line;
    colors[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0.0F);
    colors[ImGuiCol_FrameBg] = raised;
    colors[ImGuiCol_FrameBgHovered] = hover;
    colors[ImGuiCol_FrameBgActive] = accentDeep;
    colors[ImGuiCol_TitleBg] = panel;
    colors[ImGuiCol_TitleBgActive] = panel;
    colors[ImGuiCol_MenuBarBg] = panel;
    colors[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0.0F);
    colors[ImGuiCol_ScrollbarGrab] = raised;
    colors[ImGuiCol_ScrollbarGrabHovered] = hover;
    colors[ImGuiCol_ScrollbarGrabActive] = accentMid;
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = accent;
    colors[ImGuiCol_Button] = raised;
    colors[ImGuiCol_ButtonHovered] = hover;
    colors[ImGuiCol_ButtonActive] = accentDeep;
    colors[ImGuiCol_Header] = accentDeep;
    colors[ImGuiCol_HeaderHovered] = hover;
    colors[ImGuiCol_HeaderActive] = accentMid;
    colors[ImGuiCol_Separator] = line;
    colors[ImGuiCol_SeparatorHovered] = accentMid;
    colors[ImGuiCol_SeparatorActive] = accent;
    colors[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0.0F);
    colors[ImGuiCol_Tab] = raised;
    colors[ImGuiCol_TabHovered] = hover;
    colors[ImGuiCol_TabSelected] = accentDeep;
    colors[ImGuiCol_TextSelectedBg] = rgb(110, 168, 254, 0.35F);
    colors[ImGuiCol_NavCursor] = accent;
    colors[ImGuiCol_ModalWindowDimBg] = rgb(5, 7, 10, 0.6F);
    style.ScaleAllSizes(scale);
}

/** Creates the window's interface context. The caller holds the interface lock. */
[[nodiscard]] bool create_interface(HWND window) noexcept {
    ImGuiContext* const previous = ImGui::GetCurrentContext();
    g_imgui = ImGui::CreateContext();
    if (g_imgui == nullptr) {
        ImGui::SetCurrentContext(previous);
        return false;
    }
    ImGui::SetCurrentContext(g_imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0F;
    load_font(L"segoeui.ttf", g_font);
    load_font(L"segoeuib.ttf", g_boldFont);
    ImFontConfig config{};
    config.FontDataOwnedByAtlas = false;
    if (!g_font.empty()) {
        io.Fonts->AddFontFromMemoryTTF(
            g_font.data(), static_cast<int>(g_font.size()), kFontPixels * scale, &config);
    } else {
        io.Fonts->AddFontDefault();
    }
    ImFont* const heading =
        g_boldFont.empty()
            ? nullptr
            : io.Fonts->AddFontFromMemoryTTF(g_boldFont.data(), static_cast<int>(g_boldFont.size()),
                                             kFontPixels * scale, &config);
    view::set_fonts(heading, scale);
    apply_theme(scale);
    const bool ready = ImGui_ImplWin32_Init(window) && ImGui_ImplDX11_Init(g_device, g_context);
    ImGui::SetCurrentContext(previous);
    return ready;
}

/** Frees the interface context. The caller holds the interface lock. */
void destroy_interface() noexcept {
    if (g_imgui == nullptr) {
        return;
    }
    ImGuiContext* const previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_imgui);
    if (ImGui::GetIO().BackendRendererUserData != nullptr) {
        ImGui_ImplDX11_Shutdown();
    }
    if (ImGui::GetIO().BackendPlatformUserData != nullptr) {
        ImGui_ImplWin32_Shutdown();
    }
    ImGui::DestroyContext(g_imgui);
    ImGui::SetCurrentContext(previous == g_imgui ? nullptr : previous);
    g_imgui = nullptr;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM word, LPARAM value) {
    if (g_imgui != nullptr && t_holding) {
        // Nested inside a call this thread made under the lock: the context is already current.
        const LRESULT handled = ImGui_ImplWin32_WndProcHandler(window, message, word, value);
        if (handled != 0) {
            return handled;
        }
    } else if (g_imgui != nullptr && !g_abandon.load(std::memory_order_acquire)
               && renderer::lock_interface(g_abandon)) {
        t_holding = true;
        ImGuiContext* const previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_imgui);
        const LRESULT handled = ImGui_ImplWin32_WndProcHandler(window, message, word, value);
        ImGui::SetCurrentContext(previous);
        t_holding = false;
        renderer::unlock_interface();
        if (handled != 0) {
            return handled;
        }
    }
    switch (message) {
    case WM_SIZE:
        if (word != SIZE_MINIMIZED) {
            g_resizeWidth.store(LOWORD(value));
            g_resizeHeight.store(HIWORD(value));
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((word & 0xFFF0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_CLOSE:
        g_stop.store(true, std::memory_order_release);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, word, value);
}

/** One frame: the view drawn in the window's own context, under the interface lock. */
[[nodiscard]] bool frame() noexcept {
    if (!renderer::lock_interface(g_abandon)) {
        return false;
    }
    t_holding = true;
    ImGuiContext* const previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_imgui);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    view::draw();
    ImGui::Render();
    const float clear[4] = {0.07F, 0.08F, 0.10F, 1.0F};
    g_context->OMSetRenderTargets(1, &g_target, nullptr);
    g_context->ClearRenderTargetView(g_target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ImGui::SetCurrentContext(previous);
    t_holding = false;
    renderer::unlock_interface();
    return true;
}

DWORD WINAPI run(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE instance = module_instance();
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof windowClass;
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = &window_proc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);
    const HWND window = CreateWindowExW(0, kClassName, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                        CW_USEDEFAULT, 1280, 880, nullptr, nullptr, instance, nullptr);
    g_window.store(window);
    bool ready = window != nullptr && create_device(window);
    if (ready && renderer::lock_interface(g_abandon)) {
        t_holding = true;
        ready = create_interface(window);
        t_holding = false;
        renderer::unlock_interface();
    } else {
        ready = false;
    }
    if (ready) {
        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);
    }
    while (ready && !g_stop.load(std::memory_order_acquire)) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (g_stop.load(std::memory_order_acquire)) {
            break;
        }
        if (g_raise.exchange(false)) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        }
        const UINT width = g_resizeWidth.exchange(0);
        const UINT height = g_resizeHeight.exchange(0);
        if (width != 0 && height != 0) {
            release_target();
            g_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            if (!create_target()) {
                break;
            }
        }
        if (IsIconic(window) != FALSE) {
            Sleep(50);
            continue;
        }
        if (!frame()) {
            break;
        }
        g_swapChain->Present(1, 0);
    }
    // A shutdown that holds the interface lock frees the context itself, then the device.
    if (!g_abandon.load(std::memory_order_acquire) && renderer::lock_interface(g_abandon)) {
        t_holding = true;
        destroy_interface();
        t_holding = false;
        renderer::unlock_interface();
    }
    if (g_imgui == nullptr) {
        release_device();
    }
    g_window.store(nullptr);
    if (window != nullptr) {
        DestroyWindow(window);
    }
    UnregisterClassW(kClassName, instance);
    g_open.store(false, std::memory_order_release);
    return 0;
}

} // namespace

void open() noexcept {
    if (g_open.load(std::memory_order_acquire)) {
        // Only the window thread may raise it; a posted message wakes its loop.
        g_raise.store(true, std::memory_order_release);
        const HWND window = g_window.load();
        if (window != nullptr) {
            PostMessageW(window, WM_NULL, 0, 0);
        }
        return;
    }
    if (g_thread != nullptr) {
        // A window still closing may be waiting for the interface lock its caller holds.
        if (WaitForSingleObject(g_thread, 0) == WAIT_TIMEOUT) {
            return;
        }
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    g_stop.store(false);
    g_abandon.store(false);
    g_open.store(true, std::memory_order_release);
    g_thread = CreateThread(nullptr, 0, &run, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        g_open.store(false);
    }
}

bool is_open() noexcept {
    return g_open.load(std::memory_order_acquire);
}

void close() noexcept {
    g_stop.store(true, std::memory_order_release);
    const HWND window = g_window.load();
    if (window != nullptr) {
        PostMessageW(window, WM_NULL, 0, 0);
    }
}

void shutdown_locked() noexcept {
    if (g_thread == nullptr) {
        return;
    }
    g_abandon.store(true, std::memory_order_release);
    close();
    WaitForSingleObject(g_thread, 10'000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    destroy_interface();
    release_device();
    g_open.store(false);
}

} // namespace sunrise::server::ui::mission_editor::window
