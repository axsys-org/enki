#include <errno.h>
#include <pthread.h>

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
  (void)attr;
  mutex->depth = 0;
  return 0;
}
int pthread_mutex_destroy(pthread_mutex_t* mutex) { (void)mutex; return 0; }
int pthread_mutex_lock(pthread_mutex_t* mutex) { mutex->depth++; return 0; }
int pthread_mutex_trylock(pthread_mutex_t* mutex) { mutex->depth++; return 0; }
int pthread_mutex_unlock(pthread_mutex_t* mutex) {
  if (mutex->depth == 0) return EINVAL;
  mutex->depth--;
  return 0;
}
int pthread_mutexattr_init(pthread_mutexattr_t* attr) { attr->recursive = 0; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t* attr) { (void)attr; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type) {
  attr->recursive = type == PTHREAD_MUTEX_RECURSIVE;
  return 0;
}
int pthread_cond_init(pthread_cond_t* cond, const void* attr) {
  (void)attr; cond->sequence = 0; return 0;
}
int pthread_cond_destroy(pthread_cond_t* cond) { (void)cond; return 0; }
int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
  (void)cond; (void)mutex; return ENOSYS;
}
int pthread_cond_signal(pthread_cond_t* cond) { cond->sequence++; return 0; }
int pthread_cond_broadcast(pthread_cond_t* cond) { cond->sequence++; return 0; }
int pthread_once(pthread_once_t* once, void (*routine)(void)) {
  if (*once == 0) { *once = 1; routine(); }
  return 0;
}
pthread_t pthread_self(void) { return 1; }
