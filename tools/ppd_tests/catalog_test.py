"""Offline regression for the startup collector overflow; no game or GPU needed."""
import copy
import gzip
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gen_vrcam_assets", REPO / "tools/gen_vrcam_assets.py")
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)
FIXTURES = Path(__file__).resolve().parent / "fixtures"
MALE = FIXTURES / "player_ma_fpp.ent.json.gz"
FEMALE_EP1 = FIXTURES / "player_wa_fpp_ep1.ent.json.gz"
FINE = [(w, ((w * 2296 + 2064 * 4) // (2064 * 8)) * 8) for w in range(768, 6001, 8)]


class CatalogSafetyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        GEN._CRUID_TABLE.clear()

    def entity_copy(self, source=MALE):
        path = self.root / source.stem
        path.write_bytes(gzip.decompress(source.read_bytes()))
        return path

    def seed_ids(self, path, wanted):
        named, used = GEN.scan_existing_cruids([str(path)])
        GEN.build_cruid_table(wanted, named, used)

    def test_fine_ladder_refused_before_mutating_original_entity(self):
        # The old production generator writes 824 components here, causing the
        # captured engine collector overflow. Rejection must precede any write.
        path = self.entity_copy()
        before = path.read_bytes()
        self.seed_ids(path, FINE)
        with self.assertRaisesRegex(ValueError, "256"):
            GEN.process_ent(str(path), FINE, False)
        self.assertEqual(path.read_bytes(), before)
        self.assertFalse(Path(str(path) + ".orig").exists())

    def test_dry_run_also_refuses_unsafe_catalog(self):
        path = self.entity_copy()
        self.seed_ids(path, FINE)
        with self.assertRaisesRegex(ValueError, "256"):
            GEN.process_ent(str(path), FINE, True)

    def test_capacity_accounts_for_non_camera_components(self):
        # The largest original entity has 190 total components, not just its
        # 62 cameras. 66 new dimensions fit; a 67th must be refused.
        path = self.entity_copy(FEMALE_EP1)
        wanted = [(w, w) for w in range(800, 1336, 8)]
        self.seed_ids(path, wanted)
        GEN.process_ent(str(path), wanted[:66], False)
        data, _ = GEN.load_json(str(path))
        self.assertEqual(len(data["Data"]["RootChunk"]["components"]), 256)
        before = path.read_bytes()
        with self.assertRaisesRegex(ValueError, "256"):
            GEN.process_ent(str(path), wanted, False)
        self.assertEqual(path.read_bytes(), before)

    def test_selected_size_preserves_original_components_and_bindings(self):
        path = self.entity_copy(FEMALE_EP1)
        selected = [(2312, 2576)]
        self.seed_ids(path, selected)
        original, _ = GEN.load_json(str(path))
        added, _, _, _ = GEN.process_ent(str(path), selected, False)
        self.assertEqual(added, ["vrcam_2312x2576"])
        after, _ = GEN.load_json(str(path))
        base = original["Data"]["RootChunk"]
        root = after["Data"]["RootChunk"]
        self.assertEqual(len(root["components"]), 191)
        self.assertEqual(root["components"][:-1], base["components"])
        self.assertEqual(root["compiledData"]["Data"]["Chunks"][:-1], base["compiledData"]["Data"]["Chunks"])
        self.assertEqual(GEN.verify_ent(str(path), selected), ([], 63))
        cam = root["components"][-1]
        self.assertEqual((cam["resolutionWidth"], cam["resolutionHeight"]), (2312, 2576))
        self.assertEqual(cam["isEnabled"], 0)
        self.assertEqual(cam["virtualCameraName"]["$value"], "vrcam_feed_2312x2576")
        before = path.read_bytes()
        self.assertEqual(GEN.process_ent(str(path), selected, False)[0], [])
        self.assertEqual(path.read_bytes(), before)

    def test_verifier_rejects_preexisting_oversized_entity(self):
        path = self.entity_copy()
        data, style = GEN.load_json(str(path))
        root = data["Data"]["RootChunk"]
        # Preserve CR2W chunk/CRUID structure while building a captured-defect
        # sized input independently of the production guard being tested.
        for i in range(645):
            cam = copy.deepcopy(root["components"][-1])
            cam["name"]["$value"] = "vrcam_extra_%d" % i
            cam["id"] = str(900000 + i)
            root["components"].append(cam)
            root["compiledData"]["Data"]["Chunks"].append(copy.deepcopy(cam))
            root["compiledData"]["Data"]["CruidDict"][str(len(root["components"]))] = cam["id"]
        GEN.save_json(str(path), data, style)
        problems, _ = GEN.verify_ent(str(path), [])
        self.assertTrue(any("256" in p for p in problems), problems)

    def test_duplicate_requests_cannot_bypass_capacity_guard(self):
        path = self.entity_copy()
        wanted = [(2312, 2576)] * 300
        self.seed_ids(path, wanted)
        GEN.process_ent(str(path), wanted, False)
        data, _ = GEN.load_json(str(path))
        self.assertEqual(len(data["Data"]["RootChunk"]["components"]), 180)

    def project_copy(self):
        # Exercise the command with actual normalized entity structures.
        # The two fixtures cover both ends of the original component counts;
        # real eight-entity binary roundtrip is a separate archive acceptance.
        project = self.root / "project"
        for rel in GEN.ENT_FILES + GEN.REPLACER_FILES:
            source = FEMALE_EP1 if rel == GEN.ENT_FILES[3] else MALE
            data = json.loads(gzip.decompress(source.read_bytes()))
            if rel in GEN.REPLACER_FILES:
                root = data["Data"]["RootChunk"]
                for cam in root["components"] + root["compiledData"]["Data"]["Chunks"]:
                    name = GEN.component_name(cam)
                    if isinstance(name, str) and name.startswith("vrcam_"):
                        cam["name"]["$value"] = name.replace("vrcam_", GEN.REPLACER_PREFIX, 1)
            path = project / "source/raw" / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(data), encoding="utf-8")
        catalog = self.root / "vrcam.json"
        shutil.copy2(REPO / "mods/cet/CyberpunkVRPort_Stereo/vrcam.json", catalog)
        return project, catalog

    def generate(self, project, catalog, *args):
        return subprocess.run([sys.executable, str(REPO / "tools/gen_vrcam_assets.py"),
                               "--project", str(project), "--catalog", str(catalog), *args],
                              text=True, capture_output=True)

    def test_selected_cli_catalog_matches_one_new_resource_on_all_entities(self):
        project, catalog = self.project_copy()
        original = json.loads(catalog.read_text("utf-8-sig"))
        result = self.generate(project, catalog, "--selected-resolution", "2312x2576")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        cfg = json.loads(catalog.read_text("utf-8-sig"))
        self.assertEqual(set(cfg["components"]) - set(original["components"]), {"vrcam_2312x2576"})
        self.assertEqual(len(cfg["components"]), 63)
        self.assertEqual(cfg["component"], "vrcam_2312x2576")
        self.assertEqual(cfg["virtualCamera"], "vrcam_feed_2312x2576")
        for rel in GEN.ENT_FILES + GEN.REPLACER_FILES:
            prefix = GEN.REPLACER_PREFIX if rel in GEN.REPLACER_FILES else "vrcam_"
            problems, count = GEN.verify_ent(str(project / "source/raw" / rel), [(2312, 2576)], prefix)
            self.assertEqual((problems, count), ([], 63), rel)
        tex = json.loads((project / "source/raw" / GEN.DTEX_DIR / "texture_from_camera_2312x2576.dtex.json").read_text())
        self.assertEqual((tex["Data"]["RootChunk"]["width"], tex["Data"]["RootChunk"]["height"]), (2312, 2576))

    def test_selected_cli_requires_every_player_variant_before_writing(self):
        project, catalog = self.project_copy()
        (project / "source/raw" / GEN.REPLACER_FILES[-1]).unlink()
        first = project / "source/raw" / GEN.ENT_FILES[0]
        before = (first.read_bytes(), catalog.read_bytes())
        result = self.generate(project, catalog, "--selected-resolution", "2312x2576")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing", result.stderr.lower())
        self.assertEqual((first.read_bytes(), catalog.read_bytes()), before)

    def test_selected_cli_preflights_last_entity_before_writing_first(self):
        project, catalog = self.project_copy()
        last = project / "source/raw" / GEN.REPLACER_FILES[-1]
        data = json.loads(last.read_text())
        root = data["Data"]["RootChunk"]
        for i in range(80):
            cam = copy.deepcopy(root["components"][-1])
            cam["name"]["$value"] = "unrelated_%d" % i
            cam["id"] = str(990000 + i)
            root["components"].append(cam)
            root["compiledData"]["Data"]["Chunks"].append(copy.deepcopy(cam))
            root["compiledData"]["Data"]["CruidDict"][str(len(root["components"]))] = cam["id"]
        last.write_text(json.dumps(data))
        first = project / "source/raw" / GEN.ENT_FILES[0]
        before = (first.read_bytes(), catalog.read_bytes())
        result = self.generate(project, catalog, "--selected-resolution", "2312x2576")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("256", result.stderr)
        self.assertEqual((first.read_bytes(), catalog.read_bytes()), before)

    def test_ordinary_catalog_retains_existing_selection_and_camera_set(self):
        project, catalog = self.project_copy()
        original = json.loads(catalog.read_text("utf-8-sig"))
        result = self.generate(project, catalog)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        cfg = json.loads(catalog.read_text("utf-8-sig"))
        self.assertEqual(set(cfg["components"]), set(original["components"]))
        self.assertEqual(cfg["component"], original["component"])
        self.assertEqual(cfg["virtualCamera"], original["virtualCamera"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
