# Dynamic constant (condy) symbol output

CFR reads `CONSTANT_Dynamic` ("condy") constant pool entries without loading the target
classes, executing bootstrap methods, or guessing at runtime values.  A constant that can be
statically reduced by a *known* bootstrap (`java.lang.invoke.ConstantBootstraps`) is still
reduced to a plain literal (e.g. `int.class`).  Every other condy use site is shown as a typed
symbolic reference, and the class gets a legend describing each symbol.

## Use sites

Each unreduced constant read (`ldc`, `ldc_w`, `ldc2_w`) or bootstrap argument is printed as

```java
return Condy.#37;
```

`Condy.#37` is pseudocode: it names entry `#37` in the class's own legend (the raw constant
pool index).  The identity of a symbol is the owning class plus that raw index — two entries
with identical name, descriptor and bootstrap are still two different symbols, and the same
entry read from many places is always the same symbol.

The reference is typed from the constant's field descriptor (the `NameAndType`), **not** from
the bootstrap method's declared return type.  A constant described as `J` stays `long`, `D`
stays `double`, and `[I` stays `int[]`, even if the bootstrap declares `Object`:

```java
public static double pi() {
    return Condy.#49;
}
```

## Class legend

Above the class, CFR emits a comment listing every dynamic constant in raw constant pool
index order:

```
/*
 * Uses jvm11+ dynamic constants - pseudocode provided - see https://www.benf.org/other/cfr/dynamic-constants.html
 * Dynamic constant symbols (pseudocode - bootstrap methods are not executed):
 * #37 "number" : J <- INVOKE_STATIC CondySample.leaf (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/Object; args=[]
 * #45 "pair" : J <- INVOKE_STATIC CondySample.outer (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/Class;JJ)Ljava/lang/Object; args=[#37, #37]
 * #63 "bag" : J <- INVOKE_STATIC CondySample.gather (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/Class;[Ljava/lang/Object;)Ljava/lang/Object; args=[#37, "text"]
 */
```

Each line records the constant's name, its field descriptor, and the bootstrap handle's kind,
owner, member name and full descriptor, followed by the ordered static arguments.  Nested
condy arguments refer back to their own legend entries (`#37`), so shared subgraphs are
printed once and referenced — never copied into larger and larger expression trees.  A child
appearing in two argument positions (`args=[#37, #37]`) appears in both.

## Rewrites

* Known `ConstantBootstraps` reductions (e.g. `primitiveClass`) keep working and produce
  ordinary literals.
* `StringConcatFactory` rewrites only proceed when the recipe is genuinely known.  An unknown
  (dynamic constant) recipe keeps the invocation and its original argument order:

```java
public static String label(long l) {
    return StringConcatFactory.makeConcatWithConstants("makeConcatWithConstants", new Object[]{Condy.#57}, (long)l);
}
```

* Ordinary constants, lambdas and classes without condy are unaffected.

See `CondyExample.java` for a small example of the source-level shapes involved, and
`SyntheticCondy.java` for the benign generator that produces the class file above.
