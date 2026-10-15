# Working on this companion

- Keep this a standalone ESP-IDF application. The upstream Rust application is
  maintained at https://github.com/Skamba/gbs-anywhere.
- Read README.md and NOTICE before changing protocol or provisioning behavior.
  Recheck the C port when the upstream E65 support is merged and released.
- Use branches for development. Run `sh tools/check.sh`, `idf.py build` and
  `git diff --check` before publishing a change. CI covers offline tests and
  the firmware build; it does not prove hardware behavior.
- Keep the single active client rule. Tests use synthetic data and must not
  contact a grinder or vendor cloud. Flashing, pairing or real-shot testing
  requires the user's explicit hardware scope; never infer it from a build.
- Keep credentials, certificates, NVS images, device identifiers and raw device
  logs out of Git. Do not add local network or machine details to public text.
- Write public code, documentation, commits and comments in English. Preserve
  AGPL-3.0-or-later attribution and the vendor non-affiliation statement.
- Report exact-build hardware validation separately from precursor results.
  Keep the firmware experimental until its hardware gates have passed.
