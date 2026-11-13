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

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;
import java.util.PriorityQueue;
import java.util.Spliterator;
import java.util.UUID;
import java.util.function.Consumer;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;

/**
 * A lazy, stable, external (spill) sort backing
 * {@link Seq#sortedExternal(Comparator, SpillCodec, Path, int, int)}.
 * <p>
 * The source stream is not touched until the first terminal operation. Up to
 * {@code maxItems} records are buffered in memory and sorted into an initial
 * run; every further run is spilled to a private directory under
 * {@code tempRoot}. Runs are then merged in repeated passes, each combining at
 * most {@code fanIn} runs. Stability (source encounter order on comparator
 * ties) is guaranteed by carrying the original, globally unique sequence
 * number through every run and every merge round, so ties are ordered
 * correctly across runs and across rounds, not only inside one run.
 * <p>
 * Run file layout (all fields use {@link java.io.DataOutput} encoding):
 * <pre>
 *   long   recordCount
 *   record[recordCount]:
 *     long  stable sequence number
 *     ...   codec payload (exactly what {@link SpillCodec#write} emitted)
 * </pre>
 * The record count, not a {@code null} payload, delimits every run.
 *
 * @author Lukas Eder
 */
final class SpillSorter<T> {

    /** Nothing pulled from the source yet; no files or directory exist. */
    private static final int INIT    = 0;
    /** Sort finished, the whole result fits in {@link #memory}. */
    private static final int MEMORY  = 1;
    /** Sort finished, {@link #finalFile} is streamed on demand. */
    private static final int SPILLED   = 2;
    /** All results were read; further iteration is an empty, idempotent no-op. */
    private static final int EXHAUSTED = 3;
    private static final int CLOSED    = 4;

    private final Seq<T>                 source;
    private final Comparator<? super T> comparator;
    private final SpillCodec<T>          codec;
    private final Path                   tempRoot;
    private final int                    maxItems;
    private final int                    fanIn;
    private final Stats                  stats = new Stats();

    private int              phase = INIT;
    private long             sequence;
    private List<Entry>      buffer;
    private List<Entry>      memory;
    private int              memoryIndex;
    private Path             workDir;
    private final List<Path> runFiles = new ArrayList<>();
    private int              fileCounter;
    private Path             finalFile;
    private long             finalCount;
    private RunReader        currentReader;

    private SpillSorter(Seq<T> source, Comparator<? super T> comparator, SpillCodec<T> codec,
                        Path tempRoot, int maxItems, int fanIn) {
        this.source = source;
        this.comparator = comparator;
        this.codec = codec;
        this.tempRoot = tempRoot;
        this.maxItems = maxItems;
        this.fanIn = fanIn;
    }

    /**
     * Validate eagerly (without consuming the source) and wrap the lazy sort
     * into a {@link Seq}.
     */
    static <T> Seq<T> sort(Seq<T> source, Comparator<? super T> comparator, SpillCodec<T> codec,
                           Path tempRoot, int maxItems, int fanIn) {
        if (comparator == null)
            throw new NullPointerException("comparator");
        if (codec == null)
            throw new NullPointerException("codec");
        if (tempRoot == null)
            throw new NullPointerException("tempRoot");
        if (maxItems < 1)
            throw new IllegalArgumentException("maxItems must be >= 1: " + maxItems);
        if (fanIn < 2)
            throw new IllegalArgumentException("fanIn must be >= 2: " + fanIn);

        SpillSorter<T> sorter = new SpillSorter<>(source, comparator, codec, tempRoot, maxItems, fanIn);
        Spliterator<T> spliterator = new SpillSpliterator<>(sorter);
        Stream<T> stream = StreamSupport.stream(spliterator, false).onClose(sorter::handleClose);
        return new SpillSeq<>(stream, sorter);
    }

    Stats snapshotStats() {
        return stats.snapshot();
    }

    /**
     * Package-private test probe: read the statistics collected by a spill
     * sort. There is intentionally no public counterpart.
     */
    static Stats probe(Seq<?> seq) {
        if (!(seq instanceof SpillSeq))
            throw new IllegalArgumentException("not a spill-sorted Seq: " + seq);
        return ((SpillSeq<?>) seq).sorter().snapshotStats();
    }

    private Comparator<Entry> stableOrder() {
        return (a, b) -> {
            int c = comparator.compare(a.value, b.value);
            return c != 0 ? c : Long.compare(a.seq, b.seq);
        };
    }

    private Path createRunPath() {
        return workDir.resolve("run-" + (fileCounter++));
    }

    /** Pull the source, build initial runs, merge until one run remains. */
    private void run() {
        if (phase == CLOSED)
            throw new IllegalStateException("the sorted stream has already been closed");
        if (phase != INIT)
            return;

        try (Stream<? extends T> toClose = source) {
            buffer = new ArrayList<>(maxItems);
            Iterator<? extends T> it = source.iterator();
            while (it.hasNext()) {
                T value = it.next();
                buffer.add(new Entry(sequence++, value));
                stats.totalRecords++;
                if (buffer.size() > stats.peakResidentRecords)
                    stats.peakResidentRecords = buffer.size();

                if (buffer.size() == maxItems)
                    flushInitialRun();
            }

            finishInitialRuns();

            while (runFiles.size() > 1)
                mergeRound();

            if (!runFiles.isEmpty()) {
                finalFile = runFiles.get(0);
                finalCount = readCount(finalFile);
                phase = SPILLED;
            }
            else {
                memory.sort(stableOrder());
                phase = MEMORY;
            }
        }
        catch (IOException e) {
            throw fail(new UncheckedIOException(e));
        }
        catch (RuntimeException e) {
            throw fail(e);
        }
        catch (Error e) {
            throw fail(e);
        }
    }

    /** Attach any cleanup failure as a suppressed exception, keep the cause. */
    private <X extends Throwable> X fail(X primary) {
        Throwable cleanup = abortQuietly();
        if (cleanup != null && cleanup != primary)
            primary.addSuppressed(cleanup);
        return primary;
    }

    private void flushInitialRun() throws IOException {
        buffer.sort(stableOrder());
        if (workDir == null)
            Files.createDirectory(createWorkDir());
        Path run = createRunPath();
        writeRun(run, buffer);
        runFiles.add(run);
        stats.initialRuns++;
        buffer = new ArrayList<>(maxItems);
    }

    private Path createWorkDir() {
        Path dir = tempRoot.resolve("jool-spill-" + UUID.randomUUID());
        workDir = dir;
        return dir;
    }

    private void finishInitialRuns() throws IOException {
        if (!buffer.isEmpty()) {
            if (runFiles.isEmpty()) {
                // Everything fit into memory: keep the single run there.
                memory = buffer;
            }
            else {
                buffer.sort(stableOrder());
                Path run = createRunPath();
                writeRun(run, buffer);
                runFiles.add(run);
                stats.initialRuns++;
            }
        }
        else if (memory == null) {
            memory = new ArrayList<>(0);
        }
        buffer = null;
    }

    private void writeRun(Path file, List<Entry> entries) throws IOException {
        stats.outputStreamsOpened++;
        try (DataOutputStream out = new DataOutputStream(
                new BufferedOutputStream(Files.newOutputStream(file)))) {
            out.writeLong(entries.size());
            for (Entry e : entries) {
                out.writeLong(e.seq);
                codec.write(out, e.value);
            }
        }
        finally {
            stats.outputStreamsClosed++;
        }
    }

    /** One merge pass: partition the current runs into groups of at most fanIn. */
    private void mergeRound() throws IOException {
        List<Path> inputs = new ArrayList<>(runFiles);
        runFiles.clear();

        for (int start = 0; start < inputs.size(); start += fanIn) {
            int end = Math.min(start + fanIn, inputs.size());
            List<Path> group = inputs.subList(start, end);

            if (group.size() == 1) {
                runFiles.add(group.get(0));
                continue;
            }

            Path output = createRunPath();
            mergeGroup(group, output);
            for (Path consumed : group)
                Files.deleteIfExists(consumed);
            runFiles.add(output);
        }

        stats.mergeRounds++;
    }

    private void mergeGroup(List<Path> group, Path output) throws IOException {
        List<RunReader> readers = new ArrayList<>(group.size());
        PriorityQueue<Candidate> heap = new PriorityQueue<>((a, b) -> {
            int c = comparator.compare(a.value, b.value);
            return c != 0 ? c : Long.compare(a.seq, b.seq);
        });
        long total = 0;

        try {
            for (Path p : group) {
                RunReader reader = new RunReader(p);
                readers.add(reader);
                total += reader.count;
                if (reader.advance())
                    heap.add(new Candidate(reader));
            }
            stats.maxOpenInputFiles = Math.max(stats.maxOpenInputFiles, readers.size());
            stats.maxOpenFiles = Math.max(stats.maxOpenFiles, readers.size() + 1);

            stats.outputStreamsOpened++;
            try (DataOutputStream out = new DataOutputStream(
                    new BufferedOutputStream(Files.newOutputStream(output)))) {
                out.writeLong(total);
                while (!heap.isEmpty()) {
                    Candidate candidate = heap.poll();
                    out.writeLong(candidate.seq);
                    codec.write(out, candidate.value);
                    if (candidate.reader.advance()) {
                        heap.add(new Candidate(candidate.reader));
                        if (heap.size() > stats.peakResidentRecords)
                            stats.peakResidentRecords = heap.size();
                    }
                }
            }
            finally {
                stats.outputStreamsClosed++;
            }
        }
        finally {
            closeReaders(readers);
        }
    }

    private void closeReaders(List<RunReader> readers) throws IOException {
        IOException failure = null;
        for (RunReader reader : readers) {
            try {
                reader.close();
            }
            catch (IOException e) {
                if (failure == null)
                    failure = e;
                else
                    failure.addSuppressed(e);
            }
        }
        if (failure != null)
            throw failure;
    }

    private static long readCount(Path file) throws IOException {
        try (DataInputStream in = new DataInputStream(
                new BufferedInputStream(Files.newInputStream(file)))) {
            return in.readLong();
        }
    }

    private boolean tryAdvance(Consumer<? super T> action) {
        if (phase == EXHAUSTED)
            return false;
        if (phase == CLOSED)
            throw new IllegalStateException("the sorted stream has already been closed");
        if (phase == INIT)
            run();

        try {
            if (phase == MEMORY) {
                if (memoryIndex >= memory.size()) {
                    closeAfterExhaustion();
                    return false;
                }
                action.accept(memory.get(memoryIndex++).value);
                if (memoryIndex >= memory.size())
                    closeAfterExhaustion();
                return true;
            }

            if (phase == SPILLED) {
                if (currentReader == null) {
                    currentReader = new RunReader(finalFile);
                    stats.maxOpenInputFiles = Math.max(stats.maxOpenInputFiles, 1);
                    stats.maxOpenFiles = Math.max(stats.maxOpenFiles, 1);
                }

                if (currentReader.advance()) {
                    action.accept(currentReader.value);
                    return true;
                }

                currentReader.close();
                currentReader = null;
                closeAfterExhaustion();
                return false;
            }

            throw new IllegalStateException("the sorted stream has already been closed");
        }
        catch (IOException e) {
            throw fail(new UncheckedIOException(e));
        }
        catch (RuntimeException e) {
            throw fail(e);
        }
        catch (Error e) {
            throw fail(e);
        }
    }

    private void closeAfterExhaustion() throws IOException {
        try {
            deleteWorkDir();
        }
        finally {
            phase = EXHAUSTED;
        }
    }

    private synchronized void handleClose() {
        if (phase == CLOSED || phase == EXHAUSTED)
            return;

        if (phase == INIT) {
            // Never started: close the source once, create no directory.
            phase = CLOSED;
            source.close();
            return;
        }

        Throwable cleanupFailure = null;
        if (currentReader != null) {
            try {
                currentReader.close();
            }
            catch (IOException | RuntimeException e) {
                cleanupFailure = e;
            }
            currentReader = null;
        }
        try {
            deleteWorkDir();
        }
        catch (IOException | RuntimeException e) {
            if (cleanupFailure == null)
                cleanupFailure = e;
            else
                cleanupFailure.addSuppressed(e);
        }
        phase = CLOSED;

        // No primary error here: still make the cleanup failure visible.
        if (cleanupFailure instanceof IOException)
            throw new UncheckedIOException((IOException) cleanupFailure);
        if (cleanupFailure instanceof RuntimeException)
            throw (RuntimeException) cleanupFailure;
    }

    /** Best-effort cleanup on the failure path; never masks the primary error. */
    private Throwable abortQuietly() {
        Throwable failure = null;
        if (currentReader != null) {
            try {
                currentReader.close();
            }
            catch (IOException | RuntimeException e) {
                failure = e;
            }
            currentReader = null;
        }
        try {
            deleteWorkDir();
        }
        catch (IOException | RuntimeException e) {
            if (failure == null)
                failure = e;
            else
                failure.addSuppressed(e);
        }
        phase = CLOSED;
        return failure;
    }

    private void deleteWorkDir() throws IOException {
        if (workDir == null || !Files.exists(workDir))
            return;

        IOException failure = null;
        List<Path> paths;
        try (Stream<Path> walk = Files.walk(workDir)) {
            paths = new ArrayList<>();
            walk.forEach(paths::add);
        }
        paths.sort(Comparator.comparingInt(Path::getNameCount).reversed());

        for (Path p : paths) {
            try {
                Files.deleteIfExists(p);
            }
            catch (IOException e) {
                if (failure == null)
                    failure = e;
                else
                    failure.addSuppressed(e);
            }
        }
        if (failure != null)
            throw failure;
    }

    private long estimateSize() {
        if (phase == INIT)
            return Long.MAX_VALUE;
        if (phase == MEMORY)
            return memory.size() - memoryIndex;
        if (phase == SPILLED)
            return currentReader == null ? finalCount : finalCount - currentReader.read;
        return 0;
    }

    private int characteristics() {
        if (phase == INIT)
            return Spliterator.ORDERED;
        return Spliterator.ORDERED | Spliterator.SIZED | Spliterator.SUBSIZED;
    }

    private final class Entry {
        final long seq;
        final T    value;

        Entry(long seq, T value) {
            this.seq = seq;
            this.value = value;
        }
    }

    private final class Candidate {
        final RunReader reader;
        final long      seq;
        final T         value;

        Candidate(RunReader reader) {
            this.reader = reader;
            this.seq = reader.seq;
            this.value = reader.value;
        }
    }

    private final class RunReader implements AutoCloseable {
        final DataInputStream in;
        final long            count;
        long                  read;
        long                  seq;
        T                     value;
        private boolean       closed;

        RunReader(Path file) throws IOException {
            stats.inputStreamsOpened++;
            DataInputStream opened = null;
            try {
                opened = new DataInputStream(new BufferedInputStream(Files.newInputStream(file)));
                this.in = opened;
                this.count = in.readLong();
            }
            catch (IOException e) {
                if (opened != null) {
                    try {
                        opened.close();
                    }
                    catch (IOException suppressed) {
                        e.addSuppressed(suppressed);
                    }
                }
                stats.inputStreamsClosed++;
                throw e;
            }
        }

        boolean advance() throws IOException {
            if (read >= count)
                return false;
            seq = in.readLong();
            value = codec.read(in);
            read++;
            return true;
        }

        @Override
        public void close() throws IOException {
            if (closed)
                return;
            closed = true;
            stats.inputStreamsClosed++;
            in.close();
        }
    }

    private static final class SpillSpliterator<T> implements Spliterator<T> {
        private final SpillSorter<T> sorter;

        SpillSpliterator(SpillSorter<T> sorter) {
            this.sorter = sorter;
        }

        @Override
        public boolean tryAdvance(Consumer<? super T> action) {
            return sorter.tryAdvance(action);
        }

        @Override
        public Spliterator<T> trySplit() {
            return null;
        }

        @Override
        public long estimateSize() {
            return sorter.estimateSize();
        }

        @Override
        public int characteristics() {
            return sorter.characteristics();
        }
    }

    /**
     * {@link SeqImpl} variant that keeps a handle on its sorter so that
     * same-package tests can inspect the collected {@link Stats}.
     */
    static final class SpillSeq<T> extends SeqImpl<T> {
        private final SpillSorter<T> sorter;

        SpillSeq(Stream<T> stream, SpillSorter<T> sorter) {
            super(stream);
            this.sorter = sorter;
        }

        SpillSorter<T> sorter() {
            return sorter;
        }
    }

    /**
     * Package-private observability into one spill sort. Not part of the
     * public API; tests read this through the package-private probe.
     */
    static final class Stats {
        long totalRecords;
        int  initialRuns;
        int  mergeRounds;
        int  maxOpenInputFiles;
        int  maxOpenFiles;
        long peakResidentRecords;
        int  inputStreamsOpened;
        int  inputStreamsClosed;
        int  outputStreamsOpened;
        int  outputStreamsClosed;

        Stats snapshot() {
            Stats s = new Stats();
            s.totalRecords = totalRecords;
            s.initialRuns = initialRuns;
            s.mergeRounds = mergeRounds;
            s.maxOpenInputFiles = maxOpenInputFiles;
            s.maxOpenFiles = maxOpenFiles;
            s.peakResidentRecords = peakResidentRecords;
            s.inputStreamsOpened = inputStreamsOpened;
            s.inputStreamsClosed = inputStreamsClosed;
            s.outputStreamsOpened = outputStreamsOpened;
            s.outputStreamsClosed = outputStreamsClosed;
            return s;
        }

        @Override
        public String toString() {
            return "Stats[totalRecords=" + totalRecords
                + ", initialRuns=" + initialRuns
                + ", mergeRounds=" + mergeRounds
                + ", maxOpenInputFiles=" + maxOpenInputFiles
                + ", maxOpenFiles=" + maxOpenFiles
                + ", peakResidentRecords=" + peakResidentRecords
                + ", inputStreamsOpened=" + inputStreamsOpened
                + ", inputStreamsClosed=" + inputStreamsClosed
                + ", outputStreamsOpened=" + outputStreamsOpened
                + ", outputStreamsClosed=" + outputStreamsClosed
                + "]";
        }
    }
}
