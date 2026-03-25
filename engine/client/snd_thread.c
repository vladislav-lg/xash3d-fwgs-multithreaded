/*
snd_thread.c - dedicated audio mixing thread using enkiTS
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

#ifdef XASH_NET_THREAD // reuse the same platform gate as net_thread

#include "common.h"
#include "sound.h"
#include "client.h"
#include "snd_thread.h"
#include "TaskScheduler_c.h"

#if XASH_WIN32
#include <windows.h>
#elif !XASH_DOS
#include <pthread.h>
#endif

#if XASH_SDL == 2
#include <SDL.h>
#endif

/*
=============================================================================

  Command Ring Buffer (lock-free SPSC: main thread writes, audio thread reads)

=============================================================================
*/
#define SND_CMD_QUEUE_SIZE 256 // must be power of 2
#define SND_CMD_QUEUE_MASK ( SND_CMD_QUEUE_SIZE - 1 )

static struct
{
	sndcmd_t     cmds[SND_CMD_QUEUE_SIZE];
	volatile uint32_t head; // written by main thread
	volatile uint32_t tail; // written by audio thread
} snd_cmdqueue;

/*
=============================================================================

  Listener Snapshot Double Buffer

=============================================================================
*/
static snd_snapshot_t snd_snapshots[2];
static volatile int   snd_snapshot_index; // index audio thread should read

/*
=============================================================================

  Audio Thread State

=============================================================================
*/
static struct
{
	enkiTaskScheduler *scheduler;
	enkiPinnedTask    *pinned_task;

	volatile int       running;     // set to 0 to signal shutdown
	qboolean           active;      // is the thread active?
	convar_t          *cvar;        // snd_threaded cvar pointer

	/* Diagnostics counters (written by audio thread, read by main thread) */
	volatile uint32_t  mix_iterations;    // cumulative mix loop count
	volatile uint32_t  cmd_drops;         // cumulative command queue drops
	volatile uint32_t  cmd_queue_peak;    // high water mark of queue fill
	volatile int       active_channels;   // channels with volume last frame
	volatile double    active_time;       // cumulative time doing mix work
	volatile double    idle_time;         // cumulative time in condvar wait

	/* Thread handle for CPU time measurement (captured in MixLoop) */
#if XASH_WIN32
	HANDLE             thread_handle;     // duplicated real handle
#elif XASH_LINUX
	pid_t              thread_tid;        // Linux thread ID from gettid()
#endif

	/* Platform sync: condition variable to wake audio thread */
#if XASH_SDL == 2
	SDL_mutex *mutex;
	SDL_cond  *cond;
#elif XASH_WIN32
	CRITICAL_SECTION cs;
	CONDITION_VARIABLE cv;
#elif !XASH_DOS
	pthread_mutex_t  mutex;
	pthread_cond_t   cond;
#endif
} snd_thread;

/*
=============================================================================

  Condition Variable Helpers

=============================================================================
*/

static void SndThread_SyncInit( void )
{
#if XASH_SDL == 2
	snd_thread.mutex = SDL_CreateMutex();
	snd_thread.cond = SDL_CreateCond();
#elif XASH_WIN32
	InitializeCriticalSection( &snd_thread.cs );
	InitializeConditionVariable( &snd_thread.cv );
#elif !XASH_DOS
	pthread_mutex_init( &snd_thread.mutex, NULL );
	pthread_cond_init( &snd_thread.cond, NULL );
#endif
}

static void SndThread_SyncDestroy( void )
{
#if XASH_SDL == 2
	if( snd_thread.cond ) SDL_DestroyCond( snd_thread.cond );
	if( snd_thread.mutex ) SDL_DestroyMutex( snd_thread.mutex );
	snd_thread.cond = NULL;
	snd_thread.mutex = NULL;
#elif XASH_WIN32
	DeleteCriticalSection( &snd_thread.cs );
	// CONDITION_VARIABLE has no destroy on Win32
#elif !XASH_DOS
	pthread_cond_destroy( &snd_thread.cond );
	pthread_mutex_destroy( &snd_thread.mutex );
#endif
}

static void SndThread_SyncLock( void )
{
#if XASH_SDL == 2
	SDL_LockMutex( snd_thread.mutex );
#elif XASH_WIN32
	EnterCriticalSection( &snd_thread.cs );
#elif !XASH_DOS
	pthread_mutex_lock( &snd_thread.mutex );
#endif
}

static void SndThread_SyncUnlock( void )
{
#if XASH_SDL == 2
	SDL_UnlockMutex( snd_thread.mutex );
#elif XASH_WIN32
	LeaveCriticalSection( &snd_thread.cs );
#elif !XASH_DOS
	pthread_mutex_unlock( &snd_thread.mutex );
#endif
}

/*
====================
SndThread_SyncWait

  Wait on condition variable with ~5ms timeout.
  Audio thread calls this to sleep between mix passes.
====================
*/
static void SndThread_SyncWait( void )
{
#if XASH_SDL == 2
	SDL_CondWaitTimeout( snd_thread.cond, snd_thread.mutex, 5 );
#elif XASH_WIN32
	SleepConditionVariableCS( &snd_thread.cv, &snd_thread.cs, 5 );
#elif !XASH_DOS
	struct timespec ts;
	clock_gettime( CLOCK_REALTIME, &ts );
	ts.tv_nsec += 5000000; // 5ms
	if( ts.tv_nsec >= 1000000000 )
	{
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	pthread_cond_timedwait( &snd_thread.cond, &snd_thread.mutex, &ts );
#endif
}

void SndThread_Signal( void )
{
#if XASH_SDL == 2
	SDL_CondSignal( snd_thread.cond );
#elif XASH_WIN32
	WakeConditionVariable( &snd_thread.cv );
#elif !XASH_DOS
	pthread_cond_signal( &snd_thread.cond );
#endif
}

/*
=============================================================================

  Command Queue Operations

=============================================================================
*/

qboolean SndThread_QueueCommand( const sndcmd_t *cmd )
{
	uint32_t head, tail, next;

	head = snd_cmdqueue.head;
	tail = snd_cmdqueue.tail;
	next = ( head + 1 ) & SND_CMD_QUEUE_MASK;

	if( next == tail )
	{
		// queue full — drop command
		snd_thread.cmd_drops++;
		Con_DPrintf( S_WARN "SndThread: command queue full, dropping command %d\n", cmd->type );
		return false;
	}

	snd_cmdqueue.cmds[head] = *cmd;

	// write barrier: ensure command data is visible before advancing head
#if XASH_WIN32
	MemoryBarrier();
#else
	__sync_synchronize();
#endif

	snd_cmdqueue.head = next;

	// track peak queue fill
	{
		uint32_t fill = ( next - tail ) & SND_CMD_QUEUE_MASK;
		if( fill > snd_thread.cmd_queue_peak )
			snd_thread.cmd_queue_peak = fill;
	}

	return true;
}

static qboolean SndThread_DequeueCommand( sndcmd_t *cmd )
{
	uint32_t head, tail;

	tail = snd_cmdqueue.tail;
	head = snd_cmdqueue.head;

	if( tail == head )
		return false; // empty

	// read barrier
#if XASH_WIN32
	MemoryBarrier();
#else
	__sync_synchronize();
#endif

	*cmd = snd_cmdqueue.cmds[tail];
	snd_cmdqueue.tail = ( tail + 1 ) & SND_CMD_QUEUE_MASK;
	return true;
}

/*
=============================================================================

  Process Commands on Audio Thread

=============================================================================
*/

// Functions declared in sound.h: S_UpdateSoundFade, S_FreeIdleRawChannels,
// S_UpdateAmbientSounds, S_SpatializeRawChannels, S_ClearBuffer, S_AlterChannel,
// S_UpdateChannels, SND_Spatialize, S_InitAmbientChannels
// Functions declared in s_stream.c forward declarations in sound.h:
// S_StreamBackgroundTrack, S_StartBackgroundTrack, S_StopBackgroundTrack, etc.

static void SndThread_ProcessCommands( void )
{
	sndcmd_t cmd;

	while( SndThread_DequeueCommand( &cmd ))
	{
		switch( cmd.type )
		{
		case SNDCMD_START:
		{
			channel_t *target_chan;
			int ch_idx;

			// Channel was pre-picked on main thread; find it by sfx+ent match
			// For simplicity, call S_StartSound path directly
			// The prepared channel has already been set up on the main thread
			if( cmd.start.channel == CHAN_STATIC )
				target_chan = SND_PickStaticChannel( cmd.start.pos, cmd.start.sfx );
			else
				target_chan = SND_PickDynamicChannel( cmd.start.entnum, cmd.start.channel, cmd.start.sfx, NULL );

			if( !target_chan )
				break;

			// Copy the pre-prepared channel data
			*target_chan = cmd.start.prepared;
			break;
		}
		case SNDCMD_STOP:
			S_AlterChannel( cmd.stop.entnum, cmd.stop.channel, cmd.stop.sfx, 0, 0, SND_STOP );
			break;

		case SNDCMD_STOP_ALL:
			S_StopAllSounds( cmd.stopall.ambient );
			break;

		case SNDCMD_ALTER:
			S_AlterChannel( cmd.alter.entnum, cmd.alter.channel, cmd.alter.sfx,
				cmd.alter.vol, cmd.alter.pitch, cmd.alter.flags );
			break;

		case SNDCMD_MUSIC_PLAY:
			S_StartBackgroundTrack( cmd.music_play.introTrack, cmd.music_play.mainTrack,
				cmd.music_play.position, cmd.music_play.fullpath );
			break;

		case SNDCMD_MUSIC_STOP:
			S_StopBackgroundTrack();
			break;

		case SNDCMD_MUSIC_PAUSE:
			S_StreamSetPause( cmd.music_pause.pause );
			break;

		case SNDCMD_FADE:
			S_FadeMusicVolume( cmd.fade.fadePercent );
			break;

		case SNDCMD_STREAMING:
			if( cmd.streaming.start )
				S_StartStreaming();
			else
				S_StopStreaming();
			break;

		case SNDCMD_CLEAR:
			S_ClearBuffer();
			break;
		}
	}
}

/*
=============================================================================

  Snapshot Operations

=============================================================================
*/

void SndThread_UpdateSnapshot( void )
{
	int write_idx = !snd_snapshot_index; // write to the slot NOT being read
	snd_snapshot_t *snap = &snd_snapshots[write_idx];

	VectorCopy( s_listener.origin, snap->origin );
	VectorCopy( s_listener.forward, snap->forward );
	VectorCopy( s_listener.right, snap->right );
	VectorCopy( s_listener.up, snap->up );
	snap->entnum = s_listener.entnum;
	snap->waterlevel = cl.local.waterlevel;
	snap->frametime = cl.time - cl.oldtime;
	snap->active = CL_IsInGame();
	snap->inmenu = ( cls.key_dest == key_menu );
	snap->paused = cl.paused;
	snap->streaming = s_listener.streaming;
	snap->stream_paused = s_listener.stream_paused;
	snap->key_dest = cls.key_dest;
	snap->cl_time = cl.time;
	snap->cl_oldtime = cl.oldtime;

	// write barrier then flip
#if XASH_WIN32
	MemoryBarrier();
#else
	__sync_synchronize();
#endif
	snd_snapshot_index = write_idx;
}

const snd_snapshot_t *SndThread_GetSnapshot( void )
{
	return &snd_snapshots[snd_snapshot_index];
}

/*
=============================================================================

  Apply Snapshot to Listener (audio thread side)

=============================================================================
*/

static void SndThread_ApplySnapshot( void )
{
	const snd_snapshot_t *snap = SndThread_GetSnapshot();

	VectorCopy( snap->origin, s_listener.origin );
	VectorCopy( snap->forward, s_listener.forward );
	VectorCopy( snap->right, s_listener.right );
	VectorCopy( snap->up, s_listener.up );
	s_listener.entnum = snap->entnum;
	s_listener.waterlevel = snap->waterlevel;
	s_listener.frametime = snap->frametime;
	s_listener.active = snap->active;
	s_listener.inmenu = snap->inmenu;
	s_listener.paused = snap->paused;
	s_listener.streaming = snap->streaming;
	s_listener.stream_paused = snap->stream_paused;
}

/*
=============================================================================

  Audio Thread Main Loop (enkiTS PinnedTask)

=============================================================================
*/

/*
====================
SndThread_QueryThreadCPUTime

  Query cumulative CPU time (kernel + user) for the audio thread.
  Returns seconds, or -1.0 if not available.
====================
*/
static double SndThread_QueryThreadCPUTime( void )
{
#if XASH_WIN32
	FILETIME creation, exit, kernel, user;
	ULARGE_INTEGER k, u;

	if( !snd_thread.thread_handle )
		return -1.0;

	if( !GetThreadTimes( snd_thread.thread_handle, &creation, &exit, &kernel, &user ))
		return -1.0;

	k.LowPart = kernel.dwLowDateTime;
	k.HighPart = kernel.dwHighDateTime;
	u.LowPart = user.dwLowDateTime;
	u.HighPart = user.dwHighDateTime;

	return (double)( k.QuadPart + u.QuadPart ) * 1.0e-7;
#elif XASH_LINUX
	char path[64];
	FILE *f;
	unsigned long utime, stime;
	long clk_tck;

	if( snd_thread.thread_tid <= 0 )
		return -1.0;

	Q_snprintf( path, sizeof( path ), "/proc/self/task/%d/stat", (int)snd_thread.thread_tid );
	f = fopen( path, "r" );
	if( !f ) return -1.0;

	// Fields: pid (comm) state ... utime(14) stime(15)
	if( fscanf( f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
		&utime, &stime ) != 2 )
	{
		fclose( f );
		return -1.0;
	}
	fclose( f );

	clk_tck = sysconf( _SC_CLK_TCK );
	if( clk_tck <= 0 ) clk_tck = 100;

	return (double)( utime + stime ) / (double)clk_tck;
#else
	return -1.0;
#endif
}

static void SndThread_MixLoop( void *pArgs )
{
	double mix_start, mix_end, idle_start, idle_end;

	(void)pArgs;

	/* Capture OS thread handle for CPU time measurement */
#if XASH_WIN32
	DuplicateHandle( GetCurrentProcess(), GetCurrentThread(),
		GetCurrentProcess(), &snd_thread.thread_handle,
		0, FALSE, DUPLICATE_SAME_ACCESS );
#elif XASH_LINUX
	snd_thread.thread_tid = (pid_t)syscall( SYS_gettid );
#endif

	while( snd_thread.running )
	{
		mix_start = Sys_DoubleTime();

		/* 1. Process queued commands from main thread */
		SndThread_ProcessCommands();

		/* 2. Apply the latest listener snapshot */
		SndThread_ApplySnapshot();

		/* 3. Update sound fade (touches only soundfade struct) */
		S_UpdateSoundFade();

		/* 4. Release idle raw channels */
		S_FreeIdleRawChannels();

		/* 5. Update ambient sounds */
		S_UpdateAmbientSounds();

		/* 6. Spatialize all channels + count active */
		{
			int i, active = 0;
			channel_t *ch;

			for( i = NUM_AMBIENTS, ch = channels + NUM_AMBIENTS; i < total_channels; i++, ch++ )
			{
				if( !ch->sfx ) continue;
				SND_Spatialize( ch );
				if( ch->leftvol || ch->rightvol )
					active++;
			}
			snd_thread.active_channels = active;
		}

		/* 7. Spatialize raw channels */
		S_SpatializeRawChannels();

		/* 8. Stream background music */
		S_StreamBackgroundTrack();

		/* 9. Mix and submit to DMA */
		S_UpdateChannels();

		mix_end = Sys_DoubleTime();
		snd_thread.active_time += ( mix_end - mix_start );
		snd_thread.mix_iterations++;

		/* 10. Sleep until next wake or ~5ms timeout */
		idle_start = Sys_DoubleTime();
		SndThread_SyncLock();
		if( snd_thread.running )
			SndThread_SyncWait();
		SndThread_SyncUnlock();
		idle_end = Sys_DoubleTime();
		snd_thread.idle_time += ( idle_end - idle_start );
	}

	/* Clean up thread handle */
#if XASH_WIN32
	if( snd_thread.thread_handle )
	{
		CloseHandle( snd_thread.thread_handle );
		snd_thread.thread_handle = NULL;
	}
#endif
}

/*
=============================================================================

  Public API

=============================================================================
*/

void SndThread_Init( void )
{
	struct enkiTaskSchedulerConfig config;

	if( snd_thread.active )
		return;

	/* Register/fetch cvar */
	if( !snd_thread.cvar )
		snd_thread.cvar = Cvar_Get( "snd_threaded", "1", FCVAR_ARCHIVE, "enable threaded audio mixing" );

	if( !snd_thread.cvar || snd_thread.cvar->value == 0.0f )
		return;

	/* Initialize command queue */
	snd_cmdqueue.head = 0;
	snd_cmdqueue.tail = 0;
	memset( snd_cmdqueue.cmds, 0, sizeof( snd_cmdqueue.cmds ));

	/* Initialize snapshots */
	memset( snd_snapshots, 0, sizeof( snd_snapshots ));
	snd_snapshot_index = 0;

	/* Reset diagnostics counters */
	snd_thread.mix_iterations = 0;
	snd_thread.cmd_drops = 0;
	snd_thread.cmd_queue_peak = 0;
	snd_thread.active_channels = 0;
	snd_thread.active_time = 0.0;
	snd_thread.idle_time = 0.0;
#if XASH_WIN32
	snd_thread.thread_handle = NULL;
#elif XASH_LINUX
	snd_thread.thread_tid = 0;
#endif

	/* Initialize sync primitives */
	SndThread_SyncInit();

	snd_thread.running = 1;

	/* Create enkiTS scheduler with 1 task thread for audio */
	snd_thread.scheduler = enkiNewTaskScheduler();
	config = enkiGetTaskSchedulerConfig( snd_thread.scheduler );
	config.numTaskThreadsToCreate = 1;
	enkiInitTaskSchedulerWithConfig( snd_thread.scheduler, config );

	/* Create pinned task on thread 1 (the audio worker thread) */
	snd_thread.pinned_task = enkiCreatePinnedTask( snd_thread.scheduler, SndThread_MixLoop, 1 );
	enkiAddPinnedTask( snd_thread.scheduler, snd_thread.pinned_task );

	snd_thread.active = true;
	Con_Printf( "Audio: threaded mixing enabled (dedicated thread)\n" );
}

void SndThread_Shutdown( void )
{
	if( !snd_thread.active )
		return;

	/* Signal the thread to stop */
	snd_thread.running = 0;

	/* Wake the thread so it can exit */
	SndThread_Signal();

	/* Wait for the pinned task to complete */
	enkiWaitForPinnedTask( snd_thread.scheduler, snd_thread.pinned_task );

	/* Clean up enkiTS resources */
	enkiDeletePinnedTask( snd_thread.scheduler, snd_thread.pinned_task );
	snd_thread.pinned_task = NULL;

	enkiWaitforAllAndShutdown( snd_thread.scheduler );
	enkiDeleteTaskScheduler( snd_thread.scheduler );
	snd_thread.scheduler = NULL;

	/* Clean up sync primitives */
	SndThread_SyncDestroy();

	snd_thread.active = false;
	Con_Printf( "Audio: threaded mixing shutdown\n" );
}

qboolean SndThread_IsActive( void )
{
	return snd_thread.active;
}

void SndThread_CheckCvar( void )
{
	if( !snd_thread.cvar )
		return;

	if( snd_thread.cvar->value != 0.0f && !snd_thread.active )
		SndThread_Init();
	else if( snd_thread.cvar->value == 0.0f && snd_thread.active )
		SndThread_Shutdown();
}

void SndThread_GetStats( snd_thread_stats_t *stats )
{
	uint32_t head, tail;

	if( !stats ) return;
	memset( stats, 0, sizeof( *stats ));

	if( !snd_thread.active )
		return;

	head = snd_cmdqueue.head;
	tail = snd_cmdqueue.tail;
	stats->cmd_queue_count = ( head - tail ) & SND_CMD_QUEUE_MASK;
	stats->cmd_queue_peak = snd_thread.cmd_queue_peak;
	stats->cmd_drops = snd_thread.cmd_drops;
	stats->mix_iterations = snd_thread.mix_iterations;
	stats->active_time = snd_thread.active_time;
	stats->idle_time = snd_thread.idle_time;
	stats->active_channels = snd_thread.active_channels;
	stats->total_channels_snap = total_channels;
	stats->paintedtime_snap = paintedtime;
	stats->soundtime_snap = soundtime;
	stats->snd_thread_cpu_time = SndThread_QueryThreadCPUTime();
}

#else // !XASH_NET_THREAD

/*
  Stubs when threading is not available
*/
#include "common.h"
#include "sound.h"
#include "snd_thread.h"

void     SndThread_Init( void ) {}
void     SndThread_Shutdown( void ) {}
qboolean SndThread_IsActive( void ) { return false; }
void     SndThread_CheckCvar( void ) {}
qboolean SndThread_QueueCommand( const sndcmd_t *cmd ) { (void)cmd; return false; }
void     SndThread_UpdateSnapshot( void ) {}
void     SndThread_Signal( void ) {}
const snd_snapshot_t *SndThread_GetSnapshot( void ) { return NULL; }
void     SndThread_GetStats( snd_thread_stats_t *stats ) { if( stats ) memset( stats, 0, sizeof( *stats )); }

#endif // XASH_NET_THREAD
