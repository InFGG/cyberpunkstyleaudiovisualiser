#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <iostream>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>
#include "AudioCapture.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kWindowClass[] = L"CyberpsychosisOverlayWindow";
constexpr int kToggleClickThroughHotkey = 1;
constexpr int kToggleCelShadingHotkey = 2;
constexpr int kQuitHotkey = 3;
bool g_ClickThrough = true;
bool g_CelShading = false;
float g_HotkeyFeedback = 0.0f;

struct ShaderConstants {
    float time;
    float bassIntensity;
    float bassHit;
    float width;
    float height;
    UINT celShading;
    UINT clickThrough;
    float feedback;
    UINT beatIndex;
    UINT beatCount;
    float padding2;
    float padding3;
    float beatAges[8];
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return g_ClickThrough ? HTTRANSPARENT : HTCLIENT;
    case WM_HOTKEY:
        if (wParam == kToggleClickThroughHotkey) {
            g_ClickThrough = !g_ClickThrough;
            LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if (g_ClickThrough) exStyle |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
            else exStyle &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
            SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
            g_HotkeyFeedback = 1.0f;
        }
        else if (wParam == kToggleCelShadingHotkey) {
            g_CelShading = !g_CelShading;
            g_HotkeyFeedback = 1.0f;
        }
        else if (wParam == kQuitHotkey) PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostQuitMessage(0);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool CompileShader(const wchar_t* filename, const char* entryPoint, const char* profile,
                   ComPtr<ID3DBlob>& shader) {
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompileFromFile(
        filename, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint, profile,
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
    if (FAILED(result)) {
        std::cerr << "Shader compilation failed (0x" << std::hex << result << ")\n";
        if (errors) {
            std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),
                            errors->GetBufferSize());
            std::cerr << '\n';
        }
        return false;
    }
    return true;
}

class Overlay {
public:
    ~Overlay() {
        if (hwnd) {
            UnregisterHotKey(hwnd, kToggleClickThroughHotkey);
            UnregisterHotKey(hwnd, kToggleCelShadingHotkey);
            UnregisterHotKey(hwnd, kQuitHotkey);
        }
        if (hwnd) DestroyWindow(hwnd);
        if (comInitialized) CoUninitialize();
    }

    bool Initialize(HINSTANCE instance) {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult)) return ReportFailure("CoInitializeEx", comResult);
        comInitialized = true;

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kWindowClass;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&windowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            std::cerr << "RegisterClassExW failed: " << GetLastError() << '\n';
            return false;
        }

        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        const HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(monitor, &monitorInfo)) {
            std::cerr << "GetMonitorInfoW failed: " << GetLastError() << '\n';
            return false;
        }
        const RECT bounds = monitorInfo.rcMonitor;
        width = bounds.right - bounds.left;
        height = bounds.bottom - bounds.top;

        hwnd = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            kWindowClass, L"Cyberpsychosis Overlay", WS_POPUP,
            bounds.left, bounds.top, width, height, nullptr, nullptr, instance, nullptr);
        if (!hwnd) {
            std::cerr << "CreateWindowExW failed: " << GetLastError() << '\n';
            return false;
        }
        if (!SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)) {
            std::cerr << "SetLayeredWindowAttributes failed: " << GetLastError() << '\n';
            return false;
        }
        if (!RegisterHotKey(hwnd, kToggleClickThroughHotkey,
                            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'T') ||
            !RegisterHotKey(hwnd, kToggleCelShadingHotkey,
                            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C') ||
            !RegisterHotKey(hwnd, kQuitHotkey,
                            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q')) {
            const DWORD error = GetLastError();
            std::cerr << "Could not register overlay hotkeys: " << error << '\n';
            MessageBoxW(nullptr,
                        L"Could not register Ctrl+Alt+T/C/Q. Another application may "
                        L"already be using one of these shortcuts.",
                        L"Cyberpsychosis Overlay", MB_OK | MB_ICONWARNING);
            return false;
        }

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        HRESULT result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(result)) return ReportFailure("D3D11CreateDevice", result);

        ComPtr<IDXGIDevice> dxgiDevice;
        result = device.As(&dxgiDevice);
        if (FAILED(result)) return ReportFailure("Query IDXGIDevice", result);
        ComPtr<IDXGIAdapter> adapter;
        result = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(result)) return ReportFailure("IDXGIDevice::GetAdapter", result);
        ComPtr<IDXGIFactory2> factory;
        result = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(result)) return ReportFailure("Query IDXGIFactory2", result);

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = static_cast<UINT>(width);
        swapDesc.Height = static_cast<UINT>(height);
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        result = factory->CreateSwapChainForComposition(device.Get(), &swapDesc,
                                                        nullptr, &swapChain);
        if (FAILED(result)) return ReportFailure("CreateSwapChainForComposition", result);

        result = DCompositionCreateDevice(dxgiDevice.Get(),
                                          IID_PPV_ARGS(&compositionDevice));
        if (FAILED(result)) return ReportFailure("DCompositionCreateDevice", result);
        result = compositionDevice->CreateTargetForHwnd(hwnd, TRUE, &target);
        if (FAILED(result)) return ReportFailure("CreateTargetForHwnd", result);
        result = compositionDevice->CreateVisual(&visual);
        if (FAILED(result)) return ReportFailure("CreateVisual", result);
        result = visual->SetContent(swapChain.Get());
        if (FAILED(result)) return ReportFailure("SetContent", result);
        result = target->SetRoot(visual.Get());
        if (FAILED(result)) return ReportFailure("SetRoot", result);
        result = compositionDevice->Commit();
        if (FAILED(result)) return ReportFailure("DirectComposition Commit", result);

        ComPtr<ID3D11Texture2D> backBuffer;
        result = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(result)) return ReportFailure("Get swap-chain buffer", result);
        result = device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget);
        if (FAILED(result)) return ReportFailure("CreateRenderTargetView", result);

        ComPtr<ID3DBlob> vertexBytecode;
        ComPtr<ID3DBlob> pixelBytecode;
        if (!CompileShader(L"glitch.hlsl", "VSMain", "vs_5_0", vertexBytecode) ||
            !CompileShader(L"glitch.hlsl", "PSMain", "ps_5_0", pixelBytecode)) {
            return false;
        }
        result = device->CreateVertexShader(
            vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
            nullptr, &vertexShader);
        if (FAILED(result)) return ReportFailure("CreateVertexShader", result);
        result = device->CreatePixelShader(
            pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
            nullptr, &pixelShader);
        if (FAILED(result)) return ReportFailure("CreatePixelShader", result);

        D3D11_BUFFER_DESC constantsDesc{};
        constantsDesc.ByteWidth = sizeof(ShaderConstants);
        constantsDesc.Usage = D3D11_USAGE_DEFAULT;
        constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        result = device->CreateBuffer(&constantsDesc, nullptr, &constants);
        if (FAILED(result)) return ReportFailure("CreateBuffer", result);

        D3D11_BLEND_DESC blendDesc{};
        auto& blend = blendDesc.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D11_BLEND_ONE;
        blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D11_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        result = device->CreateBlendState(&blendDesc, &blendState);
        if (FAILED(result)) return ReportFailure("CreateBlendState", result);

        D3D11_RASTERIZER_DESC rasterizerDesc{};
        rasterizerDesc.FillMode = D3D11_FILL_SOLID;
        rasterizerDesc.CullMode = D3D11_CULL_NONE;
        rasterizerDesc.DepthClipEnable = TRUE;
        result = device->CreateRasterizerState(&rasterizerDesc, &rasterizerState);
        if (FAILED(result)) return ReportFailure("CreateRasterizerState", result);

        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED;
        if (g_ClickThrough) exStyle |= WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOPMOST, bounds.left, bounds.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }

    int Run(AudioAnalyzer& audio) {
        MSG message{};
        const auto start = GetTickCount64();
        bool running = true;
        while (running) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running) break;

            const auto frameStart = std::chrono::steady_clock::now();
            audio.ProcessAudioFrame();
            audio.CheckForDefaultDeviceChange();
            if (!UpdateRenderTarget()) return 1;
            g_HotkeyFeedback = (std::max)(0.0f, g_HotkeyFeedback - 1.0f / 45.0f);
            Render(static_cast<float>(GetTickCount64() - start) / 1000.0f,
                   audio.currentBassEnergy, audio.bassHit, audio.beatIndex,
                   audio.beatCount, audio.beatAges);
            const HRESULT result = swapChain->Present(0, 0);
            if (FAILED(result)) return ReportFailure("Present", result) ? 0 : 1;
            std::this_thread::sleep_until(frameStart + std::chrono::microseconds(11111));
        }
        return 0;
    }

private:
    bool UpdateRenderTarget() {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT result = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(result)) return ReportFailure("Get current swap-chain buffer", result);
        result = device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget);
        if (FAILED(result)) return ReportFailure("UpdateRenderTargetView", result);
        return true;
    }

    bool ReportFailure(const char* operation, HRESULT result) {
        std::cerr << operation << " failed: 0x" << std::hex << result << '\n';
        const std::wstring message =
            std::wstring(operation, operation + strlen(operation)) +
            L" failed (HRESULT " + std::to_wstring(
                static_cast<unsigned long>(result)) + L").";
        MessageBoxW(nullptr, message.c_str(), L"Cyberpsychosis Overlay",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    void Render(float time, float bassIntensity, float audioBassHit,
                UINT beatIndex, UINT beatCount, const float* beatAges) {
        const FLOAT transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->OMSetRenderTargets(1, renderTarget.GetAddressOf(), nullptr);
        context->ClearRenderTargetView(renderTarget.Get(), transparent);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        ShaderConstants data{time, bassIntensity, audioBassHit,
                             static_cast<float>(width), static_cast<float>(height),
                             g_CelShading ? 1u : 0u,
                             g_ClickThrough ? 1u : 0u,
                             g_HotkeyFeedback, beatIndex, beatCount,
                             0.0f, 0.0f, {}};
        std::copy(beatAges, beatAges + 8, data.beatAges);
        context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);

        context->OMSetBlendState(blendState.Get(), nullptr, 0xffffffff);
        context->RSSetState(rasterizerState.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->Draw(3, 0);
    }

    bool comInitialized = false;
    HWND hwnd = nullptr;
    int width = 0;
    int height = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<IDCompositionDevice> compositionDevice;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> blendState;
    ComPtr<ID3D11RasterizerState> rasterizerState;
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Overlay overlay;
    if (!overlay.Initialize(instance)) return 1;

    AudioAnalyzer audio;
    if (!audio.InitWASAPILoopback()) {
        std::cerr << "Audio loopback unavailable; overlay will run without audio reactivity\n";
    }
    return overlay.Run(audio);
}
