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
 * One resolved composition object of a committed display set.
 * The geometry is the result of applying the optional PCS cropping rectangle
 * and intersecting with the referenced WDS window and the video canvas.
 * Source coordinates address pixels inside the decoded object bitmap,
 * destination coordinates address the video canvas.
 */
public class ResolvedComposition {

    private final PGSObject object;
    private final int sourceX;
    private final int sourceY;
    private final int destinationX;
    private final int destinationY;
    private final int width;
    private final int height;
    private final boolean forced;

    public ResolvedComposition(PGSObject object, int sourceX, int sourceY,
                               int destinationX, int destinationY, int width, int height,
                               boolean forced) {
        this.object = object;
        this.sourceX = sourceX;
        this.sourceY = sourceY;
        this.destinationX = destinationX;
        this.destinationY = destinationY;
        this.width = width;
        this.height = height;
        this.forced = forced;
    }

    public PGSObject getObject() {
        return object;
    }

    public int getSourceX() {
        return sourceX;
    }

    public int getSourceY() {
        return sourceY;
    }

    public int getDestinationX() {
        return destinationX;
    }

    public int getDestinationY() {
        return destinationY;
    }

    public int getWidth() {
        return width;
    }

    public int getHeight() {
        return height;
    }

    public boolean isForced() {
        return forced;
    }
}
