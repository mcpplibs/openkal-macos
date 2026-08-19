# openkal-macos

An implementation of [openkal](https://github.com/mcpplibs/openkal) for macOS.

Its purpose is as much to test the specification as to be used. A specification
satisfied only by the system it was written against has not been shown to be
portable, however many programs that system hosts; a second implementation on a
different system is what turns the claim into an observation.

## Interfaces provided

All eight, as the reference implementation does. The package exports no module:
the interface belongs to the specification, and this package supplies
definitions.

## Where this system differs, and what each difference demonstrates

These are recorded because they are the return on writing a second
implementation. Each is a place where an interface could have assumed a
mechanism, and did not.

**The monotonic clock continues during suspension.** On the reference
implementation it stops. A program measuring an interval across a suspension
obtains different answers from the two, and no operation reports which it is
dealing with. The property `prop_monotonic_suspends` exists for this, and this
divergence is the clearest evidence in either implementation that the properties
of clause 6.2 were necessary rather than decorative.

**Names are compared without regard to case.** A program that creates two names
differing only in case succeeds on one implementation and not on the other.
`prop_case_sensitive` reports it in advance, which no operation could.

**The spawn has no action that sets the working directory.** The reference
implementation passes the directory as an attribute of the operation; this one
applies it around the operation, which is correct for a single-context program
and is not correct in general. The divergence is recorded in the source rather
than concealed, because an interface one platform honours as an attribute and
another can only approximate is an interface that has assumed a mechanism.

**There is no suspension primitive a program may use.** `openkal.task` declares
the boundary as a wait upon a word, because on a system that provides it the
synchronisation objects of a C library are built from it. Here the relation is
inverted: the primitive is built from a mutex and a condition variable. This is
the one place in the implementation that resembles a compatibility layer, and it
is admitted because the alternatives are worse in both directions — an interface
offering mutexes would oblige an implementation whose environment has none to
construct them, and every C library above openkal would then be built upon a
facility it does not need.

## Conformance

`mcpp test` runs the suite, which is the same suite the reference
implementation runs. That it is the same suite is the point: a conformance
suite that differed between implementations would be testing implementations
rather than the specification.

## License

Apache-2.0.
