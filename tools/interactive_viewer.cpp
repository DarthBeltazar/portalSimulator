#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <vector>

#include <Eigen/Core>

#include "render/camera.hpp"
#include "render/image.hpp"
#include "render/renderer.hpp"
#include "render/scene.hpp"

#include "demo_scene_common.hpp"

// Real-time fly-through viewer for the shared demo scene (tools/demo_scene_common), for looking
// at the render interactively instead of only inspecting static BMPs (tools/demo_scene.cpp).
// Windows-only (raw Win32 + GDI, no new dependency) and CPU-only: it calls render::renderEmbree
// once per frame, same as the static demo -- render_core's reference renderer is a plain
// per-pixel loop (docs/phase2-rendering.md §4), not built for real time, so the render
// resolution below is kept modest and stretched up to the window with GDI. The Vulkan RT path
// (render_vulkan) isn't wired in here: full_scene_gpu.cpp rebuilds its acceleration structure
// from scratch on every call (its own header says so), which is fine for a one-shot comparison
// image but not for a per-frame interactive loop.

namespace {

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 720;

// Render resolution steps (4:3, matching the window above so the display scale factor stays
// sane at every step). render_core's renderEmbree is a single-threaded per-pixel loop
// (docs/phase2-rendering.md §4) with no acceleration-structure-reuse across frames beyond what
// Embree itself caches internally, so cost scales ~linearly with pixel count; the highest step
// (matching the window 1:1) is a deliberate slideshow-quality option for standing still and
// looking at detail, not for flying around -- '-' steps back down for that.
constexpr int kResolutionSteps[][2] = {{160, 120}, {240, 180}, {320, 240}, {400, 300},
                                        {480, 360}, {640, 480}, {800, 600}, {960, 720}};
constexpr int kResolutionStepCount = sizeof(kResolutionSteps) / sizeof(kResolutionSteps[0]);
constexpr int kDefaultResolutionStep = 3; // 400x300: noticeably crisper than 320x240, still fluid

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

// Spherical yaw/pitch around a fixed world up axis (no roll) -- a standard noclip/spectator
// camera, not this project's manifold charts: this tool only ever renders from a single flat
// chart (the camera never itself crosses a portal), so ordinary Euclidean orbiting is correct
// here and doesn't need manifold::traverse.
struct FlyCamera {
    Eigen::Vector3d position{0, 0, -10};
    double yaw = 0.0;
    double pitch = 0.0;

    Eigen::Vector3d forward() const {
        return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    }
};

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

    const wchar_t* kClassName = L"PortalSimInteractiveViewer";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    RECT windowRect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, kClassName, L"Portal Simulator -- interactive viewer",
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

        // Esc: release the mouse if captured (first press), else quit -- edge-triggered so
        // holding the key doesn't toggle every frame.
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

        // '+'/'-' (main row or numpad): step render resolution up/down, edge-triggered same as
        // Esc above. Works regardless of mouse capture, since it's not a look/move control.
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
            // render::Camera::rayDirectionForPixel (src/render/camera.cpp) builds screen-right
            // from forward.cross(up) -- the same formula used for `right` below -- and that
            // vector points opposite to the direction increasing yaw turns `forward` toward, so
            // yaw must decrease (not increase) for rightward mouse motion to pan the view toward
            // screen-right instead of screen-left.
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
                flyCamera.position += move.normalized() * speed * dt;
            }
        }

        render::Camera camera =
            render::Camera::lookAt(flyCamera.position, flyCamera.position + flyCamera.forward(),
                                    Eigen::Vector3d(0, 1, 0), /*verticalFovRadians=*/1.0, renderWidth, renderHeight);
        render::Image image = render::renderEmbree(scene, camera);

        for (int y = 0; y < renderHeight; ++y) {
            for (int x = 0; x < renderWidth; ++x) {
                const Eigen::Vector3d& c = image.at(x, y);
                std::size_t idx = (static_cast<std::size_t>(y) * renderWidth + static_cast<std::size_t>(x)) * 3;
                pixels[idx + 0] = toByte(c.z()); // BGR, matching BITMAPINFO's biBitCount=24 layout
                pixels[idx + 1] = toByte(c.y());
                pixels[idx + 2] = toByte(c.x());
            }
        }

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
            title << L"Portal Simulator -- interactive viewer  |  " << renderWidth << L"x" << renderHeight
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
