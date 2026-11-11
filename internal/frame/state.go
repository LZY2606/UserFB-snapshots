// Copyright 2017 Hajime Hoshi
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package frame

import "github.com/hajimehoshi/go-mp3/internal/bits"

// A Checkpoint is a sparse snapshot of the cross-frame decoder state.
//
// MP3 frames are not self-contained: the bit reservoir lets a frame
// reference main data of previous frames (main_data_begin), and the
// overlap-add synthesis (store and v_vec) carries history between
// frames. A Checkpoint captures exactly the state that the frame
// following the saved frame depends on, so decoding can be restarted
// there without re-decoding the whole stream prefix.
//
// A Checkpoint never contains decoded PCM samples.
type Checkpoint struct {
	store        [2][32][18]float32
	vVec         [2][1024]float32
	prevMainData *bits.Bits
}

// SaveCheckpoint snapshots the state after f has been decoded.
// The returned Checkpoint is independent of f and stays valid while f
// keeps decoding later frames.
func (f *Frame) SaveCheckpoint() *Checkpoint {
	cp := &Checkpoint{
		store: f.store,
		vVec:  f.v_vec,
	}
	if f.mainDataBits != nil {
		// Copy the reservoir window: later frames may reference it
		// through main_data_begin.
		cp.prevMainData = f.mainDataBits.Clone()
	}
	return cp
}

// NewFrame builds a synthetic previous frame holding the checkpointed
// state. It can be passed to Read as the prev argument to make the next
// Read behave as if all frames up to the checkpoint had just been
// decoded.
func (cp *Checkpoint) NewFrame() *Frame {
	f := &Frame{
		store: cp.store,
		v_vec: cp.vVec,
	}
	if cp.prevMainData != nil {
		// Give the restored frame its own copy so decoding cannot mutate
		// the checkpoint through slice aliasing.
		f.mainDataBits = cp.prevMainData.Clone()
	}
	return f
}
