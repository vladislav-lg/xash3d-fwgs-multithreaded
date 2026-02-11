/*
net_thread.h - dedicated network I/O thread using enkiTS
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
#ifndef NET_THREAD_H
#define NET_THREAD_H

#ifdef XASH_NET_THREAD

#include "xash3d_types.h"
#include "net_ws.h"

/*
=============================================================================

  Network Thread API

  All network I/O (recvfrom/sendto) happens on a dedicated enkiTS
  PinnedTask thread. The main game loop communicates with this thread
  via lock-free SPSC ring buffers.

=============================================================================
*/

/* Initialize the network thread (call after sockets are created) */
void	NetThread_Init( void );

/* Shutdown the network thread (call before closing sockets) */
void	NetThread_Shutdown( void );

/* Returns true if the network thread is active */
qboolean	NetThread_IsActive( void );

/* Pop one inbound packet from the SPSC queue for the given socket type.
   Returns true if a packet was available, fills from/data/length. */
qboolean	NetThread_RecvPacket( netsrc_t sock, netadr_t *from, byte *data, size_t *length );

/* Push one outbound packet into the SPSC queue for the network thread.
   The network thread will call sendto() and handle fragmentation. */
void	NetThread_SendPacket( netsrc_t sock, size_t length, const void *data, netadr_t to, size_t splitsize );

/* Get the arrival time of the last popped inbound packet */
double	NetThread_GetLastPacketTime( void );

/* Notify the network thread that sockets have been recreated
   (e.g. after NET_OpenIP re-entry on port/map change) */
void	NetThread_SocketsUpdated( void );

/* Statistics snapshot (all fields safe to read from main thread) */
typedef struct
{
	/* Current queue fill levels (0 to NET_SPSC_QUEUE_SIZE-1) */
	uint32_t	inbound_count[NS_COUNT];
	uint32_t	outbound_count[NS_COUNT];

	/* Cumulative packet counters (monotonically increasing) */
	uint32_t	inbound_received[NS_COUNT];
	uint32_t	outbound_sent[NS_COUNT];
	uint32_t	inbound_drops[NS_COUNT];
	uint32_t	outbound_drops[NS_COUNT];

	/* Cumulative byte counters */
	uint64_t	inbound_bytes[NS_COUNT];
	uint64_t	outbound_bytes[NS_COUNT];

	/* Network thread loop iteration counter */
	uint32_t	loop_iterations;

	/* Network thread cumulative CPU time in seconds (kernel + user).
	   Queried from the main thread via OS thread handle.
	   -1.0 if not available on this platform. */
	double		net_thread_cpu_time;

	/* Network thread wall-time breakdown (cumulative seconds) */
	double		net_active_time;	/* time spent doing recv/send/queue work */
	double		net_idle_time;		/* time spent blocked in select() */
} net_thread_stats_t;

/* Snapshot current stats into *stats (thread-safe, called from main thread) */
void	NetThread_GetStats( net_thread_stats_t *stats );

#endif /* XASH_NET_THREAD */

#endif /* NET_THREAD_H */
