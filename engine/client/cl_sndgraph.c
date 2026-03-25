/*
cl_sndgraph.c - sound thread statistics overlay
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
#include "snd_thread.h"
#include "ref_common.h"

#ifdef XASH_NET_THREAD

#if XASH_LINUX
#include <time.h>
#endif

/*
=============================================================================

  Constants and Cvars

=============================================================================
*/

#define SNDGRAPH_TIMINGS		128
#define SNDGRAPH_TIMINGS_MASK		( SNDGRAPH_TIMINGS - 1 )
#define SNDGRAPH_HEIGHT			64
#define SNDGRAPH_WIDTH			SNDGRAPH_TIMINGS
#define SNDGRAPH_CMD_QUEUE_MAX		255 /* SND_CMD_QUEUE_SIZE - 1 */
#define SNDGRAPH_LINE_HEIGHT		13

static CVAR_DEFINE_AUTO( snd_thread_graph, "0", FCVAR_ARCHIVE, "draw sound thread statistics overlay (1=text, 2=text+graph)" );

/*
=============================================================================

  Per-frame sample ring buffer

=============================================================================
*/

typedef struct
{
	uint32_t	cmd_queue_count;		/* command queue fill snapshot */
	uint32_t	cmd_drops_delta;		/* drops this frame */
	float		mix_hz;				/* mix loop frequency */
	float		active_ms;			/* audio thread active ms this frame */
	float		idle_ms;			/* audio thread idle ms this frame */
	int		active_channels;		/* channels with volume */
	int		total_channels;			/* total channel slots */
	int		paintedtime;			/* samples painted */
	int		soundtime;			/* samples played */
	float		main_cpu_pct;			/* main thread CPU % */
	float		main_active_ms;			/* main thread active ms */
	float		main_idle_ms;			/* main thread idle ms */
	float		snd_cpu_pct;			/* sound thread CPU % */
	float		snd_active_ms;			/* sound thread active ms (windowed) */
	float		snd_idle_ms;			/* sound thread idle ms (windowed) */
} sndgraph_sample_t;

static sndgraph_sample_t	sg_samples[SNDGRAPH_TIMINGS];
static int			sg_sample_index;
static snd_thread_stats_t	sg_prev_stats;
static double			sg_prev_time;
static qboolean			sg_prev_valid;

/* CPU measurement sliding window (0.5s) */
#define SNDGRAPH_CPU_WINDOW	0.5

static double	sg_cpu_window_start;
static double	sg_cpu_main_start;
static double	sg_cpu_snd_start;
static double	sg_cpu_snd_active_start;
static double	sg_cpu_snd_idle_start;

static float	sg_main_cpu_pct;
static float	sg_main_active_ms;
static float	sg_main_idle_ms;
static float	sg_snd_cpu_pct;
static float	sg_snd_active_ms;
static float	sg_snd_idle_ms;
static int	sg_cpu_frame_count;

/* Previous-frame cumulative times */
static double	sg_prev_snd_active_time;
static double	sg_prev_snd_idle_time;

/*
===========
SndGraph_GetMainThreadCPUTime

  Query cumulative CPU time for the calling (main) thread.
===========
*/
static double SndGraph_GetMainThreadCPUTime( void )
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

static const rgba_t sg_text_color = { 229, 229, 178, 255 };

static const byte sg_color_queue[4]      = {  63, 255,  63, 180 };	/* green - queue fill */
static const byte sg_color_active[4]     = {  63, 200, 255, 180 };	/* cyan - active time */
static const byte sg_color_drop[4]       = { 255,   0,   0, 255 };	/* red - drops */
static const byte sg_color_peak[4]       = { 255, 255,   0, 150 };	/* yellow - peak */
static const byte sg_color_background[4] = {   0,   0,   0, 128 };	/* dark transparent */

/*
=============================================================================

  Sampling

=============================================================================
*/

static void SndGraph_Sample( void )
{
	snd_thread_stats_t	cur;
	sndgraph_sample_t	*s;
	double			now, dt;
	double			main_cpu, snd_cpu;

	if( !SndThread_IsActive() )
		return;

	SndThread_GetStats( &cur );
	main_cpu = SndGraph_GetMainThreadCPUTime();
	snd_cpu = cur.snd_thread_cpu_time;
	now = Sys_DoubleTime();
	dt = now - sg_prev_time;

	s = &sg_samples[sg_sample_index & SNDGRAPH_TIMINGS_MASK];
	memset( s, 0, sizeof( *s ));

	/* Instantaneous values */
	s->cmd_queue_count = cur.cmd_queue_count;
	s->active_channels = cur.active_channels;
	s->total_channels = cur.total_channels_snap;
	s->paintedtime = cur.paintedtime_snap;
	s->soundtime = cur.soundtime_snap;

	/* Compute rates from deltas */
	if( sg_prev_valid && dt > 0.0001 )
	{
		float inv_dt = (float)( 1.0 / dt );

		s->mix_hz = (float)( cur.mix_iterations - sg_prev_stats.mix_iterations ) * inv_dt;
		s->cmd_drops_delta = cur.cmd_drops - sg_prev_stats.cmd_drops;

		/* Per-frame active/idle in ms */
		{
			double active_delta = cur.active_time - sg_prev_snd_active_time;
			double idle_delta = cur.idle_time - sg_prev_snd_idle_time;
			s->active_ms = (float)( active_delta * 1000.0 );
			s->idle_ms = (float)( idle_delta * 1000.0 );
		}
	}

	/* CPU sliding window */
	sg_cpu_frame_count++;
	{
		double window_dt = now - sg_cpu_window_start;

		if( window_dt >= SNDGRAPH_CPU_WINDOW && sg_cpu_frame_count > 0 )
		{
			double avg_frame_ms = window_dt / sg_cpu_frame_count * 1000.0;

			/* Main thread CPU */
			if( main_cpu >= 0.0 && sg_cpu_main_start >= 0.0 )
			{
				double cpu_delta = main_cpu - sg_cpu_main_start;
				double avg_active = cpu_delta / sg_cpu_frame_count * 1000.0;

				sg_main_cpu_pct = (float)( cpu_delta / window_dt * 100.0 );
				sg_main_active_ms = (float)avg_active;
				sg_main_idle_ms = (float)( avg_frame_ms - avg_active );
				if( sg_main_idle_ms < 0.0f ) sg_main_idle_ms = 0.0f;
			}

			/* Sound thread CPU */
			if( snd_cpu >= 0.0 && sg_cpu_snd_start >= 0.0 )
			{
				double cpu_delta = snd_cpu - sg_cpu_snd_start;
				sg_snd_cpu_pct = (float)( cpu_delta / window_dt * 100.0 );
			}

			/* Sound thread active/idle (windowed) */
			{
				double active_delta = cur.active_time - sg_cpu_snd_active_start;
				double idle_delta = cur.idle_time - sg_cpu_snd_idle_start;

				sg_snd_active_ms = (float)( active_delta / sg_cpu_frame_count * 1000.0 );
				sg_snd_idle_ms = (float)( idle_delta / sg_cpu_frame_count * 1000.0 );
			}

			/* Reset window */
			sg_cpu_window_start = now;
			sg_cpu_main_start = main_cpu;
			sg_cpu_snd_start = snd_cpu;
			sg_cpu_snd_active_start = cur.active_time;
			sg_cpu_snd_idle_start = cur.idle_time;
			sg_cpu_frame_count = 0;
		}
	}

	/* Copy windowed CPU values into sample */
	s->main_cpu_pct = sg_main_cpu_pct;
	s->main_active_ms = sg_main_active_ms;
	s->main_idle_ms = sg_main_idle_ms;
	s->snd_cpu_pct = sg_snd_cpu_pct;
	s->snd_active_ms = sg_snd_active_ms;
	s->snd_idle_ms = sg_snd_idle_ms;

	memcpy( &sg_prev_stats, &cur, sizeof( sg_prev_stats ));
	sg_prev_snd_active_time = cur.active_time;
	sg_prev_snd_idle_time = cur.idle_time;
	sg_prev_time = now;
	sg_prev_valid = true;
	sg_sample_index++;
}

/*
=============================================================================

  Drawing primitives

=============================================================================
*/

static void SndGraph_DrawRect( int x, int y, int w, int h, const byte color[4] )
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

static void SndGraph_DrawGraph( int gx, int gy )
{
	int	a;
	int	latest = sg_sample_index - 1;

	/* Background */
	SndGraph_DrawRect( gx, gy, SNDGRAPH_WIDTH, SNDGRAPH_HEIGHT, sg_color_background );

	/* Peak marker */
	{
		int peak_h = (int)( (float)sg_prev_stats.cmd_queue_peak / SNDGRAPH_CMD_QUEUE_MAX * SNDGRAPH_HEIGHT );
		if( peak_h > 0 && peak_h < SNDGRAPH_HEIGHT )
			SndGraph_DrawRect( gx, gy + SNDGRAPH_HEIGHT - peak_h, SNDGRAPH_WIDTH, 1, sg_color_peak );
	}

	/* Draw columns right-to-left (newest on right) */
	for( a = 0; a < SNDGRAPH_WIDTH; a++ )
	{
		const sndgraph_sample_t *s;
		int	idx = ( latest - ( SNDGRAPH_WIDTH - 1 - a )) & SNDGRAPH_TIMINGS_MASK;
		int	col_x, bar_y, h;

		s = &sg_samples[idx];
		col_x = gx + a;
		bar_y = gy + SNDGRAPH_HEIGHT;

		/* Command queue fill (green) */
		h = (int)( (float)s->cmd_queue_count / SNDGRAPH_CMD_QUEUE_MAX * SNDGRAPH_HEIGHT * 0.5f );
		if( h > 0 )
		{
			bar_y -= h;
			SndGraph_DrawRect( col_x, bar_y, 1, h, sg_color_queue );
		}

		/* Active mix time (cyan) - scale: 5ms = full height */
		h = (int)( s->active_ms / 5.0f * SNDGRAPH_HEIGHT * 0.5f );
		if( h > SNDGRAPH_HEIGHT / 2 ) h = SNDGRAPH_HEIGHT / 2;
		if( h > 0 )
		{
			bar_y -= h;
			SndGraph_DrawRect( col_x, bar_y, 1, h, sg_color_active );
		}

		/* Drop indicator (red line at top) */
		if( s->cmd_drops_delta )
			SndGraph_DrawRect( col_x, gy, 1, 2, sg_color_drop );
	}
}

/*
=============================================================================

  Text rendering

=============================================================================
*/

static void SndGraph_DrawTextFields( int x, int y, const sndgraph_sample_t *s )
{
	cl_font_t	*font = Con_GetFont( 0 );
	int		line_y = y;

	CL_SetFontRendermode( font );

	/* Header */
	if( SndThread_IsActive() )
	{
		CL_DrawStringf( font, x, line_y, sg_text_color, FONT_DRAW_NORENDERMODE,
			"SND THREAD [active] %d Hz", (int)s->mix_hz );
	}
	else
	{
		CL_DrawStringf( font, x, line_y, sg_text_color, FONT_DRAW_NORENDERMODE,
			"SND THREAD [inactive]" );
		return;
	}
	line_y += SNDGRAPH_LINE_HEIGHT;

	/* Main thread CPU */
	CL_DrawStringf( font, x, line_y, sg_text_color, FONT_DRAW_NORENDERMODE,
		"Main: CPU %4.1f%%  active %5.1f ms  idle %5.1f ms",
		s->main_cpu_pct, s->main_active_ms, s->main_idle_ms );
	line_y += SNDGRAPH_LINE_HEIGHT;

	/* Sound thread CPU */
	CL_DrawStringf( font, x, line_y, sg_text_color, FONT_DRAW_NORENDERMODE,
		"Snd:  CPU %4.1f%%  active %5.1f ms  idle %5.1f ms",
		s->snd_cpu_pct, s->snd_active_ms, s->snd_idle_ms );
	line_y += SNDGRAPH_LINE_HEIGHT;

	/* Command queue */
	CL_DrawStringf( font, x, line_y, sg_text_color, FONT_DRAW_NORENDERMODE,
		"Cmd Q: %3u/%-3u (peak %u)  drops: %u",
		s->cmd_queue_count, SNDGRAPH_CMD_QUEUE_MAX,
		sg_prev_stats.cmd_queue_peak, sg_prev_stats.cmd_drops );
	line_y += SNDGRAPH_LINE_HEIGHT;

	/* Mix stats */
	CL_DrawStringf( font, x, line_y, sg_text_color, FONT_DRAW_NORENDERMODE,
		"Channels: %d/%d  painted: %d  sound: %d",
		s->active_channels, s->total_channels,
		s->paintedtime, s->soundtime );
	line_y += SNDGRAPH_LINE_HEIGHT;
}

/*
=============================================================================

  Public API

=============================================================================
*/

void SCR_DrawSndGraph( void )
{
	int		x, y, text_lines, graphtype;
	const sndgraph_sample_t	*s;

	if( snd_thread_graph.value == 0.0f )
		return;

	if( cls.state != ca_active )
		return;

	graphtype = (int)snd_thread_graph.value;

	/* Sample this frame */
	SndGraph_Sample();

	/* Get latest sample */
	s = &sg_samples[( sg_sample_index - 1 ) & SNDGRAPH_TIMINGS_MASK];

	/* Position: bottom-left, offset right of net_thread_graph area */
	text_lines = 5; /* header + 2 CPU + cmd queue + mix stats */
	x = 5;
	y = refState.height - ( text_lines * SNDGRAPH_LINE_HEIGHT ) - 10;

	/* Draw graph below text if mode 2 */
	if( graphtype >= 2 )
		y -= ( SNDGRAPH_HEIGHT + 4 );

	/* Draw text */
	SndGraph_DrawTextFields( x, y, s );

	/* Draw graph */
	if( graphtype >= 2 )
	{
		SndGraph_DrawGraph( x, y + text_lines * SNDGRAPH_LINE_HEIGHT + 2 );
	}
}

void CL_InitSndGraph( void )
{
	Cvar_RegisterVariable( &snd_thread_graph );

	sg_sample_index = 0;
	sg_prev_valid = false;
	sg_prev_time = 0.0;
	memset( sg_samples, 0, sizeof( sg_samples ));
	memset( &sg_prev_stats, 0, sizeof( sg_prev_stats ));
	sg_prev_snd_active_time = 0.0;
	sg_prev_snd_idle_time = 0.0;

	/* CPU window init */
	sg_cpu_window_start = 0.0;
	sg_cpu_main_start = -1.0;
	sg_cpu_snd_start = -1.0;
	sg_cpu_snd_active_start = 0.0;
	sg_cpu_snd_idle_start = 0.0;
	sg_cpu_frame_count = 0;
	sg_main_cpu_pct = 0.0f;
	sg_main_active_ms = 0.0f;
	sg_main_idle_ms = 0.0f;
	sg_snd_cpu_pct = 0.0f;
	sg_snd_active_ms = 0.0f;
	sg_snd_idle_ms = 0.0f;
}

#else /* !XASH_NET_THREAD */

void SCR_DrawSndGraph( void ) {}
void CL_InitSndGraph( void ) {}

#endif /* XASH_NET_THREAD */
