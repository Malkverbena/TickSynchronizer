# Development records

This directory contains version-bound project state, planning, and validation
evidence. It does not define TickSynchronizer behavior or public contracts.

- [`PROJECT_STATE.md`](PROJECT_STATE.md): implemented and pending source state;
- [`ROADMAP.md`](ROADMAP.md): phased development direction;
- [`VALIDATION.md`](VALIDATION.md): accepted results for the current source state.

Responsibility is deliberately split:

| Surface | Authoritative content |
|---|---|
| Versioned development records | Current capabilities, accepted evidence, strategic phases, and gates |
| GitHub Project | Actionable backlog, status, priority, assignment, scheduling, milestones, and operational views |
| GitHub Wiki after qualification | Derived navigation, tutorials, platform walkthroughs, and frequently asked questions |

The GitHub Project must not duplicate technical contracts or raw validation
logs. The Wiki never replaces versioned normative documents or ADRs. See ADR
0035 for the complete policy.
