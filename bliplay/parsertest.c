/**
 * Copyright (c) 2014 Simon Schoenenberger
 * http://blipkit.monoxid.net/
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <SDL/SDL.h>
#include "BKSTParser.h"
#include "BKCompiler.h"

static BKContext     ctx;
static BKSTTokenType token;
static BKSTParser    parser;
static BKSTCmd       cmd;
static BKCompiler    compiler;

typedef struct
{
	BKInterpreter interpreter;
	BKTrack       track;
	BKDivider     divider;
} BKTrackWrapper;

static BKTrackWrapper trackWrappers [8];

//static char const * filename = "/Users/simon/Downloads/test-new-format.blip";
static char const * filename = "/Users/simon/Downloads/Subversion/bliplay-git/bliplay/test.blip";

static void fill_audio (void * ptr, Uint8 * stream, int len)
{
	// calculate needed frames for one channel
	BKUInt numFrames = len / sizeof (BKFrame) / ctx.numChannels;

	BKContextGenerate (& ctx, (BKFrame *) stream, numFrames);
}

static BKEnum track_tick (BKCallbackInfo * info, BKTrackWrapper * wrapper)
{
	BKInt ticks;

	BKInterpreterTrackAdvance (& wrapper -> interpreter, & wrapper -> track, & ticks);

	info -> divider = ticks;

	return 0;
}

#ifdef main
#undef main
#endif

int main (int argc, char * argv [])
{
	FILE * file = fopen (filename, "r");

	if (file == NULL) {
		fprintf (stderr, "No such file: %s\n", filename);
		return 1;
	}

	BKSTParserInitWithFile (& parser, file);
	BKCompilerInit (& compiler);

	compiler.loadPath = strdup ("/Users/simon/Downloads/Subversion/bliplay-git/bliplay/examples");

	while ((token = BKSTParserNextCommand (& parser, & cmd))) {
		if (BKCompilerPushCommand (& compiler, & cmd)) {
			printf("***Failed\n");
			return 1;
		}
	}

	if (BKCompilerTerminate (& compiler, 0) < 0) {
		return 1;
	}

	BKSTParserDispose (& parser);
	fclose (file);

	BKInt totalSize = 0;

	BKByteBuffer    * buffer;
	BKCompilerTrack * cTrack;

	printf("%ld tracks\n", compiler.tracks.length);

	printf("global groups:%ld \n", compiler.globalTrack.cmdGroups.length);
	printf("global byte code length: %ld\n", compiler.globalTrack.globalCmds.size);

	for (BKInt i = 0; i < compiler.tracks.length; i ++) {
		BKArrayGetItemAtIndexCopy (& compiler.tracks, i, & cTrack);

		printf("track #%d byte code length: %ld\n", i, cTrack -> globalCmds.size);

		totalSize += cTrack -> globalCmds.size;

		for (BKInt j = 0; j < cTrack -> cmdGroups.length; j ++) {
			BKArrayGetItemAtIndexCopy (& cTrack -> cmdGroups, j, & buffer);

			if (buffer == NULL) {
				continue;
			}

			printf("    #%u size: %lu\n", j, buffer -> size);

			totalSize += buffer -> size;
		}
	}

	printf ("total byte code: %d\n", totalSize);

	BKInt const sampleRate  = 44100;
	BKInt const numChannels = 2;

	BKContextInit (& ctx, numChannels, sampleRate);

	SDL_Init (SDL_INIT_AUDIO);

	SDL_AudioSpec wanted;

	wanted.freq     = sampleRate;
	wanted.format   = AUDIO_S16SYS;
	wanted.channels = numChannels;
	wanted.samples  = 512;
	wanted.callback = (void *) fill_audio;
	wanted.userdata = NULL;

	if (SDL_OpenAudio (& wanted, NULL) < 0) {
		fprintf (stderr, "Couldn't open audio: %s\n", SDL_GetError ());
		return 1;
	}

	for (BKInt i = 0; i < compiler.tracks.length; i ++) {
		BKTrackWrapper * wrapper     = & trackWrappers [i];
		BKInterpreter  * interpreter = & wrapper -> interpreter;

		BKInterpreterInit (interpreter);
		BKTrackInit (& wrapper -> track, BK_SQUARE);

		BKCallback callback = {
			.func     = (void *) track_tick,
			.userInfo = wrapper,
		};

		BKDividerInit (& wrapper -> divider, 24, & callback);
		BKContextAttachDivider (& ctx, & wrapper -> divider, BK_CLOCK_TYPE_BEAT);

		interpreter -> instruments = & compiler.instruments;
		interpreter -> waveforms   = & compiler.waveforms;
		interpreter -> samples     = & compiler.samples;

		BKArrayGetItemAtIndexCopy (& compiler.tracks, i, & cTrack);
		interpreter -> opcode    = cTrack -> globalCmds.data;
		interpreter -> opcodePtr = interpreter -> opcode;

		BKTrackAttach (& wrapper -> track, & ctx);
	}

	SDL_PauseAudio (0);

	while (1) {
		SDL_Delay (100);
	}

	BKCompilerDispose (& compiler);

	SDL_PauseAudio (1);
	SDL_CloseAudio ();

	for (BKInt i = 0; i < compiler.tracks.length; i ++) {
		BKTrackWrapper * wrapper = & trackWrappers [i];

		BKInterpreterDispose (& wrapper -> interpreter);
		BKTrackDispose (& wrapper -> track);
		BKDividerDispose (& wrapper -> divider);
	}

	BKContextDispose (& ctx);

    return 0;
}
