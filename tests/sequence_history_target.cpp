#include "task_support/check.h"

#include "sequence/Sequence.h"
#include "sequence/SequenceEntry.h"
#include "generator/Sine.h"
#include "generator/Square.h"
#include "IReader.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace aud;

namespace
{

const Specs SPECS = {8000, CHANNELS_STEREO};
const float FPS = 25;

// One animation frame at 8 kHz.
const int FRAME = int(SPECS.rate / FPS);

std::shared_ptr<Sequence> makeSequence()
{
	return std::make_shared<Sequence>(SPECS, FPS, false);
}

std::shared_ptr<ISound> makeSine()
{
	return std::make_shared<Sine>(200, SPECS.rate);
}

std::shared_ptr<ISound> makeSquare()
{
	return std::make_shared<Square>(100, SPECS.rate);
}

std::vector<float> forward(std::shared_ptr<IReader> reader, int samples)
{
	std::vector<float> buffer(samples * SPECS.channels);
	int length = samples;
	bool eos = false;
	reader->read(length, eos, buffer.data());
	buffer.resize(length * SPECS.channels);
	return buffer;
}

std::vector<float> at(std::shared_ptr<IReader> reader, int position, int samples)
{
	reader->seek(position);
	return forward(reader, samples);
}

// Reads a frame and discards it. This lets the volume interpolation of a live
// reader settle after a mute transition before the exact energy is asserted,
// because the mixer fades over one buffer.
void settle(std::shared_ptr<IReader> reader, int position)
{
	reader->seek(position);
	forward(reader, FRAME);
}

double energy(const std::vector<float>& buffer)
{
	if(buffer.empty())
		return 0;

	double sum = 0;
	for(float value : buffer)
		sum += value * value;
	return sum / buffer.size();
}

double energyAt(std::shared_ptr<IReader> reader, int position, int samples)
{
	return energy(at(reader, position, samples));
}

bool near(double actual, double expected, double epsilon = 1e-6)
{
	return std::fabs(actual - expected) <= epsilon;
}

// Compares two buffers sample by sample.
bool sameAudio(const std::vector<float>& a, const std::vector<float>& b)
{
	if(a.size() != b.size())
		return false;

	for(size_t i = 0; i < a.size(); i++)
		if(std::fabs(a[i] - b[i]) > 1e-4)
			return false;

	return true;
}

// Audible state checks. The sine source has an average energy of 0.25 per
// sample, the square source 1.0 before channel mapping. With mono to stereo
// mapping each output channel is scaled by 1/sqrt(2), so the stereo energies
// are 0.25 (sine) and 0.5 (square). At 8 kHz, 320 samples are exactly one
// 25 fps animation frame.
bool silentAt(std::shared_ptr<IReader> reader, int frame)
{
	return energyAt(reader, frame * FRAME, FRAME) == 0.0;
}

bool sineAt(std::shared_ptr<IReader> reader, int frame)
{
	return near(energyAt(reader, frame * FRAME, FRAME), 0.25);
}

bool squareAt(std::shared_ptr<IReader> reader, int frame)
{
	return near(energyAt(reader, frame * FRAME, FRAME), 0.5);
}

// ---------------------------------------------------------------------------
// edit_groups
// ---------------------------------------------------------------------------

int addUndoRedoRoundTrip()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	sequence->add(makeSine(), 0, 2, 0);
	TASK_CHECK(sineAt(reader, 0));

	TASK_CHECK(sequence->undo());
	TASK_CHECK(silentAt(reader, 0));

	TASK_CHECK(sequence->redo());
	TASK_CHECK(sineAt(reader, 0));
	return 0;
}

int groupIsOneStep()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	// leave a one frame gap between the entries, the frame at an entry end
	// still contains the tail of its source
	auto first = sequence->add(makeSine(), 0, 7.0 / FPS, 0);
	auto second = sequence->add(makeSquare(), 9.0 / FPS, 16.0 / FPS, 0);
	TASK_CHECK(sineAt(reader, 0));
	TASK_CHECK(squareAt(reader, 9));

	sequence->beginEdit();
	first->move(9.0 / FPS, 16.0 / FPS, 0);
	second->move(0, 7.0 / FPS, 0);
	second->mute(true);
	sequence->endEdit();

	TASK_CHECK(silentAt(reader, 0));
	TASK_CHECK(sineAt(reader, 9));

	// one undo reverts all three edits of the group. An unmute ramps in over
	// one mixer buffer, so settle at each probe position first.
	TASK_CHECK(sequence->undo());
	settle(reader, 0);
	TASK_CHECK(sineAt(reader, 0));
	settle(reader, 9 * FRAME);
	TASK_CHECK(squareAt(reader, 9));

	TASK_CHECK(sequence->redo());
	TASK_CHECK(silentAt(reader, 0));
	TASK_CHECK(sineAt(reader, 9));
	return 0;
}

int groupAppliesImmediately()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	auto entry = sequence->add(makeSine(), 0, 8.0 / FPS, 0);
	TASK_CHECK(sineAt(reader, 0));

	sequence->beginEdit();
	entry->move(8.0 / FPS, 16.0 / FPS, 0);

	// no endEdit yet: the move is already audible
	TASK_CHECK(silentAt(reader, 0));
	TASK_CHECK(sineAt(reader, 8));

	sequence->endEdit();
	return 0;
}

int emptyGroupDoesNotCreateHistory()
{
	auto sequence = makeSequence();

	auto entry = sequence->add(makeSine(), 0, 2, 0);
	entry->move(1, 2, 0);
	TASK_CHECK(sequence->undo());
	entry->move(0, 2, 0);

	// the empty group neither creates a step nor drops the redo branch
	sequence->beginEdit();
	sequence->endEdit();
	TASK_CHECK(sequence->redo());
	TASK_CHECK(sequence->undo());

	// a group that only writes identical values is no effective change
	sequence->beginEdit();
	entry->move(0, 2, 0);
	entry->mute(false);
	sequence->endEdit();
	TASK_CHECK(sequence->redo());

	sequence->undo();
	sequence->beginEdit();
	entry->mute(true);
	sequence->endEdit();

	// an effective group does drop the previous redo branch
	TASK_CHECK(!sequence->redo());
	return 0;
}

int netZeroGroupDoesNotCreateHistory()
{
	auto sequence = makeSequence();

	auto entry = sequence->add(makeSine(), 0, 2, 0);
	sequence->beginEdit();
	entry->move(1, 2, 0);
	entry->move(0, 2, 0);
	sequence->endEdit();

	// moved away and back again: nothing to undo but the add itself
	TASK_CHECK(sequence->undo());
	TASK_CHECK(!sequence->undo());
	return 0;
}

int addRemoveWithinGroupIsNoChange()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	sequence->beginEdit();
	auto entry = sequence->add(makeSine(), 0, 2, 0);
	sequence->remove(entry);
	sequence->endEdit();

	TASK_CHECK(!sequence->undo());
	TASK_CHECK(silentAt(reader, 0));
	return 0;
}

int noOpEditsDoNotCreateHistory()
{
	auto sequence = makeSequence();
	auto sound = makeSine();

	auto entry = sequence->add(sound, 0, 2, 0);

	// all of these repeat the current value
	entry->move(0, 2, 0);
	entry->mute(false);
	entry->setSound(sound);

	TASK_CHECK(sequence->undo());   // undoes the add
	TASK_CHECK(!sequence->undo());
	return 0;
}

int foreignRemoveDoesNotCreateHistory()
{
	auto sequence = makeSequence();
	auto other = makeSequence();
	auto sound = makeSine();

	auto foreign = other->add(sound, 0, 2, 0);
	sequence->add(sound, 0, 2, 0);

	// removing an entry that is not part of this sequence has no effect and
	// creates no history: only the add can be undone
	sequence->remove(foreign);
	TASK_CHECK(sequence->undo());
	TASK_CHECK(!sequence->undo());

	// the foreign sequence is unaffected except for its own add step
	TASK_CHECK(other->undo());
	return 0;
}

int redoBranchIsDropped()
{
	auto sequence = makeSequence();

	auto entry = sequence->add(makeSine(), 0, 2, 0);
	entry->move(1, 2, 0);
	TASK_CHECK(sequence->undo());

	entry->mute(true);
	TASK_CHECK(!sequence->redo());
	return 0;
}

int undoRedoAtEndReturnsFalse()
{
	auto sequence = makeSequence();
	TASK_CHECK(!sequence->undo());
	TASK_CHECK(!sequence->redo());

	sequence->add(makeSine(), 0, 2, 0);
	TASK_CHECK(sequence->undo());
	TASK_CHECK(!sequence->undo());
	TASK_CHECK(sequence->redo());
	TASK_CHECK(!sequence->redo());
	return 0;
}

int groupMisuseThrowsAndKeepsState()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	auto entry = sequence->add(makeSine(), 0, 8.0 / FPS, 0);

	bool threw = false;
	try
	{
		sequence->endEdit();
	}
	catch(const std::logic_error&)
	{
		threw = true;
	}
	TASK_CHECK(threw);

	sequence->beginEdit();
	entry->move(8.0 / FPS, 16.0 / FPS, 0);

	// a nested beginEdit throws
	threw = false;
	try
	{
		sequence->beginEdit();
	}
	catch(const std::logic_error&)
	{
		threw = true;
	}
	TASK_CHECK(threw);

	// undo and redo while a group is open throw as well
	threw = false;
	try
	{
		sequence->undo();
	}
	catch(const std::logic_error&)
	{
		threw = true;
	}
	TASK_CHECK(threw);

	threw = false;
	try
	{
		sequence->redo();
	}
	catch(const std::logic_error&)
	{
		threw = true;
	}
	TASK_CHECK(threw);

	// the group is still open and the state was not torn down
	threw = false;
	try
	{
		sequence->endEdit();
	}
	catch(const std::logic_error&)
	{
		threw = true;
	}
	// endEdit now succeeded and closed the group
	TASK_CHECK(!threw);
	TASK_CHECK(sineAt(reader, 8));

	// and the group is a single history step
	TASK_CHECK(sequence->undo());
	TASK_CHECK(sineAt(reader, 0));
	return 0;
}

int globalSettingsStayOutOfHistory()
{
	auto sequence = makeSequence();

	auto entry = sequence->add(makeSine(), 0, 2, 0);
	sequence->mute(true);
	sequence->setFPS(30);

	TASK_CHECK(sequence->undo());   // only the add is undone

	// global settings are not touched by undo and redo
	TASK_CHECK(sequence->isMuted());
	TASK_CHECK(near(sequence->getFPS(), 30));

	// animated properties are not part of the history either
	auto property = entry->getAnimProperty(AP_VOLUME);
	float value = 0.5f;
	property->write(&value);

	TASK_CHECK(sequence->redo());
	TASK_CHECK(!sequence->redo());   // animation writes created no step
	TASK_CHECK(!property->isAnimated());
	TASK_CHECK(near(property->readSingle(0), 0.5));
	return 0;
}

// ---------------------------------------------------------------------------
// entry_identity
// ---------------------------------------------------------------------------

int undoRemoveRestoresSameEntry()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	auto entry = sequence->add(makeSine(), 0, 8.0 / FPS, 0);
	int id = entry->getID();

	sequence->remove(entry);
	TASK_CHECK(silentAt(reader, 0));

	TASK_CHECK(sequence->undo());
	TASK_CHECK(sineAt(reader, 0));

	// the same object with the same ID came back: editing the outside
	// reference still edits the timeline
	TASK_CHECK(entry->getID() == id);
	entry->move(8.0 / FPS, 16.0 / FPS, 0);
	TASK_CHECK(silentAt(reader, 0));
	TASK_CHECK(sineAt(reader, 8));

	TASK_CHECK(sequence->undo());
	TASK_CHECK(sineAt(reader, 0));
	return 0;
}

int undoAddKeepsEntryOutside()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	auto entry = sequence->add(makeSine(), 0, 2, 0);
	TASK_CHECK(sequence->undo());

	// the entry reference stays valid but is no longer audible
	TASK_CHECK(silentAt(reader, 0));

	// edits while detached are not recorded
	entry->mute(true);
	TASK_CHECK(!sequence->undo());

	// redo restores the state stored in the history, not the detached edit
	TASK_CHECK(sequence->redo());
	TASK_CHECK(sineAt(reader, 0));
	TASK_CHECK(!entry->isMuted());
	return 0;
}

int detachedEditsDoNotEnterHistory()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	auto entry = sequence->add(makeSine(), 0, 8.0 / FPS, 0);
	sequence->remove(entry);

	// edit the detached entry, then restore the remove step
	entry->move(0, 16.0 / FPS, 0);
	entry->setSound(makeSquare());
	entry->mute(true);

	TASK_CHECK(sequence->undo());

	// the restored state is the one stored in the history, not the detached
	// edits
	TASK_CHECK(sineAt(reader, 0));
	TASK_CHECK(!entry->isMuted());
	return 0;
}

int restoredEntryAlignsToReaderTime()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	auto entry = sequence->add(makeSine(), 0, 16.0 / FPS, 0);

	// advance the live reader past the beginning of the entry, then remove
	// and restore it without reading in between; the existing handle must
	// realign to the current reader time
	forward(reader, 5 * FRAME);
	sequence->remove(entry);
	TASK_CHECK(sequence->undo());

	auto expected = at(sequence->createReader(), 5 * FRAME, FRAME);
	auto actual = forward(reader, FRAME);
	TASK_CHECK(sameAudio(actual, expected));
	TASK_CHECK(sineAt(reader, 5));
	return 0;
}

int readerCursorDoesNotRewind()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	auto entry = sequence->add(makeSine(), 0, 16.0 / FPS, 0);

	forward(reader, 3 * FRAME);
	sequence->remove(entry);
	forward(reader, 2 * FRAME);

	int before = reader->getPosition();
	TASK_CHECK(sequence->undo());
	TASK_CHECK(reader->getPosition() == before);

	forward(reader, FRAME);
	TASK_CHECK(reader->getPosition() == before + FRAME);

	sequence->redo();
	TASK_CHECK(reader->getPosition() == before + FRAME);
	return 0;
}

int entryIdsArePreserved()
{
	auto sequence = makeSequence();

	auto first = sequence->add(makeSine(), 0, 2, 0);
	auto second = sequence->add(makeSquare(), 2, 4, 0);
	int firstID = first->getID();
	int secondID = second->getID();
	TASK_CHECK(firstID != secondID);

	sequence->remove(second);
	TASK_CHECK(sequence->undo());
	TASK_CHECK(first->getID() == firstID);
	TASK_CHECK(second->getID() == secondID);

	// restoring the first entry keeps both identities
	sequence->remove(first);
	TASK_CHECK(sequence->undo());
	TASK_CHECK(first->getID() == firstID);
	TASK_CHECK(second->getID() == secondID);
	return 0;
}

int setSoundUndoRestoresSource()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();
	auto sine = makeSine();
	auto square = makeSquare();

	auto entry = sequence->add(sine, 0, 2, 0);
	TASK_CHECK(entry->getSound().get() == sine.get());
	TASK_CHECK(sineAt(reader, 0));

	entry->setSound(square);
	TASK_CHECK(entry->getSound().get() == square.get());
	TASK_CHECK(squareAt(reader, 0));

	TASK_CHECK(sequence->undo());
	TASK_CHECK(entry->getSound().get() == sine.get());
	TASK_CHECK(sineAt(reader, 0));

	TASK_CHECK(sequence->redo());
	TASK_CHECK(entry->getSound().get() == square.get());
	TASK_CHECK(squareAt(reader, 0));
	return 0;
}

// ---------------------------------------------------------------------------
// live_readers
// ---------------------------------------------------------------------------

int readersSeeEditsWithoutRecreation()
{
	auto sequence = makeSequence();
	auto first = sequence->createReader();
	auto second = sequence->createReader();

	auto entry = sequence->add(makeSine(), 0, 8.0 / FPS, 0);

	// readers created before the add pick up the entry on their next read
	TASK_CHECK(sineAt(first, 0));
	TASK_CHECK(sineAt(second, 0));

	entry->move(8.0 / FPS, 16.0 / FPS, 0);
	TASK_CHECK(silentAt(first, 0));
	TASK_CHECK(silentAt(second, 0));

	TASK_CHECK(sequence->undo());
	TASK_CHECK(sineAt(first, 0));
	TASK_CHECK(sineAt(second, 0));
	return 0;
}

int readersSeeSoundAndMuteChanges()
{
	auto sequence = makeSequence();
	auto first = sequence->createReader();
	auto second = sequence->createReader();

	auto entry = sequence->add(makeSine(), 0, 2, 0);
	TASK_CHECK(energyAt(first, 0, FRAME) > 0.1);
	TASK_CHECK(energyAt(second, 0, FRAME) > 0.1);

	entry->setSound(makeSquare());
	TASK_CHECK(squareAt(first, 0));
	TASK_CHECK(squareAt(second, 0));

	entry->mute(true);
	TASK_CHECK(silentAt(first, 0));
	TASK_CHECK(silentAt(second, 0));

	// undo the mute; let the mixer fade in before asserting exact energy
	TASK_CHECK(sequence->undo());
	settle(first, 0);
	settle(second, 0);
	TASK_CHECK(squareAt(first, 0));
	TASK_CHECK(squareAt(second, 0));

	// undo the sound change
	TASK_CHECK(sequence->undo());
	TASK_CHECK(sineAt(first, 0));
	TASK_CHECK(sineAt(second, 0));
	return 0;
}

int readersSeeRemovalAndRestore()
{
	auto sequence = makeSequence();
	auto first = sequence->createReader();
	auto second = sequence->createReader();

	auto entry = sequence->add(makeSine(), 0, 8.0 / FPS, 0);
	TASK_CHECK(sineAt(first, 0));
	TASK_CHECK(sineAt(second, 0));

	sequence->remove(entry);
	TASK_CHECK(silentAt(first, 0));
	TASK_CHECK(silentAt(second, 0));

	TASK_CHECK(sequence->undo());
	TASK_CHECK(sineAt(first, 0));
	TASK_CHECK(sineAt(second, 0));

	TASK_CHECK(sequence->redo());
	TASK_CHECK(silentAt(first, 0));
	TASK_CHECK(silentAt(second, 0));
	return 0;
}

int multipleReadersPreserveTheirOwnCursor()
{
	auto sequence = makeSequence();

	auto entry = sequence->add(makeSine(), 0, 16.0 / FPS, 0);
	auto first = sequence->createReader();
	auto second = sequence->createReader();

	forward(first, 4 * FRAME);
	forward(second, FRAME);

	int firstPosition = first->getPosition();
	int secondPosition = second->getPosition();

	entry->mute(true);
	TASK_CHECK(sequence->undo());

	TASK_CHECK(first->getPosition() == firstPosition);
	TASK_CHECK(second->getPosition() == secondPosition);

	// both continue exactly where they were, aligned to their own time
	settle(first, firstPosition);
	settle(second, secondPosition);

	auto expectedFirst = at(sequence->createReader(), firstPosition + FRAME, FRAME);
	auto expectedSecond = at(sequence->createReader(), secondPosition + FRAME, FRAME);
	TASK_CHECK(sameAudio(forward(first, FRAME), expectedFirst));
	TASK_CHECK(sameAudio(forward(second, FRAME), expectedSecond));
	return 0;
}

int readersFollowGroupedEdits()
{
	auto sequence = makeSequence();
	auto reader = sequence->createReader();

	auto first = sequence->add(makeSine(), 0, 7.0 / FPS, 0);
	auto second = sequence->add(makeSquare(), 9.0 / FPS, 16.0 / FPS, 0);
	TASK_CHECK(sineAt(reader, 0));
	TASK_CHECK(squareAt(reader, 9));

	sequence->beginEdit();
	first->mute(true);
	second->setSound(makeSine());
	sequence->endEdit();

	TASK_CHECK(silentAt(reader, 0));
	TASK_CHECK(sineAt(reader, 9));

	TASK_CHECK(sequence->undo());
	settle(reader, 0);
	TASK_CHECK(sineAt(reader, 0));
	TASK_CHECK(squareAt(reader, 9));
	return 0;
}

} // namespace

const struct task_case task_cases[] = {
	{"edit_groups", "add_undo_redo_round_trip", addUndoRedoRoundTrip},
	{"edit_groups", "group_is_one_step", groupIsOneStep},
	{"edit_groups", "group_applies_immediately", groupAppliesImmediately},
	{"edit_groups", "empty_group_no_history", emptyGroupDoesNotCreateHistory},
	{"edit_groups", "net_zero_group_no_history", netZeroGroupDoesNotCreateHistory},
	{"edit_groups", "add_remove_group_no_change", addRemoveWithinGroupIsNoChange},
	{"edit_groups", "noop_edits_no_history", noOpEditsDoNotCreateHistory},
	{"edit_groups", "foreign_remove_no_history", foreignRemoveDoesNotCreateHistory},
	{"edit_groups", "redo_branch_dropped", redoBranchIsDropped},
	{"edit_groups", "ends_return_false", undoRedoAtEndReturnsFalse},
	{"edit_groups", "misuse_throws_keeps_state", groupMisuseThrowsAndKeepsState},
	{"edit_groups", "global_settings_out_of_history", globalSettingsStayOutOfHistory},

	{"entry_identity", "undo_remove_restores_same_entry", undoRemoveRestoresSameEntry},
	{"entry_identity", "undo_add_keeps_entry_outside", undoAddKeepsEntryOutside},
	{"entry_identity", "detached_edits_not_recorded", detachedEditsDoNotEnterHistory},
	{"entry_identity", "restored_entry_aligns_to_reader", restoredEntryAlignsToReaderTime},
	{"entry_identity", "reader_cursor_does_not_rewind", readerCursorDoesNotRewind},
	{"entry_identity", "entry_ids_preserved", entryIdsArePreserved},
	{"entry_identity", "set_sound_undo_restores_source", setSoundUndoRestoresSource},

	{"live_readers", "readers_see_edits", readersSeeEditsWithoutRecreation},
	{"live_readers", "readers_see_sound_and_mute", readersSeeSoundAndMuteChanges},
	{"live_readers", "readers_see_removal_and_restore", readersSeeRemovalAndRestore},
	{"live_readers", "readers_keep_own_cursor", multipleReadersPreserveTheirOwnCursor},
	{"live_readers", "readers_follow_groups", readersFollowGroupedEdits},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
