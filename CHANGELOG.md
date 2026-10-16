# Changelog

## 0.2.0 - 2026-10-16

- Review the bridge against PR15 `8f398c5` and PR16 `ba00c92`, retaining immediate
  task acceptance and reacceptance while waiting. Reconcile a lost accept reply
  from the next job list and stop a result when its task is no longer available.
- Track grinder and cloud failures independently, clear current errors after
  recovery and retain the last failure/recovery details for the current boot.
- Bound ordinary reconnect waits to five seconds for grinders switched off daily;
  show an explicit pairing problem after three refusals without erasing identity.
- Add accessible colored state badges, localized explanations, delivered vs cloud
  measurements, timestamps, timeout countdown and connection diagnostics.
- Stop showing cached connections as live when the page cannot reach the S3.
- Test the real worker with in-memory services, alongside protocol, provision,
  serial and eight-language UI tests. Preserve the NVS layout and task ledger.

## 0.1.0 - 2026-10-16

- Split the experimental ESP32-S3 firmware from gbs-anywhere PR18 into a
  separately maintained companion repository, preserving source attribution
  and the AGPL-3.0-or-later license.
- Check local ports 80 and 8089 before activation; a custom desktop port adds
  another check. Refuse activation on an open port or an unknown check result
  before opening USB.
- Document private Docker settings/pairing exports and the need to save the
  La Marzocco integration in the desktop app before migrating.
- Add port-ownership and saved-integration regression tests plus ESP-IDF CI.
- Clarify precursor real-cup results and the portable build's pending hardware
  validation. No hardware-validated firmware release is published yet.
