/*
 * lock-impl.c - pthread-based LockInterface implementation (Posix style)
 *
 * Copyright (c) 1998
 *      Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file.
 */

#include "config.h"
#include "config-std.h"
#include <errno.h>
#include "debug.h"

#include "jthread.h"
/* For NOTIMEOUT */
#include "jsyscall.h"
#ifdef KAFFE_BOEHM_GC
#include "boehm-gc/boehm/include/gc.h"
#endif
#include <signal.h>

static inline void
setBlockState(jthread_t cur, unsigned int newState, void *sp, sigset_t *old_mask)
{
  int suspendSig;
  sigset_t suspendMask, pendingMask;

  suspendSig = KaffePThread_getSuspendSignal();
  sigemptyset(&suspendMask);
  sigaddset(&suspendMask, suspendSig);
  pthread_sigmask(SIG_BLOCK, &suspendMask, old_mask);

  pthread_mutex_lock(&cur->suspendLock);
  cur->blockState |= newState;
  cur->stackCur  = sp;
  pthread_mutex_unlock(&cur->suspendLock);

  /* This thread is protected against suspendall. So if a signal has been
   * received it is just before the mutex_lock and after pthread_sigmask.
   * We must acknowledge and go in WaitForResume.
   */
  sigpending(&pendingMask); 
  if (sigismember(&pendingMask, suspendSig))
  {
    KaffePThread_AckAndWaitForResume(cur, newState);
  }
}

static inline void
clearBlockState(jthread_t cur, unsigned int newState, sigset_t *old_mask)
{
  pthread_mutex_lock(&cur->suspendLock);
  cur->blockState &= ~newState;
  if (cur->suspendState == SS_SUSPENDED)
    {
      DBG(JTHREADDETAIL, dprintf("Changing blockstate of %p to %d while in suspend, block again\n",  cur, newState));

      KaffePThread_WaitForResume(true, 0);
    }
  else
    {
      cur->stackCur = NULL;
      pthread_mutex_unlock(&cur->suspendLock);
    }

  /* Catch an interrupt event sent while we were being killed.
   * This is needed for Darwin's pthreads.
   */
  if (cur->status == THREAD_KILL)
    pthread_exit(NULL);

  pthread_sigmask(SIG_SETMASK, old_mask, NULL);
  /* Here the state is not SS_PENDING_SUSPEND so releasing the signal will
   * not trigger a deadlock.
   */
}

void
jmutex_lock( jmutex* lk )
{
  jthread_t cur = jthread_current ();
  sigset_t oldmask;

  setBlockState(cur, BS_MUTEX, (void*)&cur, &oldmask);
  pthread_mutex_lock( lk );
  clearBlockState(cur, BS_MUTEX, &oldmask);
}


static inline int
ThreadCondWait(jthread_t cur, jcondvar *cv, jmutex *mux)
{
  int status;
  sigset_t oldmask;

  setBlockState(cur, BS_CV, (void*)&cur, &oldmask);
  status = pthread_cond_wait( cv, mux );
  clearBlockState(cur, BS_CV, &oldmask);

  return status;
}

/*
 * Wait on the condvar with a given relative timeout in ms (which we
 * have to convert into a absolute timespec now)
 */
jboolean
jcondvar_wait ( jcondvar* cv, jmutex *mux, jlong timeout )
{
  jthread_t cur = jthread_current();
  int             status;
  struct timespec abst;
  struct timeval  now;
  sigset_t oldmask;

  /*
   * If a thread trying to get a heavy lock is interrupted, we may get here
   * with the interrupted flag set (because the thread didn't get the heavy
   * lock and has to wait again). Therefore, we must not clear the interrupted
   * flag here.
   */

  if ( timeout == NOTIMEOUT )
    {
      /* we handle this as "wait forever" */
      status = ThreadCondWait(cur, cv, mux);
    }
  else
    {
      /* timeout is in millisecs, timeval in microsecs, and timespec in nanosecs */
      gettimeofday( &now, NULL);
      abst.tv_sec = now.tv_sec + (timeout / 1000);
      if( abst.tv_sec < now.tv_sec )
	{
	  /* huge timeout value, we handle this as "wait forever" */
	  status = ThreadCondWait(cur, cv, mux);
	}
      else
	{
	  abst.tv_nsec = (now.tv_usec * 1000) + (timeout % 1000) * 1000000;
	  
	  if (abst.tv_nsec > 1000000000)
	    {
	      abst.tv_sec  += 1;
	      abst.tv_nsec -= 1000000000;
	    }

	  setBlockState(cur, BS_CV_TO, (void*)&cur, &oldmask);
	  status = pthread_cond_timedwait( cv, mux, &abst);
	  clearBlockState(cur, BS_CV_TO, &oldmask);
	}
    }

  /*
   * Since we interrupt a thread blocked on a condition variable by signaling that
   * condition variable, we cannot set the interrupted flag based on the value of
   * 'signal'. Therefore, we have to rely on the interrupting thread to set the
   * flag.
   */

  return (status == 0);
}
