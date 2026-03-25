/*
snd_thread.h - dedicated audio mixing thread
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

#ifndef SND_THREAD_H
#define SND_THREAD_H

#include "common.h"
#include "sound.h"

/*
=============================================================================

  Sound Thread Command Queue

  The main thread pushes commands (start sound, stop sound, music control, etc.)
  into a ring buffer. The audio thread drains the queue each iteration.

=============================================================================
*/

typedef enum
{
	SNDCMD_START,         // start a new sound
	SNDCMD_STOP,          // stop sounds for entity+channel
	SNDCMD_STOP_ALL,      // stop all sounds (level change, etc.)
	SNDCMD_ALTER,         // alter volume/pitch of playing sound
	SNDCMD_MUSIC_PLAY,    // start background music track
	SNDCMD_MUSIC_STOP,    // stop background music
	SNDCMD_MUSIC_PAUSE,   // pause/resume background music
	SNDCMD_CLEAR,         // clear DMA buffer
	SNDCMD_FADE,          // set music fade
	SNDCMD_STREAMING,     // start/stop AVI streaming mode
} sndcmd_type_t;

/* Data for SNDCMD_START */
typedef struct
{
	vec3_t   pos;
	int      entnum;
	int      channel;
	sfx_t   *sfx;        // pre-loaded on main thread
	float    fvol;
	float    attn;
	int      pitch;
	int      flags;
	qboolean is_sentence;
	channel_t prepared;   // pre-prepared channel data from main thread
} sndcmd_start_t;

/* Data for SNDCMD_STOP */
typedef struct
{
	int    entnum;
	int    channel;
	sfx_t *sfx;
} sndcmd_stop_t;

/* Data for SNDCMD_ALTER */
typedef struct
{
	int    entnum;
	int    channel;
	sfx_t *sfx;
	int    vol;
	int    pitch;
	int    flags;
} sndcmd_alter_t;

/* Data for SNDCMD_STOP_ALL */
typedef struct
{
	qboolean ambient;     // restart ambient sounds after stop?
} sndcmd_stopall_t;

/* Data for SNDCMD_MUSIC_PLAY */
typedef struct
{
	string  introTrack;
	string  mainTrack;
	int     position;
	qboolean fullpath;
} sndcmd_music_play_t;

/* Data for SNDCMD_MUSIC_PAUSE */
typedef struct
{
	int pause;            // 0 = resume, 1 = pause
} sndcmd_music_pause_t;

/* Data for SNDCMD_FADE */
typedef struct
{
	float fadePercent;
} sndcmd_fade_t;

/* Data for SNDCMD_STREAMING */
typedef struct
{
	qboolean start;       // true = start, false = stop
} sndcmd_streaming_t;

/*
  Sound command union — fits all command types.
  Kept small enough for a fixed-size ring buffer.
*/
typedef struct
{
	sndcmd_type_t type;

	union
	{
		sndcmd_start_t       start;
		sndcmd_stop_t        stop;
		sndcmd_alter_t       alter;
		sndcmd_stopall_t     stopall;
		sndcmd_music_play_t  music_play;
		sndcmd_music_pause_t music_pause;
		sndcmd_fade_t        fade;
		sndcmd_streaming_t   streaming;
	};
} sndcmd_t;

/*
  Listener snapshot — double-buffered, written by main thread,
  read by audio thread. No lock needed: main thread writes to
  pending slot, then flips the index atomically.
*/
typedef struct
{
	vec3_t   origin;
	vec3_t   forward;
	vec3_t   right;
	vec3_t   up;
	int      entnum;
	int      waterlevel;
	float    frametime;
	qboolean active;
	qboolean inmenu;
	qboolean paused;
	qboolean streaming;
	qboolean stream_paused;
	int      key_dest;   // cls.key_dest snapshot for mix code
	double   cl_time;
	double   cl_oldtime;
} snd_snapshot_t;

/*
=============================================================================

  Public API

=============================================================================
*/

void     SndThread_Init( void );
void     SndThread_Shutdown( void );
qboolean SndThread_IsActive( void );
void     SndThread_CheckCvar( void );

/* Queue a command from the main thread */
qboolean SndThread_QueueCommand( const sndcmd_t *cmd );

/* Write a new listener snapshot from the main thread */
void     SndThread_UpdateSnapshot( void );

/* Signal the audio thread to wake (after snapshot or commands) */
void     SndThread_Signal( void );

/* Get the current snapshot (called from audio thread) */
const snd_snapshot_t *SndThread_GetSnapshot( void );

#endif // SND_THREAD_H
