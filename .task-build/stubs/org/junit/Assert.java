package org.junit;
public class Assert {
    public static void assertTrue(boolean c) { if (!c) throw new AssertionError(); }
    public static void assertTrue(String m, boolean c) { if (!c) throw new AssertionError(m); }
    public static void assertFalse(boolean c) { if (c) throw new AssertionError(); }
    public static void assertFalse(String m, boolean c) { if (c) throw new AssertionError(m); }
    public static void assertNull(Object o) { if (o != null) throw new AssertionError(); }
    public static void assertNotNull(Object o) { if (o == null) throw new AssertionError(); }
    public static void assertSame(Object a, Object b) { if (a != b) throw new AssertionError(); }
    public static void assertNotSame(Object a, Object b) { if (a == b) throw new AssertionError(); }
    public static void assertEquals(Object a, Object b) { if (a == null ? b != null : !a.equals(b)) throw new AssertionError(a + " != " + b); }
    public static void assertEquals(long a, long b) { if (a != b) throw new AssertionError(a + " != " + b); }
    public static void assertEquals(String m, long a, long b) { if (a != b) throw new AssertionError(m); }
    public static void assertEquals(String m, Object a, Object b) { if (a == null ? b != null : !a.equals(b)) throw new AssertionError(m); }
    public static void assertEquals(double a, double b, double d) { if (Math.abs(a - b) > d) throw new AssertionError(); }
    public static void fail(String m) { throw new AssertionError(m); }
}
