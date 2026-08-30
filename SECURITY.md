# Security policy

## Plugin warning

Isle v1 plugins run as ordinary processes under the current user account. The out-of-process design protects the Isle renderer from memory corruption and crashes, but it does **not** sandbox plugin filesystem, network, registry, or process access.

Only install plugins whose source or publisher you trust.

Isle does not auto-download third-party plugins and the host never loads third-party plugin DLLs into its own process.

## Reporting

For a public repository, use GitHub private vulnerability reporting if enabled. Do not include private tokens, credentials, or personal files in an issue.
