"""Offline tests of source digest checks, extraction and source selection."""
from __future__ import annotations

import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("setup_opus", ROOT / "tools" / "setup_opus.py")
OPUS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OPUS)


class SourcePreparationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="voice-opus-", dir=Path(tempfile.gettempdir()) / "kilo")
        self.addCleanup(self.temporary.cleanup)
        self.cache = Path(self.temporary.name)
        self.archive = self.cache / f"opus-{OPUS.VERSION}.tar.gz"

    def archive_with(self, name, kind=tarfile.REGTYPE):
        with tarfile.open(self.archive, "w:gz") as tar:
            member = tarfile.TarInfo(name)
            member.type = kind
            member.linkname = "outside"
            member.size = 4 if kind == tarfile.REGTYPE else 0
            tar.addfile(member, io.BytesIO(b"test") if member.size else None)
        return hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def test_official_pin(self):
        self.assertEqual(OPUS.VERSION, "1.5.2")
        self.assertEqual(OPUS.SHA256, "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1")
        self.assertIn("FIXED_POINT", OPUS.DEFINES)
        self.assertIn("DISABLE_FLOAT_API", OPUS.DEFINES)
        self.assertIn("VAR_ARRAYS", OPUS.DEFINES)

    def test_bad_cached_digest_fails_closed(self):
        self.archive.write_bytes(b"corrupted source")
        with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
            OPUS.prepare(self.cache)

    def test_bad_download_digest_is_not_cached(self):
        with mock.patch.object(OPUS.urllib.request, "urlopen", return_value=io.BytesIO(b"bad")):
            with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
                OPUS.prepare(self.cache)
        self.assertFalse(self.archive.exists())

    def test_cached_sources_are_authenticated_and_repaired(self):
        digest = self.archive_with(f"opus-{OPUS.VERSION}/source.c")
        with mock.patch.object(OPUS, "SHA256", digest), mock.patch.object(OPUS.urllib.request, "urlopen") as network:
            root = OPUS.prepare(self.cache)
            self.assertEqual((root / "source.c").read_bytes(), b"test")
            (root / "source.c").write_bytes(b"tampered")
            OPUS.prepare(self.cache)
            self.assertEqual((root / "source.c").read_bytes(), b"test")
            network.assert_not_called()

    def test_unsafe_archive_members_are_rejected(self):
        for name, kind in (
            ("../escape", tarfile.REGTYPE),
            (f"opus-{OPUS.VERSION}/../../escape", tarfile.REGTYPE),
            ("/absolute", tarfile.REGTYPE),
            (f"opus-{OPUS.VERSION}/x\\..\\escape", tarfile.REGTYPE),
            (f"opus-{OPUS.VERSION}/link", tarfile.SYMTYPE),
            (f"opus-{OPUS.VERSION}/link", tarfile.LNKTYPE),
        ):
            with self.subTest(name=name, kind=kind):
                digest = self.archive_with(name, kind)
                with mock.patch.object(OPUS, "SHA256", digest):
                    with self.assertRaisesRegex(RuntimeError, "Unsafe Opus archive member"):
                        OPUS.prepare(self.cache)

    def test_source_selection_uses_only_generic_fixed_groups(self):
        manifests = {
            "opus_sources.mk": "OPUS_SOURCES = a.c " + "\\" + "\nb.c\nOPUS_SOURCES_FLOAT = float.c\n",
            "celt_sources.mk": "CELT_SOURCES = c.c\nCELT_SOURCES_AVX2 = avx.c\n",
            "silk_sources.mk": "SILK_SOURCES = d.c\nSILK_SOURCES_FIXED = e.c\nSILK_SOURCES_FLOAT = f.c\n",
        }
        for name, text in manifests.items():
            (self.cache / name).write_text(text)
        for name in ("a.c", "b.c", "c.c", "d.c", "e.c"):
            (self.cache / name).touch()
        self.assertEqual(OPUS.sources(self.cache), ["a.c", "b.c", "c.c", "d.c", "e.c"])
        (self.cache / "e.c").unlink()
        with self.assertRaisesRegex(RuntimeError, "Incomplete Opus source list"):
            OPUS.sources(self.cache)


if __name__ == "__main__":
    unittest.main(verbosity=2)
