/**
 * Copyright (c), Data Geekery GmbH, contact@datageekery.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.jooq.lambda;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicInteger;

import org.jooq.lambda.support.UFHarness;

/**
 * Contract tests for {@link Seq#sortedExternal(Comparator, SpillCodec, Path, int, int)}.
 * <p>
 * Registered through {@link #register(UFHarness.Registry)} and driven by
 * {@link UFHarness}; this class has no {@code main} method.
 * <p>
 * The value material is a record-style class (the project compiles with
 * {@code --release 11}, where the Java 16 {@code record} keyword is not
 * available) carrying a sort key and the original encounter-order index.
 */
public final class SpillSortContract {

    private SpillSortContract() {}

    /** Record-style material: sort key plus original encounter order. */
    static final class KeyedRecord {
        private final int key;
        private final int ordinal;

        KeyedRecord(int key, int ordinal) {
            this.key = key;
            this.ordinal = ordinal;
        }

        int key() {
            return key;
        }

        int ordinal() {
            return ordinal;
        }

        @Override
        public boolean equals(Object o) {
            if (this == o)
                return true;
            if (!(o instanceof KeyedRecord))
                return false;
            KeyedRecord other = (KeyedRecord) o;
            return key == other.key && ordinal == other.ordinal;
        }

        @Override
        public int hashCode() {
            return Objects.hash(key, ordinal);
        }

        @Override
        public String toString() {
            return "KeyedRecord(key=" + key + ", ordinal=" + ordinal + ")";
        }
    }

    /** Hand-written DataInput/DataOutput codec; null is an explicit tagged value. */
    static final class KeyedRecordCodec implements SpillCodec<KeyedRecord> {
        static final byte TAG_NORMAL = 1;
        static final byte TAG_NULL   = 0;
        static final byte TAG_BAD    = 2;

        final int failWriteOnOrdinal;
        final int failReadTagOrdinal;
        final AtomicInteger writes = new AtomicInteger();
        final AtomicInteger reads  = new AtomicInteger();

        KeyedRecordCodec() {
            this(-1, -1);
        }

        KeyedRecordCodec(int failWriteOnOrdinal, int failReadTagOrdinal) {
            this.failWriteOnOrdinal = failWriteOnOrdinal;
            this.failReadTagOrdinal = failReadTagOrdinal;
        }

        @Override
        public void write(DataOutput out, KeyedRecord value) throws IOException {
            if (value == null) {
                out.writeByte(TAG_NULL);
                writes.incrementAndGet();
                return;
            }
            if (value.ordinal() == failWriteOnOrdinal)
                throw new IOException("write-boom-" + value.ordinal());
            if (value.ordinal() == failReadTagOrdinal) {
                out.writeByte(TAG_BAD);
                out.writeInt(value.key());
                out.writeInt(value.ordinal());
            }
            else {
                out.writeByte(TAG_NORMAL);
                out.writeInt(value.key());
                out.writeInt(value.ordinal());
            }
            writes.incrementAndGet();
        }

        @Override
        public KeyedRecord read(DataInput in) throws IOException {
            byte tag = in.readByte();
            if (tag == TAG_NULL) {
                reads.incrementAndGet();
                return null;
            }
            int key = in.readInt();
            int ordinal = in.readInt();
            if (tag == TAG_BAD)
                throw new IOException("read-boom-" + ordinal);
            reads.incrementAndGet();
            return new KeyedRecord(key, ordinal);
        }
    }

    static List<KeyedRecord> rows(int n, int keyBound) {
        List<KeyedRecord> rows = new ArrayList<>(n);
        for (int i = 0; i < n; i++)
            rows.add(new KeyedRecord(i % keyBound, i));
        return rows;
    }

    static Comparator<KeyedRecord> keyComparator() {
        return Comparator.comparingInt(KeyedRecord::key);
    }

    /** Independent reference ordering: explicit key then original ordinal. */
    static List<KeyedRecord> referenceSorted(List<KeyedRecord> source) {
        List<KeyedRecord> expected = new ArrayList<>(source);
        expected.sort(Comparator.comparingInt(KeyedRecord::key)
                                .thenComparingInt(KeyedRecord::ordinal));
        return expected;
    }

    static List<KeyedRecord> referenceSortedNullsFirst(List<KeyedRecord> source) {
        List<KeyedRecord> expected = new ArrayList<>(source);
        expected.sort(Comparator.nullsFirst(
            Comparator.comparingInt(KeyedRecord::key).thenComparingInt(KeyedRecord::ordinal)));
        return expected;
    }

    /** Honor TMPDIR (sandboxed CI) but fall back to the JVM default. */
    static Path newTempRoot() throws IOException {
        String base = System.getenv("TMPDIR");
        if (base == null || base.trim().isEmpty())
            base = System.getProperty("java.io.tmpdir");
        Path root = Files.createTempDirectory(Paths.get(base), "spill-contract-");
        Files.createFile(root.resolve("sentinel.txt"));
        return root;
    }

    static void assertOnlySentinel(Path root) throws IOException {
        List<Path> leftover = new ArrayList<>();
        Files.list(root).forEach(leftover::add);
        UFHarness.check(leftover.size() == 1);
        UFHarness.check(leftover.get(0).getFileName().toString().equals("sentinel.txt"));
    }

    static void assertStreamsBalanced(SpillSorter.Stats s) {
        UFHarness.check(s.inputStreamsOpened == s.inputStreamsClosed);
        UFHarness.check(s.outputStreamsOpened == s.outputStreamsClosed);
    }

    public static void register(UFHarness.Registry r) {
        r.add("normalStableMultiRound", "normal", SpillSortContract::normalStableMultiRound);
        r.add("normalSmallStaysInMemory", "normal", SpillSortContract::normalSmallStaysInMemory);
        r.add("boundaryNullValues", "boundary", SpillSortContract::boundaryNullValues);
        r.add("boundaryEmptySource", "boundary", SpillSortContract::boundaryEmptySource);
        r.add("boundaryCloseBeforePullAndReclose", "boundary", SpillSortContract::boundaryCloseBeforePullAndReclose);
        r.add("boundaryEarlyStop", "boundary", SpillSortContract::boundaryEarlyStop);
        r.add("boundaryValidationDoesNotConsume", "boundary", SpillSortContract::boundaryValidationDoesNotConsume);
        r.add("integrationMapFilterLimitIterator", "integration", SpillSortContract::integrationMapFilterLimitIterator);
        r.add("integrationCodecWriteFailure", "integration", SpillSortContract::integrationCodecWriteFailure);
        r.add("integrationCodecReadFailure", "integration", SpillSortContract::integrationCodecReadFailure);
        r.add("integrationComparatorFailure", "integration", SpillSortContract::integrationComparatorFailure);
    }

    /** 32 records, maxItems=2, fanIn=2: 16 initial runs, four merge rounds. */
    static void normalStableMultiRound() throws Exception {
        Path root = newTempRoot();
        List<KeyedRecord> data = rows(32, 7);
        List<KeyedRecord> expected = referenceSorted(data);
        AtomicInteger closed = new AtomicInteger();

        Seq<KeyedRecord> spill = Seq.seq(data).onClose(closed::incrementAndGet)
                                    .sortedExternal(keyComparator(), new KeyedRecordCodec(), root, 2, 2);
        SpillSorter.Stats before = SpillSorter.probe(spill);
        UFHarness.check(before.totalRecords == 0);
        UFHarness.check(before.initialRuns == 0 && before.mergeRounds == 0);

        List<KeyedRecord> actual;
        try (Seq<KeyedRecord> s = spill) {
            actual = s.toList();
        }

        UFHarness.check(actual.equals(expected));
        // Duplicate keys keep the original encounter order, value and ordinal intact.
        for (int i = 1; i < actual.size(); i++) {
            KeyedRecord a = actual.get(i - 1);
            KeyedRecord b = actual.get(i);
            UFHarness.check(a.key() <= b.key());
            if (a.key() == b.key())
                UFHarness.check(a.ordinal() < b.ordinal());
        }

        SpillSorter.Stats s = SpillSorter.probe(spill);
        UFHarness.check(s.totalRecords == 32);
        UFHarness.check(s.initialRuns == 16);
        UFHarness.check(s.mergeRounds >= 3);
        UFHarness.check(s.mergeRounds == 4);
        UFHarness.check(s.maxOpenInputFiles <= 2);
        UFHarness.check(s.maxOpenFiles <= 3);
        UFHarness.check(s.peakResidentRecords <= 2);
        UFHarness.check(s.peakResidentRecords > 0);
        assertStreamsBalanced(s);
        UFHarness.check(closed.get() == 1);

        // Re-closing is a no-op and does not touch the source again.
        spill.close();
        UFHarness.check(closed.get() == 1);
        assertOnlySentinel(root);
    }

    /** Fewer records than maxItems: everything stays in memory, no files at all. */
    static void normalSmallStaysInMemory() throws Exception {
        Path root = newTempRoot();
        List<KeyedRecord> data = rows(3, 7);
        AtomicInteger closed = new AtomicInteger();

        Seq<KeyedRecord> spill = Seq.seq(data).onClose(closed::incrementAndGet)
                                    .sortedExternal(keyComparator(), new KeyedRecordCodec(), root, 5, 2);
        try (Seq<KeyedRecord> s = spill) {
            UFHarness.check(s.toList().equals(referenceSorted(data)));
        }

        SpillSorter.Stats s = SpillSorter.probe(spill);
        UFHarness.check(s.totalRecords == 3);
        UFHarness.check(s.initialRuns == 0);
        UFHarness.check(s.mergeRounds == 0);
        UFHarness.check(s.inputStreamsOpened == 0 && s.inputStreamsClosed == 0);
        UFHarness.check(s.outputStreamsOpened == 0 && s.outputStreamsClosed == 0);
        UFHarness.check(s.peakResidentRecords == 3);
        UFHarness.check(closed.get() == 1);
        assertOnlySentinel(root);
    }

    /** Null is a normal value when comparator and codec support it. */
    static void boundaryNullValues() throws Exception {
        Path root = newTempRoot();
        List<KeyedRecord> data = new ArrayList<>(Arrays.asList(
            new KeyedRecord(3, 0), null, new KeyedRecord(1, 2), null,
            new KeyedRecord(1, 4), null, new KeyedRecord(0, 6), new KeyedRecord(3, 7)));
        List<KeyedRecord> expected = referenceSortedNullsFirst(data);
        AtomicInteger closed = new AtomicInteger();

        Seq<KeyedRecord> spill = Seq.seq(data).onClose(closed::incrementAndGet).sortedExternal(
            Comparator.nullsFirst(keyComparator()), new KeyedRecordCodec(), root, 2, 2);
        try (Seq<KeyedRecord> s = spill) {
            List<KeyedRecord> actual = s.toList();
            UFHarness.check(actual.equals(expected));
            UFHarness.check(actual.get(0) == null && actual.get(1) == null && actual.get(2) == null);
            UFHarness.check(actual.get(3).equals(new KeyedRecord(0, 6)));
        }

        SpillSorter.Stats s = SpillSorter.probe(spill);
        UFHarness.check(s.totalRecords == 8);
        UFHarness.check(s.initialRuns == 4);
        assertStreamsBalanced(s);
        UFHarness.check(closed.get() == 1);
        assertOnlySentinel(root);
    }

    /** Empty source: no directory, source still closed exactly once. */
    static void boundaryEmptySource() throws Exception {
        Path root = newTempRoot();
        AtomicInteger closed = new AtomicInteger();

        Seq<KeyedRecord> spill = Seq.<KeyedRecord>empty().onClose(closed::incrementAndGet)
                                    .sortedExternal(keyComparator(), new KeyedRecordCodec(), root, 2, 2);
        try (Seq<KeyedRecord> s = spill) {
            List<KeyedRecord> actual = s.toList();
            UFHarness.check(actual.isEmpty());
        }

        SpillSorter.Stats s = SpillSorter.probe(spill);
        UFHarness.check(s.totalRecords == 0 && s.initialRuns == 0 && s.mergeRounds == 0);
        UFHarness.check(s.inputStreamsOpened == 0 && s.outputStreamsOpened == 0);
        UFHarness.check(closed.get() == 1);
        assertOnlySentinel(root);
    }

    /** Close without ever pulling: source closes once, no directory is born. */
    static void boundaryCloseBeforePullAndReclose() throws Exception {
        Path root = newTempRoot();
        AtomicInteger seen = new AtomicInteger();
        AtomicInteger closed = new AtomicInteger();

        Seq<KeyedRecord> spill = Seq.seq(rows(10, 3)).peek(r -> seen.incrementAndGet())
                                                      .onClose(closed::incrementAndGet)
                                                      .sortedExternal(keyComparator(),
                                                          new KeyedRecordCodec(), root, 2, 2);
        assertOnlySentinel(root);
        spill.close();
        spill.close();

        UFHarness.check(seen.get() == 0);
        UFHarness.check(closed.get() == 1);
        SpillSorter.Stats s = SpillSorter.probe(spill);
        UFHarness.check(s.totalRecords == 0 && s.initialRuns == 0 && s.mergeRounds == 0);
        assertOnlySentinel(root);
    }

    /** limit + close must not read the whole sorted result back into memory. */
    static void boundaryEarlyStop() throws Exception {
        Path root = newTempRoot();
        List<KeyedRecord> data = rows(32, 7);
        List<KeyedRecord> expectedHead = referenceSorted(data).subList(0, 5);
        AtomicInteger emitted = new AtomicInteger();
        AtomicInteger closed = new AtomicInteger();

        Seq<KeyedRecord> spill = Seq.seq(data).onClose(closed::incrementAndGet)
                                              .sortedExternal(keyComparator(),
                                                  new KeyedRecordCodec(), root, 2, 2);
        try (Seq<KeyedRecord> head = spill.peek(r -> emitted.incrementAndGet()).limit(5)) {
            UFHarness.check(head.toList().equals(expectedHead));
        }

        UFHarness.check(emitted.get() == 5);
        UFHarness.check(closed.get() == 1);
        SpillSorter.Stats s = SpillSorter.probe(spill);
        UFHarness.check(s.initialRuns == 16 && s.mergeRounds == 4);
        UFHarness.check(s.maxOpenInputFiles <= 2 && s.maxOpenFiles <= 3);
        assertStreamsBalanced(s);
        assertOnlySentinel(root);
    }

    /** Invalid arguments fail immediately, before any record is consumed. */
    static void boundaryValidationDoesNotConsume() throws Exception {
        Path root = newTempRoot();
        AtomicInteger seen = new AtomicInteger();
        AtomicInteger closed = new AtomicInteger();
        Seq<KeyedRecord> source = Seq.seq(rows(8, 3)).peek(r -> seen.incrementAndGet())
                                                     .onClose(closed::incrementAndGet);
        KeyedRecordCodec codec = new KeyedRecordCodec();

        expectNpe(() -> source.sortedExternal(null, codec, root, 2, 2));
        expectNpe(() -> source.sortedExternal(keyComparator(), null, root, 2, 2));
        expectNpe(() -> source.sortedExternal(keyComparator(), codec, null, 2, 2));
        expectIae(() -> source.sortedExternal(keyComparator(), codec, root, 0, 2));
        expectIae(() -> source.sortedExternal(keyComparator(), codec, root, 1, 1));
        expectIae(() -> source.sortedExternal(keyComparator(), codec, root, 2, 1));

        UFHarness.check(seen.get() == 0);
        UFHarness.check(closed.get() == 0);
        source.close();
        UFHarness.check(closed.get() == 1);
        assertOnlySentinel(root);
    }

    /** Stays composable with map/filter/limit and plain iterator consumption. */
    static void integrationMapFilterLimitIterator() throws Exception {
        Path root = newTempRoot();
        List<KeyedRecord> data = rows(40, 9);
        List<KeyedRecord> filtered = new ArrayList<>();
        for (KeyedRecord r : data)
            if (r.ordinal() % 2 == 0)
                filtered.add(new KeyedRecord(r.key(), r.ordinal()));
        List<KeyedRecord> expectedHead = referenceSorted(filtered).subList(0, 6);
        AtomicInteger closed = new AtomicInteger();

        Seq<KeyedRecord> spill = Seq.seq(data).onClose(closed::incrementAndGet)
                                              .filter(r -> r.ordinal() % 2 == 0)
                                              .map(r -> new KeyedRecord(r.key(), r.ordinal()))
                                              .sortedExternal(keyComparator(),
                                                  new KeyedRecordCodec(), root, 2, 2);
        List<KeyedRecord> head = new ArrayList<>();
        try (Seq<KeyedRecord> limited = spill.limit(6)) {
            Iterator<KeyedRecord> it = limited.iterator();
            while (it.hasNext())
                head.add(it.next());
        }

        UFHarness.check(head.equals(expectedHead));
        UFHarness.check(head.size() == 6);
        SpillSorter.Stats s = SpillSorter.probe(spill);
        UFHarness.check(s.totalRecords == 20);
        UFHarness.check(s.initialRuns == 10);
        UFHarness.check(s.mergeRounds >= 3);
        UFHarness.check(closed.get() == 1);
        assertOnlySentinel(root);
    }

    /** IOException while writing a run surfaces as UncheckedIOException, cause kept. */
    static void integrationCodecWriteFailure() throws Exception {
        Path root = newTempRoot();
        AtomicInteger closed = new AtomicInteger();
        KeyedRecordCodec codec = new KeyedRecordCodec(5, -1);

        Seq<KeyedRecord> spill = Seq.seq(rows(32, 7)).onClose(closed::incrementAndGet)
                                                      .sortedExternal(keyComparator(), codec, root, 2, 2);
        Throwable thrown = expectThrow(() -> {
            try (Seq<KeyedRecord> s = spill) {
                s.toList();
            }
        });
        UFHarness.check(thrown instanceof UncheckedIOException);
        UFHarness.check(thrown.getCause() instanceof IOException);
        UFHarness.check("write-boom-5".equals(thrown.getCause().getMessage()));
        UFHarness.check(closed.get() == 1);
        assertStreamsBalanced(SpillSorter.probe(spill));
        assertOnlySentinel(root);
    }

    /** IOException while reading a run back during merging follows the same rules. */
    static void integrationCodecReadFailure() throws Exception {
        Path root = newTempRoot();
        AtomicInteger closed = new AtomicInteger();
        KeyedRecordCodec codec = new KeyedRecordCodec(-1, 7);

        Seq<KeyedRecord> spill = Seq.seq(rows(32, 7)).onClose(closed::incrementAndGet)
                                                      .sortedExternal(keyComparator(), codec, root, 2, 2);
        Throwable thrown = expectThrow(() -> {
            try (Seq<KeyedRecord> s = spill) {
                s.toList();
            }
        });
        UFHarness.check(thrown instanceof UncheckedIOException);
        UFHarness.check(thrown.getCause() instanceof IOException);
        UFHarness.check("read-boom-7".equals(thrown.getCause().getMessage()));
        SpillSorter.Stats s = SpillSorter.probe(spill);
        UFHarness.check(s.initialRuns == 16);
        assertStreamsBalanced(s);
        UFHarness.check(closed.get() == 1);
        assertOnlySentinel(root);
    }

    /** A runtime exception from the comparator propagates as the same object. */
    static void integrationComparatorFailure() throws Exception {
        Path root = newTempRoot();
        AtomicInteger closed = new AtomicInteger();
        IllegalStateException boom = new IllegalStateException("comparator-boom");
        Comparator<KeyedRecord> failing = (a, b) -> {
            if (a.key() == 99 || b.key() == 99)
                throw boom;
            return Integer.compare(a.key(), b.key());
        };
        List<KeyedRecord> data = rows(31, 7);
        data.add(new KeyedRecord(99, 31));

        Seq<KeyedRecord> spill = Seq.seq(data).onClose(closed::incrementAndGet)
                                              .sortedExternal(failing, new KeyedRecordCodec(), root, 2, 2);
        Throwable thrown = expectThrow(() -> {
            try (Seq<KeyedRecord> s = spill) {
                s.toList();
            }
        });
        UFHarness.check(thrown == boom);
        UFHarness.check(closed.get() == 1);
        assertOnlySentinel(root);
    }

    interface ThrowingRunnable {
        void run() throws Exception;
    }

    static void expectNpe(ThrowingRunnable r) {
        Throwable t = expectThrow(r);
        UFHarness.check(t instanceof NullPointerException);
    }

    static void expectIae(ThrowingRunnable r) {
        Throwable t = expectThrow(r);
        UFHarness.check(t instanceof IllegalArgumentException);
    }

    static Throwable expectThrow(ThrowingRunnable r) {
        try {
            r.run();
        }
        catch (Throwable t) {
            return t;
        }
        UFHarness.check(false);
        throw new AssertionError("unreachable");
    }
}
