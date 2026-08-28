# Privacy and publication policy

## Scope

TickSynchronizer's public repository, releases, GitHub Project, Wiki, source
packages, documentation, and published validation summaries must not expose:

- credentials, access tokens, private keys, or authenticated URLs;
- private local paths, mounted-volume paths, or drive-qualified paths;
- host names, user names, or operator identity;
- Android serial identifiers or other device-unique identifiers;
- names or hashes of unpublished qualification artifacts.

Public evidence may identify operating systems, CPU architectures, public
device models, compiler families, module commits, qualified public Godot
commits, aggregate measurements, and privacy-safe executable basenames when
those fields are necessary to reproduce a technical conclusion.

## Generated local reports

Build reports are local diagnostic artifacts. They may contain absolute source
paths and general host configuration, so they must be reviewed and sanitized
before publication. Git remote URLs are deliberately excluded because they may
contain private hosts, account names, or embedded credentials.

Benchmark reports and execution packages use the stricter policy enforced by
the benchmark validators: compiler and executable provenance must use
privacy-safe identifiers, and host-specific path or serial leakage is fatal.
Generated benchmark binaries, result archives, and deployment packages remain
private qualification artifacts unless a later publication policy explicitly
authorizes them.

## Review checklist

Before publishing a repository change or derived report:

1. search tracked text for private path prefixes, drive-qualified paths,
   credentials, authenticated URLs, host names, user names, and serials;
2. confirm that archive names and hashes refer only to published source
   artifacts or public commits;
3. use aggregate conclusions instead of raw private execution logs;
4. keep qualification archives outside Git and public releases;
5. run `scripts/verify_source_consistency.sh` and the relevant benchmark
   privacy validators;
6. inspect the final staged diff and commit inventory before publication.

Private handoff packages may carry authenticated private evidence when needed
for continuity, but they are not public project artifacts and must retain
restricted handling.
