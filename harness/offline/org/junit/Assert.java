package org.junit;
import java.util.Arrays;
import java.util.Objects;
/** Only the assertions needed to compile the two original parser test classes. */
public final class Assert {
    public static void assertEquals(Object expected, Object actual) { assertEquals("", expected, actual); }
    public static void assertEquals(String message, Object expected, Object actual) {
        if (!Objects.equals(expected, actual)) throw new AssertionError(message + " expected <" + expected + "> actual <" + actual + ">");
    }
    public static void assertEquals(long expected, long actual) {
        if (expected != actual) throw new AssertionError("expected " + expected + " actual " + actual);
    }
    public static void assertTrue(boolean actual) { assertTrue("expected true", actual); }
    public static void assertTrue(String message, boolean actual) { if (!actual) throw new AssertionError(message); }
    public static void assertFalse(boolean actual) { assertTrue("expected false", !actual); }
    public static void assertFalse(String message, boolean actual) { assertTrue(message, !actual); }
    public static void assertArrayEquals(Object[] expected, Object[] actual) {
        if (!Arrays.equals(expected, actual)) throw new AssertionError("array mismatch " + Arrays.toString(expected) + " / " + Arrays.toString(actual));
    }
}
