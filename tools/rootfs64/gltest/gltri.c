/*
 * gltri.c — minimal Win32 OpenGL "first light" test for Boxedwine64's 3D path.
 *
 * Opens a window, creates a legacy WGL OpenGL context, and renders a colored
 * triangle on an orange background, swapping buffers in a loop. Used to prove
 * the wine64 -> winex11.drv -> opengl32 -> guest libGL.so.1 -> host-GL bridge
 * pipeline end to end (a recognizable frame should appear in the Boxedwine
 * window). Build with mingw-w64:
 *
 *   x86_64-w64-mingw32-gcc -O2 -o gltri.exe gltri.c -lopengl32 -lgdi32 -luser32
 */

#include <windows.h>
#include <GL/gl.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "gltri";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA("gltri", "Boxedwine64 GL first light",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, 480, 360,
                              0, 0, hInst, 0);
    HDC hdc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(hdc, &pfd);
    SetPixelFormat(hdc, pf, &pfd);

    HGLRC rc = wglCreateContext(hdc);
    wglMakeCurrent(hdc, rc);

    glViewport(0, 0, 480, 360);

    MSG msg;
    int frames = 0;
    for (;;) {
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        glClearColor(1.0f, 0.5f, 0.0f, 1.0f);   // orange — unmistakable
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
            glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(-0.6f, -0.5f);
            glColor3f(0.0f, 1.0f, 0.0f); glVertex2f( 0.6f, -0.5f);
            glColor3f(0.0f, 0.0f, 1.0f); glVertex2f( 0.0f,  0.6f);
        glEnd();
        SwapBuffers(hdc);
        if (++frames > 100000) break;   // safety cap so it can't run forever
        Sleep(16);
    }
done:
    wglMakeCurrent(0, 0);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, hdc);
    return 0;
}
