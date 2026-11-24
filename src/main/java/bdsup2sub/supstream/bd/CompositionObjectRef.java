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

/**
 * One composition object reference of a PCS segment.
 * A PCS may contain up to two of these, referencing object IDs which do not
 * have to be consecutive.
 */
public class CompositionObjectRef {

    /** 16 bit object_id_ref */
    private final int objectId;
    /** window_id_ref (0..1) */
    private final int windowId;
    /** composition_object_horizontal_position */
    private final int x;
    /** composition_object_vertical_position */
    private final int y;
    /** object_cropped_flag */
    private final boolean cropped;
    /** object_cropping_horizontal_position */
    private final int cropX;
    /** object_cropping_vertical_position */
    private final int cropY;
    /** object_cropping_width */
    private final int cropWidth;
    /** object_cropping_height */
    private final int cropHeight;
    /** forced_on_flag */
    private final boolean forced;

    public CompositionObjectRef(int objectId, int windowId, int x, int y,
                                boolean cropped, int cropX, int cropY, int cropWidth, int cropHeight,
                                boolean forced) {
        this.objectId = objectId;
        this.windowId = windowId;
        this.x = x;
        this.y = y;
        this.cropped = cropped;
        this.cropX = cropX;
        this.cropY = cropY;
        this.cropWidth = cropWidth;
        this.cropHeight = cropHeight;
        this.forced = forced;
    }

    public int getObjectId() {
        return objectId;
    }

    public int getWindowId() {
        return windowId;
    }

    public int getX() {
        return x;
    }

    public int getY() {
        return y;
    }

    public boolean isCropped() {
        return cropped;
    }

    public int getCropX() {
        return cropX;
    }

    public int getCropY() {
        return cropY;
    }

    public int getCropWidth() {
        return cropWidth;
    }

    public int getCropHeight() {
        return cropHeight;
    }

    public boolean isForced() {
        return forced;
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (o == null || getClass() != o.getClass()) return false;

        CompositionObjectRef that = (CompositionObjectRef) o;

        if (objectId != that.objectId) return false;
        if (windowId != that.windowId) return false;
        if (x != that.x) return false;
        if (y != that.y) return false;
        if (cropped != that.cropped) return false;
        if (cropX != that.cropX) return false;
        if (cropY != that.cropY) return false;
        if (cropWidth != that.cropWidth) return false;
        if (cropHeight != that.cropHeight) return false;
        if (forced != that.forced) return false;

        return true;
    }

    @Override
    public int hashCode() {
        int result = objectId;
        result = 31 * result + windowId;
        result = 31 * result + x;
        result = 31 * result + y;
        result = 31 * result + (cropped ? 1 : 0);
        result = 31 * result + cropX;
        result = 31 * result + cropY;
        result = 31 * result + cropWidth;
        result = 31 * result + cropHeight;
        result = 31 * result + (forced ? 1 : 0);
        return result;
    }
}
