# Source provenance and synchronization

The integrated runtime was imported from LamaBuddy source revision
`050790dc62b0c121da70911652f108e049b40754`, subtree
`firmware/esp32-s3-hub`, on 2026-10-22. That source incorporated the public
companion's `423e0866756e0372d0d98520455d6ac09466578b` touchscreen baseline.
The complete runtime is included here; the source repository is not a build
dependency. The Hub contribution is published under this repository's
AGPL-3.0-or-later license. Existing upstream, font and icon notices remain intact.

Included: runtime C/headers, ESP-IDF manifests, normal board configurations and
partition tables, UI assets, simulator, synthetic tests, first-install import,
authenticated OTA and read-only diagnostics tools. Excluded: App/Watch projects,
private device configuration, credentials/certificates, hardware logs, binary
images, commercial planning and armed Secure Boot/eFuse provisioning tools.

Runtime files retain their imported behavior. Publication adaptations are the
public documentation, a complete offline-test entry point using the configured
ESP-IDF path, an explicit official ESP-IDF 5.5.4 build requirement for public
CI, and profile-specific
NVS generation (0x15000 for touchscreen, 0x6000 for headless). These tooling
changes do not alter the application running on the already-tested device.

The nested `integrated/` directory retains its original layout deliberately so
the deployed source, imports and upstream patches can be compared directly.
The root lightweight firmware remains independently buildable. Do not replace
its standalone networking/identity code with the Hub adapter without reviewing
its callers and tests.

Rust upstream was fetched and reviewed through
`ebaac65901fee0570ac26f9fda202c61f74ba055`. Both C job/coffee parsers include the
null/missing coffee-list and minimum-one-millisecond target fixes. Adjustment
window hints, desktop API names, saved-file takeover and stop signals remain
reviewed differences; see [the port matrix](../../docs/UPSTREAM.md).

For subsequent imports, review the source range, preserve all local credential,
deadline and no-replay guards, run both offline suites and both integrated
profiles, and update this source pin. A new public build has a new build ID;
the source build's one-cup result does not certify that new binary or all boards.
