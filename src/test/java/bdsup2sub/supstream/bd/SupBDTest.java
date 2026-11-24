package bdsup2sub.supstream.bd;

import bdsup2sub.bitmap.ErasePatch;
import bdsup2sub.core.CoreException;
import bdsup2sub.supstream.PaletteInfo;
import org.junit.Test;

import java.io.File;
import java.io.IOException;
import java.net.URISyntaxException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public class SupBDTest {

    @Test
    public void shouldParseBDSupStream() throws CoreException, IOException, URISyntaxException {
        SupBD supBD = new SupBD(new File(ClassLoader.getSystemResource("test.sup").toURI()).getAbsolutePath());

        assertEquals(1, supBD.getFrameCount());

        SubPictureBD actual = (SubPictureBD) supBD.getSubPicture(0);
        assertEquals(16, actual.getType());
        assertEquals(1920, actual.getWidth());
        assertEquals(1080, actual.getHeight());
        assertEquals(2781531, actual.getStartTime());
        assertEquals(2980480, actual.getEndTime());
        assertFalse(actual.isForced());
        assertEquals(0, actual.getCompositionNumber());
        assertFalse(actual.isWasDecoded());
        assertFalse(actual.isExcluded());
        assertEquals(new ArrayList<ErasePatch>(), actual.getErasePatch());
        assertEquals(0, actual.getPaletteId());

        // one PCS composition reference
        assertEquals(1, actual.getCompositionRefs().size());
        CompositionObjectRef ref = actual.getCompositionRefs().get(0);
        assertEquals(0, ref.getObjectId());
        assertEquals(0, ref.getWindowId());
        assertEquals(0, ref.getX());
        assertEquals(931, ref.getY());
        assertFalse(ref.isCropped());
        assertFalse(ref.isForced());

        // one window definition
        assertEquals(1, actual.getWindows().size());
        WindowDefinition window = actual.getWindows().get(0);
        assertEquals(0, window.getId());
        assertEquals(0, window.getX());
        assertEquals(931, window.getY());
        assertEquals(1920, window.getWidth());
        assertEquals(125, window.getHeight());

        // the composed frame covers the full object at its absolute position
        assertEquals(0, actual.getXOffset());
        assertEquals(931, actual.getYOffset());
        assertEquals(1920, actual.getImageWidth());
        assertEquals(125, actual.getImageHeight());

        // one resolved composition backed by a complete 1920x125 object
        assertEquals(1, actual.getResolvedCompositions().size());
        ResolvedComposition composition = actual.getResolvedCompositions().get(0);
        assertEquals(0, composition.getObject().getObjectId());
        assertTrue(composition.getObject().isComplete());
        assertEquals(1920, composition.getObject().getWidth());
        assertEquals(125, composition.getObject().getHeight());
        assertEquals(0, composition.getSourceX());
        assertEquals(0, composition.getSourceY());
        assertEquals(0, composition.getDestinationX());
        assertEquals(931, composition.getDestinationY());
        assertEquals(1920, composition.getWidth());
        assertEquals(125, composition.getHeight());

        // palette snapshot with 16 entries at the PDS payload offset
        List<List<PaletteInfo>> palettes = actual.getPalettes();
        assertEquals(8, palettes.size());
        assertEquals(1, palettes.get(0).size());
        assertEquals(new PaletteInfo(70, 16), palettes.get(0).get(0));
        for (int i = 1; i < palettes.size(); i++) {
            assertTrue(palettes.get(i).isEmpty());
        }

        // no stale window entries
        Map<Integer, WindowDefinition> windows = actual.getWindows();
        assertEquals(1, windows.size());
    }
}
