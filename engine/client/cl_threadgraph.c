/*
cl_threadgraph.c - network thread statistics overlay
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

#include "common.h"
#include "client.h"
#include "net_thread.h"
#include "ref_common.h"

#ifdef XASH_NET_THREAD

#include "net_spsc_queue.h" /* for NET_SPSC_QUEUE_SIZE */

#if XASH_LINUX
#include <time.h>
#endif

/*
=============================================================================

  Constants and Cvars

=============================================================================
*/

#define THREADGRAPH_TIMINGS		128
#define THREADGRAPH_TIMINGS_MASK	( THREADGRAPH_TIMINGS - 1 )
#define THREADGRAPH_HEIGHT		64
#define THREADGRAPH_WIDTH		THREADGRAPH_TIMINGS
#define THREADGRAPH_QUEUE_MAX		( NET_SPSC_QUEUE_SIZE - 1 ) /* 511 usable slots */
#define THREADGRAPH_LINE_HEIGHT		13

static CVAR_DEFINE_AUTO( net_thread_graph, "0", FCVAR_ARCHIVE, "draw network thread statistics overlay (1=graph+text, 2=text only)" );

/*
=============================================================================

  Per-frame sample ring buffer

=============================================================================
*/

typedef struct
{
	uint32_t	inbound_count[NS_COUNT];	/* queue fill snapshot */
	uint32_t	outbound_count[NS_COUNT];
	float		inbound_pps[NS_COUNT];		/* packets/sec */
	float		outbound_pps[NS_COUNT];
	float		inbound_kbps[NS_COUNT];		/* KB/sec */
	float		outbound_kbps[NS_COUNT];
	uint32_t	inbound_drops_delta[NS_COUNT];	/* drops this frame */
	uint32_t	outbound_drops_delta[NS_COUNT];
	float		loop_hz;			/* IO loop frequency */
	float		loopback_recv_pps[NS_COUNT];	/* loopback recv packets/sec */
	float		loopback_send_pps[NS_COUNT];	/* loopback send packets/sec */
	float		loopback_recv_kbps[NS_COUNT];	/* loopback recv KB/sec */
	float		loopback_send_kbps[NS_COUNT];	/* loopback send KB/sec */
	float		main_cpu_pct;			/* main thread CPU usage % */
	float		main_active_ms;			/* main thread active ms per frame */
	float		main_idle_ms;			/* main thread idle ms per frame */
	float		net_cpu_pct;			/* net thread CPU usage % */
	float		net_active_ms;			/* net thread active ms per frame */
	float		net_idle_ms;			/* net thread idle ms per frame */
} threadgraph_sample_t;

static threadgraph_sample_t	tg_samples[THREADGRAPH_TIMINGS];
static int			tg_sample_index;
static net_thread_stats_t	tg_prev_stats;
static double			tg_prev_time;
static qboolean			tg_prev_valid;

/* Peak queue fill tracking */
static uint32_t			tg_peak_inbound[NS_COUNT];
static uint32_t			tg_peak_outbound[NS_COUNT];

/* Previous-frame loopback stats for delta computation */
static net_loopback_stats_t	tg_prev_loopback;

/* CPU measurement uses a sliding window (0.5s) to overcome
   GetThreadTimes resolution (~15.6ms on Windows) */
#define THREADGRAPH_CPU_WINDOW	0.5	/* seconds between CPU updates */

static double			tg_cpu_window_start;	/* wall time at window start */
static double			tg_cpu_main_start;	/* main thread CPU time at window start */
static double			tg_cpu_net_start;	/* net thread CPU time at window start */
static double			tg_cpu_net_active_start;/* net thread active time at window start */
static double			tg_cpu_net_idle_start;	/* net thread idle time at window start */

/* Latest computed CPU values (updated every THREADGRAPH_CPU_WINDOW) */
static float			tg_main_cpu_pct;
static float			tg_main_active_ms;	/* avg active ms per frame in window */
static float			tg_main_idle_ms;	/* avg idle ms per frame in window */
static float			tg_net_cpu_pct;
static float			tg_net_active_ms;	/* avg active ms per frame in window */
static float			tg_net_idle_ms;		/* avg idle ms per frame in window */
static int			tg_cpu_frame_count;	/* frames in current window */

/* Previous-frame net thread cumulative times (for per-frame deltas still used in ring buffer) */
static double			tg_prev_net_active_time;
static double			tg_prev_net_idle_time;

/*
===========
ThreadGraph_GetMainThreadCPUTime

  Query cumulative CPU time (kernel + user) for the calling (main) thread.
  Returns seconds, or -1.0 if not available.
===========
*/
static double ThreadGraph_GetMainThreadCPUTime( void )
{
#if XASH_WIN32
	FILETIME creation, exit, kernel, user;
	ULARGE_INTEGER k, u;

	if( !GetThreadTimes( GetCurrentThread(), &creation, &exit, &kernel, &user ))
		return -1.0;

	k.LowPart  = kernel.dwLowDateTime;
	k.HighPart  = kernel.dwHighDateTime;
	u.LowPart  = user.dwLowDateTime;
	u.HighPart  = user.dwHighDateTime;

	return (double)( k.QuadPart + u.QuadPart ) * 1.0e-7;
#elif XASH_LINUX
	struct timespec ts;

	if( clock_gettime( CLOCK_THREAD_CPUTIME_ID, &ts ) != 0 )
		return -1.0;

	return (double)ts.tv_sec + (double)ts.tv_nsec * 1.0e-9;
#else
	return -1.0;
#endif
}

/*
=============================================================================

  Colors

=============================================================================
*/

/* Text color - same warm white as net_graph */
static const rgba_t tg_text_color = { 229, 229, 178, 255 };

/* Graph bar colors per socket type */
static const byte tg_color_inbound_client[4]  = {  63, 255,  63, 180 };	/* green */
static const byte tg_color_outbound_client[4] = {  63, 200, 255, 180 };	/* cyan */
static const byte tg_color_inbound_server[4]  = { 255, 255,  63, 180 };	/* yellow */
static const byte tg_color_outbound_server[4] = { 255, 180,  63, 180 };	/* orange */
static const byte tg_color_drop[4]            = { 255,   0,   0, 255 };	/* red */
static const byte tg_color_peak[4]            = { 255, 255,   0, 150 };	/* yellow */
static const byte tg_color_background[4]      = {   0,   0,   0, 128 };	/* dark transparent */

/*
=============================================================================

  Sampling

=============================================================================
*/

/*
===========
ThreadGraph_Sample

  Called once per frame. Snapshots stats, computes delta-based rates.
===========
*/
static void ThreadGraph_Sample( void )
{
	net_thread_stats_t	cur;
	net_loopback_stats_t	lb;
	threadgraph_sample_t	*s;
	double			now, dt;
	double			main_cpu, net_cpu;
	int			i;

	if( !NetThread_IsActive( ))
		return;

	NetThread_GetStats( &cur );
	NET_GetLoopbackStats( &lb );
	main_cpu = ThreadGraph_GetMainThreadCPUTime();
	net_cpu = cur.net_thread_cpu_time;
	now = Sys_DoubleTime();
	dt = now - tg_prev_time;

	s = &tg_samples[tg_sample_index & THREADGRAPH_TIMINGS_MASK];
	memset( s, 0, sizeof( *s ));

	/* Queue fill levels are instantaneous */
	for( i = 0; i < NS_COUNT; i++ )
	{
		s->inbound_count[i] = cur.inbound_count[i];
		s->outbound_count[i] = cur.outbound_count[i];

		/* Track peaks */
		if( cur.inbound_count[i] > tg_peak_inbound[i] )
			tg_peak_inbound[i] = cur.inbound_count[i];
		if( cur.outbound_count[i] > tg_peak_outbound[i] )
			tg_peak_outbound[i] = cur.outbound_count[i];
	}

	/* Compute rates from deltas */
	if( tg_prev_valid && dt > 0.0001 )
	{
		float inv_dt = (float)( 1.0 / dt );

		for( i = 0; i < NS_COUNT; i++ )
		{
			s->inbound_pps[i]  = (float)( cur.inbound_received[i] - tg_prev_stats.inbound_received[i] ) * inv_dt;
			s->outbound_pps[i] = (float)( cur.outbound_sent[i]    - tg_prev_stats.outbound_sent[i] )    * inv_dt;
			s->inbound_kbps[i] = (float)( cur.inbound_bytes[i]    - tg_prev_stats.inbound_bytes[i] )    * inv_dt / 1024.0f;
			s->outbound_kbps[i]= (float)( cur.outbound_bytes[i]   - tg_prev_stats.outbound_bytes[i] )   * inv_dt / 1024.0f;
			s->inbound_drops_delta[i]  = cur.inbound_drops[i]  - tg_prev_stats.inbound_drops[i];
			s->outbound_drops_delta[i] = cur.outbound_drops[i] - tg_prev_stats.outbound_drops[i];
		}

		s->loop_hz = (float)( cur.loop_iterations - tg_prev_stats.loop_iterations ) * inv_dt;

		/* Loopback rates */
		for( i = 0; i < NS_COUNT; i++ )
		{
			s->loopback_recv_pps[i]  = (float)( lb.packets_recv[i] - tg_prev_loopback.packets_recv[i] ) * inv_dt;
			s->loopback_send_pps[i]  = (float)( lb.packets_sent[i] - tg_prev_loopback.packets_sent[i] ) * inv_dt;
			s->loopback_recv_kbps[i] = (float)( lb.bytes_recv[i]   - tg_prev_loopback.bytes_recv[i] )   * inv_dt / 1024.0f;
			s->loopback_send_kbps[i] = (float)( lb.bytes_sent[i]   - tg_prev_loopback.bytes_sent[i] )   * inv_dt / 1024.0f;
		}

		/* Net thread active/idle in milliseconds per frame */
		{
			double active_delta = cur.net_active_time - tg_prev_net_active_time;
			double idle_delta = cur.net_idle_time - tg_prev_net_idle_time;

			s->net_active_ms = (float)( active_delta * 1000.0 );
			s->net_idle_ms = (float)( idle_delta * 1000.0 );
		}
	}

	/* CPU window: accumulate frames and update averages every THREADGRAPH_CPU_WINDOW seconds.
	   GetThreadTimes has ~15.6ms resolution on Windows, so per-frame deltas are useless. */
	tg_cpu_frame_count++;
	{
		double window_dt = now - tg_cpu_window_start;

		if( window_dt >= THREADGRAPH_CPU_WINDOW && tg_cpu_frame_count > 0 )
		{
			double avg_frame_ms = window_dt / tg_cpu_frame_count * 1000.0;

			/* Main thread CPU */
			if( main_cpu >= 0.0 && tg_cpu_main_start >= 0.0 )
			{
				double cpu_delta = main_cpu - tg_cpu_main_start;
				double avg_active = cpu_delta / tg_cpu_frame_count * 1000.0;

				tg_main_cpu_pct = (float)( cpu_delta / window_dt * 100.0 );
				tg_main_active_ms = (float)avg_active;
				tg_main_idle_ms = (float)( avg_frame_ms - avg_active );
				if( tg_main_idle_ms < 0.0f ) tg_main_idle_ms = 0.0f;
			}

			/* Net thread CPU */
			if( net_cpu >= 0.0 && tg_cpu_net_start >= 0.0 )
			{
				double cpu_delta = net_cpu - tg_cpu_net_start;
				tg_net_cpu_pct = (float)( cpu_delta / window_dt * 100.0 );
			}

			/* Net thread active/idle (high-res, from Sys_DoubleTime instrumentation) */
			{
				double active_delta = cur.net_active_time - tg_cpu_net_active_start;
				double idle_delta = cur.net_idle_time - tg_cpu_net_idle_start;
				double avg_active = active_delta / tg_cpu_frame_count * 1000.0;
				double avg_idle = idle_delta / tg_cpu_frame_count * 1000.0;

				tg_net_active_ms = (float)avg_active;
				tg_net_idle_ms = (float)avg_idle;
			}

			/* Reset window */
			tg_cpu_window_start = now;
			tg_cpu_main_start = main_cpu;
			tg_cpu_net_start = net_cpu;
			tg_cpu_net_active_start = cur.net_active_time;
			tg_cpu_net_idle_start = cur.net_idle_time;
			tg_cpu_frame_count = 0;
		}
	}

	/* Copy windowed CPU values into the sample for display */
	s->main_cpu_pct = tg_main_cpu_pct;
	s->main_active_ms = tg_main_active_ms;
	s->main_idle_ms = tg_main_idle_ms;
	s->net_cpu_pct = tg_net_cpu_pct;
	/* net_active_ms and net_idle_ms already set per-frame above;
	   overwrite with windowed averages for smoother display */
	s->net_active_ms = tg_net_active_ms;
	s->net_idle_ms = tg_net_idle_ms;

	memcpy( &tg_prev_stats, &cur, sizeof( tg_prev_stats ));
	memcpy( &tg_prev_loopback, &lb, sizeof( tg_prev_loopback ));
	tg_prev_net_active_time = cur.net_active_time;
	tg_prev_net_idle_time = cur.net_idle_time;
	tg_prev_time = now;
	tg_prev_valid = true;
	tg_sample_index++;
}

/*
=============================================================================

  Drawing primitives

=============================================================================
*/

/*
===========
ThreadGraph_DrawRect

  Draw a solid colored quad. Same pattern as NetGraph_DrawRect.
  wrect_t fields: left=x, top=y, right=width, bottom=height
===========
*/
static void ThreadGraph_DrawRect( int x, int y, int w, int h, const byte color[4] )
{
	ref.dllFuncs.Color4ub( color[0], color[1], color[2], color[3] );
	ref.dllFuncs.Vertex3f( (float)x, (float)y, 0 );
	ref.dllFuncs.Vertex3f( (float)( x + w ), (float)y, 0 );
	ref.dllFuncs.Vertex3f( (float)( x + w ), (float)( y + h ), 0 );
	ref.dllFuncs.Vertex3f( (float)x, (float)( y + h ), 0 );
}

/*
=============================================================================

  Graph rendering

=============================================================================
*/

/*
===========
ThreadGraph_DrawGraph

  Draw the scrolling bar chart showing queue fill levels over time.
  Each column represents one frame sample.
===========
*/
static void ThreadGraph_DrawGraph( int gx, int gy )
{
	int	a, col_x;
	int	latest = tg_sample_index - 1;

	/* Background */
	ThreadGraph_DrawRect( gx, gy, THREADGRAPH_WIDTH, THREADGRAPH_HEIGHT, tg_color_background );

	/* Draw columns right-to-left (newest on right) */
	for( a = 0; a < THREADGRAPH_WIDTH; a++ )
	{
		const threadgraph_sample_t *s;
		int	idx = ( latest - ( THREADGRAPH_WIDTH - 1 - a )) & THREADGRAPH_TIMINGS_MASK;
		int	bar_y;
		int	h;
		qboolean had_drops = false;

		s = &tg_samples[idx];
		col_x = gx + a;
		bar_y = gy + THREADGRAPH_HEIGHT; /* start from bottom */

		/* Client inbound (green) */
		h = (int)( (float)s->inbound_count[NS_CLIENT] / THREADGRAPH_QUEUE_MAX * THREADGRAPH_HEIGHT * 0.25f );
		if( h > 0 )
		{
			bar_y -= h;
			ThreadGraph_DrawRect( col_x, bar_y, 1, h, tg_color_inbound_client );
		}

		/* Client outbound (cyan) */
		h = (int)( (float)s->outbound_count[NS_CLIENT] / THREADGRAPH_QUEUE_MAX * THREADGRAPH_HEIGHT * 0.25f );
		if( h > 0 )
		{
			bar_y -= h;
			ThreadGraph_DrawRect( col_x, bar_y, 1, h, tg_color_outbound_client );
		}

		/* Server inbound (yellow) */
		h = (int)( (float)s->inbound_count[NS_SERVER] / THREADGRAPH_QUEUE_MAX * THREADGRAPH_HEIGHT * 0.25f );
		if( h > 0 )
		{
			bar_y -= h;
			ThreadGraph_DrawRect( col_x, bar_y, 1, h, tg_color_inbound_server );
		}

		/* Server outbound (orange) */
		h = (int)( (float)s->outbound_count[NS_SERVER] / THREADGRAPH_QUEUE_MAX * THREADGRAPH_HEIGHT * 0.25f );
		if( h > 0 )
		{
			bar_y -= h;
			ThreadGraph_DrawRect( col_x, bar_y, 1, h, tg_color_outbound_server );
		}

		/* Drop indicator (red line at top) */
		if( s->inbound_drops_delta[NS_CLIENT] || s->outbound_drops_delta[NS_CLIENT]
		 || s->inbound_drops_delta[NS_SERVER] || s->outbound_drops_delta[NS_SERVER] )
		{
			ThreadGraph_DrawRect( col_x, gy, 1, 2, tg_color_drop );
		}
	}

	/* Peak markers: horizontal lines showing historical peak fill */
	{
		int peak_total, peak_h;

		/* Client peak */
		peak_total = tg_peak_inbound[NS_CLIENT] + tg_peak_outbound[NS_CLIENT];
		peak_h = (int)( (float)peak_total / THREADGRAPH_QUEUE_MAX * THREADGRAPH_HEIGHT * 0.25f );
		if( peak_h > 0 && peak_h < THREADGRAPH_HEIGHT )
			ThreadGraph_DrawRect( gx, gy + THREADGRAPH_HEIGHT - peak_h, THREADGRAPH_WIDTH, 1, tg_color_peak );

		/* Server peak */
		peak_total = tg_peak_inbound[NS_SERVER] + tg_peak_outbound[NS_SERVER];
		peak_h = (int)( (float)peak_total / THREADGRAPH_QUEUE_MAX * THREADGRAPH_HEIGHT * 0.25f );
		if( peak_h > 0 && peak_h < THREADGRAPH_HEIGHT )
			ThreadGraph_DrawRect( gx, gy + THREADGRAPH_HEIGHT - peak_h, THREADGRAPH_WIDTH, 1, tg_color_peak );
	}
}

/*
=============================================================================

  Text rendering

=============================================================================
*/

/*
===========
ThreadGraph_DrawTextFields

  Draw all text statistics above the graph area.
===========
*/
static void ThreadGraph_DrawTextFields( int x, int y, const threadgraph_sample_t *s )
{
	cl_font_t	*font = Con_GetFont( 0 );
	int		line_y = y;

	CL_SetFontRendermode( font );

	/* Header line */
	if( NetThread_IsActive( ))
	{
		CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
			"NET THREAD [active] %d Hz", (int)s->loop_hz );
	}
	else
	{
		CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
			"NET THREAD [inactive]" );
		return;
	}
	line_y += THREADGRAPH_LINE_HEIGHT;

	/* Main thread: CPU%, active ms, idle ms */
	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		"Main: CPU %4.1f%%  active %5.1f ms  idle %5.1f ms",
		s->main_cpu_pct, s->main_active_ms, s->main_idle_ms );
	line_y += THREADGRAPH_LINE_HEIGHT;

	/* Net thread: CPU%, active ms, idle ms */
	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		"Net:  CPU %4.1f%%  active %5.1f ms  idle %5.1f ms",
		s->net_cpu_pct, s->net_active_ms, s->net_idle_ms );
	line_y += THREADGRAPH_LINE_HEIGHT;

	/* CLIENT section */
	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		"--- CLIENT ---" );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Q IN: %3u/%-3u (pk %u)  OUT: %3u/%-3u (pk %u)",
		s->inbound_count[NS_CLIENT], THREADGRAPH_QUEUE_MAX,
		tg_peak_inbound[NS_CLIENT],
		s->outbound_count[NS_CLIENT], THREADGRAPH_QUEUE_MAX,
		tg_peak_outbound[NS_CLIENT] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Recv: %5.0f pkt/s %6.1f KB/s",
		s->inbound_pps[NS_CLIENT], s->inbound_kbps[NS_CLIENT] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Send: %5.0f pkt/s %6.1f KB/s",
		s->outbound_pps[NS_CLIENT], s->outbound_kbps[NS_CLIENT] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Loop Recv: %5.0f pkt/s %6.1f KB/s",
		s->loopback_recv_pps[NS_CLIENT], s->loopback_recv_kbps[NS_CLIENT] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Loop Send: %5.0f pkt/s %6.1f KB/s",
		s->loopback_send_pps[NS_CLIENT], s->loopback_send_kbps[NS_CLIENT] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Drops: IN %u  OUT %u",
		tg_prev_stats.inbound_drops[NS_CLIENT],
		tg_prev_stats.outbound_drops[NS_CLIENT] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	/* SERVER section */
	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		"--- SERVER ---" );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Q IN: %3u/%-3u (pk %u)  OUT: %3u/%-3u (pk %u)",
		s->inbound_count[NS_SERVER], THREADGRAPH_QUEUE_MAX,
		tg_peak_inbound[NS_SERVER],
		s->outbound_count[NS_SERVER], THREADGRAPH_QUEUE_MAX,
		tg_peak_outbound[NS_SERVER] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Recv: %5.0f pkt/s %6.1f KB/s",
		s->inbound_pps[NS_SERVER], s->inbound_kbps[NS_SERVER] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Send: %5.0f pkt/s %6.1f KB/s",
		s->outbound_pps[NS_SERVER], s->outbound_kbps[NS_SERVER] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Loop Recv: %5.0f pkt/s %6.1f KB/s",
		s->loopback_recv_pps[NS_SERVER], s->loopback_recv_kbps[NS_SERVER] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Loop Send: %5.0f pkt/s %6.1f KB/s",
		s->loopback_send_pps[NS_SERVER], s->loopback_send_kbps[NS_SERVER] );
	line_y += THREADGRAPH_LINE_HEIGHT;

	CL_DrawStringf( font, x, line_y, tg_text_color, FONT_DRAW_NORENDERMODE,
		" Drops: IN %u  OUT %u",
		tg_prev_stats.inbound_drops[NS_SERVER],
		tg_prev_stats.outbound_drops[NS_SERVER] );
}

/*
=============================================================================

  Public API

=============================================================================
*/

/*
===========
SCR_DrawThreadGraph

  Top-level entry point, called from V_PostRender().
===========
*/
void SCR_DrawThreadGraph( void )
{
	int			x, y, text_lines;
	int			graphtype;
	const threadgraph_sample_t	*s;

	if( net_thread_graph.value == 0.0f )
		return;

	if( cls.state != ca_active )
		return;

	graphtype = (int)net_thread_graph.value;

	/* Sample this frame */
	ThreadGraph_Sample();

	/* Get latest sample for text display */
	s = &tg_samples[( tg_sample_index - 1 ) & THREADGRAPH_TIMINGS_MASK];

	/* Position: bottom-left corner (net_graph is bottom-right) */
	text_lines = 17; /* header + 2 CPU lines + 7 lines per socket type (incl. loopback) */
	x = 5;
	y = refState.height - ( text_lines * THREADGRAPH_LINE_HEIGHT ) - 10;

	/* Draw text */
	ThreadGraph_DrawTextFields( x, y, s );
}

/*
===========
CL_InitThreadGraph

  Register cvars. Called from SCR_Init().
===========
*/
void CL_InitThreadGraph( void )
{
	int i;

	Cvar_RegisterVariable( &net_thread_graph );

	tg_sample_index = 0;
	tg_prev_valid = false;
	tg_prev_time = 0.0;
	memset( tg_samples, 0, sizeof( tg_samples ));
	memset( &tg_prev_stats, 0, sizeof( tg_prev_stats ));
	memset( &tg_prev_loopback, 0, sizeof( tg_prev_loopback ));
	tg_prev_net_active_time = 0.0;
	tg_prev_net_idle_time = 0.0;

	/* CPU window init */
	tg_cpu_window_start = 0.0;
	tg_cpu_main_start = -1.0;
	tg_cpu_net_start = -1.0;
	tg_cpu_net_active_start = 0.0;
	tg_cpu_net_idle_start = 0.0;
	tg_cpu_frame_count = 0;
	tg_main_cpu_pct = 0.0f;
	tg_main_active_ms = 0.0f;
	tg_main_idle_ms = 0.0f;
	tg_net_cpu_pct = 0.0f;
	tg_net_active_ms = 0.0f;
	tg_net_idle_ms = 0.0f;

	for( i = 0; i < NS_COUNT; i++ )
	{
		tg_peak_inbound[i] = 0;
		tg_peak_outbound[i] = 0;
	}
}

#else /* !XASH_NET_THREAD */

/*
  Stubs for builds without network threading support
*/
void SCR_DrawThreadGraph( void ) {}
void CL_InitThreadGraph( void ) {}

#endif /* XASH_NET_THREAD */
