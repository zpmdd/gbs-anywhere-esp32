import importlib.util
from pathlib import Path
import unittest
import argparse
from datetime import datetime
import contextlib
import io
import json
import tempfile
from unittest.mock import patch
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

spec = importlib.util.spec_from_file_location("provision", Path(__file__).resolve().parents[1] / "tools/provision.py")
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)


class ProvisionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        key = ec.generate_private_key(ec.SECP256R1())
        cls.hub_id = "22222222-2222-4222-8222-222222222222"
        for attribute, common_name in (("certificate", "synthetic-grinder"),
                                      ("hub_certificate", "LamaBuddy Hub " + cls.hub_id),
                                      ("forged_certificate", "LamaBuddy Hub " + cls.hub_id)):
            name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])
            signer = ec.generate_private_key(ec.SECP256R1()) if attribute == "forged_certificate" else key
            cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                    .public_key(key.public_key()).serial_number(1)
                    .not_valid_before(datetime(2025, 1, 1))
                    .not_valid_after(datetime(2035, 1, 1)).sign(signer, hashes.SHA256()))
            setattr(cls, attribute, cert.public_bytes(serialization.Encoding.PEM).decode())

    def hub_config(self, **overrides):
        source = {"url": "https://hub.example.test:443", "token": "synthetic-hub-token",
                  "hub_id": self.hub_id, "serial": "SYNTHETIC-MACHINE", "certificate": self.hub_certificate}
        source.update(overrides)
        return source

    def test_hub_trust_record_rejects_unsafe_or_mismatched_inputs(self):
        for url in ("http://hub.example.test", "https://user:pass@hub.example.test", "https://hub.example.test/path",
                    "https://@hub.example.test", "https://hub.example.test:",
                    "https://hub.example.test?token=secret", "https://hub.example.test#x", "https://[::1]",
                    "https://hub.example.test:0", "https://hub.example.test:65536", "https://hub.example.test\n"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                provision.build_hub_config(self.hub_config(url=url))
        for fields in ({"token": "short"}, {"token": "x" * 129}, {"token": "token\r\nOther-Header:value"},
                       {"serial": ""}, {"serial": "x" * 64}, {"hub_id": "not-a-uuid"},
                       {"certificate": self.certificate}, {"certificate": self.forged_certificate},
                       {"certificate": self.hub_certificate * 2},
                       {"certificate": self.hub_certificate + "-----BEGIN PRIVATE KEY-----"}):
            with self.subTest(field=next(iter(fields))), self.assertRaises(ValueError):
                provision.build_hub_config(self.hub_config(**fields))

    def test_hub_record_normalizes_to_firmware_format(self):
        config = provision.build_hub_config(self.hub_config(
            url="HTTPS://hub.example.test:443/", certificate=" \n" + self.hub_certificate + "\n"))
        self.assertEqual(config["url"], "https://hub.example.test:443")
        self.assertEqual(config["certificate"], self.hub_certificate)

    def config(self, **overrides):
        args = dict(application={"integrations": [{"kind": "la_marzocco", "settings": {
            "username": "rick@example.org", "password": "hunter2", "serial": ""}}]},
            pairing={"pairing_id": 42, "brew_device_id": "11111111-1111-4111-8111-111111111111"},
            grinder="192.168.1.20", certificate=self.certificate,
            ssid="Example WiFi", wifi_password="example-password")
        args.update(overrides)
        return provision.build_config(**args)

    def test_import_preserves_identity_and_certificate(self):
        c = self.config()
        self.assertEqual(c["pairing_id"], 42)
        self.assertEqual(c["brew_device_id"], "11111111-1111-4111-8111-111111111111")
        self.assertEqual(c["grinder_cert"], self.certificate)
        self.assertEqual(c["installation_id"], "1f7adb07-de9a-4453-941f-1b054dd04a7a")

    def test_bad_inputs_are_rejected_before_nvs_generation(self):
        for pairing_id in (None, True, 0, -1, 65536, 1.5):
            with self.subTest(pairing_id=pairing_id), self.assertRaises(ValueError):
                self.config(pairing={"pairing_id": pairing_id})
        for host in ("example.org", "192.168.1.20/path", "::1"):
            with self.assertRaises(ValueError): self.config(grinder=host)
        for cert in ("not a certificate", self.certificate * 2,
                     self.certificate + "-----BEGIN PRIVATE KEY-----"):
            with self.assertRaises(ValueError): self.config(certificate=cert)
        for password in ("", "x" * 256, "secret\0tail"):
            with self.assertRaises(ValueError):
                self.config(application={"integrations": [{"kind": "la_marzocco", "settings": {
                    "username": "rick@example.org", "password": password}}]})

    def test_export_requires_exactly_one_saved_cloud_integration(self):
        cloud = {"kind": "la_marzocco", "settings": {
            "username": "rick@example.org", "password": "hunter2"}}
        for entries in ([], [{"kind": "half_decent", "settings": {}}], [cloud, cloud]):
            with self.subTest(entries=len(entries)), self.assertRaisesRegex(
                    ValueError, "Exactly one La Marzocco integration is required"):
                self.config(application={"integrations": entries})

    def test_unconfirmed_refused_pairing_requires_desktop_recovery(self):
        pairing = {"pairing_id": None, "refused_pairing_id": 42,
                   "brew_device_id": "11111111-1111-4111-8111-111111111111"}
        with self.assertRaisesRegex(ValueError, "Finish pairing in gbs-anywhere"):
            self.config(pairing=pairing)
        pairing["pairing_id"] = 7
        self.assertEqual(self.config(pairing=pairing)["pairing_id"], 7)

    def test_creates_private_nvs_image_in_monitor_mode(self):
        for include_hub in (False, True):
            with self.subTest(include_hub=include_hub), tempfile.TemporaryDirectory() as folder:
                self.check_private_nvs(Path(folder), include_hub)

    def check_private_nvs(self, root, include_hub):
        (root / "config.json").write_text(json.dumps({
            "wifi_ssid": "Example WiFi", "wifi_password": "example-password"}))
        cloud = root / "cloud.json"
        cloud.write_text(json.dumps({"integrations": [{"kind": "la_marzocco", "settings": {
            "username": "rick@example.org", "password": "hunter2"}}]}))
        pairing = root / "pairing.json"
        pairing.write_text(json.dumps({"pairing_id": 42,
            "brew_device_id": "11111111-1111-4111-8111-111111111111"}))
        cert = root / "cert.pem"; cert.write_text(self.certificate)
        hub_file = root / "hub-import.json"; hub_file.write_text(json.dumps(self.hub_config()))
        args = argparse.Namespace(reuse_wifi=True, cloud_config=cloud, pairing_file=pairing,
                                  grinder="192.168.1.20", grinder_cert=cert,
                                  hub_config=hub_file if include_hub else None)
        output = io.StringIO()
        with patch.object(provision, "PRIVATE", root), contextlib.redirect_stdout(output):
            provision.prepare(args)
        self.assertNotIn(self.hub_config()["token"], output.getvalue())
        self.assertEqual((root / "config-nvs.bin").stat().st_size, 0x16000)
        self.assertIn("active,data,u8,0", (root / "config.csv").read_text())
        self.assertEqual((root / "config.json").stat().st_mode & 0o777, 0o600)
        self.assertEqual((root / "config-nvs.bin").stat().st_mode & 0o777, 0o600)
        config = json.loads((root / "config.json").read_text())
        self.assertEqual(config["installation_id"], self.config()["installation_id"])
        self.assertEqual(config["pairing_id"], 42)
        if include_hub:
            self.assertEqual(config["serial"], self.hub_config()["serial"])
            self.assertIn("gbs_hub,namespace", (root / "config.csv").read_text())
            self.assertEqual((root / "hub-config.json").stat().st_mode & 0o777, 0o600)
            self.assertEqual(json.loads((root / "hub-config.json").read_text()), self.hub_config())
        else:
            self.assertEqual(config["serial"], "")
            self.assertNotIn("gbs_hub,namespace", (root / "config.csv").read_text())
        return args

    def test_hub_machine_mismatch_preserves_existing_config(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args = self.check_private_nvs(root, True)
            before = (root / "config.json").read_bytes()
            cloud = json.loads(args.cloud_config.read_text())
            cloud["integrations"][0]["settings"]["serial"] = "ANOTHER-MACHINE"
            args.cloud_config.write_text(json.dumps(cloud))
            with patch.object(provision, "PRIVATE", root), patch.object(provision.subprocess, "run") as generate:
                with self.assertRaisesRegex(ValueError, "same coffee machine"):
                    provision.prepare(args)
                generate.assert_not_called()
            self.assertEqual((root / "config.json").read_bytes(), before)

    def test_matches_upstream_installation_vector(self):
        key = provision.installation("rick@example.org", "hunter2")
        self.assertEqual(key["installation_id"], "1f7adb07-de9a-4453-941f-1b054dd04a7a")
        self.assertEqual(key["public_key"], "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEVCFLIfS/hQzZ3TwM1pAwaHVBw5YvJzV4GZbaB5BaViDxBYR+mIzSXprUaOhqPaiBXBfb5CovzvMom1//AGD4PA==")
        self.assertEqual(key["secret"], "nKkcrK3K9+vUSI1k4VD/FTWNPZfbRrM+E0DFDogvf4E=")
        self.assertEqual(key, provision.installation(" Rick@Example.org ", "hunter2"))

    def test_proof_vector(self):
        self.assertEqual(provision.proof("abc", bytes(32)), "P5ynD3LZU7oRYIN0D2ZtOlbpepJdBXQH9vdqJGOfq/o=")

    def test_wifi_byte_limits(self):
        provision.validate_wifi("home", "password")
        for ssid, password in [("", "password"), ("家" * 11, "password"), ("home", "short"), ("home", "x" * 64)]:
            with self.assertRaises(ValueError):
                provision.validate_wifi(ssid, password)


if __name__ == "__main__":
    unittest.main()
