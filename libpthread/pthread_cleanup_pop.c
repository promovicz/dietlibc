#include <unistd.h>
#include <errno.h>

#include <pthread.h>
#include "thread_internal.h"

void pthread_cleanup_pop(int execute)
{
  _pthread_descr this;
  struct thread_cleanup_t tmp;
  int i;

  __THREAD_INIT();

  this = __thread_self();

  for (i=0; i<PTHREAD_MAX_CLEANUP; ++i)
    if (this->cleanup_stack[i].func)
      if (this->cleanup_stack[i].func!=(void*)1) {
	tmp.func=this->cleanup_stack[i].func;
	tmp.arg=this->cleanup_stack[i].arg;
	this->cleanup_stack[i].func=0;
      }
  tmp.func(tmp.arg);
}