/*
 * Copyright 2014 Volker Oth (0xdeadbeef) / Miklos Juhasz (mjuhasz)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package bdsup2sub.supstream.bd;

import bdsup2sub.supstream.ImageObjectFragment;

import java.util.ArrayList;
import java.util.List;

/**
 * Definition of one object identified by its 16 bit object ID.
 * Object definitions are independent of the display references in PCS
 * segments: fragments of different objects may be interleaved, and later PCS
 * segments may reuse a completed object without carrying another ODS.
 *
 * A new version of an object id creates a new instance, so frames which
 * reference the previous version keep their own snapshot and are never
 * modified by the update.
 */
public class PGSObject {

    private final int objectId;
    private final int version;
    private final int width;
    private final int height;
    /** declared full RLE buffer length (object_data_length minus the 4 size bytes) */
    private final int declaredBufferSize;
    private final List<ImageObjectFragment> fragments = new ArrayList<ImageObjectFragment>();
    private int bufferSize;
    private boolean complete;

    public PGSObject(int objectId, int version, int width, int height, int declaredBufferSize,
                     ImageObjectFragment firstFragment, boolean last) {
        this.objectId = objectId;
        this.version = version;
        this.width = width;
        this.height = height;
        this.declaredBufferSize = declaredBufferSize;
        this.fragments.add(firstFragment);
        this.bufferSize = firstFragment.getImagePacketSize();
        this.complete = last && bufferSize == declaredBufferSize;
    }

    /**
     * Append an intermediate or last ODS fragment of this object.
     * @param last true if the fragment carries the last_in_sequence flag
     */
    public void addFragment(ImageObjectFragment fragment, boolean last) {
        fragments.add(fragment);
        bufferSize += fragment.getImagePacketSize();
        if (last) {
            // a last fragment only completes the object if the declared amount
            // of RLE data has actually arrived
            complete = bufferSize == declaredBufferSize;
        }
    }

    /**
     * An object is complete once a last_in_sequence fragment was seen and all
     * declared RLE data has been collected.
     */
    public boolean isComplete() {
        return complete;
    }

    public int getObjectId() {
        return objectId;
    }

    public int getVersion() {
        return version;
    }

    public int getWidth() {
        return width;
    }

    public int getHeight() {
        return height;
    }

    public int getBufferSize() {
        return bufferSize;
    }

    public int getDeclaredBufferSize() {
        return declaredBufferSize;
    }

    public List<ImageObjectFragment> getFragments() {
        return fragments;
    }
}
