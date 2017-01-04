#include <stdio.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include "eloop.h"

#define SECS_TO_COUNT 100//线程创建完毕后统计多少秒

static time_t g_base;
static int g_count[SECS_TO_COUNT];
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int fix; //for fix alogrithm
static int begin,end;//for random alogrithm
static void (*g_func)(eloop_t *loop,event_t *evt);

void alogrithm_fix_time(eloop_t *loop,event_t *evt)
{
  e_mod_timer_event(loop,evt,fix,0);
}

void alogrithm_rand_time(eloop_t *loop,event_t *evt)
{
  e_mod_timer_event(loop,evt,rand()%(end-begin+1)+begin,0);
}

void alogrithm_multiple_time(eloop_t *loop,event_t *evt)
{
  static int i=0,v[4]={1,2,4,8};
  e_mod_timer_event(loop,evt,v[i],0);
  i=(i+1)%4;
}

void print_and_leave()
{
  int i=0;
  printf("------------------------------------------------RESULT---------------------------------------------\n");
  for(i=0;i<SECS_TO_COUNT;i++)
    printf("%d ",g_count[i]);
  printf("\n");
  printf("---------------------------------------------------------------------------------------------------\n");
  exit(0);
}

void timer_proc(event_t *evt)
{
  eloop_t *loop = e_get_event_arg(evt);
  pthread_mutex_lock(&g_mutex);
  if(g_base == 0){
    goto end;
  }

  int index = (time(0) - g_base);

  if(index >= SECS_TO_COUNT)
    print_and_leave();

  g_count[index]++;
  //printf("thread %lu increase the number in %d second\n",pthread_self(),index);

 end:
  g_func(loop,evt);//modify timer
  pthread_mutex_unlock(&g_mutex);
}

void* work_thread()
{
  eloop_t loop;
	event_t timer;

	e_init(&loop);

	e_init_timer_event(&timer,0,0,timer_proc,&loop);

  e_add_event(&loop,&timer);

	e_dispatch_event(&loop);
}

void usage()
{
  printf("usage:\n");
  printf("\t./tt c f N   (c-thread count,f N - use fixed alogrithm and the interval is N seconds)\n");
  printf("\t./tt c r B E (c-thread count,r B E - use random alogrithm and the random seconds is between B and E,E must greater than B)\n");
  printf("\t./tt c m     (c-thread count,m - multiple alogrithm)\n");
  exit(0);
}

int main(int argc,char** argv)
{
  int i=0,n = 10;
  pthread_t id;

  if(argc == 1){
    usage();
  }else if(*argv[2] == 'f'){
    fix = atoi(argv[3]);
    g_func = alogrithm_fix_time;
  }else if(*argv[2] == 'r'){
    begin = atoi(argv[3]);
    end = atoi(argv[4]);
    g_func = alogrithm_rand_time;
  }else if(*argv[2] == 'm'){
    g_func = alogrithm_multiple_time;
  }

  n = atoi(argv[1]);

  srand(time(0));

  //create threads in random time
  //printf("create threads randomly\n");
  while(1){
    if(rand()%100==0) {
      pthread_create(&id,NULL,work_thread,NULL);
      //printf("create thread(%d) tid:%lu succeeded\n",i,id);
      usleep(rand()%500000);
      i++;
    }

    if(i==n)
      break;
  }
  //printf("threads created over\n");

  pthread_mutex_lock(&g_mutex);
  g_base = time(0);
  pthread_mutex_unlock(&g_mutex);

  while(1){
    sleep(1);
  }
}
