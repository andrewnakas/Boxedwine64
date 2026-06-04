#include <windows.h>
#include <stdio.h>
#include <io.h>
static void rawm(const char*p,unsigned v){char b[64];int i=0;while(p[i]){b[i]=p[i];i++;}const char*h="0123456789abcdef";b[i++]='0';b[i++]='x';for(int s=28;s>=0;s-=4)b[i++]=h[(v>>s)&0xf];b[i++]='\n';_write(2,b,i);}
static LRESULT CALLBACK WP(HWND h,UINT m,WPARAM w,LPARAM l){ rawm("WND msg=",m); if(m==WM_CLOSE||m==WM_DESTROY){PostQuitMessage(0);return 0;} return DefWindowProc(h,m,w,l);}
int WINAPI WinMain(HINSTANCE hi,HINSTANCE hp,LPSTR c,int s){
  WNDCLASSA wc={0}; wc.lpfnWndProc=WP; wc.hInstance=hi; wc.lpszClassName="ml"; RegisterClassA(&wc);
  HWND w=CreateWindowA("ml","msgloop",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,400,300,0,0,hi,0);
  rawm("created hwnd=",(unsigned)(UINT_PTR)w);
  MSG msg; int it=0;
  for(;;){ rawm("iter=",it);
    while(PeekMessageA(&msg,0,0,0,PM_REMOVE)){ if(msg.message==WM_QUIT){rawm("QUIT=",0);goto d;} TranslateMessage(&msg); DispatchMessageA(&msg);}
    rawm("drained,sleeping it=",it); it++; Sleep(100);
  }
 d: return 0;
}
