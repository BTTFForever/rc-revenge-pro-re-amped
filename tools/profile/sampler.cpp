#include <string>
// LD_PRELOAD sampler: SIGPROF on thread CPU time of the thread named by SAMPLE_THREAD.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <thread>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
static const int MAX=400000;
struct Rec{uint64_t rip,caller;};
static Rec* g_rec; static std::atomic<int> g_n{0};
static uint64_t g_lo,g_hi; // main executable text
static void onSig(int,siginfo_t*,void*uc){ auto c=(ucontext_t*)uc; int i=g_n.fetch_add(1,std::memory_order_relaxed); if(i>=MAX)return;
 uint64_t rip=c->uc_mcontext.gregs[REG_RIP], sp=c->uc_mcontext.gregs[REG_RSP], caller=0;
 if(rip<g_lo||rip>=g_hi){ for(uint64_t* p=(uint64_t*)sp; (uint64_t)p<sp+32768 && (((uint64_t)p)&0xfff)!=0xff8; ++p){ if(*p>=g_lo&&*p<g_hi){caller=*p;break;} } }
 else caller=rip;
 g_rec[i]={rip,caller}; }
static void dump(const char*out){ FILE*f=fopen(out,"w"); int n=g_n.load(); if(n>MAX)n=MAX; fprintf(f,"%d\n",n); for(int i=0;i<n;i++)fprintf(f,"%lx %lx\n",g_rec[i].rip,g_rec[i].caller);
 FILE*m=fopen("/proc/self/maps","r"); char l[1024]; fprintf(f,"MAPS\n"); while(fgets(l,sizeof l,m))fputs(l,f); fclose(m); fclose(f);}
__attribute__((constructor)) static void start(){ const char*name=getenv("SAMPLE_THREAD"); const char*out=getenv("SAMPLE_OUT"); if(!name||!out)return; { char e[512]=""; ssize_t k=readlink("/proc/self/exe",e,511); if(k<=0||!strstr(e,"rc_revenge_pro"))return; }
 g_rec=(Rec*)malloc(sizeof(Rec)*MAX);
 FILE*m=fopen("/proc/self/maps","r"); char l[1024]; if(fgets(l,sizeof l,m)){} 
 rewind(m); while(fgets(l,sizeof l,m)){ uint64_t a,b; char p[8]; char path[512]=""; if(sscanf(l,"%lx-%lx %4s %*x %*s %*d %511s",&a,&b,p,path)>=3 && p[2]=='x' && strstr(l,"rc_revenge_pro")){ if(!g_lo)g_lo=a; g_hi=b; } } fclose(m);
 std::string n=name; 
 std::thread([n=std::string(name),o=std::string(out)]{ char buf[64]; pid_t tid=0;
  for(;;){ DIR*d=opendir("/proc/self/task"); for(dirent*e;d&&(e=readdir(d));){ if(e->d_name[0]=='.')continue; char p[128]; snprintf(p,128,"/proc/self/task/%s/comm",e->d_name); FILE*f=fopen(p,"r"); if(f){ if(fgets(buf,64,f)){ buf[strcspn(buf,"\n")]=0; if(n==buf)tid=atoi(e->d_name);} fclose(f);} } if(d)closedir(d); if(tid)break; usleep(100000); }
  struct sigaction sa{}; sa.sa_sigaction=onSig; sa.sa_flags=SA_SIGINFO|SA_RESTART; sigaction(SIGPROF,&sa,nullptr);
  sleep(atoi(getenv("SAMPLE_DELAY")?:"12"));
  clockid_t cid=(-((clockid_t)tid)-1)*8+6; // CPUCLOCK_SCHED on a thread
  sigevent ev{}; ev.sigev_notify=SIGEV_THREAD_ID; ev.sigev_signo=SIGPROF; ev._sigev_un._tid=tid; timer_t t; if(timer_create(cid,&ev,&t)!=0){perror("timer_create");return;}
  itimerspec it{}; it.it_value.tv_nsec=1000000; it.it_interval.tv_nsec=1000000; timer_settime(t,0,&it,nullptr);
  for(;;){ sleep(2); dump(o.c_str()); }
 }).detach(); }
