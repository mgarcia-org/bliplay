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

#include "BKCompiler.h"
#include "BKInterpreter.h"

typedef struct BKContextWrapper BKContextWrapper;
typedef struct BKTrackWrapper   BKTrackWrapper;

struct BKContextWrapper
{
	BKObject   object;
	BKContext  ctx;
	BKCompiler compiler;
	BKArray    instruments;
	BKArray    waveforms;
	BKArray    samples;
	BKArray    tracks;
	BKInt      stepTicks;
	BKEnum     options;
};

struct BKTrackWrapper
{
	BKObject           object;
	BKContextWrapper * ctx;
	BKInterpreter      interpreter;
	BKTrack            track;
	BKDivider          divider;
	BKByteBuffer       opcode;
	BKEnum             waveform;
	BKInt              slot;
	BKByteBuffer       timingData;
};

enum
{
	BKTrackWrapperOptionTimingShift     = 0,
	BKTrackWrapperOptionTimingDataSecs  = 1 << 0,
	BKTrackWrapperOptionTimingDataTicks = 2 << 0, // next field is at 2
	BKTrackWrapperOptionTimingDataMask  = 3 << 0,
};

/**
 * Initialize context wrapper
 */
extern BKInt BKContextWrapperInit (BKContextWrapper * wrapper, BKUInt numChannels, BKUInt sampleRate, BKEnum options);

/**
 * Load data to compile
 */
extern BKInt BKContextWrapperLoadData (BKContextWrapper * wrapper, char const * data, size_t size, char const * loadPath);

/**
 * Load data to compile from file
 */
extern BKInt BKContextWrapperLoadFile (BKContextWrapper * wrapper, FILE * file, char const * loadPath);

/**
 * Create tracks from compiler data
 *
 * This function is invoked by `BKContextWrapperLoadData` and
 * `BKContextWrapperLoadFile` automatically
 */
extern BKInt BKContextWrapperFinish (BKContextWrapper * wrapper);
