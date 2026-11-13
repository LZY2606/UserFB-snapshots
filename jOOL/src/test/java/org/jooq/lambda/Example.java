package org.jooq.lambda;

import java.math.BigDecimal;

import static org.jooq.lambda.tuple.Tuple.tuple;
import static java.util.Comparator.naturalOrder;
import static java.util.Comparator.reverseOrder;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Optional;
import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Path;
import java.nio.file.Paths;
import org.jooq.lambda.tuple.Tuple3;

import org.jooq.lambda.tuple.Tuple4;

public class Example {
    public static void main(String[] args) {
        List<Person> personsList = new ArrayList<>();

        personsList.add(new Person("John", "Doe", 25, 1.80, 80));
        personsList.add(new Person("Jane", "Doe", 30, 1.69, 60));
        personsList.add(new Person("John", "Smith", 35, 174, 70));

        Tuple4<Long, Optional<Integer>, Optional<Double>, Optional<Double>> r1 =
        Seq.seq(personsList)
           .filter(p -> p.getFirstName().equals("John"))
           .collect(
                Agg.count(),
                Agg.max(Person::getAge),
                Agg.min(Person::getHeight),
                Agg.avg(Person::getWeight)
           );

        System.out.println(r1);
        System.out.println();

        System.out.println(
            Seq.of("a", "a", "a", "b", "c", "c", "d", "e")
               .window(naturalOrder())
               .map(w -> tuple(
                    w.value(),
                    w.rowNumber(),
                    w.rank(),
                    w.denseRank()
               ))
               .format()
        );
        System.out.println();
        
        
        System.out.println(
            Seq.of("a", "a", "a", "b", "c", "c", "d", "e")
               .window(naturalOrder())
               .map(w -> tuple(
                    w.value(),
                    w.count(),
                    w.median(),
                    w.lead(),
                    w.lag(),
                    w.toString()
               ))
               .format()
        );
        System.out.println();
        
        
        BigDecimal currentBalance = new BigDecimal("19985.81");
        
        System.out.println(
            Seq.of(
                    tuple(9997, "2014-03-18", new BigDecimal("99.17")),
                    tuple(9981, "2014-03-16", new BigDecimal("71.44")),
                    tuple(9979, "2014-03-16", new BigDecimal("-94.60")),
                    tuple(9977, "2014-03-16", new BigDecimal("-6.96")),
                    tuple(9971, "2014-03-15", new BigDecimal("-65.95")))
               .window(Comparator.comparing((Tuple3<Integer, String, BigDecimal> t) -> t.v1, reverseOrder()).thenComparing(t -> t.v2), Long.MIN_VALUE, -1)
               .map(w -> w.value().concat(
                    currentBalance.subtract(w.sum(t -> t.v3).orElse(BigDecimal.ZERO))
               ))
               .format()
        );
        System.out.println();
        
        System.out.println(
        Seq.of(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
            .window(i -> i % 2, -1, 1)
            .map(w -> tuple(w.value(), w.sum()))

            .format());

        spillSortExample();
    }

    /**
     * External (spill) sort example: records larger than memory are sorted
     * through run files under a private temp directory. Always close the
     * returned Seq with try-with-resources, because short-circuiting terminal
     * operations such as {@code limit} do not close streams on their own.
     */
    static void spillSortExample() {
        List<Person> persons = new ArrayList<>();
        for (int i = 0; i < 100; i++)
            persons.add(new Person("Name" + (i % 5), "Family", i % 7, 1.75, 70));

        SpillCodec<Person> codec = new SpillCodec<Person>() {
            @Override
            public void write(DataOutput out, Person p) throws IOException {
                // null support is explicit: a one-byte tag marks the value
                out.writeBoolean(p != null);
                if (p == null)
                    return;
                out.writeUTF(p.getFirstName());
                out.writeUTF(p.getLastName());
                out.writeInt(p.getAge());
                out.writeDouble(p.getHeight());
                out.writeDouble(p.getWeight());
            }

            @Override
            public Person read(DataInput in) throws IOException {
                if (!in.readBoolean())
                    return null;
                return new Person(in.readUTF(), in.readUTF(), in.readInt(),
                                  in.readDouble(), in.readDouble());
            }
        };

        String base = System.getenv("TMPDIR");
        if (base == null || base.trim().isEmpty())
            base = System.getProperty("java.io.tmpdir");
        Path tempRoot = Paths.get(base);

        // At most 10 records in memory per initial run, 4-way merge rounds.
        try (Seq<Person> sorted = Seq.seq(persons)
                                     .sortedExternal(
                                         Comparator.comparingInt(Person::getAge),
                                         codec, tempRoot, 10, 4)) {
            sorted.limit(5)
                  .map(p -> p.getFirstName() + ":" + p.getAge())
                  .forEach(System.out::println);
        }
        catch (UncheckedIOException e) {
            // IO failures from the codec or the file system arrive wrapped,
            // with the original IOException retained as the cause.
            throw e;
        }
    }
}

class Person {

    private String firstName;
    private String lastName;
    private int    age;
    private double height;
    private double weight;

    public Person(String firstName, String lastName, int age, double height, double weight) {
        this.firstName = firstName;
        this.lastName = lastName;
        this.age = age;
        this.height = height;
        this.weight = weight;
    }

    public String getFirstName() {
        return firstName;
    }

    public String getLastName() {
        return lastName;
    }

    public int getAge() {
        return age;
    }

    public double getHeight() {
        return height;
    }

    public double getWeight() {
        return weight;
    }

}
