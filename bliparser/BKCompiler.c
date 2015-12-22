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

#include <stdio.h>
#include <unistd.h>
#include "BKTone.h"
#include "BKCompiler.h"

#define VOLUME_UNIT (BK_MAX_VOLUME / 255)
#define PITCH_UNIT (BK_FINT20_UNIT / 100)

#define BK_MAX_GROUP           255
#define BK_MAX_SEQ_LENGTH      256
#define BK_MAX_WAVEFORM_LENGTH  64
#define BK_MAX_PATH           2048
#define BK_MAX_STEPTICKS       240

#define BUFFER_INIT_SIZE 4096

enum BKCompilerFlag
{
	BKCompilerFlagAllocated = 1 << 0,
	BKCompilerFlagOpenGroup = 1 << 1,
	BKCompilerFlagArpeggio  = 1 << 2,
};

enum BKCompilerEnvelopeType
{
	BKCompilerEnvelopeTypeVolumeSeq,
	BKCompilerEnvelopeTypePitchSeq,
	BKCompilerEnvelopeTypePanningSeq,
	BKCompilerEnvelopeTypeDutyCycleSeq,
	BKCompilerEnvelopeTypeADSR,
	BKCompilerEnvelopeTypeVolumeEnv,
	BKCompilerEnvelopeTypePitchEnv,
	BKCompilerEnvelopeTypePanningEnv,
	BKCompilerEnvelopeTypeDutyCycleEnv,
};

enum BKCompilerMiscCmds
{
	BKCompilerMiscLoad,
	BKCompilerMiscRaw,
	BKCompilerMiscPitch,
	BKCompilerMiscSampleSustainRange,
};

/**
 * Used for lookup tables to assign a string to a value
 */
typedef struct
{
	char * name;
	BKInt  value;
	BKUInt flags;
} const strval;

/**
 * Group stack item
 */
typedef struct {
	BKUInt            level;
	BKInstruction     groupType;
	BKCompilerTrack * track;
	BKByteBuffer    * cmdBuffer;
} BKCompilerGroup;

/**
 * Note lookup table
 */
static strval noteNames [] =
{
	{"a",  9},
	{"a#", 10},
	{"b",  11},
	{"c",  0},
	{"c#", 1},
	{"d",  2},
	{"d#", 3},
	{"e",  4},
	{"f",  5},
	{"f#", 6},
	{"g",  7},
	{"g#", 8},
	{"h",  11},
};

/**
 * Command lookup table
 */
static strval cmdNames [] =
{
	{"a",         BKIntrAttack},
	{"as",        BKIntrArpeggioSpeed},
	{"at",        BKIntrAttackTicks},
	{"d",         BKIntrSample},
	{"dc",        BKIntrDutyCycle},
	{"dn",        BKIntrSampleRange},
	{"dr",        BKIntrSampleRepeat},
	{"ds",        BKIntrSampleSustainRange},
	{"e",         BKIntrEffect},
	{"g",         BKIntrGroupJump},
	{"grp",       BKIntrGroupDef, BKCompilerFlagOpenGroup},
	{"i",         BKIntrInstrument},
	{"instr",     BKIntrInstrumentDef, BKCompilerFlagOpenGroup},
	{"m",         BKIntrMute},
	{"mt",        BKIntrMuteTicks},
	{"p",         BKIntrPanning},
	{"pt",        BKIntrPitch},
	{"pw",        BKIntrPhaseWrap},
	{"r",         BKIntrRelease},
	{"rt",        BKIntrReleaseTicks},
	{"s",         BKIntrStep},
	{"samp",      BKIntrSampleDef},
	{"st",        BKIntrStepTicks},
	{"stepticks", BKIntrStepTicks},
	{"t",         BKIntrTicks},
	{"track",     BKIntrTrackDef, BKCompilerFlagOpenGroup},
	{"v",         BKIntrVolume},
	{"vm",        BKIntrMasterVolume},
	{"w",         BKIntrWaveform},
	{"wave",      BKIntrWaveformDef, BKCompilerFlagOpenGroup},
	{"x",         BKIntrRepeat},
	{"xb",        BKIntrSetRepeatStart},
	{"z",         BKIntrEnd},
};

/**
 * Effect lookup table
 */
static strval effectNames [] =
{
	{"pr", BK_EFFECT_PORTAMENTO},
	{"ps", BK_EFFECT_PANNING_SLIDE},
	{"tr", BK_EFFECT_TREMOLO},
	{"vb", BK_EFFECT_VIBRATO},
	{"vs", BK_EFFECT_VOLUME_SLIDE},
};

/**
 * Waveform lookup table
 */
static strval waveformNames [] =
{
	{"no",       BK_NOISE},
	{"noi",      BK_NOISE},
	{"noise",    BK_NOISE},
	{"sam",      BK_SAMPLE},
	{"sample",   BK_SAMPLE},
	{"saw",      BK_SAWTOOTH},
	{"sawtooth", BK_SAWTOOTH},
	{"si",       BK_SINE},
	{"sin",      BK_SINE},
	{"sine",     BK_SINE},
	{"sm",       BK_SAMPLE},
	{"sq",       BK_SQUARE},
	{"sqr",      BK_SQUARE},
	{"square",   BK_SQUARE},
	{"sw",       BK_SAWTOOTH},
	{"tr",       BK_TRIANGLE},
	{"tri",      BK_TRIANGLE},
	{"triangle", BK_TRIANGLE},
};

/**
 * Envelope lookup table
 */
static strval envelopeNames [] =
{
	{"a",    BKCompilerEnvelopeTypePitchSeq},
	{"adsr", BKCompilerEnvelopeTypeADSR},
	{"anv",  BKCompilerEnvelopeTypePitchEnv},
	{"dc",   BKCompilerEnvelopeTypeDutyCycleSeq},
	{"dcnv", BKCompilerEnvelopeTypeDutyCycleEnv},
	{"p",    BKCompilerEnvelopeTypePanningSeq},
	{"pnv",  BKCompilerEnvelopeTypePanningEnv},
	{"v",    BKCompilerEnvelopeTypeVolumeSeq},
	{"vnv",  BKCompilerEnvelopeTypeVolumeEnv},
};

/**
 * Miscellaneous lookup table
 */
static strval miscNames [] =
{
	{"ds",   BKCompilerMiscSampleSustainRange},
	{"load", BKCompilerMiscLoad},
	{"pt",   BKCompilerMiscPitch},
	{"raw",  BKCompilerMiscRaw},
};

#define NUM_WAVEFORM_NAMES (sizeof (waveformNames) / sizeof (strval))
#define NUM_CMD_NAMES (sizeof (cmdNames) / sizeof (strval))
#define NUM_EFFECT_NAMES (sizeof (effectNames) / sizeof (strval))
#define NUM_NOTE_NAMES (sizeof (noteNames) / sizeof (strval))
#define NUM_ENVELOPE_NAMES (sizeof (envelopeNames) / sizeof (strval))
#define NUM_MISC_NAMES (sizeof (miscNames) / sizeof (strval))

extern BKClass BKCompilerClass;

/**
 * Convert string to signed integer like `atoi`
 * Returns alternative value if string is NULL
 */
static int atoix (char const * str, int alt)
{
	return str ? atoi (str) : alt;
}

/**
 * Compare two strings like `strcmp`
 * Returns -1 is if one of the strings is NULL
 */
static int strcmpx (char const * a, char const * b)
{
	return (a && b) ? strcmp (a, b) : -1;
}

/**
 * Compare name of command with string
 * Used as callback for `bsearch`
 */
static int strvalcmp (char const * name, strval const * item)
{
	return strcmp (name, item -> name);
}

static BKInt BKCompilerTrackInit (BKCompilerTrack * track)
{
	memset (track, 0, sizeof (* track));

	if (BKArrayInit (& track -> cmdGroups, sizeof (BKByteBuffer *), 0) < 0) {
		return -1;
	}

	if (BKByteBufferInit (& track -> globalCmds, BUFFER_INIT_SIZE, BKByteBufferOptionKeepBytes | BKByteBufferOptionContinuousStorage) < 0) {
		return -1;
	}

	track -> slot = -1;

	return 0;
}

static BKInt BKCompilerTrackAlloc (BKCompilerTrack ** outTrack)
{
	BKInt res;
	BKCompilerTrack * track = malloc (sizeof (* track));

	* outTrack = NULL;

	if (track == NULL) {
		return -1;
	}

	res = BKCompilerTrackInit (track);

	if (res != 0) {
		free (track);
		return res;
	}

	track -> flags |= BKCompilerFlagAllocated;
	* outTrack = track;

	return 0;
}

static void BKCompilerTrackReset (BKCompilerTrack * track, BKInt keepData)
{
	BKByteBuffer * buffer;

	BKArrayEmpty (& track ->  cmdGroups, keepData);
	BKByteBufferClear (& track ->  globalCmds, BKBitCond2 (BKByteBufferOptionReuseStorage, keepData));

	for (BKInt i = 0; i < track -> cmdGroups.length; i ++) {
		BKArrayGetItemAtIndexCopy (& track -> cmdGroups, i, & buffer);
		BKDispose (buffer);
	}
}

static void BKCompilerTrackDispose (BKCompilerTrack * track)
{
	BKUInt flags;

	if (track == NULL) {
		return;
	}

	BKCompilerTrackReset (track, 0);

	BKDispose (& track -> cmdGroups);
	BKDispose (& track -> globalCmds);

	flags = track -> flags;
	memset (track, 0, sizeof (* track));

	if (flags & BKCompilerFlagAllocated) {
		free (track);
	}
}

BKInt BKCompilerInit (BKCompiler * compiler)
{
	if (BKObjectInit (compiler, & BKCompilerClass, sizeof (*compiler))) {
		return -1;
	}

	if (BKArrayInit (& compiler -> groupStack, sizeof (BKCompilerGroup), 8) < 0) {
		BKDispose (compiler);
		return -1;
	}

	if (BKArrayInit (& compiler -> tracks, sizeof (BKCompilerTrack *), 8) < 0) {
		BKDispose (compiler);
		return -1;
	}

	if (BKCompilerTrackInit (& compiler -> globalTrack) < 0) {
		BKDispose (compiler);
		return -1;
	}

	if (BKArrayInit (& compiler -> instruments, sizeof (BKInstrument *), 4) < 0) {
		BKDispose (compiler);
		return -1;
	}

	if (BKArrayInit (& compiler -> waveforms, sizeof (BKData *), 4) < 0) {
		BKDispose (compiler);
		return -1;
	}

	if (BKArrayInit (& compiler -> samples, sizeof (BKData *), 4) < 0) {
		BKDispose (compiler);
		return -1;
	}

	if (BKStringInit (& compiler -> loadPath, NULL, 0)) {
		BKDispose (compiler);
		return -1;
	}

	BKCompilerReset (compiler, 0);

	return 0;
}

static void BKCompilerDispose (BKCompiler * compiler)
{
	BKCompilerReset (compiler, 0);
	BKDispose (& compiler -> loadPath);
}

static BKInt BKCompilerStrvalTableLookup (strval table [], BKSize size, char const * name, BKInt * outValue, BKUInt * outFlags)
{
	strval * item;

	if (name == NULL) {
		return 0;
	}

	item = bsearch (name, table, size, sizeof (strval), (void *) strvalcmp);

	if (item == NULL) {
		return 0;
	}

	* outValue = item -> value;

	if (outFlags) {
		* outFlags = item -> flags;
	}

	return 1;
}

/**
 * Get note value for note string
 *
 * note octave [+-pitch]
 * Examples: c#3, e2+56, a#2-26
 */
static BKInt BKCompilerParseNote (char const * name)
{
	char  note [3];
	BKInt value  = 0;
	BKInt octave = 0;
	BKInt pitch  = 0;

	strcpy (note, "");  // empty name
	sscanf (name, "%2[a-z#]%u%d", note, & octave, & pitch);  // scan string; d#3[+-p] => "d#", 3, p

	if (BKCompilerStrvalTableLookup (noteNames, NUM_NOTE_NAMES, note, & value, NULL)) {
		value += octave * 12;
		value = BKClamp (value, BK_MIN_NOTE, BK_MAX_NOTE);
		value = (value << BK_FINT20_SHIFT) + pitch * PITCH_UNIT;
	}

	return value;
}

static BKByteBuffer * BKCompilerGetCmdGroupForIndex (BKCompiler * compiler, BKInt index)
{
	BKByteBuffer    * buffer = NULL;
	BKCompilerGroup * group  = BKArrayGetLastItem (& compiler -> groupStack);
	BKCompilerTrack * track  = group -> track;

	// search for free slot
	if (index == -1) {
		for (BKInt i = 0; i < track -> cmdGroups.length; i ++) {
			BKArrayGetItemAtIndexCopy (& track -> cmdGroups, i, & buffer);

			// is empty slot
			if (buffer == NULL) {
				index = i;
				break;
			}
		}

		if (index == -1) {
			index  = (BKInt) track -> cmdGroups.length;
			buffer = NULL;
		}
	}
	else {
		// search item at slot
		BKArrayGetItemAtIndexCopy (& track -> cmdGroups, index, & buffer);

		// overwrite existing buffer
		if (buffer != NULL) {
			BKByteBufferClear (buffer, BKByteBufferOptionReuseStorage);
		}
	}

	// create new item
	if (buffer == NULL) {
		// fill slots with empty buffers
		while (track -> cmdGroups.length <= index) {
			if (BKArrayPushPtr (& track -> cmdGroups) == NULL) {
				return NULL;
			}
		}

		if (BKByteBufferAlloc (& buffer, BUFFER_INIT_SIZE, BKByteBufferOptionKeepBytes | BKByteBufferOptionContinuousStorage) < 0) {
			return NULL;
		}

		// set item at index
		if (BKArraySetItemAtIndex (& track -> cmdGroups, & buffer, index) < 0) {
			return NULL;
		}
	}

	return buffer;
}

static BKInstrument * BKCompilerGetInstrumentForIndex (BKCompiler * compiler, BKInt index)
{
	BKInstrument * instrument = NULL;

	// search for free slot
	if (index == -1) {
		for (BKInt i = 0; i < compiler -> instruments.length; i ++) {
			BKArrayGetItemAtIndexCopy (& compiler -> instruments, i, & instrument);

			// is empty slot
			if (instrument == NULL) {
				index = i;
				break;
			}
		}

		if (index == -1) {
			index = (BKInt) compiler -> instruments.length;
			instrument = NULL;
		}
	}
	else {
		// search item at slot
		BKArrayGetItemAtIndexCopy (& compiler -> instruments, index, & instrument);
	}

	// create new item
	if (instrument == NULL) {
		// fill slots with empty buffers
		while (compiler -> instruments.length <= index) {
			if (BKArrayPushPtr (& compiler -> instruments) == NULL) {
				return NULL;
			}
		}

		if (BKInstrumentAlloc (& instrument) < 0) {
			return NULL;
		}

		// set item at index
		if (BKArraySetItemAtIndex (& compiler -> instruments, & instrument, index) < 0) {
			BKDispose (instrument);
			return NULL;
		}
	}

	return instrument;
}

static BKData * BKCompilerGetWaveformForIndex (BKCompiler * compiler, BKInt index)
{
	BKData * data = NULL;

	// search for free slot
	if (index == -1) {
		for (BKInt i = 0; i < compiler -> waveforms.length; i ++) {
			BKArrayGetItemAtIndexCopy (& compiler -> waveforms, i, & data);

			// is empty slot
			if (data == NULL) {
				index = i;
				break;
			}
		}

		if (index == -1) {
			index = (BKInt) compiler -> waveforms.length;
			data = NULL;
		}
	}
	else {
		// search item at slot
		BKArrayGetItemAtIndexCopy (& compiler -> waveforms, index, & data);
	}

	// create new item
	if (data == NULL) {
		// fill slots with empty buffers
		while (compiler -> waveforms.length <= index) {
			if (BKArrayPushPtr (& compiler -> waveforms) == NULL) {
				return NULL;
			}
		}

		if (BKDataAlloc (& data) < 0) {
			return NULL;
		}

		// set item at index
		if (BKArraySetItemAtIndex (& compiler -> waveforms, & data, index) < 0) {
			BKDispose (data);
			return NULL;
		}
	}

	return data;
}

static BKData * BKCompilerGetSampleForIndex (BKCompiler * compiler, BKInt index)
{
	BKData * data = NULL;

	// search for free slot
	if (index == -1) {
		for (BKInt i = 0; i < compiler -> samples.length; i ++) {
			BKArrayGetItemAtIndexCopy (& compiler -> samples, i, & data);

			// is empty slot
			if (data == NULL) {
				index = i;
				break;
			}
		}

		if (index == -1) {
			index = (BKInt) compiler -> samples.length;
			data = NULL;
		}
	}
	else {
		// search item at slot
		BKArrayGetItemAtIndexCopy (& compiler -> samples, index, & data);
	}

	// create new item
	if (data == NULL) {
		// fill slots with empty buffers
		while (compiler -> samples.length <= index) {
			if (BKArrayPushPtr (& compiler -> samples) == NULL) {
				return NULL;
			}
		}

		if (BKDataAlloc (& data) < 0) {
			return NULL;
		}

		// set item at index
		if (BKArraySetItemAtIndex (& compiler -> samples, & data, index) < 0) {
			BKDispose (data);
			return NULL;
		}
	}

	return data;
}

static BKInt BKCompilerInstrumentSequenceParse (BKSTCmd const * cmd, BKInt * sequence, BKInt * outRepeatBegin, BKInt * outRepeatLength, BKInt multiplier)
{
	BKInt length = (BKInt) cmd -> numArgs - 2;
	BKInt repeatBegin = 0, repeatLength = 0;

	length       = BKMin (length, BK_MAX_SEQ_LENGTH);
	repeatBegin  = atoix (cmd -> args [0].arg, 0);
	repeatLength = atoix (cmd -> args [1].arg, 1);

	if (repeatBegin > length) {
		repeatBegin = length;
	}

	if (repeatBegin + repeatLength > length) {
		repeatLength = length - repeatBegin;
	}

	* outRepeatBegin  = repeatBegin;
	* outRepeatLength = repeatLength;

	for (BKInt i = 0; i < length; i ++) {
		sequence [i] = atoix (cmd -> args [i + 2].arg, 0) * multiplier;
	}

	return length;
}

static BKInt BKCompilerInstrumentEnvelopeParse (BKSTCmd const * cmd, BKSequencePhase * phases, BKInt * outRepeatBegin, BKInt * outRepeatLength, BKInt multiplier)
{
	BKInt length = (BKInt) cmd -> numArgs - 2;
	BKInt repeatBegin = 0, repeatLength = 0;

	length       = BKMin (length, BK_MAX_SEQ_LENGTH);
	repeatBegin  = atoix (cmd -> args [0].arg, 0);
	repeatLength = atoix (cmd -> args [1].arg, 1);

	if (repeatBegin > length) {
		repeatBegin = length;
	}

	if (repeatBegin + repeatLength > length) {
		repeatLength = length - repeatBegin;
	}

	* outRepeatBegin  = repeatBegin;
	* outRepeatLength = repeatLength;

	for (BKInt i = 0, j = 0; i < length; i += 2, j ++) {
		phases [j].steps = atoix (cmd -> args [i + 2].arg, 0);
		phases [j].value = atoix (cmd -> args [i + 3].arg, 0) * multiplier;
	}

	return length / 2;
}

static BKInt BKCompilerPushCommandInstrument (BKCompiler * compiler, BKSTCmd const * cmd)
{
	BKInt value;
	BKInt sequence [BK_MAX_SEQ_LENGTH];
	BKSequencePhase phases [BK_MAX_SEQ_LENGTH];
	BKInt sequenceLength, repeatBegin, repeatLength;
	BKInt asdr [4];
	BKInt res = 0;
	BKInstrument * instrument = compiler -> currentInstrument;

	// search for envelope type
	if (BKCompilerStrvalTableLookup (envelopeNames, NUM_ENVELOPE_NAMES, cmd -> name, & value, NULL) == 0) {
		fprintf (stderr, "Unknown command '%s' on line %u:%u\n", cmd -> name, cmd -> lineno, cmd -> colno);
		return 0;
	}

	switch (value) {
		case BKCompilerEnvelopeTypeVolumeSeq:
		case BKCompilerEnvelopeTypePitchSeq:
		case BKCompilerEnvelopeTypePanningSeq:
		case BKCompilerEnvelopeTypeDutyCycleSeq: {
			if (cmd -> numArgs < 3) {
				fprintf (stderr, "Sequence '%s' needs at least 3 values on line %u:%u\n", cmd -> name, cmd -> lineno, cmd -> colno);
				return 0;
			}
			break;
		}
		case BKCompilerEnvelopeTypeADSR: {
			if (cmd -> numArgs < 4) {
				fprintf (stderr, "Sequence '%s' needs 4 values on line %u:%u\n", cmd -> name, cmd -> lineno, cmd -> colno);
				return 0;
			}
			break;
		}
		case BKCompilerEnvelopeTypeVolumeEnv:
		case BKCompilerEnvelopeTypePitchEnv:
		case BKCompilerEnvelopeTypePanningEnv:
		case BKCompilerEnvelopeTypeDutyCycleEnv: {
			if (cmd -> numArgs < 4) {
				fprintf (stderr, "Sequence '%s' needs at least 4 values on line %u:%u\n", cmd -> name, cmd -> lineno, cmd -> colno);
				return 0;
			}
			break;
		}
	}

	switch (value) {
		case BKCompilerEnvelopeTypeVolumeSeq: {
			sequenceLength = BKCompilerInstrumentSequenceParse (cmd, sequence, & repeatBegin, & repeatLength, (BK_MAX_VOLUME / 255));
			res = BKInstrumentSetSequence (instrument, BK_SEQUENCE_VOLUME, sequence, sequenceLength, repeatBegin, repeatLength);
			break;
		}
		case BKCompilerEnvelopeTypePitchSeq: {
			sequenceLength = BKCompilerInstrumentSequenceParse (cmd, sequence, & repeatBegin, & repeatLength, (BK_FINT20_UNIT / 100));
			res = BKInstrumentSetSequence (instrument, BK_SEQUENCE_PITCH, sequence, sequenceLength, repeatBegin, repeatLength);
			break;
		};
		case BKCompilerEnvelopeTypePanningSeq: {
			sequenceLength = BKCompilerInstrumentSequenceParse (cmd, sequence, & repeatBegin, & repeatLength, (BK_MAX_VOLUME / 255));
			res = BKInstrumentSetSequence (instrument, BK_SEQUENCE_PANNING, sequence, sequenceLength, repeatBegin, repeatLength);
			break;
		}
		case BKCompilerEnvelopeTypeDutyCycleSeq: {
			sequenceLength = BKCompilerInstrumentSequenceParse (cmd, sequence, & repeatBegin, & repeatLength, 1);
			res = BKInstrumentSetSequence (instrument, BK_SEQUENCE_DUTY_CYCLE, sequence, sequenceLength, repeatBegin, repeatLength);
			break;
		}
		case BKCompilerEnvelopeTypeADSR: {
			asdr [0] = atoix (cmd -> args [0].arg, 0);
			asdr [1] = atoix (cmd -> args [1].arg, 0);
			asdr [2] = atoix (cmd -> args [2].arg, 0) * (BK_MAX_VOLUME / 255);
			asdr [3] = atoix (cmd -> args [3].arg, 0);
			res = BKInstrumentSetEnvelopeADSR (instrument, asdr [0], asdr [1], asdr [2], asdr [3]);
			break;
		}
		case BKCompilerEnvelopeTypeVolumeEnv: {
			sequenceLength = BKCompilerInstrumentEnvelopeParse (cmd, phases, & repeatBegin, & repeatLength, (BK_MAX_VOLUME / 255));
			res = BKInstrumentSetEnvelope (instrument, BK_SEQUENCE_VOLUME, phases, sequenceLength, repeatBegin, repeatLength);
			break;
		}
		case BKCompilerEnvelopeTypePitchEnv: {
			sequenceLength = BKCompilerInstrumentEnvelopeParse (cmd, phases, & repeatBegin, & repeatLength, (BK_FINT20_UNIT / 100));
			res = BKInstrumentSetEnvelope (instrument, BK_SEQUENCE_PITCH, phases, sequenceLength, repeatBegin, repeatLength);
			break;
		}
		case BKCompilerEnvelopeTypePanningEnv: {
			sequenceLength = BKCompilerInstrumentEnvelopeParse (cmd, phases, & repeatBegin, & repeatLength, (BK_MAX_VOLUME / 255));
			res = BKInstrumentSetEnvelope (instrument, BK_SEQUENCE_PANNING, phases, sequenceLength, repeatBegin, repeatLength);
			break;
		}
		case BKCompilerEnvelopeTypeDutyCycleEnv: {
			sequenceLength = BKCompilerInstrumentEnvelopeParse (cmd, phases, & repeatBegin, & repeatLength, 1);
			res = BKInstrumentSetEnvelope (instrument, BK_SEQUENCE_DUTY_CYCLE, phases, sequenceLength, repeatBegin, repeatLength);
			break;
		}
	}

	if (res < 0) {
		fprintf (stderr, "Invalid sequence '%s' (%d) on line %u:%u\n", cmd -> name, res, cmd -> lineno, cmd -> colno);
	}

	return res;
}

static BKInt BKCompilerTrimParentPartOfPath (BKString * filename)
{
	do {
		if (BKStringBeginsWithChars (filename, "./")) {
			if (BKStringReplaceCharsInRange (filename, NULL, 0, 0, 2) < 0) {
				return -1;
			}
		}
		else if (BKStringBeginsWithChars (filename, "../")) {
			if (BKStringReplaceCharsInRange (filename, NULL, 0, 0, 3) < 0) {
				return -1;
			}
		}
		else {
			break;
		}
	}
	while (filename -> length);

	return 0;
}

static BKInt BKCompilerMakeFilePath (BKCompiler * compiler, BKString * filepath, BKString * filename)
{
	char cwd [BK_MAX_PATH];

	if (BKCompilerTrimParentPartOfPath (filename) < 0) {
		return -1;
	}

	if (compiler -> loadPath.length == 0) {
		if (getcwd (cwd, BK_MAX_PATH) == NULL) {
			return -1;
		}

		if (BKStringAppendChars (& compiler -> loadPath, cwd) < 0) {
			return -1;
		}
	}

	if (BKStringAppend (filepath, & compiler -> loadPath) < 0) {
		return -1;
	}

	if (BKStringAppendChars (filepath, "/") < 0) {
		return -1;
	}

	if (BKStringAppend (filepath, filename) < 0) {
		return -1;
	}

	return 0;
}

static BKInt BKCompilerPushCommandSample (BKCompiler * compiler, BKSTCmd const * cmd)
{
	BKInt value;
	BKInt values [2];
	BKString filename, filepath;
	FILE * file;
	BKData * sample = compiler -> currentSample;

	// search for command
	if (BKCompilerStrvalTableLookup (miscNames, NUM_MISC_NAMES, cmd -> name, & value, NULL) == 0) {
		fprintf (stderr, "Unknown command '%s' on line %u:%u\n", cmd -> name, cmd -> lineno, cmd -> colno);
		return 0;
	}

	switch (value) {
		case BKCompilerMiscLoad: {
			if (cmd -> numArgs >= 2) {
				if (strcmpx (cmd -> args [0].arg, "wav") == 0) {
					if (BKStringInit (& filename, cmd -> args [1].arg, -1) < 0) {
						return -1;
					}

					if (BKStringInit (& filepath, "", -1) < 0) {
						BKDispose (& filename);
						return -1;
					}

					if (BKCompilerMakeFilePath (compiler, & filepath, & filename) < 0) {
						return -1;
					}

					file = fopen (filepath.chars, "rb");

					if (file == NULL) {
						fprintf (stderr, "File '%s' not found on line %u:%u\n", filename.chars, cmd -> lineno, cmd -> colno);
						BKDispose (& filename);
						BKDispose (& filepath);
						return -1;
					}

					if (BKDataLoadWAVE (sample, file) < 0) {
						fprintf (stderr, "Failed to load WAVE file '%s' on line %u:%u\n", filename.chars, cmd -> lineno, cmd -> colno);
						BKDispose (& filename);
						BKDispose (& filepath);
						fclose (file);
						return -1;
					}

					BKDispose (& filename);
					BKDispose (& filepath);
					fclose (file);
				}
			}

			break;
		}
		case BKCompilerMiscPitch: {
			if (cmd -> numArgs >= 1) {
				BKSetAttr (sample, BK_SAMPLE_PITCH, atoix (cmd -> args [0].arg, 0) * PITCH_UNIT);
			}
			break;
		}
		case BKCompilerMiscSampleSustainRange: {
			values [0] = atoix (cmd -> args [0].arg, 0);
			values [1] = atoix (cmd -> args [1].arg, 0);

			BKSetPtr (sample, BK_SAMPLE_SUSTAIN_RANGE, values, sizeof (BKInt [2]));
			break;
		}
		case BKCompilerMiscRaw: {
			break;
		}
	}

	return 0;
}

static BKInt BKCompilerPushCommandTrack (BKCompiler * compiler, BKSTCmd const * cmd)
{
	char const      * arg0str;
	BKInt             value, values [2];
	BKInt             args [3];
	BKSize            numArgs;
	BKInstruction     instr;
	BKCompilerGroup * group = BKArrayGetLastItem (& compiler -> groupStack);
	BKByteBuffer    * cmds  = group -> cmdBuffer;

	arg0str = cmd -> args [0].arg;
	numArgs = cmd -> numArgs;

	if (BKCompilerStrvalTableLookup (cmdNames, NUM_CMD_NAMES, cmd -> name, & value, NULL) == 0) {
		fprintf (stderr, "Unknown command '%s' on line %u:%u\n", cmd -> name, cmd -> lineno, cmd -> colno);
		return 0;
	}

	if (compiler -> lineno != cmd -> lineno) {
		compiler -> lineno = cmd -> lineno;
		BKByteBufferWriteByte (cmds, BKIntrLineNo);
		BKByteBufferWriteInt32 (cmds, cmd -> lineno);
	}

	instr = value;

	switch (instr) {
		// command:8
		// group number:8
		case BKIntrGroupJump: {
			args [0] = atoix (cmd -> args [1].arg, 0);

			// jump to group
			// command will be replaced with BKIntrCall + Offset
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt32 (cmds, atoix (arg0str, 0));
			break;
		}
		// command:8
		// note:32
		// --- arpeggio
		// command:8
		// num values:8
		// values:32*
		case BKIntrAttack: {
			values [0] = BKCompilerParseNote (arg0str);

			if (values [0] > -1) {
				BKByteBufferWriteByte (cmds, instr);
				BKByteBufferWriteInt32 (cmds, values [0]);

				// set arpeggio
				if (numArgs > 1) {
					numArgs = BKMin (numArgs, BK_MAX_ARPEGGIO);

					BKByteBufferWriteByte (cmds, BKIntrArpeggio);
					BKByteBufferWriteByte (cmds, numArgs);

					for (BKInt j = 0; j < numArgs; j ++) {
						values [1] = BKCompilerParseNote (cmd -> args [j].arg);

						if (values [1] < 0) {
							values [1] = 0;
						}

						BKByteBufferWriteInt32 (cmds, values [1] - values [0]);
					}

					compiler -> object.flags |= BKCompilerFlagArpeggio;
				}
				// disable arpeggio
				else if (compiler -> object.flags & BKCompilerFlagArpeggio) {
					BKByteBufferWriteByte (cmds, BKIntrArpeggio);
					BKByteBufferWriteByte (cmds, 0);
					compiler -> object.flags &= ~BKCompilerFlagArpeggio;
				}
			}

			break;
		}
		// command:8
		// --- arpeggio
		// command:8
		// 0:8
		case BKIntrRelease:
		case BKIntrMute: {
			// disable arpeggio
			if (compiler -> object.flags & BKCompilerFlagArpeggio) {
				if (instr == BKIntrMute) {
					BKByteBufferWriteByte (cmds, BKIntrArpeggio);
					BKByteBufferWriteByte (cmds, 0);
					compiler -> object.flags &= ~BKCompilerFlagArpeggio;
				}
			}

			BKByteBufferWriteByte (cmds, instr);
			break;
		}
		// command:8
		// ticks:16
		case BKIntrAttackTicks:
		case BKIntrReleaseTicks:
		case BKIntrMuteTicks: {
			values [0] = atoix (arg0str, 0);

			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt16 (cmds, values [0]);
			break;
		}
		// command:8
		// volume:16
		case BKIntrVolume: {
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt16 (cmds, atoix (arg0str, 0) * VOLUME_UNIT);
			break;
		}
		// command:8
		// volume:16
		case BKIntrMasterVolume: {
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt16 (cmds, atoix (arg0str, 0) * VOLUME_UNIT);
			break;
		}
		// command:8
		// value:16
		case BKIntrStep:
		case BKIntrTicks:
		case BKIntrStepTicks: {
			values [0] = atoix (arg0str, 0);

			if (values [0] == 0) {
				return 0;
			}

			values [0] = BKClamp(values[0], 1, BK_MAX_STEPTICKS);

			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt16 (cmds, values [0]);

			if (instr == BKIntrStepTicks) {
				compiler -> stepTicks = atoix (cmd -> args [0].arg, 0);
			}

			break;
		}
		// command:8
		// value[4]:32
		case BKIntrEffect: {
			if (arg0str == NULL) {
				fprintf (stderr, "Effect command '%s' has no type on line %u:%u\n", cmd -> name, cmd -> lineno, cmd -> colno);
				break;
			}

			if (BKCompilerStrvalTableLookup (effectNames, NUM_EFFECT_NAMES, arg0str, & values [0], NULL)) {
				args [0] = atoix (cmd -> args [1].arg, 0);
				args [1] = atoix (cmd -> args [2].arg, 0);
				args [2] = atoix (cmd -> args [3].arg, 0);

				switch (values [0]) {
					case BK_EFFECT_TREMOLO: {
						args [1] *= VOLUME_UNIT;
						break;
					}
					case BK_EFFECT_VIBRATO: {
						args [1] *= PITCH_UNIT;
						break;
					}
				}

				BKByteBufferWriteByte (cmds, instr);
				BKByteBufferWriteInt16 (cmds, values [0]);
				BKByteBufferWriteInt32 (cmds, args [0]);
				BKByteBufferWriteInt32 (cmds, args [1]);
				BKByteBufferWriteInt32 (cmds, args [2]);
			}
			break;
		}
		// command:8
		// duty cycle:8
		case BKIntrDutyCycle: {
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteByte (cmds, atoix (arg0str, 0));
			break;
		}
		// command:8
		// phase wrap:16
		case BKIntrPhaseWrap: {
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt32 (cmds, atoix (arg0str, 0));
			break;
		}
		// command:8
		// panning:16
		case BKIntrPanning: {
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt16 (cmds, (int16_t) (atoix (arg0str, 0) * VOLUME_UNIT));
			break;
		}
		// command:8
		// pitch:32
		case BKIntrPitch: {
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt32 (cmds, atoix (arg0str, 0) * PITCH_UNIT);
			break;
		}
		// command:8
		// instrument:16
		case BKIntrInstrument: {
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt16 (cmds, atoix (arg0str, -1));
			break;
		}
		// command:8
		// waveform:16
		case BKIntrWaveform: {
			if (arg0str) {
				BKByteBufferWriteByte (cmds, instr);

				if (BKCompilerStrvalTableLookup (waveformNames, NUM_WAVEFORM_NAMES, arg0str, & values [0], NULL) == 0) {
					values [0] = atoix (arg0str, 0) | BK_INTR_CUSTOM_WAVEFORM_FLAG;
				}

				BKByteBufferWriteInt16 (cmds, values [0]);
			}
			break;
		}
		// command:8
		// sample:16
		case BKIntrSample: {
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt16 (cmds, atoix (arg0str, -1));
			break;
		}
		// command:8
		// repeat:8
		case BKIntrSampleRepeat: {
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteByte (cmds, atoix (arg0str, 0));
			break;
		}
		// command:8
		// from:32
		// to:32
		case BKIntrSampleRange: {
			values [0] = atoix (cmd -> args [0].arg, 0);
			values [1] = atoix (cmd -> args [1].arg, 0);

			// set full range if empty
			if (values [0] == 0 && values [1] == 0) {
				values [1] = -1;
			}

			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt32 (cmds, values [0]);
			BKByteBufferWriteInt32 (cmds, values [1]);
			break;
		}
		// command:8
		// from:32
		// to:32
		case BKIntrSampleSustainRange: {
			values [0] = atoix (cmd -> args [0].arg, 0);
			values [1] = atoix (cmd -> args [1].arg, 0);

			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt32 (cmds, values [0]);
			BKByteBufferWriteInt32 (cmds, values [1]);
			break;
		}
		// command:8
		// speed:8
		case BKIntrArpeggioSpeed: {
			BKByteBufferWriteByte (cmds, instr);
			BKByteBufferWriteInt16 (cmds, BKMax (atoix (arg0str, 0), 1));
			break;
		}
		// command:8
		case BKIntrSetRepeatStart: {
			BKByteBufferWriteByte (cmds, instr);
			break;
		}
		// command:8
		// jump:8
		case BKIntrRepeat: {
			BKByteBufferWriteByte (cmds, BKIntrJump);
			BKByteBufferWriteInt32 (cmds, -1);
			break;
		}
		// command:8
		case BKIntrEnd: {
			BKByteBufferWriteByte (cmds, instr);
			break;
		}
		// ignore invalid command
		default: {
			break;
		}
	}

	return 0;
}

static BKInt BKCompilerPushCommandWaveform (BKCompiler * compiler, BKSTCmd const * cmd)
{
	BKInt    value;
	BKFrame  sequence [BK_MAX_WAVEFORM_LENGTH];
	BKInt    length = 0;
	BKData * waveform = compiler -> currentWaveform;

	// search for envelope type
	if (BKCompilerStrvalTableLookup (cmdNames, NUM_CMD_NAMES, cmd -> name, & value, NULL) == 0) {
		fprintf (stderr, "Unknown command '%s' on line %u:%u\n", cmd -> name, cmd -> lineno, cmd -> colno);
		return 0;
	}

	switch (value) {
		case BKIntrStep: {
			length = (BKInt) cmd -> numArgs;
			length = BKMin (length, BK_MAX_WAVEFORM_LENGTH);

			if (length < 2) {
				fprintf (stderr, "Waveform needs at least 2 values on line %u:%u\n", cmd -> lineno, cmd -> colno);
				return 0;
			}

			for (BKInt i = 0; i < length; i ++) {
				sequence [i] = atoix (cmd -> args [i].arg, 0) * (BK_MAX_VOLUME / 255);
			}

			if (BKDataSetFrames (waveform, sequence, length, 1, 1) < 0) {
				return -1;
			}

			break;
		}
	}

	return 0;
}

BKInt BKCompilerPushCommand (BKCompiler * compiler, BKSTCmd const * cmd)
{
	BKInt             index, value;
	BKUInt            flags;
	BKInstruction     instr;
	BKCompilerGroup * group, * newGroup;
	BKCompilerTrack * track;
	BKByteBuffer    * cmds;
	char const      * args [2];

	switch (cmd -> token) {
		case BKSTTokenValue: {
			// ignore current group
			if (compiler -> ignoreGroupLevel > -1) {
				return 0;
			}

			group = BKArrayGetLastItem (& compiler -> groupStack);

			switch (group -> groupType) {
				case BKIntrInstrumentDef: {
					if (BKCompilerPushCommandInstrument (compiler, cmd) < 0) {
						return -1;
					}
					break;
				}
				case BKIntrSampleDef: {
					if (BKCompilerPushCommandSample (compiler, cmd) < 0) {
						return -1;
					}
					break;
				}
				default:
				case BKIntrGroupDef:
				case BKIntrTrackDef: {
					if (BKCompilerPushCommandTrack (compiler, cmd) < 0) {
						return -1;
					}
					break;
				}
				case BKIntrWaveformDef: {
					if (BKCompilerPushCommandWaveform (compiler, cmd) < 0) {
						return -1;
					}
					break;
				}
			}

			break;
		}
		case BKSTTokenGrpBegin: {
			newGroup = BKArrayPushPtr (& compiler -> groupStack);

			if (newGroup == NULL) {
				fprintf (stderr, "Allocation error\n");
				return -1;
			}

			group = BKArrayGetItemAtIndex (& compiler -> groupStack, compiler -> groupStack.length - 2);

			memcpy (newGroup, group, sizeof (* group));
			newGroup -> level ++;

			flags = 0;
			value = 0;

			if (BKCompilerStrvalTableLookup (cmdNames, NUM_CMD_NAMES, cmd -> name, & value, & flags) == 0) {
				fprintf (stderr, "Ignoring unknown group '%s' on line %u:%u\n", cmd -> name, cmd -> lineno, cmd -> colno);
				compiler -> ignoreGroupLevel = (BKInt) (compiler -> groupStack.length - 1);
				return 0;
			}

			instr = value;

			switch (instr) {
				case BKIntrGroupDef: {
					BKByteBuffer * buffer;

					index = atoix (cmd -> args [0].arg, -1);

					if (index > BK_MAX_GROUP) {
						fprintf (stderr, "Group number is limited to %u on line %u:%u\n", BK_MAX_GROUP, cmd -> lineno, cmd -> colno);
						compiler -> ignoreGroupLevel = (BKInt) (compiler -> groupStack.length - 1);
						return 0;
					}

					buffer = BKCompilerGetCmdGroupForIndex (compiler, index);

					if (buffer == NULL) {
						return -1;
					}

					newGroup -> cmdBuffer = buffer;
					break;
				}
				case BKIntrInstrumentDef: {
					BKInstrument * instrument;

					index = atoix (cmd -> args [0].arg, -1);

					if (index > BK_MAX_GROUP) {
						fprintf (stderr, "Instrument number is limited to %u on line %u:%u\n", BK_MAX_GROUP, cmd -> lineno, cmd -> colno);
						compiler -> ignoreGroupLevel = (BKInt) (compiler -> groupStack.length - 1);
						return 0;
					}

					instrument = BKCompilerGetInstrumentForIndex (compiler, index);

					if (instrument == NULL) {
						return -1;
					}

					compiler -> currentInstrument = instrument;

					break;
				}
				case BKIntrSampleDef: {
					BKData * sample;

					index = atoix (cmd -> args [0].arg, -1);

					if (index > BK_MAX_GROUP) {
						fprintf (stderr, "Sample number is limited to %u on line %u:%u\n", BK_MAX_GROUP, cmd -> lineno, cmd -> colno);
						compiler -> ignoreGroupLevel = (BKInt) (compiler -> groupStack.length - 1);
						return 0;
					}

					sample = BKCompilerGetSampleForIndex (compiler, index);

					if (sample == NULL) {
						return -1;
					}

					compiler -> currentSample = sample;
					break;
				}
				case BKIntrTrackDef: {
					BKInt waveform = BK_SQUARE;
					BKInt slot     = -1;

					if (BKCompilerTrackAlloc (& track) < 0) {
						return -1;
					}

					if (BKArrayPush (& compiler -> tracks, & track) < 0) {
						return -1;
					}

					newGroup -> track     = track;
					newGroup -> cmdBuffer = & track -> globalCmds;

					if (cmd -> numArgs > 0) {
						args [0] = cmd -> args [0].arg;

						if (cmd -> numArgs > 1) {
							slot = atoix (cmd -> args [1].arg, 0);
						}

						// use custom waveform
						if (BKCompilerStrvalTableLookup (waveformNames, NUM_WAVEFORM_NAMES, args [0], & waveform, NULL) == 0) {
							waveform = atoix (args [0], 0) | BK_INTR_CUSTOM_WAVEFORM_FLAG;
						}
					}

					// set initial waveform
					BKByteBufferWriteByte (& track -> globalCmds, BKIntrWaveform);
					BKByteBufferWriteInt16 (& track -> globalCmds, waveform);

					// set stepticks
					BKByteBufferWriteByte (& track -> globalCmds, BKIntrStepTicks);
					BKByteBufferWriteInt16 (& track -> globalCmds, compiler -> stepTicks);

					track -> waveform = waveform;
					track -> slot     = slot;

					break;
				}
				case BKIntrWaveformDef: {
					BKData * waveform;

					index = atoix (cmd -> args [0].arg, -1);

					if (index > BK_MAX_GROUP) {
						fprintf (stderr, "Waveform number is limited to %u on line %u:%u\n", BK_MAX_GROUP, cmd -> lineno, cmd -> colno);
						compiler -> ignoreGroupLevel = (BKInt) (compiler -> groupStack.length - 1);
						return 0;
					}

					waveform = BKCompilerGetWaveformForIndex (compiler, index);

					if (waveform == NULL) {
						return -1;
					}

					compiler -> currentWaveform = waveform;
					break;
				}
				default: {
					fprintf (stderr, "Unknown group '%s' on line %u:%u\n", cmd -> name, cmd -> lineno, cmd -> colno);
					break;
				}
			}

			newGroup -> groupType = instr;

			break;
		}
		case BKSTTokenGrpEnd: {
			if (compiler -> groupStack.length <= 1) { // needs at minimum 1 item
				fprintf (stderr, "Unbalanced group on line %d:%d\n", cmd -> lineno, cmd -> colno);
				return -1;
			}

			group = BKArrayGetLastItem (& compiler -> groupStack);
			cmds  = group -> cmdBuffer;

			BKByteBufferWriteByte (cmds, BKIntrLineNo);
			BKByteBufferWriteInt32 (cmds, cmd -> lineno);

			BKArrayPop (& compiler -> groupStack, NULL);

			if (compiler -> groupStack.length <= compiler -> ignoreGroupLevel) {
				compiler -> ignoreGroupLevel = -1;
			}

			break;
		}
		case BKSTTokenEnd:
		case BKSTTokenNone:
		case BKSTTokenComment:
		case BKSTTokenArgSep:
		case BKSTTokenCmdSep: {
			// ignore
			break;
		}
	}

	return 0;
}

static BKInt BKCompilerByteCodeLink (BKCompilerTrack * track, BKByteBuffer * group, BKArray * groupOffsets)
{
	void * opcode    = group -> firstSegment -> data;
	void * opcodeEnd = group -> firstSegment -> data + BKByteBufferGetSize (group);
	uint8_t cmd;
	BKInt arg, offset;
	BKInt cmdSize;

	while (opcode < opcodeEnd) {
		cmd = * (uint8_t *) opcode ++;
		cmdSize = 0;

		switch (cmd) {
			case BKIntrGroupJump: {
				arg = * (uint32_t *) opcode;

				if (arg < groupOffsets -> length) {
					opcode --;
					cmdSize = 4;

					BKArrayGetItemAtIndexCopy (groupOffsets, arg, & offset);

					if (offset == -1) {
						fprintf (stderr, "Undefined group number %d (%ld)\n", arg, groupOffsets -> length);
					}

					(* (uint8_t *) opcode ++) = BKIntrCall;
					(* (uint32_t *) opcode)   = offset;
				}
				else {
					fprintf (stderr, "Undefined group number %d (%ld)\n", arg, groupOffsets -> length);
					return -1;
				}
				break;
			}
			case BKIntrAttack: {
				cmdSize = 4;
				break;
			}
			case BKIntrArpeggio: {
				arg = * (uint8_t *) opcode;
				cmdSize = 1 + arg * 4;
				break;
			}
			case BKIntrRelease:
			case BKIntrMute: {
				cmdSize = 0;
				break;
			}
			case BKIntrAttackTicks:
			case BKIntrReleaseTicks: {
				cmdSize = 2;
				break;
			}
			case BKIntrVolume: {
				cmdSize = 2;
				break;
			}
			case BKIntrMasterVolume: {
				cmdSize = 2;
				break;
			}
			case BKIntrStep:
			case BKIntrTicks:
			case BKIntrStepTicks: {
				cmdSize = 2;
				break;
			}
			case BKIntrEffect: {
				cmdSize = 2 + 4 + 4 + 4;
				break;
			}
			case BKIntrDutyCycle: {
				cmdSize = 1;
				break;
			}
			case BKIntrPhaseWrap: {
				cmdSize = 4;
				break;
			}
			case BKIntrPanning: {
				cmdSize = 2;
				break;
			}
			case BKIntrPitch: {
				cmdSize = 4;
				break;
			}
			case BKIntrInstrument:
			case BKIntrWaveform:
			case BKIntrSample: {
				cmdSize = 2;
				break;
			}
			case BKIntrSampleRepeat: {
				cmdSize = 1;
				break;
			}
			case BKIntrSampleRange: {
				cmdSize = 4 + 4;
				break;
			}
			case BKIntrArpeggioSpeed: {
				cmdSize = 2;
				break;
			}
			case BKIntrSetRepeatStart: {
				cmdSize = 0;
				break;
			}
			case BKIntrRepeat: {
				cmdSize = 4;
				break;
			}
			case BKIntrLineNo: {
				cmdSize = 4;
				break;
			}
		}

		opcode += cmdSize;
	}

	return 0;
}

/**
 * Combine group commands into global commands of track
 */
static BKInt BKCompilerTrackLink (BKCompilerTrack * track)
{
	BKArray groupOffsets;
	BKByteBuffer * group;
	BKSize codeOffset, offset;

	if (BKArrayInit (& groupOffsets, sizeof (BKInt), track -> cmdGroups.length) < 0) {
		return -1;
	}

	codeOffset = BKByteBufferGetSize (& track -> globalCmds);

	for (BKInt i = 0; i < track -> cmdGroups.length; i ++) {
		BKArrayGetItemAtIndexCopy (& track -> cmdGroups, i, & group);
		offset = codeOffset;

		if (group == NULL) {
			offset = -1;
		}

		if (BKArrayPush (& groupOffsets, & offset) < 0) {
			return -1;
		}

		if (group) {
			// add return command
			BKByteBufferWriteByte (group, BKIntrReturn);

			codeOffset += BKByteBufferGetSize (group);
		}
	}

	// link global commands
	if (BKCompilerByteCodeLink (track, & track -> globalCmds, & groupOffsets) < 0) {
		return -1;
	}

	for (BKInt i = 0; i < track -> cmdGroups.length; i ++) {
		BKArrayGetItemAtIndexCopy (& track -> cmdGroups, i, & group);

		if (group == NULL) {
			continue;
		}

		// link group
		if (BKCompilerByteCodeLink (track, group, & groupOffsets) < 0) {
			return -1;
		}

		// append group
		if (BKByteBufferWriteBytes (& track -> globalCmds, group -> firstSegment -> data, BKByteBufferGetSize (group)) < 0) {
			return -1;
		}

		BKByteBufferClear (group, 0);
	}

	BKDispose (& groupOffsets);

	return 0;
}

/**
 * Combine each track's commands into the global commands
 */
static BKInt BKCompilerLink (BKCompiler * compiler)
{
	BKCompilerTrack * track;

	track = & compiler -> globalTrack;

	// append end command
	BKByteBufferWriteByte (& track -> globalCmds, BKIntrEnd);

	if (BKCompilerTrackLink (track) < 0) {
		return -1;
	}

	// before linking
	for (BKInt i = 0; i < compiler -> tracks.length; i ++) {
		BKArrayGetItemAtIndexCopy (& compiler -> tracks, i, & track);

		// append end command
		BKByteBufferWriteByte (& track -> globalCmds, BKIntrEnd);
	}

	// linking tracks
	for (BKInt i = 0; i < compiler -> tracks.length; i ++) {
		BKArrayGetItemAtIndexCopy (& compiler -> tracks, i, & track);

		if (BKCompilerTrackLink (track) < 0) {
			return -1;
		}
	}

	return 0;
}

BKInt BKCompilerTerminate (BKCompiler * compiler, BKEnum options)
{
	if (compiler -> groupStack.length > 1) {
		fprintf (stderr, "Unterminated groups (%lu)\n", compiler -> groupStack.length);
		return -1;
	}

	// combine commands and group commands into one array
	if (BKCompilerLink (compiler) < 0) {
		return -1;
	}

	return 0;
}

static BKInt BKCompilerParse (BKCompiler * compiler, BKSTParser * parser)
{
	BKSTCmd cmd;

	while (BKSTParserNextCommand (parser, & cmd)) {
		if (BKCompilerPushCommand (compiler, & cmd) < 0) {
			return -1;
		}
	}

	return 0;
}

BKInt BKCompilerCompile (BKCompiler * compiler, BKSTParser * parser, BKEnum options)
{
	if (BKCompilerParse (compiler, parser) < 0) {
		return -1;
	}

	if (BKCompilerTerminate (compiler, options) < 0) {
		return -1;
	}

	return 0;
}

void BKCompilerReset (BKCompiler * compiler, BKInt keepData)
{
	BKCompilerGroup * group;
	BKCompilerTrack * track;
	BKInstrument    * instrument;
	BKData          * data;

	BKCompilerTrackReset (& compiler -> globalTrack, keepData);

	for (BKInt i = 0; i < compiler -> tracks.length; i ++) {
		BKArrayGetItemAtIndexCopy (& compiler -> tracks, i, & track);
		BKCompilerTrackDispose (track);
	}

	BKArrayEmpty (& compiler -> groupStack, keepData);
	group = BKArrayPushPtr (& compiler -> groupStack);
	group -> track = & compiler -> globalTrack;
	group -> cmdBuffer = & group -> track -> globalCmds;
	group -> groupType = BKIntrTrackDef;

	compiler -> ignoreGroupLevel = -1;

	for (BKInt i = 0; i < compiler -> instruments.length; i ++) {
		BKArrayGetItemAtIndexCopy (& compiler -> instruments, i, & instrument);
		BKDispose (instrument);
	}

	for (BKInt i = 0; i < compiler -> waveforms.length; i ++) {
		BKArrayGetItemAtIndexCopy (& compiler -> waveforms, i, & data);
		BKDispose (data);
	}

	for (BKInt i = 0; i < compiler -> samples.length; i ++) {
		BKArrayGetItemAtIndexCopy (& compiler -> samples, i, & data);
		BKDispose (data);
	}

	BKArrayEmpty (& compiler -> instruments, keepData);
	BKArrayEmpty (& compiler -> waveforms, keepData);
	BKArrayEmpty (& compiler -> samples, keepData);

	compiler -> stepTicks         = 24;
	compiler -> currentInstrument = NULL;
	compiler -> currentWaveform   = NULL;
	compiler -> currentSample     = NULL;
	compiler -> lineno            = -1;
}

BKClass BKCompilerClass =
{
	.instanceSize = sizeof (BKCompiler),
	.dispose      = (BKDisposeFunc) BKCompilerDispose,
};
