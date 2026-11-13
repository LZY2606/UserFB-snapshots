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

/**
 * A strategy for serializing the individual values of a
 * {@link Seq#sortedExternal(Comparator, SpillCodec, java.nio.file.Path, int, int)}
 * spill sort to and from binary run files.
 * <p>
 * The codec is responsible for the round trip of a single value only. The
 * sorter itself manages everything that delimits and orders the runs: the
 * number of records written, the stable encounter-order position and the end
 * of a run. In particular, an implementation must <strong>not</strong> rely on
 * {@link #read(DataInput)} returning {@code null} to signal end of file;
 * {@code null} is a legal value whenever the codec supports it.
 * <p>
 * Neither {@link Object#toString()} nor Java object serialization
 * ({@link java.io.Serializable}) may be used as a substitute for an explicit
 * codec: the binary representation is entirely defined by the implementation.
 * Serialized values are not required to retain the identity of the original
 * objects, only their equality and the comparator's ordering.
 *
 * @author Lukas Eder
 */
public interface SpillCodec<T> {

    /**
     * Write one value. If the codec supports {@code null}, {@code null} is a
     * regular value and must be round-trippable.
     */
    void write(DataOutput out, T value) throws IOException;

    /**
     * Read one value previously written with {@link #write(DataOutput, Object)}.
     * <p>
     * Returning {@code null} does <strong>not</strong> mark the end of a run.
     * The sorter knows the record count of every run and stops reading at that
     * boundary.
     */
    T read(DataInput in) throws IOException;
}
