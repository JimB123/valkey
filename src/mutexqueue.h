#ifndef __MUTEXQUEUE_H
#define __MUTEXQUEUE_H

#include <stdbool.h>
#include "fifo.h"

/*
 * A thread-safe queue, protected by a mutex.
 *
 * Supports:
 *   - Adding an item to the end of the queue
 *   - Adding a list of items (Fifo) to the end of the queue
 *   - Insertion of a priority item at the beginning of the queue (but after existing priority items)
 *   - Removing an item from the beginning of the queue
 *   - Removing ALL items as (as new Fifo) from the queue
 *   - Synchronous waiting on the queue for new items
 *
 * The caller is responsible for memory management for items in the queue.
 */


/* The mutexQueue is an opaque structure.  */
typedef void * mutexQueue;


mutexQueue mutexQueueCreate(void);
void mutexQueueRelease(mutexQueue theQueue);
unsigned long mutexQueueLength(mutexQueue theQueue);
void mutexQueueAddPriority(mutexQueue theQueue, void *value);
void mutexQueueAdd(mutexQueue theQueue, void *value);
void mutexQueueAddMultiple(mutexQueue theQueue, Fifo *valueFifo);
void * mutexQueuePop(mutexQueue theQueue, bool blocking);
Fifo * mutexQueuePopAll(mutexQueue theQueue, bool blocking);

#endif
