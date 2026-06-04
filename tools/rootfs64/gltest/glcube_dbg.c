/*
 * glcube.c — animated spinning cube, a second Win32 OpenGL test for
 * Boxedwine64's 3D path (sibling of gltri.c).
 *
 * Where gltri.c clears to orange and draws ONE static triangle, this draws a
 * continuously rotating, depth-tested, multi-color cube. The motion is the
 * point: a single rendered frame can be faked by the GDI/software present path,
 * but a smoothly SPINNING cube proves the guest -> winex11.drv -> opengl32 ->
 * guest libGL.so.1 -> host-GL bridge is actually executing gl* calls every
 * frame (matrix transforms, GL_DEPTH_TEST, per-face color) and swapping. It
 * also exercises GL state gltri never touches: glEnable(GL_DEPTH_TEST),
 * gluPerspective-style glFrustum projection, glRotatef/glTranslatef modelview.
 *
 * Build with mingw-w64 (same toolchain as gltri.exe):
 *
 *   x86_64-w64-mingw32-gcc -O2 -o glcube.exe glcube.c -lopengl32 -lgdi32 -luser32
 */

#include <windows.h>
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>
#include <io.h>
#define DBG(msg) do{ fprintf(stderr,"GLCUBE_DBG: " msg "\n"); fflush(stderr);}while(0)

static void rawlog(const char* s){ int n=0; while(s[n])n++; _write(2,s,n);} 
static void rawlogm(const char* p, unsigned v){ char b[64]; int i=0; while(p[i]){b[i]=p[i];i++;} const char* hex="0123456789abcdef"; b[i++]='0';b[i++]='x'; for(int s=28;s>=0;s-=4) b[i++]=hex[(v>>s)&0xf]; b[i++]='\n'; _write(2,b,i);} 
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    rawlogm("GLCUBE_WND: msg=", m);
    if (m == WM_CLOSE || m == WM_DESTROY) { rawlog("GLCUBE_WND: CLOSE/DESTROY -> quit\n"); PostQuitMessage(0); return 0; }
    if (m == WM_PAINT)    { PAINTSTRUCT ps; BeginPaint(h,&ps); EndPaint(h,&ps); return 0; } /* validate -> stop the paint storm */
    if (m == WM_ERASEBKGND) return 1; /* we paint via GL; claim erased */
    return DefWindowProc(h, m, w, l);
}

/* The 6 faces of a unit cube, each a distinct color. */
static void drawCube(void) {
    glBegin(GL_QUADS);
        glColor3f(1.0f, 0.0f, 0.0f);                       /* +X red    */
        glVertex3f( 1,-1,-1); glVertex3f( 1, 1,-1);
        glVertex3f( 1, 1, 1); glVertex3f( 1,-1, 1);
        glColor3f(0.0f, 1.0f, 0.0f);                       /* -X green  */
        glVertex3f(-1,-1,-1); glVertex3f(-1,-1, 1);
        glVertex3f(-1, 1, 1); glVertex3f(-1, 1,-1);
        glColor3f(0.0f, 0.0f, 1.0f);                       /* +Y blue   */
        glVertex3f(-1, 1,-1); glVertex3f(-1, 1, 1);
        glVertex3f( 1, 1, 1); glVertex3f( 1, 1,-1);
        glColor3f(1.0f, 1.0f, 0.0f);                       /* -Y yellow */
        glVertex3f(-1,-1,-1); glVertex3f( 1,-1,-1);
        glVertex3f( 1,-1, 1); glVertex3f(-1,-1, 1);
        glColor3f(1.0f, 0.0f, 1.0f);                       /* +Z magenta*/
        glVertex3f(-1,-1, 1); glVertex3f( 1,-1, 1);
        glVertex3f( 1, 1, 1); glVertex3f(-1, 1, 1);
        glColor3f(0.0f, 1.0f, 1.0f);                       /* -Z cyan   */
        glVertex3f(-1,-1,-1); glVertex3f(-1, 1,-1);
        glVertex3f( 1, 1,-1); glVertex3f( 1,-1,-1);
    glEnd();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "glcube";
    RegisterClassA(&wc); DBG("registered class");

    const int W = 480, H = 360;
    HWND hwnd = CreateWindowA("glcube", "Boxedwine64 GL spinning cube",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, W, H,
                              0, 0, hInst, 0);
    DBG("window created"); HDC hdc = GetDC(hwnd); DBG("got DC");

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(hdc, &pfd); fprintf(stderr,"GLCUBE_DBG: ChoosePixelFormat -> pf=%d\n", pf); fflush(stderr);
    BOOL spf = SetPixelFormat(hdc, pf, &pfd); fprintf(stderr,"GLCUBE_DBG: SetPixelFormat -> %d\n", spf); fflush(stderr);

    DBG("before wglCreateContext"); HGLRC rc = wglCreateContext(hdc); fprintf(stderr,"GLCUBE_DBG: wglCreateContext -> rc=%p\n",(void*)rc); fflush(stderr);
    BOOL mc = wglMakeCurrent(hdc, rc); fprintf(stderr,"GLCUBE_DBG: wglMakeCurrent -> %d\n", mc); fflush(stderr);

    DBG("entering GL setup"); glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);

    /* Perspective projection (glFrustum, avoids depending on glu). */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
        double aspect = (double)W / (double)H;
        double f = 0.5;   /* near-plane half-height */
        glFrustum(-f * aspect, f * aspect, -f, f, 1.0, 20.0);
    }
    glMatrixMode(GL_MODELVIEW);

    MSG msg;
    float angle = 0.0f;
    int frames = 0;
    int iter = 0;
    DBG("entering render loop"); for (;;) {
        fprintf(stderr,"GLCUBE_DBG: iter=%d before PeekMessage drain\n",iter); fflush(stderr);
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { DBG("got WM_QUIT -> exiting"); goto done; } else { fprintf(stderr,"GLCUBE_DBG: msg=0x%x\n",(unsigned)msg.message); fflush(stderr); }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        fprintf(stderr,"GLCUBE_DBG: iter=%d PeekMessage drained, calling glClearColor\n",iter); fflush(stderr);
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);   /* dark slate background */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -6.0f);
        glRotatef(angle,        1.0f, 0.0f, 0.0f);
        glRotatef(angle * 0.7f, 0.0f, 1.0f, 0.0f);
        drawCube();

        fprintf(stderr,"GLCUBE_DBG: iter=%d drew, SwapBuffers\n",iter); fflush(stderr);
        SwapBuffers(hdc);
        angle += 1.5f;
        if (angle >= 360.0f) angle -= 360.0f;
        iter++; if (++frames > 100000) break;   /* safety cap */
        Sleep(16);
    }
done:
    wglMakeCurrent(0, 0);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, hdc);
    return 0;
}
