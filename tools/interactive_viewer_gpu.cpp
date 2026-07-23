#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include <Eigen/Core>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "manifold/traverse.hpp"

#include "render/camera.hpp"
#include "render/image.hpp"
#include "render/persistent_gpu_renderer.hpp"
#include "render/scene.hpp"
#include "render/vulkan_context.hpp"

#include "demo_scene_common.hpp"

// GPU counterpart to tools/interactive_viewer.cpp (docs/DECISIONS.md #0011): identical Win32 +
// GDI window, fly camera, and input handling, but each frame dispatches
// render::PersistentGpuRenderer (Vulkan RT, full_scene.slang) instead of render::renderEmbree.
// Still headless-Vulkan + CPU readback + GDI blit -- no swapchain, same presentation path as the
// CPU viewer -- see persistent_gpu_renderer.hpp's header comment for why that's the right scope
// here (a debug view, not a product surface; a swapchain is a separate, larger task).
//
// Deliberately duplicates interactive_viewer.cpp's window/camera/input code rather than sharing
// it: that tool is already tested-by-use and green: docs/DECISIONS.md #0010; not worth the risk
// of touching it to extract a shared shell for a second, structurally-identical call site.

namespace {

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 720;

// Same resolution ladder as interactive_viewer.cpp. Measured on this dev machine (docs/
// DECISIONS.md #0011's follow-up note): per-frame cost is dominated by GPU dispatch+fence-wait
// latency that scales sublinearly with ray count (a large fixed per-submission floor, likely
// power-state ramp on a bursty single-dispatch-per-frame pattern, plus a smaller compute-scaling
// term) -- not CPU work, and not a hardware ceiling (RTX 4080 SUPER). Concretely: ~58 fps at
// 160x120, ~35 fps at 320x240, ~12 fps at 960x720. Top-of-ladder therefore is not "fluid" in
// practice, so default to a middle step like the CPU viewer does, for the same reason; '+' still
// reaches full window resolution for standing still and looking at detail.
constexpr int kResolutionSteps[][2] = {{160, 120}, {240, 180}, {320, 240}, {400, 300},
                                        {480, 360}, {640, 480}, {800, 600}, {960, 720}};
constexpr int kResolutionStepCount = sizeof(kResolutionSteps) / sizeof(kResolutionSteps[0]);
constexpr int kDefaultResolutionStep = 3; // 400x300: fluid, matches the CPU viewer's own default

constexpr double kMoveSpeed = 6.0; // world units / second
constexpr double kSprintMultiplier = 3.0;
constexpr double kMouseSensitivity = 0.0025; // radians / pixel
constexpr double kMaxPitch = 1.5;            // radians; stays short of +-pi/2 to avoid gimbal singularity

unsigned char toByte(double radiance) {
    double tonemapped = radiance / (1.0 + radiance); // Reinhard, matches tools/image_io.cpp
    double gammaCorrected = std::pow(std::clamp(tonemapped, 0.0, 1.0), 1.0 / 2.2);
    return static_cast<unsigned char>(std::lround(gammaCorrected * 255.0));
}

bool keyDown(int virtualKey) { return (GetAsyncKeyState(virtualKey) & 0x8000) != 0; }

// Same spectator camera as interactive_viewer.cpp -- see that file's header comment for why plain
// Euclidean yaw/pitch is correct between crossings, and applyPortalCrossing below (duplicated from
// there, same rationale as this file's other deliberate duplication) for how a crossing updates it.
struct FlyCamera {
    Eigen::Vector3d position{0, 0, -10};
    double yaw = 0.0;
    double pitch = 0.0;
    // Accumulated SE3 mapping the home chart into whatever chart this camera currently sits in --
    // see interactive_viewer.cpp's identical field for the full rationale (docs/DECISIONS.md
    // #0013's follow-up). Passed to PersistentGpuRenderer::render as cameraChart.
    manifold::SE3 chart = manifold::SE3::identity();

    Eigen::Vector3d forward() const {
        return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    }
};

// See interactive_viewer.cpp's identical helper for the full rationale: teleports position,
// orientation, and chart by the nearest portal's SE3 when this frame's movement segment crosses a
// disk, via manifold::stepThroughNearestPortal (the shared primitive), otherwise a plain Euclidean
// step.
void applyPortalCrossing(FlyCamera& camera, const std::vector<manifold::Portal>& portals,
                          const Eigen::Vector3d& displacement) {
    manifold::PortalHopResult hop = manifold::stepThroughNearestPortal(camera.position, displacement, portals, 1.0);
    if (!hop.crossed) {
        camera.position += displacement;
        return;
    }

    camera.position = hop.newOrigin + hop.newDirection * (1.0 - hop.distanceToHit);

    Eigen::Vector3d newForward = hop.hopTransform.applyToVector(camera.forward()).normalized();
    camera.yaw = std::atan2(newForward.x(), newForward.z());
    camera.pitch = std::asin(std::clamp(newForward.y(), -1.0, 1.0));

    camera.chart = hop.hopTransform * camera.chart;
}

struct AppState {
    bool running = true;
    bool mouseCaptured = false;
};

AppState* g_app = nullptr; // WndProc has no other way to reach the loop state; one window per process

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_LBUTTONDOWN:
            if (g_app != nullptr && !g_app->mouseCaptured) {
                g_app->mouseCaptured = true;
                ShowCursor(FALSE);
                SetCapture(hwnd);
            }
            return 0;
        case WM_KILLFOCUS:
            if (g_app != nullptr && g_app->mouseCaptured) {
                g_app->mouseCaptured = false;
                ShowCursor(TRUE);
                ReleaseCapture();
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int main() {
    render::Scene scene;
    tools::buildDemoScene(scene);

    // No CPU fallback in this tool (that's interactive_viewer.cpp) -- report cleanly and exit
    // rather than an unhandled-exception crash if this machine lacks Vulkan RT.
    std::unique_ptr<render::VulkanContext> context;
    std::unique_ptr<render::PersistentGpuRenderer> gpuRendererPtr;
    try {
        context = std::make_unique<render::VulkanContext>();
        gpuRendererPtr =
            std::make_unique<render::PersistentGpuRenderer>(*context, scene.portals(), scene.lights(), scene.meshes());
    } catch (const std::exception& e) {
        std::cerr << "Vulkan RT unavailable on this machine (" << e.what() << ") -- cannot run the GPU viewer.\n";
        return 1;
    }
    render::PersistentGpuRenderer& gpuRenderer = *gpuRendererPtr;

    const wchar_t* kClassName = L"PortalSimInteractiveViewerGpu";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    RECT windowRect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, kClassName, L"Portal Simulator -- interactive viewer (GPU)",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 windowRect.right - windowRect.left, windowRect.bottom - windowRect.top, nullptr,
                                 nullptr, wc.hInstance, nullptr);

    AppState app;
    g_app = &app;
    ShowWindow(hwnd, SW_SHOW);

    FlyCamera flyCamera;

    int resolutionStep = kDefaultResolutionStep;
    int renderWidth = kResolutionSteps[resolutionStep][0];
    int renderHeight = kResolutionSteps[resolutionStep][1];
    std::vector<unsigned char> pixels(static_cast<std::size_t>(renderWidth) * renderHeight * 3);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    auto applyResolution = [&]() {
        renderWidth = kResolutionSteps[resolutionStep][0];
        renderHeight = kResolutionSteps[resolutionStep][1];
        pixels.assign(static_cast<std::size_t>(renderWidth) * renderHeight * 3, 0);
        bmi.bmiHeader.biWidth = renderWidth;
        bmi.bmiHeader.biHeight = -renderHeight; // negative: top-down rows, matching render::Image's row 0 = top
        std::cout << "render resolution: " << renderWidth << "x" << renderHeight << "\n";
    };
    applyResolution();

    auto lastTime = std::chrono::steady_clock::now();
    double fpsAccumSeconds = 0.0;
    int fpsFrameCount = 0;
    bool prevEscapeDown = false;
    bool prevResUpDown = false;
    bool prevResDownDown = false;

    while (app.running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                app.running = false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!app.running) {
            break;
        }

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastTime).count();
        lastTime = now;
        dt = std::min(dt, 0.1); // clamp so a stall (window drag, breakpoint) doesn't teleport the camera

        bool escapeDown = keyDown(VK_ESCAPE);
        if (escapeDown && !prevEscapeDown) {
            if (app.mouseCaptured) {
                app.mouseCaptured = false;
                ShowCursor(TRUE);
                ReleaseCapture();
            } else {
                app.running = false;
            }
        }
        prevEscapeDown = escapeDown;

        bool resUpDown = keyDown(VK_OEM_PLUS) || keyDown(VK_ADD);
        bool resDownDown = keyDown(VK_OEM_MINUS) || keyDown(VK_SUBTRACT);
        if (resUpDown && !prevResUpDown && resolutionStep + 1 < kResolutionStepCount) {
            ++resolutionStep;
            applyResolution();
        } else if (resDownDown && !prevResDownDown && resolutionStep > 0) {
            --resolutionStep;
            applyResolution();
        }
        prevResUpDown = resUpDown;
        prevResDownDown = resDownDown;

        if (app.mouseCaptured) {
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            POINT centerScreen{(clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2};
            ClientToScreen(hwnd, &centerScreen);

            POINT cursor;
            GetCursorPos(&cursor);
            double dx = static_cast<double>(cursor.x - centerScreen.x);
            double dy = static_cast<double>(cursor.y - centerScreen.y);
            flyCamera.yaw -= dx * kMouseSensitivity;
            flyCamera.pitch = std::clamp(flyCamera.pitch - dy * kMouseSensitivity, -kMaxPitch, kMaxPitch);
            SetCursorPos(centerScreen.x, centerScreen.y);

            Eigen::Vector3d forward = flyCamera.forward();
            Eigen::Vector3d worldUp(0, 1, 0);
            Eigen::Vector3d right = forward.cross(worldUp).normalized();
            double speed = kMoveSpeed * (keyDown(VK_SHIFT) ? kSprintMultiplier : 1.0);

            Eigen::Vector3d move = Eigen::Vector3d::Zero();
            if (keyDown('W')) move += forward;
            if (keyDown('S')) move -= forward;
            if (keyDown('D')) move += right;
            if (keyDown('A')) move -= right;
            if (keyDown(VK_SPACE)) move += worldUp;
            if (keyDown(VK_CONTROL)) move -= worldUp;
            if (move.squaredNorm() > 0.0) {
                applyPortalCrossing(flyCamera, scene.portals(), move.normalized() * speed * dt);
            }
        }

        render::Camera camera =
            render::Camera::lookAt(flyCamera.position, flyCamera.position + flyCamera.forward(),
                                    Eigen::Vector3d(0, 1, 0), /*verticalFovRadians=*/1.0, renderWidth, renderHeight);
        // 2x2 supersampling: suppresses the crawling stair-step aliasing on the portal disk's
        // curved rim (docs/DECISIONS.md #0015), matching the CPU viewer. 4x the rays per frame; the
        // resolution ladder above is sized with that in mind.
        render::Image image = gpuRenderer.render(camera, flyCamera.chart, /*samplesPerAxis=*/2);

        // Dominated by std::pow (gamma correction) per channel per pixel -- measured as the single
        // largest per-frame cost in this viewer, larger than the GPU dispatch itself. Each pixel
        // writes only its own bytes, so parallelizing across rows is safe (same pattern already
        // used for renderEmbree/persistent_gpu_renderer's per-pixel loops).
        tbb::parallel_for(tbb::blocked_range<int>(0, renderHeight), [&](const tbb::blocked_range<int>& rows) {
            for (int y = rows.begin(); y != rows.end(); ++y) {
                for (int x = 0; x < renderWidth; ++x) {
                    const Eigen::Vector3d& c = image.at(x, y);
                    std::size_t idx = (static_cast<std::size_t>(y) * renderWidth + static_cast<std::size_t>(x)) * 3;
                    pixels[idx + 0] = toByte(c.z()); // BGR, matching BITMAPINFO's biBitCount=24 layout
                    pixels[idx + 1] = toByte(c.y());
                    pixels[idx + 2] = toByte(c.x());
                }
            }
        });

        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        HDC hdc = GetDC(hwnd);
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(hdc, 0, 0, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top, 0, 0,
                      renderWidth, renderHeight, pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, hdc);

        ++fpsFrameCount;
        fpsAccumSeconds += dt;
        if (fpsAccumSeconds >= 0.5) {
            std::wostringstream title;
            title << L"Portal Simulator -- interactive viewer (GPU)  |  " << renderWidth << L"x" << renderHeight
                  << L"  |  " << static_cast<int>(fpsFrameCount / fpsAccumSeconds) << L" fps ([+]/[-])  |  "
                  << (app.mouseCaptured ? L"WASD+mouse, Esc to release" : L"click to look around");
            SetWindowTextW(hwnd, title.str().c_str());
            std::cout << (fpsFrameCount / fpsAccumSeconds) << " fps\n";
            fpsAccumSeconds = 0.0;
            fpsFrameCount = 0;
        }
    }

    return 0;
}
