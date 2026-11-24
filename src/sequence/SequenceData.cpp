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

#include "sequence/SequenceData.h"
#include "sequence/SequenceReader.h"
#include "sequence/SequenceEntry.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>

AUD_NAMESPACE_BEGIN

SequenceData::SequenceData(Specs specs, float fps, bool muted) :
	m_specs(specs),
	m_status(0),
	m_entry_status(0),
	m_id(0),
	m_muted(muted),
	m_fps(fps),
	m_speed_of_sound(343.3f),
	m_doppler_factor(1),
	m_distance_model(DISTANCE_MODEL_INVERSE_CLAMPED),
	m_volume(1, 1.0f),
	m_location(3),
	m_orientation(4),
	m_editing(false)
{
	Quaternion q;
	m_orientation.write(q.get());
	float f = 1;
	m_volume.write(&f);
}

SequenceData::~SequenceData()
{
}

void SequenceData::lock()
{
	m_mutex.lock();
}

void SequenceData::unlock()
{
	m_mutex.unlock();
}

Specs SequenceData::getSpecs()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	return m_specs;
}

void SequenceData::setSpecs(Specs specs)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	m_specs = specs;
	m_status++;
}

float SequenceData::getFPS() const
{
	return m_fps;
}

void SequenceData::setFPS(float fps)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	m_fps = fps;
}

void SequenceData::mute(bool muted)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	m_muted = muted;
}

bool SequenceData::isMuted() const
{
	return m_muted;
}

float SequenceData::getSpeedOfSound() const
{
	return m_speed_of_sound;
}

void SequenceData::setSpeedOfSound(float speed)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	m_speed_of_sound = speed;
	m_status++;
}

float SequenceData::getDopplerFactor() const
{
	return m_doppler_factor;
}

void SequenceData::setDopplerFactor(float factor)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	m_doppler_factor = factor;
	m_status++;
}

DistanceModel SequenceData::getDistanceModel() const
{
	return m_distance_model;
}

void SequenceData::setDistanceModel(DistanceModel model)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	m_distance_model = model;
	m_status++;
}

AnimateableProperty* SequenceData::getAnimProperty(AnimateablePropertyType type)
{
	switch(type)
	{
	case AP_VOLUME:
		return &m_volume;
	case AP_LOCATION:
		return &m_location;
	case AP_ORIENTATION:
		return &m_orientation;
	default:
		return nullptr;
	}
}

std::shared_ptr<SequenceEntry> SequenceData::add(std::shared_ptr<ISound> sound, std::shared_ptr<SequenceData> sequence_data, double begin, double end, double skip)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	std::shared_ptr<SequenceEntry> entry = std::shared_ptr<SequenceEntry>(new SequenceEntry(sound, begin, end, skip, sequence_data, m_id++));

	m_entries.push_back(entry);
	m_entry_status++;

	EditOperation operation;
	operation.type = EDIT_ADD;
	operation.entry = entry;
	operation.after = captureEntryState(entry);
	recordOperation(operation);

	return entry;
}

void SequenceData::remove(std::shared_ptr<SequenceEntry> entry)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	if(!detachEntry(entry))
		// the entry does not belong to this sequence, nothing to do
		return;

	EditOperation operation;
	operation.type = EDIT_REMOVE;
	operation.entry = entry;
	operation.before = captureEntryState(entry);
	recordOperation(operation);
}

SequenceData::EntryState SequenceData::captureEntryState(const std::shared_ptr<SequenceEntry>& entry) const
{
	std::lock_guard<ILockable> lock(*entry);

	EntryState state;
	state.sound = entry->m_sound;
	state.begin = entry->m_begin;
	state.end = entry->m_end;
	state.skip = entry->m_skip;
	state.muted = entry->m_muted;
	return state;
}

void SequenceData::applyEntryState(const std::shared_ptr<SequenceEntry>& entry, const EntryState& state)
{
	std::lock_guard<ILockable> lock(*entry);

	if(entry->m_sound.get() != state.sound.get())
	{
		entry->m_sound = state.sound;
		entry->m_sound_status++;
	}

	if(entry->m_begin != state.begin || entry->m_end != state.end || entry->m_skip != state.skip)
	{
		entry->m_begin = state.begin;
		entry->m_end = state.end;
		entry->m_skip = state.skip;
		entry->m_pos_status++;
	}

	entry->m_muted = state.muted;
}

void SequenceData::recordOperation(const EditOperation& operation)
{
	if(m_editing)
	{
		m_current_edit.push_back(operation);
		return;
	}

	// an effective edit invalidates the redo branch
	m_redo_history.clear();
	m_undo_history.push_back(EditStep(1, operation));
}

void SequenceData::recordEntryEdit(SequenceEntry* entry, EditType type, const EntryState& before, const EntryState& after)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	// only entries that are currently part of this sequence create history,
	// entries that have been detached do not
	bool found = false;
	for(auto& candidate : m_entries)
	{
		if(candidate.get() == entry)
		{
			found = true;
			break;
		}
	}

	if(!found)
		return;

	// repeated writes of the same value are not effective changes
	if(before.sound.get() == after.sound.get() && before.begin == after.begin &&
			before.end == after.end && before.skip == after.skip &&
			before.muted == after.muted)
		return;

	EditOperation operation;
	operation.type = type;
	operation.before = before;
	operation.after = after;

	for(auto& candidate : m_entries)
	{
		if(candidate.get() == entry)
		{
			operation.entry = candidate;
			break;
		}
	}

	recordOperation(operation);
}

void SequenceData::insertEntry(const std::shared_ptr<SequenceEntry>& entry)
{
	{
		std::lock_guard<ILockable> lock(*entry);

		// The entry is reattached, so a potentially existing handle has to
		// realign its source to the current reader time instead of resuming
		// at the source position it had when it was detached.
		entry->m_pos_status++;
	}

	auto it = m_entries.begin();

	while(it != m_entries.end() && (*it)->getID() < entry->getID())
		it++;

	m_entries.insert(it, entry);
	m_entry_status++;
}

bool SequenceData::detachEntry(const std::shared_ptr<SequenceEntry>& entry)
{
	for(auto it = m_entries.begin(); it != m_entries.end(); it++)
	{
		if(it->get() == entry.get())
		{
			m_entries.erase(it);
			m_entry_status++;
			return true;
		}
	}

	return false;
}

void SequenceData::undoOperation(const EditOperation& operation)
{
	switch(operation.type)
	{
	case EDIT_ADD:
		detachEntry(operation.entry);
		break;
	case EDIT_REMOVE:
		insertEntry(operation.entry);
		applyEntryState(operation.entry, operation.before);
		break;
	case EDIT_MOVE:
	case EDIT_MUTE:
	case EDIT_SOUND:
		applyEntryState(operation.entry, operation.before);
		break;
	}
}

void SequenceData::redoOperation(const EditOperation& operation)
{
	switch(operation.type)
	{
	case EDIT_ADD:
		insertEntry(operation.entry);
		applyEntryState(operation.entry, operation.after);
		break;
	case EDIT_REMOVE:
		detachEntry(operation.entry);
		break;
	case EDIT_MOVE:
	case EDIT_MUTE:
	case EDIT_SOUND:
		applyEntryState(operation.entry, operation.after);
		break;
	}
}

void SequenceData::beginEdit()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	if(m_editing)
		throw std::logic_error("Sequence::beginEdit called while an edit group is already open");

	m_editing = true;
	m_current_edit.clear();
}

void SequenceData::endEdit()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	if(!m_editing)
		throw std::logic_error("Sequence::endEdit called without an open edit group");

	m_editing = false;

	if(m_current_edit.empty())
		return;

	// Collapse the collected operations: entries whose overall state did not
	// change (and which were not added or removed in the end) are dropped.
	std::vector<std::shared_ptr<SequenceEntry> > entries;
	std::vector<bool> keep;

	for(auto& operation : m_current_edit)
	{
		bool seen = false;
		for(auto& entry : entries)
		{
			if(entry.get() == operation.entry.get())
			{
				seen = true;
				break;
			}
		}

		if(!seen)
		{
			entries.push_back(operation.entry);
			keep.push_back(false);
		}
	}

	for(size_t i = 0; i < entries.size(); i++)
	{
		const EditOperation* first = nullptr;
		const EditOperation* last = nullptr;

		for(auto& operation : m_current_edit)
		{
			if(operation.entry.get() != entries[i].get())
				continue;

			if(!first)
				first = &operation;

			last = &operation;
		}

		if(!first || !last)
			continue;

		bool present_before = first->type != EDIT_ADD;
		bool present_after = last->type != EDIT_REMOVE;

		if(!present_before && !present_after)
			// added and removed again, no net change
			continue;

		if(present_before && present_after &&
				first->before.sound.get() == last->after.sound.get() &&
				first->before.begin == last->after.begin &&
				first->before.end == last->after.end &&
				first->before.skip == last->after.skip &&
				first->before.muted == last->after.muted)
			// modified and restored again, no net change
			continue;

		keep[i] = true;
	}

	EditStep step;

	for(auto& operation : m_current_edit)
	{
		for(size_t i = 0; i < entries.size(); i++)
		{
			if(keep[i] && entries[i].get() == operation.entry.get())
			{
				step.push_back(operation);
				break;
			}
		}
	}

	m_current_edit.clear();

	if(step.empty())
		return;

	// an effective group invalidates the redo branch
	m_redo_history.clear();
	m_undo_history.push_back(step);
}

bool SequenceData::undo()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	if(m_editing)
		throw std::logic_error("Sequence::undo called while an edit group is open");

	if(m_undo_history.empty())
		return false;

	EditStep step = m_undo_history.back();
	m_undo_history.pop_back();

	for(auto it = step.rbegin(); it != step.rend(); it++)
		undoOperation(*it);

	m_redo_history.push_back(step);
	return true;
}

bool SequenceData::redo()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	if(m_editing)
		throw std::logic_error("Sequence::redo called while an edit group is open");

	if(m_redo_history.empty())
		return false;

	EditStep step = m_redo_history.back();
	m_redo_history.pop_back();

	for(auto& operation : step)
		redoOperation(operation);

	m_undo_history.push_back(step);
	return true;
}

AUD_NAMESPACE_END
