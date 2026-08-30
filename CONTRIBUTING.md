# Contributing

Keep the shell small, native, and predictable.

- Prefer public Windows APIs.
- Do not add a WebView dependency to the permanent overlay without a strong architectural reason.
- Provider code publishes `Activity` values; it should not draw UI directly.
- Third-party extension code stays out-of-process.
- Avoid permanent high-frequency polling when an event-driven API exists.
- New animations must respect the geometry/content staging described in `docs/DESIGN.md`.
- Run a Release build for x64 before opening a PR; ARM64 changes should also compile when the toolchain is available.
