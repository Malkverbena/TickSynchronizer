# ADR 0035: Separate repository, Project, and Wiki responsibilities

## Status

Accepted.

## Context

TickSynchronizer needs three different kinds of project information:

- technical contracts and evidence that must match a specific source state;
- operational planning that changes more frequently than the source;
- user-facing guidance that benefits from Wiki navigation and incremental
  presentation.

Keeping assignments, priorities, and board status in the module manual would
create noisy source changes and duplicate GitHub planning state. Moving
normative or version-bound documentation out of the repository, however, would
make a checkout insufficient to explain its own architecture, wire contract,
build requirements, and accepted validation evidence.

The current benchmark qualification is also incomplete. Publishing provisional
results as general Wiki guidance before the current acceptance and final
portability gates close could make diagnostic evidence appear authoritative.

## Decision

The versioned repository remains authoritative for:

- architecture, protocol, compatibility, build, testing, security, and
  benchmark methodology contracts;
- benchmark decisions and architecture decision records;
- source-specific project state and validation evidence;
- the strategic phase sequence and acceptance gates in the development
  roadmap.

The GitHub Project is the canonical operational planning surface. It owns the
actionable backlog, status, priority, assignment, scheduling, target milestone,
and Kanban or timeline views. The versioned roadmap may describe phases,
dependencies, and gates, but must not duplicate item-level Project state.

GitHub Wiki publication begins only after the current source acceptance gate
and the complete final portability matrix in ADR 0036 are complete. That matrix
contains 18 matched candidate pairs, or 36 reports. Wiki content is a derived
publication for navigation, getting-started material, tutorials, platform
walkthroughs, and frequently asked questions. A version-sensitive Wiki page
must identify the source release or commit it describes and link to the
corresponding versioned document.

Normative documents and ADRs remain in the repository even when a derived Wiki
page presents the same subject. The Wiki must never be the sole source of truth.
If Wiki text conflicts with a versioned repository document, the repository
document governs.

Public Project and Wiki content follows the repository privacy policy. It must
not expose credentials, private local paths, host names, operator identity,
serial identifiers, or names and hashes of unpublished qualification artifacts.
Raw private inventories and diagnostic logs are not copied into either surface.

## Consequences

- Every checkout remains self-describing and reviewable without GitHub Project
  or Wiki availability.
- Operational status can change without creating documentation-only commits.
- The strategic roadmap stays stable while the Project exposes current work.
- Wiki readers receive approachable guidance without weakening source-to-doc
  traceability.
- Wiki publication requires an explicit derivation and maintenance step after
  the current qualification gate closes.
- Links to the specific GitHub Project and Wiki may be added after those
  surfaces exist without changing this responsibility split.
