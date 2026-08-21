# ADR 0033: Enforce the Godot C++ policy boundary

## Status

Accepted.

## Context

TickSynchronizer has two C++ compilation contexts. Module sources and tests are
compiled inside Godot and must follow the engine's restricted C++17 subset. The
standalone protocol benchmark does not link Godot and intentionally uses the C++
standard library to stay independent of engine initialization and runtime types.

Godot disables C++ exception handling and does not permit STL containers,
ordinary `auto` type inference, or avoidable lambdas in engine code. The module
did not contain exception handling or RTTI, but its protocol layer used
`std::array` for fixed wire fields and one local lambda. The standalone benchmark
used exception handling for command, allocation, and candidate failures.

## Decision

All module-linked C++ under `src/`, module tests, and registration files follow
the Godot policy:

- no C++ exception handling or RTTI;
- no STL containers;
- no `auto` type inference;
- no avoidable lambdas;
- explicit Godot error values and project result enums remain the normal failure
  contracts.

The fixed SHA-1, opaque identifier, and Godot version fields use the project-owned
`ProtocolFixedBytes` value type. It preserves the exact field sizes, indexing,
zero initialization, comparisons, and serialized bytes without an STL container.
The Godot version parser uses a named helper instead of a lambda.

The standalone benchmark remains independent of Godot and may use STL containers
and lambdas where they support portable datasets and measurement callbacks. It
must nevertheless be exception-free and RTTI-free:

- every SCons target compiles with `-fno-exceptions` and `-fno-rtti`;
- a forced compiler-contract header rejects builds that re-enable either feature;
- command parsing and candidate execution propagate explicit status values;
- nothrow allocation entry points return null on failure;
- allocation failure through the ordinary global allocation entry points
  terminates the benchmark because their language contract cannot return null.

`verify_source_consistency.sh` strips comments and literals before enforcing the
module and benchmark feature policies. The benchmark suite version remains 1 and
the wire revision remains 2 because datasets, timed regions, statistics,
correctness semantics, and serialized bytes do not change.

## Consequences

- The module can be reviewed and compiled under the same restricted C++ subset as
  Godot 4.7.1.
- Unsupported language features fail source consistency before a long engine
  build.
- Benchmark failures remain explicit without relying on compiler unwinding.
- The standalone benchmark keeps its engine-independent data model.
- Out-of-memory termination is deterministic and cannot be mistaken for a valid
  benchmark report.

## References

- [Godot C++ rules and guidelines](https://contributing.godotengine.org/en/latest/engine/guidelines/cpp_usage_guidelines.html)
