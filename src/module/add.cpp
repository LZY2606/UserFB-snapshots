// add.cpp
//
// Copyright (C) 2003, 2004 Jason Bevins
//
// This library is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation; either version 2.1 of the License, or (at
// your option) any later version.
//
// This library is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
// License (COPYING.txt) for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this library; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// The developer's email is jlbezigvins@gmzigail.com (for great email, take
// off every 'zig'.)
//

#include "module/add.h"

using namespace noise::module;

Add::Add ():
  Module (GetSourceModuleCount ())
{
}

double Add::GetValue (double x, double y, double z) const
{
  assert (m_pSourceModule[0] != NULL);
  assert (m_pSourceModule[1] != NULL);

  return m_pSourceModule[0]->GetValue (x, y, z)
       + m_pSourceModule[1]->GetValue (x, y, z);
}

noise::Sample3 Add::GetSample (double x, double y, double z) const
{
  assert (m_pSourceModule[0] != NULL);
  assert (m_pSourceModule[1] != NULL);

  Sample3 sample0 = m_pSourceModule[0]->GetSample (x, y, z);
  Sample3 sample1 = m_pSourceModule[1]->GetSample (x, y, z);

  double value = sample0.value + sample1.value;
  if (!sample0.differentiable || !sample1.differentiable) {
    return Sample3::NotDifferentiable (value);
  }
  return Sample3 (value,
    sample0.dx + sample1.dx,
    sample0.dy + sample1.dy,
    sample0.dz + sample1.dz,
    true);
}
