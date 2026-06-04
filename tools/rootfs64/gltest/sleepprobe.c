#include <windows.h>
#include <stdio.h>
#include <io.h>

/* Raw stderr writer (no CRT stdio dependence), prints "<label><unsigned hex>\n". */
static void rawm(const char*p,unsigned v){
  char b[80];int i=0;
  while(p[i]){b[i]=p[i];i++;}
  const char*h="0123456789abcdef";
  b[i++]='0';b[i++]='x';
  for(int s=28;s>=0;s-=4)b[i++]=h[(v>>s)&0xf];
  b[i++]='\n';
  _write(2,b,i);
}

/* Print a decimal unsigned with a label, for human-readable ms values. */
static void rawd(const char*p,unsigned v){
  char b[80];int i=0;
  while(p[i]){b[i]=p[i];i++;}
  char num[16];int n=0;
  if(v==0){num[n++]='0';}
  while(v){num[n++]='0'+(v%10);v/=10;}
  while(n)b[i++]=num[--n];
  b[i++]='\n';
  _write(2,b,i);
}

/*
 * Wall-clock Sleep probe.
 *
 * Purpose: separate "server-side select-with-timeout is broken (Sleep returns
 * instantly / busy-spins)" from "X-socket readiness mismatch in the message
 * pump". This probe has NO window and NO message pump at all -- it is a pure
 * console process whose ONLY blocking primitive is Sleep(). If Sleep() honors
 * its timeout, each measured delta will be ~= the requested ms. If the
 * wineserver select-with-timeout returns immediately, the deltas will be ~0
 * and the whole run will finish in well under a second.
 *
 * We measure with GetTickCount() (a guest-side WinAPI tick, itself ultimately
 * backed by the host clock, NOT by the wineserver select) AND we rely on the
 * outer harness wall-clock: a correct run takes ~ (100+250+500+1000+2000)=3850ms
 * of real time; a broken run returns near-instantly.
 */
int main(void){
  static const unsigned durs[]={100,250,500,1000,2000};
  rawm("SLEEPPROBE: start ",0);

  unsigned t0all=GetTickCount();
  for(int i=0;i<5;i++){
    unsigned want=durs[i];
    unsigned a=GetTickCount();
    Sleep(want);
    unsigned b=GetTickCount();
    unsigned got=b-a;
    rawd("SLEEPPROBE: want_ms=",want);
    rawd("SLEEPPROBE: got_ms =",got);
  }
  unsigned t1all=GetTickCount();
  rawd("SLEEPPROBE: total_ms=",t1all-t0all);
  rawm("SLEEPPROBE: done ",0);
  return 0;
}
