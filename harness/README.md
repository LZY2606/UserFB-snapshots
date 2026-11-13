Offline parser harness: JDK 11 or newer, javac --release 8; no Maven, services, or downloaded jars.

baseline.sh compiles production sources and two original upstream test classes. The small local org.junit package implements only their annotation/assertion surface; it is not a JUnit distribution or a substitute for the full suite. LegacyCases invokes the 13 explicitly selected original methods and rejects ignored/missing methods. Original source and fixtures are unchanged.

StartingPoint records this revision's missing template support, so it is a starting-point check and is not run by target.sh after the feature changes.

target.sh requires the new default-package contract/TemplateParsingTest.java. Public static void methods marked @TemplateHarness.Case(group="modes"), "boundaries", or "fragments" are invoked by the fixed driver. Each group must execute, each method must use TemplateHarness.expect/equal at least once, all must return successfully, then the original parser methods run. No skip mechanism is provided.

Only HTML template tree construction is in scope. No browser, scripting, separate DocumentFragment object, shadow DOM, or online specification lookup is required at runtime.
