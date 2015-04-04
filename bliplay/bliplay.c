/**
 * Copyright (c) 2012-2015 Simon Schoenenberger
 * http://blipkit.audio
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

#define __USE_POSIX

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#if !BK_USE_PLAYER
#define BK_USE_PLAYER 1
#endif

#include <getopt.h>
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#if BK_USE_PLAYER
#	include <SDL/SDL.h>
#endif
#include "BKContextWrapper.h"
#include "BKWaveFileWriter.h"
#include "BKString.h"

#define BK_BLIPLAY_VERSION "2.3"

#ifndef FD_COPY
#define FD_COPY(src, dest) memcpy ((dest), (src), sizeof (*(dest)))
#endif

#ifndef PROGRAM_NAME
#define PROGRAM_NAME "bliplay"
#endif

enum OUTPUT_TYPE
{
	OUTPUT_TYPE_NONE,
	OUTPUT_TYPE_RAW,
	OUTPUT_TYPE_WAVE,
};

enum FLAG
{
	FLAG_HAS_SEEK_TIME     = 1 << 0,
	FLAG_HAS_END_TIME      = 1 << 1,
	FLAG_PRINT_NO_TIME     = 1 << 2,
	FLAG_NO_SOUND          = 1 << 3,
	FLAG_INFO              = 1 << 4,
	FLAG_INFO_EXPLICITE    = 1 << 5,
	FLAG_YES               = 1 << 6,
	FLAG_TIMING_UNIT_SHIFT = 7,
	FLAG_TIMING_UNIT_SECS  = 1 << 7,
	FLAG_TIMING_UNIT_TICKS = 2 << 7,
	FLAG_TIMING_UNIT_MASK  = 3 << 7, // next flag is at 9
};

static BKInt            istty;
static BKInt            flags = FLAG_INFO;
static BKContextWrapper ctx;
static BKUInt           sampleRate = 44100;
static BKTime           seekTime, endTime;
static BKInt            numChannels = 2;
static char const     * filename;
static char const     * outputFilename;
static FILE           * outputFile;
static FILE           * timingFile;
static BKEnum           outputType = OUTPUT_TYPE_NONE;
static BKWaveFileWriter waveWriter;
static char             seekTimeString [64];
static char             endTimeString [64];

#if BK_USE_PLAYER
static int              updateUSecs = 91200;
#endif

static char const * colorNormal = "";
static char const * colorYellow = "";
static char const * colorRed    = "";
static char const * colorGreen  = "";

struct option const options [] =
{
	{"load-dir",     required_argument, NULL, 'd'},
	{"fast-forward", required_argument, NULL, 'f'},
	{"help",         no_argument,       NULL, 'h'},
	{"info",         required_argument, NULL, 'i'},
	{"end-time",     required_argument, NULL, 'l'},
	{"no-time",      no_argument,       NULL, 'n'},
	{"output",       required_argument, NULL, 'o'},
	{"samplerate",   required_argument, NULL, 'r'},
	{"timing-data",  required_argument, NULL, 't'},
	{"version",      no_argument,       NULL, 'v'},
	{"yes",          no_argument,       NULL, 'y'},
	{NULL,           0,                 NULL, 0},
};

#if BK_USE_PLAYER
static int set_noecho (int nocanon)
{
	struct termios oldtc, newtc;

	tcgetattr (STDIN_FILENO, & oldtc);

	newtc = oldtc;

	if (nocanon) {
		newtc.c_lflag &= ~(ICANON | ECHO);
	}
	else {
		newtc.c_lflag |= (ICANON | ECHO);
	}

	tcsetattr (STDIN_FILENO, TCSANOW, & newtc);

	return 0;
}

static int getchar_nocanon (unsigned tcflags)
{
	int c;
	struct termios oldtc, newtc;

	tcgetattr (STDIN_FILENO, & oldtc);

	newtc = oldtc;
	newtc.c_lflag &= ~(ICANON | ECHO | tcflags);

	tcsetattr (STDIN_FILENO, TCSANOW, & newtc);
	c = getchar ();
	tcsetattr (STDIN_FILENO, TCSANOW, & oldtc);

	return c;
}
#endif

static int string_ends_with (char const * str, char const * tail)
{
	char const * sc, * tc;
	BKSize sl, tl;

	if (str == NULL || tail == NULL) {
		return 0;
	}

	sl = strlen (str);
	tl = strlen (tail);

	if (sl == 0 || tl == 0 || tl > sl) {
		return 0;
	}

	sc = & str [sl - 1];
	tc = & tail [tl - 1];

	for (; tc > tail; tc --, sc --) {
		if (* tc != * sc) {
			return 0;
		}
	}

	return 1;
}

static void print_version (void)
{
	printf ("%s v%s using BlipKit v%s\n", PROGRAM_NAME, BK_BLIPLAY_VERSION, BK_VERSION);
}

static void print_help (void)
{
	print_version ();
	printf (
		"usage: %1$s [options] file\n"
		"  %2$s-d, --load-dir path%3$s\n"
		"      Sets the path for loading resources\n"
		"      If not set, the parent directory of the input file is used\n"
		"  %2$s-f, --fast-forward time%3$s\n"
		"      Fast forward to time\n"
		"      Time format: number[s|b|t|f]\n"
		"      s: seconds, b: beats, t: ticks, f: frames\n"
		"      e.g., 12.4s, 24b, 760t, 45600f\n"
		"  %2$s-h, --help%3$s\n"
		"      Print this screen and exit\n"
		"  %2$s-i, --info%3$s\n"
		"      Print info about input file\n"
		"      Exits when not using with %2$s-o%3$s\n"
		"  %2$s-l, --end-time time%3$s\n"
		"      Maximum end time to export\n"
		"      Time format is the same as of %2$s-f%3$s\n"
		"  %2$s-n, --no-time%3$s\n"
		"      Do not print play time\n"
		"  %2$s-o, --output file.[wav|raw]%3$s\n"
		"      Write audio data to file\n"
		"      WAVE format: PCM 16 bit, stereo\n"
		"      RAW format: headerless native signed 16 bit, stereo\n"
		"      Does not print file info. Use %2$s-i%3$s to print info explictly.\n"
		"  %2$s-r, --samplerate value%3$s\n"
		"      Set sample rate of output (default: 44100)\n"
		"      Range: 16000 - 96000\n"
		"  %2$s-t, --timing-data unit[s|t]%3$s\n"
		"      Write timing data to [output file].txt\n"
		"      s: seconds, t: ticks\n"
		"  %2$s-y, --yes%3$s\n"
		"      Overwrite output file without asking\n",
		PROGRAM_NAME, colorYellow, colorNormal
	);
}

static void set_color (FILE * stream, int level)
{
	char const * color;

	if (!istty) {
		return;
	}

	switch (level) {
		default:
		case 0: {
			color = colorNormal;
			break;
		}
		case 1: {
			color = colorYellow;
			break;
		}
		case 2: {
			color = colorRed;
			break;
		}
	}

	fprintf (stream, "%s", color);
}

static void print_color_string (char const * format, va_list args, int level)
{
	FILE * stream = stdout;

	switch (level) {
		case 2: {
			stream = stderr;
			break;
		}
	}

	set_color (stream, level);
	vfprintf (stream, format, args);
	set_color (stream, 0);
	fflush (stream);
}

static void print_message (char const * format, ...)
{
	va_list args;

	va_start (args, format);
	print_color_string (format, args, 0);
	va_end (args);
}

static void print_notice (char const * format, ...)
{
	va_list args;

	va_start (args, format);
	print_color_string (format, args, 1);
	va_end (args);

}

static void print_error (char const * format, ...)
{
	va_list args;

	va_start (args, format);
	print_color_string (format, args, 2);
	va_end (args);
}

static void output_chunk (BKFrame const frames [], BKInt numFrames)
{
	switch (outputType) {
		case OUTPUT_TYPE_RAW: {
			fwrite (frames, numFrames, sizeof (BKFrame), outputFile);
			break;
		}
		case OUTPUT_TYPE_WAVE: {
			BKWaveFileWriterAppendFrames (& waveWriter, frames, numFrames);
			break;
		}
	}
}

#if BK_USE_PLAYER
static void fill_audio (BKContextWrapper * ctx, Uint8 * stream, int len)
{
	BKUInt numChannels = ctx -> ctx.numChannels;
	BKUInt numFrames   = len / sizeof (BKFrame) / numChannels;

	BKContextGenerate (& ctx -> ctx, (BKFrame *) stream, numFrames);
	output_chunk ((BKFrame *) stream, numFrames * numChannels);
}
#endif

static BKInt push_frames (BKFrame inFrames [], BKUInt size, void * info)
{
	return 0;
}

static void seek_context (BKContextWrapper * ctx, BKTime time)
{
	BKContextGenerateToTime (& ctx -> ctx, time, push_frames, NULL);
}

#if BK_USE_PLAYER
static BKInt init_sdl (BKContextWrapper * ctx, char const ** error)
{
	SDL_Init (SDL_INIT_AUDIO);

	SDL_AudioSpec wanted;

	wanted.freq     = ctx -> ctx.sampleRate;
	wanted.format   = AUDIO_S16SYS;
	wanted.channels = ctx -> ctx.numChannels;
	wanted.samples  = 512;
	wanted.callback = (void *) fill_audio;
	wanted.userdata = ctx;

	if (SDL_OpenAudio (& wanted, NULL) < 0) {
		* error = SDL_GetError ();
		return -1;
	}

	return 0;
}
#endif

static BKInt check_tracks_running (BKContextWrapper * ctx)
{
	BKTrackWrapper * track;
	BKInt numActive = ctx -> tracks.length;

	for (BKInt i = 0; i < ctx -> tracks.length; i ++) {
		track = BKArrayGetItemAtIndex (& ctx -> tracks, i);

		// exit if tracks have repeated
		if (flags & FLAG_NO_SOUND) {
			if (track -> interpreter.object.flags & (BKInterpreterFlagHasStopped | BKInterpreterFlagHasRepeated)) {
				numActive --;
			}
		}
		// exit if tracks have stopped
		else if (track -> interpreter.object.flags & BKInterpreterFlagHasStopped) {
			numActive --;
		}
	}

	if (numActive == 0) {
		return 0;
	}

	return 1;
}

static BKInt parse_seek_time (char const * string, BKTime * outTime, BKInt speed)
{
	double value;
	char   type;
	BKTime time;
	BKInt  argc;

	argc = sscanf (string, "%lf%c", & value, & type);

	if (argc < 1) {
		print_error ("--fast-forward: invalid format\n");
		return -1;
	}

	if (value < 0) {
		print_error ("--fast-forward: must be a positive\n");
		return -1;
	}

	if (argc < 2) {
		type = 't';
	}

	switch (type) {
		case 'f': {
			time = BKTimeMake (value, 0);
			break;
		}
		case 'b': {
			time = BKTimeFromSeconds (& ctx.ctx, (1.0 / 240) * speed * value);
			break;
		}
		case 't': {
			time = BKTimeFromSeconds (& ctx.ctx, (1.0 / 240) * value);
			break;
		}
		case 's': {
			time = BKTimeFromSeconds (& ctx.ctx, value);
			break;
		}
		default: {
			print_error ("--fast-forward: unknown unit '%c'\n", type);
			return -1;
		}
	}

	* outTime = time;

	return 0;
}

static void print_time (BKContextWrapper * ctx)
{
	int frames = BKTimeGetTime (ctx -> ctx.currentTime) * 100 / ctx -> ctx.sampleRate;
	int frac   = frames % 100;
	int hsecs  = frames / 100;

	int secs = hsecs % 60;
	int mins = hsecs / 60;

	printf ("%s%4d:%02d.%02d%s\r", colorGreen, mins, secs, frac, colorNormal);
	fflush (stdout);
}

static BKInt count_slots (BKArray * array)
{
	void * ptr;
	BKInt count = 0;

	for (BKInt i = 0; i < array -> length; i ++) {
		BKArrayGetItemAtIndexCopy (array, i, & ptr);

		if (ptr) {
			count ++;
		}
	}

	return count;
}

static void waveform_get_name (char name [], BKSize size, BKEnum waveform, BKInt * isCustom)
{
	char const * waveformName;

	if (waveform & BK_INTR_CUSTOM_WAVEFORM_FLAG) {
		if (isCustom) {
			*isCustom = 1;
		}

		snprintf (name, size, "%u", waveform &~ BK_INTR_CUSTOM_WAVEFORM_FLAG);
		return;
	}

	switch (waveform) {
		case BK_SQUARE:   waveformName = "square";   break;
		case BK_TRIANGLE: waveformName = "triangle"; break;
		case BK_NOISE:    waveformName = "noise";    break;
		case BK_SAWTOOTH: waveformName = "sawtooth"; break;
		case BK_SINE:     waveformName = "sine";     break;
		case BK_SAMPLE:   waveformName = "sample";   break;
		default:          waveformName = "unknown";  break;
	}

	if (isCustom) {
		*isCustom = 0;
	}

	strcpy (name, waveformName);
}

static void print_track_info (BKContextWrapper * ctx)
{
	BKEnum waveform;
	BKTrackWrapper * track;
	BKInt isCustom;
	char name [64];

	for (BKInt i = 0; i < ctx -> tracks.length; i ++) {
		track = BKArrayGetItemAtIndex (& ctx -> tracks, i);
		waveform = track -> waveform;

		waveform_get_name (name, sizeof (name), waveform, & isCustom);

		if (isCustom) {
			print_message ("              #%d: custom %s\n", i, name);
		}
		else {
			print_message ("              #%d: %s\n", i, name);
		}
	}
}

static void print_info (BKContextWrapper * ctx)
{
	print_message ("      step ticks: %d\n", ctx -> stepTicks);
	print_message ("     instruments: %d\n", count_slots (& ctx -> instruments));
	print_message ("custom waveforms: %d\n", count_slots (& ctx -> waveforms));
	print_message ("         samples: %d\n", count_slots (& ctx -> samples));
	print_message ("          tracks: %d\n", ctx -> tracks.length);
	print_track_info (ctx);
	print_message ("     sample rate: %d\n", ctx -> ctx.sampleRate);
	print_message ("        channels: %d\n\n", ctx -> ctx.numChannels);
}

static BKInt should_overwrite_output (char const * filename)
{
	char line [8];

	print_notice ("Output file already exists. Overwrite? [Y/n] ");

	strcpy (line, "");
	fgets (line, sizeof (line), stdin);

	if (line [0] == 'Y') {
		return 1;
	}

	return 0;
}

static BKInt handle_options (BKContextWrapper * ctx, int argc, char * argv [])
{
	int    opt;
	int    longoptind = 1;
	BKUInt speed      = 0;
	FILE * inputFile;
	struct stat st;
	BKString path, * loadPath = NULL;
	BKEnum opts;

	opterr = 0;

	while ((opt = getopt_long (argc, (void *) argv, "d:f:hil:no:pr:t:vy", options, & longoptind)) != -1) {
		switch (opt) {
			case 'd': {
				if (BKStringAlloc (& loadPath, optarg, -1) < 0) {
					print_error ("Allocation error\n");
					return -1;
				}
				break;
			}
			case 'f': {
				flags |= FLAG_HAS_SEEK_TIME;
				strncpy (seekTimeString, optarg, 64);
				break;
			}
			case 'h':
			case 'v': {
				print_help ();
				exit (0);
				break;
			}
			case 'i': {
				flags |= FLAG_INFO_EXPLICITE;
				break;
			}
			case 'l': {
				flags |= FLAG_HAS_END_TIME;
				strncpy (endTimeString, optarg, 64);
				break;
			}
			case 'n': {
				flags |= FLAG_PRINT_NO_TIME;
				break;
			}
			case 'o': {
				outputFilename = optarg;
				flags = (flags & ~FLAG_INFO) | FLAG_NO_SOUND;
				break;
			}
			case 'r': {
				sampleRate = atoi (optarg);
				break;
			}
			case 't': {
				if (strcmp (optarg, "s") == 0) {
					flags |= FLAG_TIMING_UNIT_SECS;
				}
				else if (strcmp (optarg, "t") == 0) {
					flags |= FLAG_TIMING_UNIT_TICKS;
				}
				else {
					print_error ("Unknown timing unit '%s'; use 's' or 't'\n", optarg);
					return -1;
				}

				break;
			}
			case 'y': {
				flags |= FLAG_YES;
				break;
			}
			default: {
				print_help ();

				if (optind < argc) {
					print_error ("Unknown option '%c' before %s\n", optopt, argv [optind]);
				}
				else {
					print_error ("Unknown option '%c'\n", optopt);
				}

				exit (1);
				break;
			}
		}
	}

	if (flags & FLAG_NO_SOUND) {
		flags &= ~FLAG_INFO;
	}

	if (flags & FLAG_INFO_EXPLICITE) {
		flags |= FLAG_INFO;
	}

	if (optind <= argc) {
		filename = argv [optind];
	}
	else {
		print_help ();
		return -1;
	}

	if (filename == NULL) {
		print_help ();
		print_error ("No input file given\n");
		return -1;
	}

	if (outputFilename) {
		// output file already exists
		if (stat (outputFilename, & st) == 0) {
			if (!S_ISREG (st.st_mode)) {
				print_error ("Output file already exists and is not a file: %s\n", outputFilename);
				return -1;
			}

			// ask if file should be overwritten
			if (!(flags & FLAG_YES)) {
				if (!should_overwrite_output (outputFilename)) {
					print_error ("Not overwriting\n");
					return -1;
				}
			}
		}

		if (string_ends_with (outputFilename, ".wav")) {
			outputType = OUTPUT_TYPE_WAVE;
		}
		else if (string_ends_with (outputFilename, ".raw")) {
			outputType = OUTPUT_TYPE_RAW;
		}
		else {
			print_error ("Only .wav and .raw is supported for output\n");
			return -1;
		}

		outputFile = fopen (outputFilename, "wb+");

		if (outputFile == NULL) {
			print_error ("Could not open output file: %s\n", outputFilename);
			return -1;
		}

		if (outputType == OUTPUT_TYPE_WAVE) {
			if (BKWaveFileWriterInit (& waveWriter, outputFile, numChannels, sampleRate) < 0) {
				return -1;
			}
		}
	}
#if !BK_USE_PLAYER
	else {
		print_error ("SDL support disabled. Output file must be given\n");
		return -1;
	}
#endif

	opts = ((flags & FLAG_TIMING_UNIT_MASK) >> FLAG_TIMING_UNIT_SHIFT) << BKTrackWrapperOptionTimingShift;

	if (BKContextWrapperInit (ctx, numChannels, sampleRate, opts) < 0) {
		print_error ("Could not initialize context\n");
		return -1;
	}

#if BK_USE_PLAYER
	if ((flags & FLAG_NO_SOUND) == 0) {
		char const * error = NULL;

		if (init_sdl (ctx, & error) < 0) {
			print_error ("Could not initialize SDL: %s\n", error);
			return -1;
		}
	}
#endif

	if (BKStringInit (& path, filename, -1) < 0) {
		return -1;
	}

	// use stdin
	if (BKStringIsEqualToChars (& path, "-")) {
		inputFile = stdin;
	}
	// use path
	else {
		if (stat (path.chars, & st) < 0) {
			print_error ("No such file: %s\n", path.chars);
			return -1;
		}

		if (S_ISDIR (st.st_mode)) {
			if (BKStringAppendChars (& path, "/DATA.blip") < 0) {
				return -1;
			}
		}

		inputFile = fopen (path.chars, "rb");

		if (inputFile == NULL) {
			print_error ("No such file: %s\n", path.chars);
			return -1;
		}

		set_color (stderr, 2);
	}

	// path set by option
	if (loadPath) {
		if (stat (loadPath -> chars, & st) < 0) {
			print_error ("No such path for loading: %s\n", loadPath -> chars);
			return -1;
		}

		if (!S_ISDIR (st.st_mode)) {
			print_error ("Load path is not a directory: %s\n", loadPath -> chars);
			return -1;
		}

		BKStringReplaceInRange (& ctx -> compiler.loadPath, loadPath, 0, ctx -> compiler.loadPath.length);
		BKDispose (loadPath);

		loadPath = & ctx -> compiler.loadPath;
	}
	// parent directory from input file
	else {
		loadPath = & ctx -> compiler.loadPath;

		if (BKStringGetDirname (& loadPath, & path) < 0) {
			return -1;
		}
	}

	if ((flags & FLAG_TIMING_UNIT_MASK) && outputFilename) {
		BKString path;

		if (BKStringInit (& path, outputFilename, -1) < 0) {
			print_error ("Allocation error\n");
			return -1;
		}

		if (BKStringAppendChars (& path, ".txt") < 0) {
			print_error ("Allocation error\n");
			return -1;
		}

		timingFile = fopen (path.chars, "w");

		BKDispose (& path);
	}

	if (BKContextWrapperLoadFile (ctx, inputFile, NULL) < 0) {
		print_error ("Failed to load file: %s\n", path.chars);
		set_color (stderr, 0);
		fclose (inputFile);
		return -1;
	}

	set_color (stderr, 0);

	if (inputFile != stdin) {
		fclose (inputFile);
	}

	if (flags & FLAG_HAS_SEEK_TIME) {
		speed = ctx -> stepTicks;

		if (parse_seek_time (seekTimeString, & seekTime, speed) < 0) {
			return -1;
		}
	}

	if (flags & FLAG_HAS_END_TIME) {
		speed = ctx -> stepTicks;

		if (parse_seek_time (endTimeString, & endTime, speed) < 0) {
			return -1;
		}
	}

	return 0;
}

static void writeTimingData (void)
{
	BKEnum waveform;
	BKTrackWrapper * track;

	if (timingFile) {
		char name [64];
		char buffer [4096];
		BKSize size;

		fprintf (
			timingFile,
			"%% Timing data for tracks contained in '%s'\n"
			"%%\n"
			"%% track:[waveformtype]:[tracknumber]\n"
			"%% l:[tick]:[lineno]\n"
			"%% If 'lineno' is ommited, the line follows the previous one"
			"\n\n",
			outputFilename
		);

		for (BKInt i = 0; i < ctx.tracks.length; i ++) {
			track = BKArrayGetItemAtIndex (& ctx.tracks, i);
			waveform = track -> waveform;

			if (i > 0) {
				fprintf (timingFile, "\n");
			}

			waveform_get_name (name, sizeof (name), waveform, NULL);
			fprintf (timingFile, "track:%s:%d\n", name, track -> slot);

			while ((size = BKByteBufferReadBytes (& track -> timingData, buffer, sizeof (buffer)))) {
				if (size < 0) {
					print_error ("Buffer error\n");
					return;
				}

				fwrite (buffer, sizeof (char), size, timingFile);
			}
		}

		fclose (timingFile);
	}
}

static void cleanup (void)
{
	if (outputFile) {
		if (outputType == OUTPUT_TYPE_WAVE) {
			BKWaveFileWriterTerminate (& waveWriter);
			BKDispose (& waveWriter);
		}

		fclose (outputFile);
	}

	BKDispose (& ctx);
#if BK_USE_PLAYER
	SDL_CloseAudio ();
#endif
}

static BKInt write_output (BKContextWrapper * ctx)
{
	BKInt numFrames = 512;
	BKInt numChannels = ctx -> ctx.numChannels;
	BKFrame * frames = malloc (numFrames * numChannels * sizeof (BKFrame));

	if (frames == NULL) {
		return -1;
	}

	while (check_tracks_running (ctx)) {
		BKContextGenerate (& ctx -> ctx, frames, numFrames);
		output_chunk (frames, numFrames * numChannels);

		if (flags & FLAG_HAS_END_TIME) {
			if (BKTimeIsGreaterEqual (ctx -> ctx.currentTime, endTime)) {
				break;
			}
		}
	}

	free (frames);

	return 0;
}

static BKInt runloop (BKContextWrapper * ctx)
{
#if BK_USE_PLAYER
	int c;
	int res;
	int nfds;
	int flag = 1;
	fd_set fds, fdsc;
	struct timeval timeout;

	FD_ZERO (& fds);
	FD_SET (STDIN_FILENO, & fds);
	nfds = STDIN_FILENO + 1;

	set_noecho (1);
	print_notice ("Press [q] to quit\n");

	SDL_PauseAudio (0);

	do {
		FD_COPY (& fds, & fdsc);
		timeout.tv_sec  = 0;
		timeout.tv_usec = updateUSecs;

		res = select (nfds, & fdsc, NULL, NULL, & timeout);

		if (res < 0) {
			return -1;
		}
		else if (res > 0) {
			c = getchar_nocanon (0);

			switch (c) {
				case 'q': {
					flag = 0;
					break;
				}
			}
		}

		if (!(flags & FLAG_PRINT_NO_TIME)) {
			print_time (ctx);
		}

		if (check_tracks_running (ctx) == 0) {
			break;
		}
	}
	while (flag);

	set_noecho (0);

	printf ("\n");
#else
	do {
		if (check_tracks_running (ctx) == 0) {
			break;
		}
	}
	while (1);
#endif

#if BK_USE_PLAYER
	SDL_PauseAudio (1);
#endif

	return 0;
}

// undef SDL main
#ifdef main
#undef main
#endif

int main (int argc, char * argv [])
{
	istty = isatty (STDOUT_FILENO);

	if (istty) {
		colorRed    = "\033[31m";
		colorYellow = "\033[33m";
		colorGreen  = "\033[32m";
		colorNormal = "\033[0m";
	}

	if (handle_options (& ctx, argc, argv) < 0) {
		return 1;
	}

	if (flags & FLAG_INFO) {
		print_info (& ctx);
	}

	if (flags & FLAG_INFO_EXPLICITE) {
		return 0;
	}

	if (flags & FLAG_HAS_SEEK_TIME) {
		print_notice ("=> Fast forward to %s\n", seekTimeString);
		seek_context (& ctx, seekTime);
	}

	if (flags & FLAG_NO_SOUND) {
		if (write_output (& ctx) < 0) {
			return 2;
		}
	}
	else {
		if (runloop (& ctx) < 0) {
			return 3;
		}
	}

	writeTimingData ();

	cleanup ();

	return 0;
}
