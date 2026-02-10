/*
net_spsc_queue.h - lock-free single-producer single-consumer ring buffer
Copyright (C) 2024 Uncle Mike, mittorn, a1batross, SNMetamorph

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#ifndef NET_SPSC_QUEUE_H
#define NET_SPSC_QUEUE_H

#include "xash3d_types.h"
#include "netchan.h"
#include "net_ws.h"

#include <string.h>

/*
=============================================================================

  Lock-free SPSC (Single-Producer Single-Consumer) ring buffer

  - Fixed capacity, power-of-two size (default 512 slots)
  - head and tail on separate cache lines (64-byte padding)
  - Acquire/release memory ordering via atomics (GCC/Clang) or
    ReadWriteBarrier (MSVC)
  - No dynamic allocation, all slots pre-allocated in the struct
  - Queue is empty when head == tail
  - Queue is full when (head + 1) & MASK == tail

=============================================================================
*/

#define NET_SPSC_QUEUE_SIZE 512
#define NET_SPSC_QUEUE_MASK ( NET_SPSC_QUEUE_SIZE - 1 )

/* Verify power-of-two at compile time */
typedef char net_spsc_queue_size_check_t[( NET_SPSC_QUEUE_SIZE & NET_SPSC_QUEUE_MASK ) == 0 ? 1 : -1];

/*
=============================================================================
  Atomic helpers
=============================================================================
*/

#if defined( _MSC_VER )
  #include <intrin.h>

  #if defined( _M_ARM ) || defined( _M_ARM64 )
    /* ARM needs a hardware memory barrier */
    #define SPSC_STORE_RELEASE( ptr, val ) \
      do { __dmb( _ARM_BARRIER_ISH ); *(volatile uint32_t *)(ptr) = (val); } while( 0 )

    #define SPSC_LOAD_ACQUIRE( ptr ) \
      _spsc_load_acquire_arm( (volatile const uint32_t *)(ptr) )

    static __forceinline uint32_t _spsc_load_acquire_arm( volatile const uint32_t *ptr )
    {
      uint32_t val = *ptr;
      __dmb( _ARM_BARRIER_ISH );
      return val;
    }
  #else
    /* x86/x64: stores and loads are already ordered, compiler barrier suffices */
    #define SPSC_STORE_RELEASE( ptr, val ) \
      do { _ReadWriteBarrier(); *(volatile uint32_t *)(ptr) = (val); } while( 0 )

    #define SPSC_LOAD_ACQUIRE( ptr ) \
      _spsc_load_acquire_x86( (volatile const uint32_t *)(ptr) )

    static __forceinline uint32_t _spsc_load_acquire_x86( volatile const uint32_t *ptr )
    {
      uint32_t val = *ptr;
      _ReadWriteBarrier();
      return val;
    }
  #endif

#elif defined( __GNUC__ ) || defined( __clang__ )

  #define SPSC_STORE_RELEASE( ptr, val ) \
    __atomic_store_n( (uint32_t *)(ptr), (val), __ATOMIC_RELEASE )

  #define SPSC_LOAD_ACQUIRE( ptr ) \
    __atomic_load_n( (uint32_t *)(ptr), __ATOMIC_ACQUIRE )

#else
  #error "Unsupported compiler for SPSC atomics"
#endif

/*
=============================================================================
  Packet types for the queues
=============================================================================
*/

/* For inbound packets (recv thread -> main thread) */
typedef struct
{
	netadr_t	from;
	size_t		length;
	double		arrival_time; /* Sys_DoubleTime() captured at recvfrom */
	byte		data[NET_MAX_FRAGMENT];
} net_inbound_packet_t;

/* For outbound packets (main thread -> send thread) */
typedef struct
{
	netadr_t	to;
	size_t		length;
	netsrc_t	sock;      /* NS_SERVER or NS_CLIENT */
	size_t		splitsize; /* for NET_SendLong fragmentation */
	byte		data[NET_MAX_FRAGMENT];
} net_outbound_packet_t;

/*
=============================================================================
  Queue structures with cache-line-separated head and tail
=============================================================================
*/

#define NET_CACHE_LINE_SIZE 64

typedef struct
{
	/* Producer side: only written by producer */
	volatile uint32_t		head;
	byte				_pad_head[NET_CACHE_LINE_SIZE - sizeof( uint32_t )];

	/* Consumer side: only written by consumer */
	volatile uint32_t		tail;
	byte				_pad_tail[NET_CACHE_LINE_SIZE - sizeof( uint32_t )];

	/* Data slots */
	net_inbound_packet_t		slots[NET_SPSC_QUEUE_SIZE];
} net_inbound_queue_t;

typedef struct
{
	/* Producer side: only written by producer */
	volatile uint32_t		head;
	byte				_pad_head[NET_CACHE_LINE_SIZE - sizeof( uint32_t )];

	/* Consumer side: only written by consumer */
	volatile uint32_t		tail;
	byte				_pad_tail[NET_CACHE_LINE_SIZE - sizeof( uint32_t )];

	/* Data slots */
	net_outbound_packet_t		slots[NET_SPSC_QUEUE_SIZE];
} net_outbound_queue_t;

/*
=============================================================================
  Inline queue operations
=============================================================================
*/

/*
====================
NetQueue_Init

  Zero head and tail
====================
*/
static inline void NetQueue_InitInbound( net_inbound_queue_t *queue )
{
	queue->head = 0;
	queue->tail = 0;
}

static inline void NetQueue_InitOutbound( net_outbound_queue_t *queue )
{
	queue->head = 0;
	queue->tail = 0;
}

/*
====================
NetQueue_Push (inbound)

  Push an item into the inbound queue.
  Returns true on success, false if full.
  Called by: network thread (producer)
====================
*/
static inline qboolean NetQueue_PushInbound( net_inbound_queue_t *queue, const net_inbound_packet_t *item )
{
	uint32_t head = queue->head; /* only producer writes head, relaxed read is fine */
	uint32_t tail = SPSC_LOAD_ACQUIRE( &queue->tail ); /* consumer may have advanced tail */
	uint32_t next_head = ( head + 1 ) & NET_SPSC_QUEUE_MASK;

	if( next_head == tail )
		return false; /* queue is full */

	memcpy( &queue->slots[head], item, sizeof( *item ) );

	SPSC_STORE_RELEASE( &queue->head, next_head );

	return true;
}

/*
====================
NetQueue_Pop (inbound)

  Pop an item from the inbound queue.
  Returns true on success, false if empty.
  Called by: main thread (consumer)
====================
*/
static inline qboolean NetQueue_PopInbound( net_inbound_queue_t *queue, net_inbound_packet_t *item )
{
	uint32_t tail = queue->tail; /* only consumer writes tail, relaxed read is fine */
	uint32_t head = SPSC_LOAD_ACQUIRE( &queue->head ); /* producer may have advanced head */

	if( tail == head )
		return false; /* queue is empty */

	memcpy( item, &queue->slots[tail], sizeof( *item ) );

	SPSC_STORE_RELEASE( &queue->tail, ( tail + 1 ) & NET_SPSC_QUEUE_MASK );

	return true;
}

/*
====================
NetQueue_Push (outbound)

  Push an item into the outbound queue.
  Returns true on success, false if full.
  Called by: main thread (producer)
====================
*/
static inline qboolean NetQueue_PushOutbound( net_outbound_queue_t *queue, const net_outbound_packet_t *item )
{
	uint32_t head = queue->head; /* only producer writes head */
	uint32_t tail = SPSC_LOAD_ACQUIRE( &queue->tail );
	uint32_t next_head = ( head + 1 ) & NET_SPSC_QUEUE_MASK;

	if( next_head == tail )
		return false; /* queue is full */

	memcpy( &queue->slots[head], item, sizeof( *item ) );

	SPSC_STORE_RELEASE( &queue->head, next_head );

	return true;
}

/*
====================
NetQueue_Pop (outbound)

  Pop an item from the outbound queue.
  Returns true on success, false if empty.
  Called by: network thread (consumer)
====================
*/
static inline qboolean NetQueue_PopOutbound( net_outbound_queue_t *queue, net_outbound_packet_t *item )
{
	uint32_t tail = queue->tail; /* only consumer writes tail */
	uint32_t head = SPSC_LOAD_ACQUIRE( &queue->head );

	if( tail == head )
		return false; /* queue is empty */

	memcpy( item, &queue->slots[tail], sizeof( *item ) );

	SPSC_STORE_RELEASE( &queue->tail, ( tail + 1 ) & NET_SPSC_QUEUE_MASK );

	return true;
}

/*
====================
NetQueue_IsEmpty (inbound)
====================
*/
static inline qboolean NetQueue_IsEmptyInbound( net_inbound_queue_t *queue )
{
	uint32_t head = SPSC_LOAD_ACQUIRE( &queue->head );
	uint32_t tail = SPSC_LOAD_ACQUIRE( &queue->tail );

	return ( head == tail ) ? true : false;
}

/*
====================
NetQueue_IsEmpty (outbound)
====================
*/
static inline qboolean NetQueue_IsEmptyOutbound( net_outbound_queue_t *queue )
{
	uint32_t head = SPSC_LOAD_ACQUIRE( &queue->head );
	uint32_t tail = SPSC_LOAD_ACQUIRE( &queue->tail );

	return ( head == tail ) ? true : false;
}

/*
====================
NetQueue_Count (inbound) - approximate count
====================
*/
static inline uint32_t NetQueue_CountInbound( net_inbound_queue_t *queue )
{
	uint32_t head = SPSC_LOAD_ACQUIRE( &queue->head );
	uint32_t tail = SPSC_LOAD_ACQUIRE( &queue->tail );

	return ( head - tail ) & NET_SPSC_QUEUE_MASK;
}

/*
====================
NetQueue_Count (outbound) - approximate count
====================
*/
static inline uint32_t NetQueue_CountOutbound( net_outbound_queue_t *queue )
{
	uint32_t head = SPSC_LOAD_ACQUIRE( &queue->head );
	uint32_t tail = SPSC_LOAD_ACQUIRE( &queue->tail );

	return ( head - tail ) & NET_SPSC_QUEUE_MASK;
}

#endif /* NET_SPSC_QUEUE_H */
