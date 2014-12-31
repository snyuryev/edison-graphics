// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	System interface for sound.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_unix.c,v 1.5 1997/02/03 22:45:10 b1 Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>

#include <mraa/pwm.h>

#include "z_zone.h"

#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"

#include "doomdef.h"


// Config for audio PWM pin
static const int PWM_CHIP_ID = 0;
static const int PWM_PIN = 0;
static const int PWM_ARDUINO_PIN = 3; // Arduino pin 3 is PWM0 on chip 0
static const int PWM_PERIOD_USEC = 10; // Pick a period that works well with your capacitor/resistor low pass filter


// The number of internal mixing channels,
//  the samples calculated for each mixing step,
//  the size of the 16bit, 2 hardware channel (stereo)
//  mixing buffer, and the samplerate of the raw data.

#define NUM_CHANNELS		8

#define SAMPLERATE		11025	// Hz
#define SAMPLESIZE		2   	// 16bit

// The actual lengths of all sound effects.
int 		lengths[NUMSFX];


// The channel step amount...
unsigned int	channelstep[NUM_CHANNELS];
// ... and a 0.16 bit remainder of last step.
unsigned int	channelstepremainder[NUM_CHANNELS];


// The channel data pointers, start and end.
unsigned char*	channels[NUM_CHANNELS];
unsigned char*	channelsend[NUM_CHANNELS];


// Time/gametic that the channel started playing,
//  used to determine oldest, which automatically
//  has lowest priority.
// In case number of active sounds exceeds
//  available channels.
int		channelstart[NUM_CHANNELS];

// The sound in channel handles,
//  determined on registration,
//  might be used to unregister/stop/modify,
//  currently unused.
int 		channelhandles[NUM_CHANNELS];

// SFX id of the playing sound effect.
// Used to catch duplicates (like chainsaw).
int		channelids[NUM_CHANNELS];			

// Pitch to stepping lookup, unused.
int		steptable[256];

// Volume lookups.
int		vol_lookup[128*256];

// Hardware left and right channel volume lookup.
int*		channelleftvol_lookup[NUM_CHANNELS];
int*		channelrightvol_lookup[NUM_CHANNELS];



//
// This function loads the sound data from the WAD lump,
//  for single sound.
//
void* getsfx(
		char*         sfxname,
		int*          len )
{
	char                name[20];
	int                 sfxlump;


	// Get the sound data from the WAD, allocate lump
	//  in zone memory.
	sprintf(name, "ds%s", sfxname);

	// Now, there is a severe problem with the
	//  sound handling, in it is not (yet/anymore)
	//  gamemode aware. That means, sounds from
	//  DOOM II will be requested even with DOOM
	//  shareware.
	// The sound list is wired into sounds.c,
	//  which sets the external variable.
	// I do not do runtime patches to that
	//  variable. Instead, we will use a
	//  default sound for replacement.
	if ( W_CheckNumForName(name) == -1 )
		sfxlump = W_GetNumForName("dspistol");
	else
		sfxlump = W_GetNumForName(name);

	*len = W_LumpLength( sfxlump );

	// Debug.
	// fprintf( stderr, "." );
	//fprintf( stderr, " -loading  %s (lump %d, %d bytes)\n",
	//	     sfxname, sfxlump, *len );
	//fflush( stderr );

	return W_CacheLumpNum( sfxlump, PU_STATIC );
}





//
// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
//
int addsfx(
		int		sfxid,
		int		volume,
		int		step,
		int		seperation )
{
	static unsigned short	handlenums = 0;

	int		i;
	int		rc = -1;

	int		oldest = gametic;
	int		oldestnum = 0;
	int		slot;

	int		rightvol;
	int		leftvol;

	// Chainsaw troubles.
	// Play these sound effects only one at a time.
	if ( sfxid == sfx_sawup
			|| sfxid == sfx_sawidl
			|| sfxid == sfx_sawful
			|| sfxid == sfx_sawhit
			|| sfxid == sfx_stnmov
			|| sfxid == sfx_pistol	 )
	{
		// Loop all channels, check.
		for (i=0 ; i<NUM_CHANNELS ; i++)
		{
			// Active, and using the same SFX?
			if ( (channels[i])
					&& (channelids[i] == sfxid) )
			{
				// Reset.
				channels[i] = 0;
				// We are sure that iff,
				//  there will only be one.
				break;
			}
		}
	}

	// Loop all channels to find oldest SFX.
	for (i=0; (i<NUM_CHANNELS) && (channels[i]); i++)
	{
		if (channelstart[i] < oldest)
		{
			oldestnum = i;
			oldest = channelstart[i];
		}
	}

	// Tales from the cryptic.
	// If we found a channel, fine.
	// If not, we simply overwrite the first one, 0.
	// Probably only happens at startup.
	if (i == NUM_CHANNELS)
		slot = oldestnum;
	else
		slot = i;

	// Okay, in the less recent channel,
	//  we will handle the new SFX.
	// Set pointer to raw data.
//	channels[slot] = (unsigned char *) S_sfx[sfxid].data;
	// Set pointer to end of raw data.
//	channelsend[slot] = channels[slot] + lengths[sfxid];
	channelsend[slot] = (unsigned char *) S_sfx[sfxid].data + lengths[sfxid];

	// Reset current handle number, limited to 0..100.
	if (!handlenums)
		handlenums = 100;

	// Assign current handle number.
	// Preserved so sounds could be stopped (unused).
	channelhandles[slot] = rc = handlenums++;

	// Set stepping???
	// Kinda getting the impression this is never used.
	channelstep[slot] = step;
	// ???
	channelstepremainder[slot] = 0;
	// Should be gametic, I presume.
	channelstart[slot] = gametic;

#if 0 // No support for stereo for now
	// Separation, that is, orientation/stereo.
	//  range is: 1 - 256
	seperation += 1;

	// Per left/right channel.
	//  x^2 seperation,
	//  adjust volume properly.
	leftvol =
			volume - ((volume*seperation*seperation) >> 16); ///(256*256);
	seperation = seperation - 257;
	rightvol =
			volume - ((volume*seperation*seperation) >> 16);
#else
	leftvol = volume;
	rightvol = volume;
#endif

	// Sanity check, clamp volume.
	if (rightvol < 0 || rightvol > 127)
		I_Error("rightvol out of bounds");

	if (leftvol < 0 || leftvol > 127)
		I_Error("leftvol out of bounds");

	// Get the proper lookup table piece
	//  for this volume level???
	channelleftvol_lookup[slot] = &vol_lookup[leftvol*256];
	channelrightvol_lookup[slot] = &vol_lookup[rightvol*256];

	// Preserve sound SFX id,
	//  e.g. for avoiding duplicates of chainsaw.
	channelids[slot] = sfxid;

	channels[slot] = (unsigned char *) S_sfx[sfxid].data;

	// You tell me.
	return rc;
}





//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// See soundserver initdata().
//
void I_SetChannels()
{
	// Init internal lookups (raw data, mixing buffer, channels).
	// This function sets up internal lookups used during
	//  the mixing process.
	int		i;
	int		j;

	int*	steptablemid = steptable + 128;

	// Okay, reset internal mixing channels to zero.
	/*for (i=0; i<NUM_CHANNELS; i++)
  {
    channels[i] = 0;
  }*/

	// This table provides step widths for pitch parameters.
	// I fail to see that this is currently used.
	for (i=-128 ; i<128 ; i++)
		steptablemid[i] = (int)(pow(2.0, (i/64.0))*65536.0);


	// Generates volume lookup tables
	//  which also turn the unsigned samples
	//  into signed samples.
	for (i=0 ; i<128 ; i++)
		for (j=0 ; j<256 ; j++)
			vol_lookup[i*256+j] = (i*(j-128)*256)/127;
}	


void I_SetSfxVolume(int volume)
{
	// Identical to DOS.
	// Basically, this should propagate
	//  the menu/config file setting
	//  to the state variable used in
	//  the mixing.
	snd_SfxVolume = volume;
}

// MUSIC API - dummy. Some code from DOS version.
void I_SetMusicVolume(int volume)
{
	// Internal state variable.
	snd_MusicVolume = volume;
	// Now set volume on output device.
	// Whatever( snd_MusciVolume );
}


//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
	char namebuf[9];
	sprintf(namebuf, "ds%s", sfx->name);
	return W_GetNumForName(namebuf);
}

//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
int
I_StartSound(
		int		id,
		int		vol,
		int		sep,
		int		pitch,
		int		priority )
{

	// UNUSED
	priority = 0;

	// Debug.
	//fprintf( stderr, "starting sound %d", id );

	// Returns a handle (not used).
	id = addsfx( id, vol, steptable[pitch], sep );

	//fprintf( stderr, "/handle is %d\n", id );

	return id;
}



void I_StopSound (int handle)
{
	// You need the handle returned by StartSound.
	// Would be looping all channels,
	//  tracking down the handle,
	//  an setting the channel to zero.

	// UNUSED.
	handle = 0;
}


int I_SoundIsPlaying(int handle)
{
	// Ouch.
	return gametic < handle;
}




//
// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the global
//  mixbuffer, clamping it to the allowed range,
//  and sets up everything for transferring the
//  contents of the mixbuffer to the (two)
//  hardware channels (left and right, that is).
//
// This function currently supports only 16bit.
//
void I_UpdateSound( void )
{
}



void* audioThreadMain(void* arg)
{
	mraa_pwm_context pwm;
	pwm = mraa_pwm_init(PWM_ARDUINO_PIN);
	if (pwm == NULL) {
		fprintf(stderr, "Cannot open PWM pin %d\n", PWM_ARDUINO_PIN);
		return 0;
	}

	mraa_pwm_period_us(pwm, PWM_PERIOD_USEC);
	//mraa_pwm_write_period(pwm, PWM_PERIOD_USEC * 1000);
	mraa_pwm_enable(pwm, 1);

	char bu[64];
	snprintf(bu, sizeof(bu), "/sys/class/pwm/pwmchip%d/pwm%d/duty_cycle", PWM_CHIP_ID, PWM_PIN);
	int duty_fp = open(bu, O_RDWR);
	if (duty_fp == -1) {
		fprintf(stderr, "Cannot open PWM duty file %s\n", bu);
		return 0;
	}

	double startTime = 0;
	while (1)
	{
		struct timespec t;
		clock_gettime(CLOCK_REALTIME, &t);
		double time = (double)t.tv_sec + (double)t.tv_nsec / 1.0e9;

		if (startTime == 0)
			startTime = time;

		double sampleIndex = (time - startTime) * SAMPLERATE;

		static int lastPlayedSample = 0;
		if (lastPlayedSample != (int)sampleIndex)
		{
			lastPlayedSample = (int)sampleIndex;

#ifdef SNDTIMINGDEBUG
			static int samplesPlayed = 0;
			static double lastTime = 0;
			static double samplesTooLate = 0.0;
			static double maxSampleTooLate = 0.0;
			static int numLateSamples = 0;

			samplesPlayed++;
			// Timing error measurement: How far into the sample are we already?
			double sampleTooLate = sampleIndex - (int)sampleIndex;
			samplesTooLate += sampleTooLate;
			if (sampleTooLate > maxSampleTooLate)
				maxSampleTooLate = sampleTooLate;
			if (sampleTooLate > 0.1)
				numLateSamples++;

			if (time - lastTime > 10.0)
			{
				printf("%f samples per second, timing precision: avg %f, max %f, late %f%%\n",
						samplesPlayed / (time - lastTime),
						samplesTooLate / samplesPlayed, maxSampleTooLate,
						100.0 * numLateSamples / samplesPlayed);
				lastTime = time;
				samplesPlayed = 0;
				samplesTooLate = 0.0;
				maxSampleTooLate = 0.0;
				numLateSamples = 0;
			}
#endif


			//float value = audioData[(int)sampleIndex % sizeof(audioData)] / 255.0f;
			//value = sin(time*hertz * 6.28) * 0.5 + 0.5;

			// Mix current sound data.
			// Data, from raw sound, for right and left.
			unsigned int	sample;
			int		dl = 0;

			// Mixing channel index.
			int				chan;

			// Love thy L2 chache - made this a loop.
			// Now more channels could be set at compile time
			//  as well. Thus loop those  channels.
			for (chan = 0; chan < NUM_CHANNELS; chan++ )
			{
				// Check channel, if active.
				if (channels[ chan ])
				{
					// Get the raw data from the channel.
					sample = *channels[ chan ];
					// Add left and right part
					//  for this channel (sound)
					//  to the current data.
					// Adjust volume accordingly.
					dl += channelleftvol_lookup[ chan ][sample];
					// Increment index ???
					channelstepremainder[ chan ] += channelstep[ chan ];
					// MSB is next sample???
					channels[ chan ] += channelstepremainder[ chan ] >> 16;
					// Limit to LSB???
					channelstepremainder[ chan ] &= 65536-1;

					// Check whether we are done.
					if (channels[ chan ] >= channelsend[ chan ])
						channels[ chan ] = 0;
				}
			}

			dl *= 8; // PWM has very limited bit resolution, make sounds louder to use more of the available range...

			// Clamp to range. Left hardware channel.
			float value;
			if (dl > 0x7fff)
				value = 1.0f;
			else if (dl < -0x8000)
				value = 0.0f;
			else
				value = (float)dl / 0x10000 + 0.5f;

			//mraa_pwm_write(pwm, value);
			//mraa_pwm_pulsewidth_us(pwm, (int) (value * PWM_PERIOD_USEC));
		    int length = snprintf(bu, sizeof(bu), "%d", (int)(value * (PWM_PERIOD_USEC * 1000)));
		    if (write(duty_fp, bu, length * sizeof(char)) == -1)
		    {
		    	fprintf(stderr, "PWM duty write failure\n");
		    }

			// usleep sleeps to long, need to investigate kernel scheduler (maybe SCHED_FIFO?)
			//usleep(1000000 / SAMPLERATE);
			//usleep(1);
		}
	}

	return 0;
}

// 
// This would be used to write out the mixbuffer
//  during each game loop update.
// Updates sound buffer and audio device at runtime. 
// It is called during Timer interrupt with SNDINTR.
// Mixing now done synchronous, and
//  only output be done asynchronous?
//
void
I_SubmitSound(void)
{
}



void
I_UpdateSoundParams(
		int	handle,
		int	vol,
		int	sep,
		int	pitch)
{
	// I fail too see that this is used.
	// Would be using the handle to identify
	//  on which channel the sound might be active,
	//  and resetting the channel parameters.

	// UNUSED.
	handle = vol = sep = pitch = 0;
}




void I_ShutdownSound(void)
{    
	// Wait till all pending sounds are finished.
	int done = 0;
	int i;


	// FIXME (below).
	fprintf( stderr, "I_ShutdownSound: NOT finishing pending sounds\n");
	fflush( stderr );

	while ( !done )
	{
		for( i=0 ; i<8 && !channels[i] ; i++);

		// FIXME. No proper channel output.
		//if (i==8)
		done=1;
	}

	// TODO: Clean up audio thread.

	// Done.
	return;
}






void
I_InitSound()
{ 
	int i;

	// Secure and configure sound device first.
	fprintf( stderr, "I_InitSound: ");

	// Initialize channels to 0 before thread starts
	for (i=0; i<NUM_CHANNELS; i++)
	{
		channels[i] = 0;
	}

	pthread_t audioThread = 0;
	int err = pthread_create(&audioThread, NULL, &audioThreadMain, NULL);
	if (err != 0)
		fprintf(stderr, "Can't create audio thread: %s\n", strerror(err));

	fprintf(stderr, " configured audio device\n" );


	// Initialize external data (all sounds) at start, keep static.
	fprintf( stderr, "I_InitSound: ");

	for (i=1 ; i<NUMSFX ; i++)
	{
		// Alias? Example is the chaingun sound linked to pistol.
		if (!S_sfx[i].link)
		{
			// Load data from WAD file.
			S_sfx[i].data = getsfx( S_sfx[i].name, &lengths[i] );
		}
		else
		{
			// Previously loaded already?
			S_sfx[i].data = S_sfx[i].link->data;
			lengths[i] = lengths[(S_sfx[i].link - S_sfx)/sizeof(sfxinfo_t)];
		}
	}

	fprintf( stderr, " pre-cached all sound data\n");

	// Finished initialization.
	fprintf(stderr, "I_InitSound: sound module ready\n");
}




//
// MUSIC API.
// Still no music done.
// Remains. Dummies.
//
void I_InitMusic(void)		{ }
void I_ShutdownMusic(void)	{ }

static int	looping=0;
static int	musicdies=-1;

void I_PlaySong(int handle, int looping)
{
	// UNUSED.
	handle = looping = 0;
	musicdies = gametic + TICRATE*30;
}

void I_PauseSong (int handle)
{
	// UNUSED.
	handle = 0;
}

void I_ResumeSong (int handle)
{
	// UNUSED.
	handle = 0;
}

void I_StopSong(int handle)
{
	// UNUSED.
	handle = 0;

	looping = 0;
	musicdies = 0;
}

void I_UnRegisterSong(int handle)
{
	// UNUSED.
	handle = 0;
}

int I_RegisterSong(void* data)
{
	// UNUSED.
	data = NULL;

	return 1;
}

// Is the song playing?
int I_QrySongPlaying(int handle)
{
	// UNUSED.
	handle = 0;
	return looping || musicdies > gametic;
}
