#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include <sched.h>
#include <sys/resource.h>

#include <stdio.h>
#include "thread_internal.h"

static struct _pthread_descr_struct threads[PTHREAD_THREADS_MAX];
static int _max_used_thread_id=1;
pthread_once_t __thread_inited;

static struct _pthread_fastlock __manager_thread_signal_lock = {0};
static _pthread_descr __manager_thread_signal_lock_owner = 0;
static struct _pthread_fastlock __manager_thread_data_lock = {1};
static struct _pthread_fastlock __manager_thread_data_go_lock = {1};

//#define DEBUG

/* find thread */
int __find_thread_id(int pid)
{
  register int i;
  for (i=0; i<_max_used_thread_id; i++)
    if (threads[i].pid==pid)
      return i;
  return -1;
}

/* get thread */
_pthread_descr __get_thread_struct(int id)
{
  return threads+id;
}

/* thread errno location */
int *__errno_location(void)
{
  int id=0;
  if (__thread_inited) id=__find_thread_id(getpid());
  if (id<0)
    return 0;
  else
    return &threads[id].errno;
}

/* thread self */
_pthread_descr __thread_self()
{
  register int i=__find_thread_id(getpid());
  if (i<0)
    return 0;
  else
    return threads+i;
}

/* allocate a thread slot */
_pthread_descr __thread_get_free()
{
  int i;

  for (i=0; i<PTHREAD_THREADS_MAX; i++)
    if (threads[i].pid==0)
      if (!(__testandset(&threads[i].pid))) { /* atomic get slot :) */
	if (i>=_max_used_thread_id) _max_used_thread_id=i+1;
	return threads+i;
      }

  return 0;
}

/* sleep a little (reschedule for this time) */
void __thread_wait_some_time()
{
  struct timespec reg;
  reg.tv_sec=0;
  reg.tv_nsec=SPIN_SLEEP_DURATION;
  nanosleep(&reg,0);
}

/* cleanup a thread struct */
void __thread_cleanup(_pthread_descr th)
{
  /* lib provided stack should be freed */
  if (!(th->userstack)) free(th->stack_begin);

  /* hopefully a deadlock prevention */
  if (th->manager_lock) {
    if (th->manager_lock->__spinlock)  {
      if (__manager_thread_signal_lock_owner==th) {
	/* Ups thread exited in signal_manager_thread and still has the lock */
	__pthread_unlock(th->manager_lock);
      }
    }
  }

  /* an other thread has joined this on */
  if (th->joined) {
    th->joined->retval=th->retval;
    th->joined->join=0;
    th->joined=0;
  }
  th->pid=0;
}

/* SIGHUP handler (thread cnacel) PTHREAD_CANCEL_ASYNCHRONOUS */
static void __thread_cancel_handler(int sig)
{
  _pthread_descr this;
  this = __thread_self();
  this->canceled=1;
  if (this->canceltype==PTHREAD_CANCEL_ASYNCHRONOUS)
    pthread_exit(PTHREAD_CANCELED);
  signal( SIGHUP, __thread_cancel_handler );
}

/* kill ALL threads / other then prime task and manager thread */
static void __kill_all_threads()
{
  int i;

  for (i=2; i<_max_used_thread_id; i++) {
    if (threads[i].pid>1) {
#ifdef DEBUG
      printf("CANCEL ! %d\n",threads[i].pid);
#endif
      threads[i].canceled=1;
      kill(threads[i].pid, SIGHUP);	/* cancel thread */
    }
  }

  __thread_wait_some_time();

  for (i=2; i<_max_used_thread_id; i++) {
    if (threads[i].pid>1) {
#ifdef DEBUG
      printf("KILL ! %d\n",threads[i].pid);
#endif
      kill(threads[i].pid, SIGTERM);	/* KILL thread */
    }
  }

}

/* support for manager */
static void *__mthread_starter(void *arg)
{
  _pthread_descr td = (_pthread_descr)arg;
  int limit = td->stack_size-4096;

  /* just to be sure */
  td->pid=getpid();

  /* signal handling for a thread */
  signal(SIGTERM, _exit);
  signal(SIGCHLD, SIG_DFL);
  signal(SIGHUP, __thread_cancel_handler );

  /* limit stack so that we NEVER have to worry */
  setrlimit(RLIMIT_STACK, (struct rlimit *)(&limit));

  /* set scheduler */
  if (td->policy!=SCHED_OTHER) {
    struct sched_param sp;
    sp.sched_priority=td->priority;
    sched_setscheduler(getpid(),td->policy, &sp);
  }

#ifdef DEBUG
  printf("in starter %d, parameter %8p\n", td->pid, td->func);
#endif

  if (!td->canceled) {
    td->retval=td->func(td->arg);
  }

#ifdef DEBUG
  printf("end starter %d, retval %8p\n", td->pid, td->retval);
#endif

#if 0
  /* wake joined thread and put retval */
  if (td->joined) {
    td->joined->retval=td->retval;
    td->joined->join=0;
    td->joined=0;
  }
#endif

  return 0;
}


/* manager thread */
static char __manager_thread_stack[12*1024];
static _pthread_descr __manager_thread_data;
static void __manager_SIGCHLD(int sig)
{
  int pid, status, i;

  while(1) {
    pid = waitpid (-1, &status, WNOHANG);
    if (pid <= 0) break;

    for (i=0; i<_max_used_thread_id; i++) {
      if (threads[i].pid==pid) {
	__thread_cleanup(threads+i);
	break;
      }
    }
  }
}

static void __manager_SIGTERM(int sig)
{
  __kill_all_threads();
  _exit(0);
}

static void* __manager_thread(void *arg)
{
  struct sigaction sig_action_chld;
  sig_action_chld.sa_handler = __manager_SIGCHLD;
  sigemptyset(&sig_action_chld.sa_mask);
  sig_action_chld.sa_flags = SA_RESTART;

  sigaction(SIGCHLD, &sig_action_chld, 0);
  signal(SIGTERM, __manager_SIGTERM);
  signal(SIGHUP, SIG_IGN);

  while(1) {
    do {
      __thread_wait_some_time();
      if (getppid()<0) __manager_SIGTERM(0);
    } while (__pthread_trylock(&__manager_thread_data_lock));

    __manager_thread_data->pid =
      __clone(__mthread_starter,
	      __manager_thread_data->stack_addr,
	      CLONE_VM | CLONE_FS | CLONE_FILES | SIGCHLD,
	      __manager_thread_data);
    __pthread_unlock(&__manager_thread_data_go_lock);	/* release sender */
  }
  return 0;
}

int signal_manager_thread(_pthread_descr td)
{
  _pthread_descr this = __thread_self();

  this->manager_lock=&__manager_thread_signal_lock;	/* lock sender in use */
  __pthread_lock(&__manager_thread_signal_lock);
  __manager_thread_signal_lock_owner=this;

  __manager_thread_data=td;
  __pthread_unlock(&__manager_thread_data_lock);	/* signal manager to start */

  __pthread_lock(&__manager_thread_data_go_lock);	/* wait for manager */

  __pthread_unlock(&__manager_thread_signal_lock);	/* unlock */
  this->manager_lock=0;

  return td->pid;
}


/* thread stop */
static void __thread_main_exit()
{
  if (getpid()!=threads[0].pid) {
#ifdef DEBUG
    printf("A THREAD ? %d\n",getpid());
#endif
    kill(threads[0].pid, SIGTERM);
    while(1) __thread_wait_some_time();
  }
#ifdef DEBUG
  else
    printf("EXIT ! %d\n",getpid());
#endif

  /* stop ALL threads */
  kill(threads[1].pid, SIGTERM);
  __thread_wait_some_time();
  __kill_all_threads();
}

/* thread intern init */
void __thread_init()
{
  if (atexit(__thread_main_exit)==-1)
    exit(42);

#ifdef DEBUG
  printf("INIT ! %d\n",getpid());
  memset(threads,0,sizeof(threads));
#endif

  threads[0].pid = getpid();

  ++_max_used_thread_id;
  threads[1].stack_size=sizeof(__manager_thread_stack);
  threads[1].stack_addr=&__manager_thread_stack[sizeof(__manager_thread_stack)];
  threads[1].userstack=1;
  threads[1].func=__manager_thread;

  threads[1].pid =
        __clone(__mthread_starter,
		threads[1].stack_addr,
		CLONE_VM | CLONE_FS | CLONE_FILES,
		threads+1);

#ifdef DEBUG
  printf("manager thread @ : %d\n",threads[1].pid);
#endif
  sched_yield();
}
