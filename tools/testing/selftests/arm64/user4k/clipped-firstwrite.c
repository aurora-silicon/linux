// SPDX-License-Identifier: GPL-2.0-only
/* Real clipped-window batching, zeroing, reuse and physical backing density. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#define LEAF 4096UL
#define NATIVE 16384UL
#define GROUPS 256UL
#define STRIDE (4 * NATIVE)
#define PFN_MASK ((UINT64_C(1)<<55)-1)
#define PRESENT (UINT64_C(1)<<63)
#define SWAPPED (UINT64_C(1)<<62)
static volatile sig_atomic_t child;
static void stop(int sig) { if(child>0)kill(child,SIGKILL);_exit(128+sig); }
static bool visible(void)
{
 FILE *f=fopen("/proc/self/status","re");char line[256];unsigned long long caps=0;
 if(!f)return false;
 while(fgets(line,sizeof(line),f))if(sscanf(line,"CapEff: %llx",&caps)==1)break;
 fclose(f);return !!(caps&(1ULL<<21));
}
static bool entries(int fd,unsigned char *base,unsigned n,uint64_t *out)
{
 if(pread(fd,out,n*8,(uintptr_t)base/LEAF*8)!=(ssize_t)n*8)return false;
 for(unsigned i=0;i<n;i++)if(!(out[i]&PRESENT)||(out[i]&SWAPPED)||!(out[i]&PFN_MASK))return false;
 return true;
}
static int compare(const void *a,const void *b)
{
 uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);
}
static unsigned char tag(unsigned group,unsigned slot,unsigned round)
{
 return (unsigned char)(1+(group*31+slot*17+round*73)%251);
}
static bool touch_window(int fd,unsigned char *p,unsigned n,unsigned group,unsigned round)
{
 uint64_t map[4];
 *(volatile unsigned char *)p=tag(group,0,round);
 /* Metadata assertion precedes any neighbor read that could fault it in. */
 if(!entries(fd,p,n,map)){fprintf(stderr,"FAIL missing prefault group=%u n=%u round=%u\n",group,n,round);return false;}
 for(unsigned slot=0;slot<n;slot++){
  for(size_t b=0;b<LEAF;b++){
   unsigned char expected=slot==0&&b==0?tag(group,0,round):0;
   if(p[slot*LEAF+b]!=expected){fprintf(stderr,"FAIL zero group=%u slot=%u byte=%zu\n",group,slot,b);return false;}
  }
  memset(p+slot*LEAF,tag(group,slot,round),LEAF);
 }
 return true;
}
static bool check_window(unsigned char *p,unsigned n,unsigned group,unsigned round)
{
 for(unsigned slot=0;slot<n;slot++)for(size_t b=0;b<LEAF;b++)
  if(p[slot*LEAF+b]!=tag(group,slot,round))return false;
 return true;
}
static int exercise(unsigned count,unsigned quarter,bool mix)
{
 size_t total=GROUPS*STRIDE+NATIVE;
 unsigned char *mapping=mmap(NULL,total,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(mapping==MAP_FAILED)return 2;
 unsigned char *base=(void *)(((uintptr_t)mapping+NATIVE-1)&~(NATIVE-1));
 int fd=open("/proc/self/pagemap",O_RDONLY|O_CLOEXEC);
 if(fd<0)return 2;
 for(unsigned g=0;g<GROUPS;g++){
  if(mprotect(base+g*STRIDE+quarter*LEAF,count*LEAF,PROT_READ|PROT_WRITE))return 2;
  if(mix&&mprotect(base+g*STRIDE+2*NATIVE,LEAF,PROT_READ|PROT_WRITE))return 2;
 }
 for(unsigned round=0;round<2;round++){
  /* Populate all count3 requests before count1: catches insufficient-count
   * duplicate-supply rejection and tests eventual reuse of leftover quarters. */
  for(unsigned g=0;g<GROUPS;g++)
   if(!touch_window(fd,base+g*STRIDE+quarter*LEAF,count,g,round))return 3;
  if(mix)for(unsigned g=0;g<GROUPS;g++)
   if(!touch_window(fd,base+g*STRIDE+2*NATIVE,1,g+GROUPS,round))return 3;
  uint64_t native[GROUPS*4],map[4];size_t used=0;
  for(unsigned g=0;g<GROUPS;g++){
   unsigned char *p=base+g*STRIDE+quarter*LEAF;
   if(!check_window(p,count,g,round)||!entries(fd,p,count,map))return 4;
   for(unsigned j=0;j<count;j++)native[used++]=(map[j]&PFN_MASK)>>2;
   if(mix){
    p=base+g*STRIDE+2*NATIVE;
    if(!check_window(p,1,g+GROUPS,round)||!entries(fd,p,1,map))return 4;
    native[used++]=(map[0]&PFN_MASK)>>2;
   }
  }
  qsort(native,used,sizeof(*native),compare);size_t unique=0;
  for(size_t i=0;i<used;i++)if(!i||native[i]!=native[i-1])unique++;
  size_t useful=used*LEAF,backing=unique*NATIVE;
  /* count3 batches are indivisible within one owner; alone16K/12K is natural.
   * <=1MiB slack bounds16 pool shards, background COW and bucket imbalance. */
  size_t target=(count==3&&!mix)?GROUPS*NATIVE:useful;
  printf("{\"count\":%u,\"quarter\":%u,\"mixed_3_plus_1\":%s,\"round\":%u,\"useful_bytes\":%zu,\"native_bytes\":%zu,\"limit_bytes\":%zu,\"all_neighbors_prefaulted\":true,\"zero_content_ok\":true}\n",count,quarter,mix?"true":"false",round,useful,backing,target+(1UL<<20));
  if(backing>target+(1UL<<20)){fprintf(stderr,"FAIL physical density\n");return 5;}
  if(!round)for(unsigned g=0;g<GROUPS;g++){
   if(madvise(base+g*STRIDE+quarter*LEAF,count*LEAF,MADV_DONTNEED))return 2;
   if(mix&&madvise(base+g*STRIDE+2*NATIVE,LEAF,MADV_DONTNEED))return 2;
  }
 }
 close(fd);munmap(mapping,total);return 0;
}
static bool run(unsigned count,unsigned quarter,bool mix)
{
 pid_t pid=fork();int status;
 if(pid<0)return false;
 if(!pid){child=0;alarm(8);_exit(exercise(count,quarter,mix));}
 child=pid;pid_t got;
 do{got=waitpid(pid,&status,0);}while(got<0&&errno==EINTR);
 child=0;
 if(got!=pid||!WIFEXITED(status)||WEXITSTATUS(status)){fprintf(stderr,"FAIL arm count=%u quarter=%u mix=%d status=%d\n",count,quarter,mix,got==pid?status:-1);return false;}
 return true;
}
int main(int argc,char **argv)
{
 if(argc==2&&!strcmp(argv[1],"--selftest")){
  uint64_t x[]={7,2,7,1};qsort(x,4,sizeof(*x),compare);
  if(x[0]!=1||x[1]!=2||x[2]!=7||x[3]!=7)return 1;
  for(unsigned g=0;g<GROUPS;g++)for(unsigned q=0;q<4;q++)
   if(!tag(g,q,0)||tag(g,q,0)==tag(g,q,1))return 1;
  puts("PASS tag and PFN sort selftest");return 0;
 }
 if(argc!=2||strcmp(argv[1],"--native-16k")||getauxval(AT_PAGESZ)!=LEAF||!visible()){
  fputs("Requires established4K launcher/native16K kernel and visiblePFNs\n",stderr);return 77;
 }
 signal(SIGALRM,stop);signal(SIGTERM,stop);signal(SIGINT,stop);setvbuf(stdout,NULL,_IONBF,0);alarm(40);
 unsigned arms=0;
 for(unsigned n=1;n<=3;n++)for(unsigned q=0;q+n<=4;q++){if(!run(n,q,false))return 1;arms++;}
 if(!run(3,0,true)||!run(4,0,false))return 1;
 printf("PASS %u arms, two rounds each; clipped batching/density/zeroing/reuse and dense control\n",arms+2);alarm(0);return 0;
}
