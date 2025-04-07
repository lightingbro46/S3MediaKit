#ifndef MK_THREAD_H
#define MK_THREAD_H

#include <assert.h>
#include "mk_common.h"
#include "mk_tcp.h"

#ifdef __cplusplus
extern "C" {
#endif

// /////////////////////////////////////////Event thread/////////////////////////////////////////////
typedef struct mk_thread_t *mk_thread;

/**
 * Get the event thread where the tcp session object is located
 * @param ctx tcp session object
 * @return The event thread where the object is located
 */
API_EXPORT mk_thread API_CALL mk_thread_from_tcp_session(mk_tcp_session ctx);

/**
 * Get the event thread where the tcp client object is located
 * @param ctx tcp client
 * @return The event thread where the object is located
 */
API_EXPORT mk_thread API_CALL mk_thread_from_tcp_client(mk_tcp_client ctx);

/**
 * Get an event thread randomly from the event thread pool according to the load balancing algorithm
 * If this function is executed within the event thread, it will return the current event thread
 * Event thread refers to timer, network io event thread
 * @return Event thread
 */
API_EXPORT mk_thread API_CALL mk_thread_from_pool();

/**
 * Get a thread randomly from the background thread pool according to the load balancing algorithm
 * Background threads are essentially the same as event threads, but they have lower priority and can execute short-term blocking tasks
 * Background threads in S3MediaKit are used for dns resolution, file demultiplexing during mp4 on-demand
 * @return Background thread
 */
API_EXPORT mk_thread API_CALL mk_thread_from_pool_work();

typedef struct mk_thread_pool_t *mk_thread_pool;

/**
 * Create a thread pool
 * @param name Thread pool name, for debugging
 * @param n_thread Number of threads, 0 for the number of cpus
 * @param priority Thread priority, divided into PRIORITY_LOWEST = 0,PRIORITY_LOW, PRIORITY_NORMAL, PRIORITY_HIGH, PRIORITY_HIGHEST
 * @return Thread pool
 */
API_EXPORT mk_thread_pool API_CALL mk_thread_pool_create(const char *name, size_t n_thread, int priority);

/**
 * Destroy the thread pool
 * @param pool Thread pool
 * @return 0: Success
 */
API_EXPORT int API_CALL mk_thread_pool_release(mk_thread_pool pool);

/**
 * Get a thread from the thread pool
 * @param pool Thread pool
 * @return Thread
 */
API_EXPORT mk_thread API_CALL mk_thread_from_thread_pool(mk_thread_pool pool);

// /////////////////////////////////////////Thread switching/////////////////////////////////////////////
typedef void (API_CALL *on_mk_async)(void *user_data);

/**
 * Switch to the event thread and execute asynchronously
 * @param ctx Event thread
 * @param cb Callback function
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_async_do(mk_thread ctx, on_mk_async cb, void *user_data);
API_EXPORT void API_CALL mk_async_do2(mk_thread ctx, on_mk_async cb, void *user_data, on_user_data_free user_data_free);

/**
 * Switch to the event thread and execute with delay
 * @param ctx Event thread
 * @param ms Delay time, in milliseconds
 * @param cb Callback function
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_async_do_delay(mk_thread ctx, size_t ms, on_mk_async cb, void *user_data);
API_EXPORT void API_CALL mk_async_do_delay2(mk_thread ctx, size_t ms, on_mk_async cb, void *user_data, on_user_data_free user_data_free);

/**
 * Switch to the event thread and execute synchronously
 * @param ctx Event thread
 * @param cb Callback function
 * @param user_data User data pointer
 */
API_EXPORT void API_CALL mk_sync_do(mk_thread ctx, on_mk_async cb, void *user_data);

// /////////////////////////////////////////Timer/////////////////////////////////////////////
typedef struct mk_timer_t *mk_timer;

/**
 * Timer trigger event
 * @return Next trigger delay (in milliseconds), return 0 to stop repeating
 */
typedef uint64_t (API_CALL *on_mk_timer)(void *user_data);

/**
 * Create a timer
 * @param ctx Thread object
 * @param delay_ms Execution delay, in milliseconds
 * @param cb Callback function
 * @param user_data User data pointer
 * @return Timer object
 */
API_EXPORT mk_timer API_CALL mk_timer_create(mk_thread ctx, uint64_t delay_ms, on_mk_timer cb, void *user_data);
API_EXPORT mk_timer API_CALL mk_timer_create2(mk_thread ctx, uint64_t delay_ms, on_mk_timer cb, void *user_data, on_user_data_free user_data_free);

/**
 * Destroy and cancel the timer
 * @param ctx Timer object
 */
API_EXPORT void API_CALL mk_timer_release(mk_timer ctx);

// /////////////////////////////////////////Signal volume/////////////////////////////////////////////

typedef struct mk_sem_t *mk_sem;

/**
 * Create a semaphore
 */
API_EXPORT mk_sem API_CALL mk_sem_create();

/**
 * Destroy the semaphore
 */
API_EXPORT void API_CALL mk_sem_release(mk_sem sem);

/**
 * Increase the semaphore by n
 */
API_EXPORT void API_CALL mk_sem_post(mk_sem sem, size_t n);

/**
 * Decrease the semaphore by 1
 * @param sem
 */
API_EXPORT void API_CALL mk_sem_wait(mk_sem sem);

#ifdef __cplusplus
}
#endif
#endif //MK_THREAD_H
