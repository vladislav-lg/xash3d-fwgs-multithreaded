/*
net_thread.c - dedicated network I/O thread using enkiTS
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

#ifdef XASH_NET_THREAD

#include "common.h"
#include "netchan.h"
#include "net_ws.h"
#include "net_ws_private.h"
#include "net_spsc_queue.h"
#include "net_thread.h"
#include "TaskScheduler_c.h"

#include <string.h>

#if XASH_LINUX
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

/*
=============================================================================

  Network Thread State

=============================================================================
*/

/* Split packet header for fragmentation on the network thread.
   Must match SPLITPACKET in net_ws.c (packed, 10 bytes). */
#pragma pack(push, 1)
typedef struct
{
	int	net_id;
	int	sequence_number;
	short	packet_id;
} net_thread_splitpacket_t;
#pragma pack(pop)

#define NET_THREAD_SPLITPACKET_MAX_SIZE 64000
#define NET_THREAD_HEADER_SPLITPACKET   ( -2 )

/* Per-socket-type (NS_SERVER / NS_CLIENT) queue pair */
typedef struct
{
	net_inbound_queue_t	inbound;
	net_outbound_queue_t	outbound;
} net_queue_pair_t;

/* Global network thread state */
static struct
{
	enkiTaskScheduler	*scheduler;
	enkiPinnedTask		*pinned_task;

	net_queue_pair_t	queues[NS_COUNT];

	/* Cached socket fds (refreshed via NetThread_SocketsUpdated) */
	volatile int		ip_sockets[NS_COUNT];
	volatile int		ip6_sockets[NS_COUNT];

	/* Counters for diagnostics */
	volatile uint32_t	packets_dropped;

	/* Per-socket, per-direction counters for the thread graph overlay.
	   Written only by the owning thread (net thread or main thread). */
	volatile uint32_t	inbound_received[NS_COUNT];	/* net thread: successful recvfrom */
	volatile uint32_t	outbound_sent[NS_COUNT];	/* net thread: successful queue pop -> sendto */
	volatile uint32_t	inbound_drops[NS_COUNT];	/* net thread: inbound queue full */
	volatile uint32_t	outbound_drops[NS_COUNT];	/* main thread: outbound queue full */
	volatile uint64_t	inbound_bytes[NS_COUNT];	/* net thread: sum of recv bytes */
	volatile uint64_t	outbound_bytes[NS_COUNT];	/* net thread: sum of sent bytes */
	volatile uint32_t	loop_iterations;		/* net thread: IO loop count */

	/* Cumulative wall-time breakdown (seconds, written by net thread) */
	volatile double		active_time;			/* net thread: time doing real work */
	volatile double		idle_time;			/* net thread: time blocked in select */

	/* Last inbound packet arrival time (per main-thread pop) */
	double			last_packet_time;

	/* Running flag: set to 0 to signal shutdown */
	volatile int		running;

	/* Is the thread active? */
	qboolean		active;

	/* Sequence number for split packets (network thread only) */
	int			sequence_number;

	/* Thread handle for CPU time measurement (captured in IOLoop) */
#if XASH_WIN32
	HANDLE			thread_handle;	/* duplicated real handle */
#elif XASH_LINUX
	pid_t			thread_tid;	/* Linux thread ID from gettid() */
#endif
} net_thread;

/*
=============================================================================

  Forward declarations for functions safe to call from the network thread

=============================================================================
*/

/* These are defined in net_ws.c and have no global state dependencies */
extern int NET_GetIPSocket( netsrc_t sock );
extern int NET_GetIP6Socket( netsrc_t sock );

/*
=============================================================================

  Fragmentation logic (duplicated from NET_SendLong in net_ws.c)
  This runs on the NETWORK THREAD - no engine globals allowed!

=============================================================================
*/

/*
====================
NetThread_SendFragment

  Send a single packet or perform split-packet fragmentation and send.
  This is a standalone version of NET_SendLong that takes only raw
  parameters with no dependency on engine globals.
====================
*/
static void NetThread_SendFragment( int net_socket, const byte *buf, size_t len, const struct sockaddr_storage *to, size_t tolen, size_t splitsize, netsrc_t sock )
{
	/* do we need to break this packet up?
	   Only servers send split packets (mirrors NET_SendLong logic) */
	if( splitsize > sizeof( net_thread_splitpacket_t ) && sock == NS_SERVER && len > splitsize )
	{
		char	packet[NET_THREAD_SPLITPACKET_MAX_SIZE];
		int	body_size = (int)( splitsize - sizeof( net_thread_splitpacket_t ));
		int	packet_number = 0;
		int	packet_count;
		size_t	remaining = len;
		net_thread_splitpacket_t *p_header;

		net_thread.sequence_number++;
		if( net_thread.sequence_number <= 0 )
			net_thread.sequence_number = 1;

		p_header = (net_thread_splitpacket_t *)packet;
		p_header->sequence_number = net_thread.sequence_number;
		p_header->net_id = NET_THREAD_HEADER_SPLITPACKET;
		packet_count = (int)(( len + body_size - 1 ) / body_size);

		while( remaining > 0 )
		{
			int size = (int)( remaining < (size_t)body_size ? remaining : (size_t)body_size );
			p_header->packet_id = (short)(( packet_number << 8 ) + packet_count);
			memcpy( packet + sizeof( net_thread_splitpacket_t ), buf + ( packet_number * body_size ), size );

			sendto( net_socket, packet, (int)( size + sizeof( net_thread_splitpacket_t )), 0, (const struct sockaddr *)to, (int)tolen );

			remaining -= size;
			packet_number++;
		}
	}
	else
	{
		sendto( net_socket, (const char *)buf, (int)len, 0, (const struct sockaddr *)to, (int)tolen );
	}
}

/*
=============================================================================

  Inbound: Network Thread -> Main Thread

=============================================================================
*/

/*
====================
NetThread_SockadrToNetadr

  Thread-safe version of NET_SockadrToNetadr (no global state).
====================
*/
static void NetThread_SockadrToNetadr( const struct sockaddr_storage *s, netadr_t *a )
{
	if( s->ss_family == AF_INET )
	{
		NET_NetadrSetType( a, NA_IP );
		a->ip4 = ((const struct sockaddr_in *)s)->sin_addr.s_addr;
		a->port = ((const struct sockaddr_in *)s)->sin_port;
	}
	else if( s->ss_family == AF_INET6 )
	{
		NET_NetadrSetType( a, NA_IP6 );
		NET_IP6BytesToNetadr( a, ((const struct sockaddr_in6 *)s)->sin6_addr.s6_addr );
		a->port = ((const struct sockaddr_in6 *)s)->sin6_port;
	}
}

/*
====================
NetThread_DetermineNetsrc

  Given a socket fd, determine which netsrc_t (NS_SERVER / NS_CLIENT)
  and whether it's IPv4 or IPv6.
====================
*/
static netsrc_t NetThread_DetermineNetsrc( int fd, int ip4[NS_COUNT], int ip6[NS_COUNT] )
{
	int i;

	for( i = 0; i < NS_COUNT; i++ )
	{
		if( fd == ip4[i] || fd == ip6[i] )
			return (netsrc_t)i;
	}

	return NS_CLIENT; /* fallback, should not happen */
}

/*
=============================================================================

  Network Thread Loop (enkiTS PinnedTask)

=============================================================================
*/

/*
====================
NetThread_IOLoop

  The main network I/O loop running on the enkiTS pinned task thread.
  Performs:
    1. select()/poll() on all valid sockets with 1ms timeout
    2. recvfrom() in a loop until EWOULDBLOCK for each readable socket
    3. Drain outbound queues and call sendto() for each
====================
*/
static void NetThread_IOLoop( void *pArgs )
{
	(void)pArgs;

	/* Capture OS thread handle for CPU time measurement from main thread */
#if XASH_WIN32
	DuplicateHandle( GetCurrentProcess(), GetCurrentThread(),
		GetCurrentProcess(), &net_thread.thread_handle,
		THREAD_QUERY_INFORMATION, FALSE, 0 );
#elif XASH_LINUX
	net_thread.thread_tid = (pid_t)syscall( __NR_gettid );
#endif

	while( net_thread.running )
	{
		double		loop_start, select_end;
		fd_set		fdset;
		struct timeval	timeout;
		int		max_fd = 0;
		int		ret, i;
		int		ip4[NS_COUNT], ip6[NS_COUNT];
		int		sockets[4]; /* up to 4 sockets to monitor */
		int		num_sockets = 0;

		net_thread.loop_iterations++;
		loop_start = Sys_DoubleTime();

		/* Cache current socket fds (may change via NetThread_SocketsUpdated) */
		for( i = 0; i < NS_COUNT; i++ )
		{
			ip4[i] = net_thread.ip_sockets[i];
			ip6[i] = net_thread.ip6_sockets[i];
		}

		/* Build fd_set for select */
		FD_ZERO( &fdset );

		for( i = 0; i < NS_COUNT; i++ )
		{
			if( NET_IsSocketValid( ip4[i] ))
			{
				FD_SET( (unsigned int)ip4[i], &fdset );
				if( ip4[i] > max_fd ) max_fd = ip4[i];
				sockets[num_sockets++] = ip4[i];
			}
			if( NET_IsSocketValid( ip6[i] ))
			{
				FD_SET( (unsigned int)ip6[i], &fdset );
				if( ip6[i] > max_fd ) max_fd = ip6[i];
				sockets[num_sockets++] = ip6[i];
			}
		}

		if( num_sockets == 0 )
		{
			/* No valid sockets yet, just sleep a bit */
			timeout.tv_sec = 0;
			timeout.tv_usec = 1000; /* 1ms */
			select( 0, NULL, NULL, NULL, &timeout );
			select_end = Sys_DoubleTime();
			net_thread.idle_time += select_end - loop_start;
			goto drain_outbound;
		}

		/* select with 1ms timeout */
		timeout.tv_sec = 0;
		timeout.tv_usec = 1000; /* 1ms */
		ret = select( max_fd + 1, &fdset, NULL, NULL, &timeout );
		select_end = Sys_DoubleTime();
		net_thread.idle_time += select_end - loop_start;

		if( ret > 0 )
		{
			int s;

			for( s = 0; s < num_sockets; s++ )
			{
				int sock_fd = sockets[s];

				if( !FD_ISSET( (unsigned int)sock_fd, &fdset ))
					continue;

				/* recvfrom in a loop until EWOULDBLOCK */
				for( ;; )
				{
					byte			buf[NET_MAX_FRAGMENT];
					struct sockaddr_storage	addr;
					WSAsize_t		addr_len = sizeof( addr );
					int			recv_ret;
					net_inbound_packet_t	pkt;
					netsrc_t		ns;

					memset( &addr, 0, sizeof( addr ));
					recv_ret = recvfrom( sock_fd, (char *)buf, sizeof( buf ), 0, (struct sockaddr *)&addr, &addr_len );

					if( NET_IsSocketError( recv_ret ))
					{
						/* EWOULDBLOCK means no more data */
						break;
					}

					if( recv_ret >= NET_MAX_FRAGMENT )
						continue; /* oversize, drop */

					/* Build inbound packet */
					pkt.arrival_time = Sys_DoubleTime();
					memset( &pkt.from, 0, sizeof( pkt.from ));
					NetThread_SockadrToNetadr( &addr, &pkt.from );
					pkt.length = (size_t)recv_ret;
					memcpy( pkt.data, buf, recv_ret );

				/* Determine which netsrc_t this socket belongs to */
				ns = NetThread_DetermineNetsrc( sock_fd, ip4, ip6 );

				/* Track received packet stats */
				net_thread.inbound_received[ns]++;
				net_thread.inbound_bytes[ns] += (uint64_t)recv_ret;

				/* Push into inbound SPSC queue */
				if( !NetQueue_PushInbound( &net_thread.queues[ns].inbound, &pkt ))
				{
					/* Queue full, increment dropped counter */
					net_thread.packets_dropped++;
					net_thread.inbound_drops[ns]++;
				}
				}
			}
		}

drain_outbound:
		/* Drain outbound queues */
		for( i = 0; i < NS_COUNT; i++ )
		{
			net_outbound_packet_t out;

			while( NetQueue_PopOutbound( &net_thread.queues[i].outbound, &out ))
			{
				struct sockaddr_storage	dest_addr;
				int			net_socket = 0;
				netadrtype_t		addr_type = NET_NetadrType( &out.to );

				memset( &dest_addr, 0, sizeof( dest_addr ));

				/* Resolve socket fd from sock type and address type */
				if( addr_type == NA_BROADCAST || addr_type == NA_IP )
				{
					net_socket = ip4[out.sock];
				}
				else if( addr_type == NA_MULTICAST_IP6 || addr_type == NA_IP6 )
				{
					net_socket = ip6[out.sock];
				}

				if( !NET_IsSocketValid( net_socket ))
					continue;

				/* Convert netadr_t to sockaddr_storage.
				   NET_NetadrToSockadr is static in net_ws.c, so we duplicate the logic here.
				   This function has no global state dependencies. */
				if( addr_type == NA_BROADCAST )
				{
					dest_addr.ss_family = AF_INET;
					((struct sockaddr_in *)&dest_addr)->sin_port = out.to.port;
					((struct sockaddr_in *)&dest_addr)->sin_addr.s_addr = INADDR_BROADCAST;
				}
				else if( addr_type == NA_IP )
				{
					dest_addr.ss_family = AF_INET;
					((struct sockaddr_in *)&dest_addr)->sin_port = out.to.port;
					((struct sockaddr_in *)&dest_addr)->sin_addr.s_addr = out.to.ip4;
				}
				else if( addr_type == NA_IP6 )
				{
					dest_addr.ss_family = AF_INET6;
					((struct sockaddr_in6 *)&dest_addr)->sin6_port = out.to.port;
					NET_NetadrToIP6Bytes(((struct sockaddr_in6 *)&dest_addr)->sin6_addr.s6_addr, &out.to );
				}
				else if( addr_type == NA_MULTICAST_IP6 )
				{
					static const uint8_t k_linklocal[16] = { 0xff, 0x02, 0,0,0,0,0,0,0,0,0,0,0,0,0,1 };

					dest_addr.ss_family = AF_INET6;
					((struct sockaddr_in6 *)&dest_addr)->sin6_port = out.to.port;
					memcpy(((struct sockaddr_in6 *)&dest_addr)->sin6_addr.s6_addr, k_linklocal, sizeof( struct in6_addr ));
				}
				else
				{
					continue;
				}

				NetThread_SendFragment( net_socket, out.data, out.length, &dest_addr, NET_SockAddrLen( &dest_addr ), out.splitsize, out.sock );

				/* Track sent packet stats */
				net_thread.outbound_sent[i]++;
				net_thread.outbound_bytes[i] += (uint64_t)out.length;
			}
		}

		/* Accumulate active time (everything after select returned) */
		net_thread.active_time += Sys_DoubleTime() - select_end;
	}
}

/*
=============================================================================

  Public API

=============================================================================
*/

/*
====================
NetThread_Init

  Initialize the enkiTS scheduler and start the network I/O thread.
  Must be called after sockets are created.
====================
*/
void NetThread_Init( void )
{
	struct enkiTaskSchedulerConfig config;
	convar_t *net_thread_cvar;
	int i;

	if( net_thread.active )
		return;

	/* Check cvar */
	net_thread_cvar = Cvar_Get( "net_thread", "1", FCVAR_ARCHIVE, "enable threaded network I/O" );
	if( !net_thread_cvar || net_thread_cvar->value == 0.0f )
		return;

	/* Initialize queues */
	for( i = 0; i < NS_COUNT; i++ )
	{
		NetQueue_InitInbound( &net_thread.queues[i].inbound );
		NetQueue_InitOutbound( &net_thread.queues[i].outbound );
	}

	/* Cache socket fds */
	for( i = 0; i < NS_COUNT; i++ )
	{
		net_thread.ip_sockets[i] = NET_GetIPSocket( (netsrc_t)i );
		net_thread.ip6_sockets[i] = NET_GetIP6Socket( (netsrc_t)i );
	}

	net_thread.packets_dropped = 0;
	net_thread.last_packet_time = 0.0;
	net_thread.sequence_number = 1;
	net_thread.loop_iterations = 0;
	net_thread.active_time = 0.0;
	net_thread.idle_time = 0.0;

	for( i = 0; i < NS_COUNT; i++ )
	{
		net_thread.inbound_received[i] = 0;
		net_thread.outbound_sent[i] = 0;
		net_thread.inbound_drops[i] = 0;
		net_thread.outbound_drops[i] = 0;
		net_thread.inbound_bytes[i] = 0;
		net_thread.outbound_bytes[i] = 0;
	}

	net_thread.running = 1;

	/* Create enkiTS scheduler with 1 task thread */
	net_thread.scheduler = enkiNewTaskScheduler();
	config = enkiGetTaskSchedulerConfig( net_thread.scheduler );
	config.numTaskThreadsToCreate = 1;
	enkiInitTaskSchedulerWithConfig( net_thread.scheduler, config );

	/* Create pinned task on thread 1 (the worker thread) */
	net_thread.pinned_task = enkiCreatePinnedTask( net_thread.scheduler, NetThread_IOLoop, 1 );
	enkiAddPinnedTask( net_thread.scheduler, net_thread.pinned_task );

	net_thread.active = true;
}

/*
====================
NetThread_Shutdown

  Stop the network thread and clean up.
  Must be called before closing sockets.
====================
*/
void NetThread_Shutdown( void )
{
	if( !net_thread.active )
		return;

	/* Signal the thread to stop */
	net_thread.running = 0;

	/* Wait for the pinned task to complete (1ms select timeout ensures quick exit) */
	enkiWaitForPinnedTask( net_thread.scheduler, net_thread.pinned_task );

	/* Close duplicated thread handle */
#if XASH_WIN32
	if( net_thread.thread_handle )
	{
		CloseHandle( net_thread.thread_handle );
		net_thread.thread_handle = NULL;
	}
#endif

	/* Clean up enkiTS resources */
	enkiDeletePinnedTask( net_thread.scheduler, net_thread.pinned_task );
	net_thread.pinned_task = NULL;

	enkiWaitforAllAndShutdown( net_thread.scheduler );
	enkiDeleteTaskScheduler( net_thread.scheduler );
	net_thread.scheduler = NULL;

	net_thread.active = false;
}

/*
====================
NetThread_IsActive

  Returns true if the network thread is running.
====================
*/
qboolean NetThread_IsActive( void )
{
	return net_thread.active;
}

/*
====================
NetThread_RecvPacket

  Pop one inbound packet from the SPSC queue for the given socket type.
  Returns true if a packet was available.
====================
*/
qboolean NetThread_RecvPacket( netsrc_t sock, netadr_t *from, byte *data, size_t *length )
{
	net_inbound_packet_t pkt;

	if( sock < 0 || sock >= NS_COUNT )
		return false;

	if( !NetQueue_PopInbound( &net_thread.queues[sock].inbound, &pkt ))
		return false;

	/* Copy data out */
	memcpy( from, &pkt.from, sizeof( *from ));
	memcpy( data, pkt.data, pkt.length );
	*length = pkt.length;

	/* Update last packet arrival time */
	net_thread.last_packet_time = pkt.arrival_time;

	return true;
}

/*
====================
NetThread_SendPacket

  Push one outbound packet into the SPSC queue.
  The network thread will handle sendto() and fragmentation.
====================
*/
void NetThread_SendPacket( netsrc_t sock, size_t length, const void *data, netadr_t to, size_t splitsize )
{
	net_outbound_packet_t out;

	if( sock < 0 || sock >= NS_COUNT )
		return;

	if( length > NET_MAX_FRAGMENT )
		return;

	memset( &out, 0, sizeof( out ));
	memcpy( &out.to, &to, sizeof( out.to ));
	out.length = length;
	out.sock = sock;
	out.splitsize = splitsize;
	memcpy( out.data, data, length );

	if( !NetQueue_PushOutbound( &net_thread.queues[sock].outbound, &out ))
	{
		/* Queue full, packet dropped */
		net_thread.packets_dropped++;
		net_thread.outbound_drops[sock]++;
	}
}

/*
====================
NetThread_GetLastPacketTime

  Returns the arrival time of the last popped inbound packet.
====================
*/
double NetThread_GetLastPacketTime( void )
{
	return net_thread.last_packet_time;
}

/*
====================
NetThread_SocketsUpdated

  Refresh cached socket fds after socket recreation.
====================
*/
void NetThread_SocketsUpdated( void )
{
	int i;

	if( !net_thread.active )
		return;

	for( i = 0; i < NS_COUNT; i++ )
	{
		net_thread.ip_sockets[i] = NET_GetIPSocket( (netsrc_t)i );
		net_thread.ip6_sockets[i] = NET_GetIP6Socket( (netsrc_t)i );
	}
}

/*
====================
NetThread_QueryThreadCPUTime

  Query the cumulative CPU time (kernel + user) for the network thread
  using the OS thread handle captured at IOLoop start.
  Returns seconds of CPU time, or -1.0 if not available.
====================
*/
static double NetThread_QueryThreadCPUTime( void )
{
#if XASH_WIN32
	FILETIME creation, exit, kernel, user;
	ULARGE_INTEGER k, u;

	if( !net_thread.thread_handle )
		return -1.0;

	if( !GetThreadTimes( net_thread.thread_handle, &creation, &exit, &kernel, &user ))
		return -1.0;

	k.LowPart  = kernel.dwLowDateTime;
	k.HighPart  = kernel.dwHighDateTime;
	u.LowPart  = user.dwLowDateTime;
	u.HighPart  = user.dwHighDateTime;

	/* FILETIME is in 100-nanosecond intervals */
	return (double)( k.QuadPart + u.QuadPart ) * 1.0e-7;
#elif XASH_LINUX
	char path[64];
	FILE *f;
	unsigned long utime, stime;
	long clk_tck;
	int n;

	if( net_thread.thread_tid <= 0 )
		return -1.0;

	Q_snprintf( path, sizeof( path ), "/proc/self/task/%d/stat", (int)net_thread.thread_tid );
	f = fopen( path, "r" );
	if( !f )
		return -1.0;

	/* Fields: pid (comm) state ... field 14=utime, field 15=stime */
	n = fscanf( f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
		&utime, &stime );
	fclose( f );

	if( n != 2 )
		return -1.0;

	clk_tck = sysconf( _SC_CLK_TCK );
	if( clk_tck <= 0 )
		clk_tck = 100;

	return (double)( utime + stime ) / (double)clk_tck;
#else
	return -1.0;
#endif
}

/*
====================
NetThread_GetStats

  Snapshot all statistics into the provided struct.
  Uses acquire-load semantics for queue counts; all volatile
  uint32_t/uint64_t reads are naturally atomic on supported platforms.
====================
*/
void NetThread_GetStats( net_thread_stats_t *stats )
{
	int i;

	if( !stats )
		return;

	for( i = 0; i < NS_COUNT; i++ )
	{
		stats->inbound_count[i]    = NetQueue_CountInbound( &net_thread.queues[i].inbound );
		stats->outbound_count[i]   = NetQueue_CountOutbound( &net_thread.queues[i].outbound );
		stats->inbound_received[i] = net_thread.inbound_received[i];
		stats->outbound_sent[i]    = net_thread.outbound_sent[i];
		stats->inbound_drops[i]    = net_thread.inbound_drops[i];
		stats->outbound_drops[i]   = net_thread.outbound_drops[i];
		stats->inbound_bytes[i]    = net_thread.inbound_bytes[i];
		stats->outbound_bytes[i]   = net_thread.outbound_bytes[i];
	}

	stats->loop_iterations = net_thread.loop_iterations;
	stats->net_thread_cpu_time = NetThread_QueryThreadCPUTime();
	stats->net_active_time = net_thread.active_time;
	stats->net_idle_time = net_thread.idle_time;
}

#endif /* XASH_NET_THREAD */
