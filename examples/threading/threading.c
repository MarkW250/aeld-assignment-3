#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
    struct thread_data* tdata = (struct thread_data*)(thread_param);

    if(tdata == NULL)
    {
        return NULL;
    }

    tdata->thread_complete_success = false;

    int wait_to_obtain_ms = tdata->wait_to_obtain_ms;
    int wait_to_release_ms = tdata->wait_to_release_ms;

    if((wait_to_obtain_ms < 0) || (wait_to_release_ms < 0))
    {
        return tdata;
    }

    usleep(wait_to_obtain_ms * 1000);

    int lockStatus = pthread_mutex_lock(tdata->mutex);
    if(lockStatus != 0)
    {
        return tdata;
    }
    
    usleep(wait_to_release_ms * 1000);
    
    int unlockStatus = pthread_mutex_unlock(tdata->mutex);
    if(unlockStatus != 0)
    {
        return tdata;
    }

    tdata->thread_complete_success = true;

    return (void*)(tdata);
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    /**
     * TODO: allocate memory for thread_data, setup mutex and wait arguments, pass thread_data to created thread
     * using threadfunc() as entry point.
     *
     * return true if successful.
     *
     * See implementation details in threading.h file comment block
     */
    struct thread_data* tdata = malloc(sizeof(struct thread_data));
    if(tdata == NULL)
    {
        return false;
    }

    tdata->thread_complete_success = false;
    tdata->mutex = mutex;
    tdata->wait_to_obtain_ms = wait_to_obtain_ms;
    tdata->wait_to_release_ms = wait_to_release_ms;

    int threadCreationStatus = pthread_create(thread, NULL, threadfunc, tdata);
    if(threadCreationStatus != 0)
    {
        return false;
    }

    return true;
}

