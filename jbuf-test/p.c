#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "eloop.h"

#define PACKET_BUF_SIZE 640

eloop_t *loop;
event_t *wtimer;
event_t *stimer;

int gfd = 0;
int num = 30;
int count = 0;
void s_callback(eloop_t *loop,event_t *evt,long fd,void *arg)
{
  printf("send %d packets in last second\n",count);
  count = 0;
}

void w_callback(eloop_t *loop,event_t *evt,long fd,void *arg)
{
  char buf[PACKET_BUF_SIZE] = {0};
  write(gfd,buf,sizeof(buf));
  count++;
  num--;

  if(num){
    int j = rand()%10;//0~99ms jitter
    e_event_mod(loop,wtimer,20+j);
  }else{
    num = rand()%1000+25;//make new packets
    int s = rand()%30+1; //sleep 0.5~15s
    e_event_mod(loop,wtimer,500*s);
    count = 0;
  }
}

int main()
{
  srand(time(0));
  loop = e_loop_new();
  gfd = open("/tmp/pc_fifo",O_WRONLY);
  printf("gfd:%d\n",gfd);

  wtimer = e_event_new(E_TIMER,20,w_callback,NULL);
  stimer = e_event_new(E_TIMER,1000,s_callback,NULL);

  e_event_add(loop,wtimer);
  e_event_add(loop,stimer);
  e_loop_run(loop);
  return 0;
}
