/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "picture-boundary.h"

unsigned picture_boundary_tracker::note_first_slice()
{
  return ++m_pictures_started;
}

void picture_boundary_tracker::note_boundary(uint64_t tag)
{
  // All NAL units of this span have now been dequeued, so the picture
  // numbering cannot change for it anymore. A span in which no picture
  // started since the previous marker (or since the beginning) is an empty
  // boundary: swallow its tag, it must never generate a notification.
  if (m_pictures_started != m_pictures_at_last_boundary) {
    m_active = true;
    m_pictures_closed = m_pictures_started;
    m_tags.push_back(pending_tag{m_pictures_started, tag});
  }

  m_pictures_at_last_boundary = m_pictures_started;

  publish_due();
}

void picture_boundary_tracker::note_picture_finished(unsigned picture_number)
{
  if (picture_number > m_pictures_finished) {
    m_pictures_finished = picture_number;
  }

  publish_due();
}

bool picture_boundary_tracker::is_picture_closed(unsigned picture_number) const
{
  return picture_number <= m_pictures_closed;
}

int picture_boundary_tracker::pop_completed(uint64_t* tag)
{
  if (m_completed.empty()) {
    return 0;
  }

  uint64_t value = m_completed.front();
  m_completed.pop_front();

  if (tag) {
    *tag = value;
  }

  return 1;
}

void picture_boundary_tracker::publish_due()
{
  // Both queues are strictly ordered by picture number, so every pending
  // tag whose picture has finished moves to the completion queue in order.
  while (!m_tags.empty() && m_tags.front().picture_number <= m_pictures_finished) {
    m_completed.push_back(m_tags.front().tag);
    m_tags.pop_front();
  }
}

void picture_boundary_tracker::reset()
{
  m_pictures_started = 0;
  m_pictures_at_last_boundary = 0;
  m_pictures_closed = 0;
  m_pictures_finished = 0;
  m_active = false;
  m_tags.clear();
  m_completed.clear();
}
