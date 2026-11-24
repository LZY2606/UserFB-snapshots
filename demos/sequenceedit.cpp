/*******************************************************************************
 * Copyright 2009-2016 Jörg Müller
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

/**
 * Demonstrates editing a sequence and undoing the edit group.
 *
 * This demo does not need a sound card: it renders the sequence into memory
 * with a SequenceReader and prints the resulting levels.
 */

#include "sequence/Sequence.h"
#include "sequence/SequenceEntry.h"
#include "generator/Sine.h"
#include "IReader.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

using namespace aud;

static double level(std::shared_ptr<IReader> reader, int frame, int frameSize)
{
	reader->seek(frame * frameSize);

	int length = frameSize;
	bool eos = false;
	std::vector<float> buffer(length * reader->getSpecs().channels);
	reader->read(length, eos, buffer.data());

	double sum = 0;
	for(float value : buffer)
		sum += value * value;

	return std::sqrt(sum / buffer.size());
}

int main()
{
	const Specs specs = {8000, CHANNELS_STEREO};
	const float fps = 25;
	const int frameSize = int(specs.rate / fps);

	auto sequence = std::make_shared<Sequence>(specs, fps, false);

	// a tone on the first two frames of the timeline
	auto tone = sequence->add(std::make_shared<Sine>(440, specs.rate), 0, 2.0 / fps, 0);

	auto reader = sequence->createReader();

	// group a move and a mute into a single history step
	sequence->beginEdit();
	tone->move(2.0 / fps, 4.0 / fps, 0);
	tone->mute(true);
	sequence->endEdit();

	std::cout << "after the grouped edit:"
	          << " frame 0 = " << level(reader, 0, frameSize)
	          << ", frame 1 = " << level(reader, 1, frameSize)
	          << ", frame 2 = " << level(reader, 2, frameSize) << std::endl;

	// one undo reverts the whole group, audible without recreating the reader
	sequence->undo();
	std::cout << "after undo:"
	          << " frame 0 = " << level(reader, 0, frameSize)
	          << ", frame 1 = " << level(reader, 1, frameSize)
	          << ", frame 2 = " << level(reader, 2, frameSize) << std::endl;

	// and one redo applies the whole group again
	sequence->redo();
	std::cout << "after redo:"
	          << " frame 0 = " << level(reader, 0, frameSize)
	          << ", frame 1 = " << level(reader, 1, frameSize)
	          << ", frame 2 = " << level(reader, 2, frameSize) << std::endl;

	// removing and restoring keeps the identically same entry
	sequence->undo();
	int id = tone->getID();
	sequence->remove(tone);
	sequence->undo();
	std::cout << "entry " << tone->getID() << " restored (id preserved: "
	          << (tone->getID() == id ? "yes" : "no") << "), frame 0 = "
	          << level(reader, 0, frameSize) << std::endl;

	return 0;
}
