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

import bdsup2sub.supstream.PaletteInfo;
import bdsup2sub.supstream.SubPicture;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * One committed display set (caption frame) of a BD-SUP stream.
 * The frame stores the PCS composition references, the WDS windows, its own
 * palette snapshot and - once committed - the resolved composition objects.
 * Because every frame keeps references to the exact object versions it
 * displays, later object or palette updates can never rewrite an earlier
 * frame, and a new epoch does not need to keep the old object cache.
 */
public class SubPictureBD extends SubPicture {

    /** raw composition references in PCS order (0..2 entries), used for interval detection */
    private final List<CompositionObjectRef> compositionRefs = new ArrayList<CompositionObjectRef>();
    /** window definitions keyed by window id (0..2 entries) */
    private final Map<Integer, WindowDefinition> windows = new LinkedHashMap<Integer, WindowDefinition>();
    /** resolved, clipped compositions (filled when the display set is committed) */
    private final List<ResolvedComposition> resolvedCompositions = new ArrayList<ResolvedComposition>();
    /** palette updates visible to this frame, one list per palette id */
    private final List<List<PaletteInfo>> palettes = new ArrayList<List<PaletteInfo>>();
    {
        for (int i = 0; i < 8; i++) {
            palettes.add(new ArrayList<PaletteInfo>());
        }
    }
    /** FPS type (e.g. 0x10 = 24p) */
    private int type;
    /** palette_id_ref of the PCS */
    private int paletteId;
    /** bounding box of the cropped object rectangles: x offset on the video canvas */
    private int frameX;
    /** bounding box of the cropped object rectangles: y offset on the video canvas */
    private int frameY;
    /** bounding box width */
    private int frameWidth;
    /** bounding box height */
    private int frameHeight;

    public SubPictureBD() {
    }

    public SubPictureBD(SubPictureBD other) {
        super(other);

        this.type = other.type;
        this.paletteId = other.paletteId;
        this.frameX = other.frameX;
        this.frameY = other.frameY;
        this.frameWidth = other.frameWidth;
        this.frameHeight = other.frameHeight;

        this.compositionRefs.addAll(other.compositionRefs);
        this.windows.putAll(other.windows);
        this.resolvedCompositions.addAll(other.resolvedCompositions);
        for (int i = 0; i < other.palettes.size(); i++) {
            this.palettes.get(i).addAll(other.palettes.get(i));
        }
    }

    @Override
    public int getImageWidth() {
        return frameWidth;
    }

    @Override
    public int getImageHeight() {
        return frameHeight;
    }

    @Override
    public int getXOffset() {
        return frameX;
    }

    @Override
    public int getYOffset() {
        return frameY;
    }

    public List<CompositionObjectRef> getCompositionRefs() {
        return compositionRefs;
    }

    public Map<Integer, WindowDefinition> getWindows() {
        return windows;
    }

    public List<ResolvedComposition> getResolvedCompositions() {
        return resolvedCompositions;
    }

    public List<List<PaletteInfo>> getPalettes() {
        return palettes;
    }

    public int getType() {
        return type;
    }

    public void setType(int type) {
        this.type = type;
    }

    public int getPaletteId() {
        return paletteId;
    }

    public void setPaletteId(int paletteId) {
        this.paletteId = paletteId;
    }

    public void setFrameBounds(int x, int y, int width, int height) {
        this.frameX = x;
        this.frameY = y;
        this.frameWidth = width;
        this.frameHeight = height;
    }
}
