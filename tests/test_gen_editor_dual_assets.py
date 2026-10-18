"""Synthetic offline contracts for the same-owner editor RTT asset candidate."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
GENERATOR = REPO / "tools/gen_editor_dual_assets.py"
ENTITY_RESOURCE = r"base\gameplay\gui\fullscreen\main_menu\target_face.ent"
TEXTURE_RESOURCE = r"base\gameplay\gui\fullscreen\main_menu\preview.dtex"
RIGHT_TEXTURE = r"cpvr\editor\preview_right.dtex"


def cname(value):
    return {"$type": "CName", "$storage": "string", "$value": value}


def resource(value):
    return {"DepotPath": {"$type": "ResourcePath", "$storage": "string", "$value": value},
            "Flags": "Default"}


def fixtures():
    """Authored synthetic values, not a copied vanilla asset fixture."""
    owner = {"$type": "gameuiEntityPreviewGameObject", "cameraSettings": {"autoEnable": 0},
             "syntheticOwnerField": [2, 9, "preserve"]}
    areas = [{"HandleId": str(i), "Data": {"$type": name, "syntheticValue": i + 10}}
             for i, name in enumerate(("CameraAreaSettings", "LightingAreaSettings", "ColorGradingAreaSettings"))]
    binding = {"HandleId": "3", "Data": {"$type": "entHardTransformBinding",
               "bindName": cname("TargetPoint7121"), "slotName": cname("None"), "enabled": 1,
               "syntheticBindingField": "keep"}}
    full = {"$type": "entRenderToTextureCameraComponent", "id": "1735897481869320192",
            "name": cname("RenderToTextureCamera1587"), "virtualCameraName": cname("character_customization"),
            "dynamicTextureRes": resource(TEXTURE_RESOURCE), "isEnabled": 0,
            "isReplicable": 0, "resolutionWidth": 960, "resolutionHeight": 720,
            "fov": 15, "renderingMode": "Shaded", "renderSceneLayer": "Default",
            "env": resource("synthetic/editor.env"), "params": {"areaParameters": areas},
            "parentTransform": binding, "localTransform": {"syntheticPosition": [0, -5, 1]},
            "features": {"antiAliasing": "RTFP_All", "syntheticFeature": True},
            "syntheticUnknownProperty": {"nested": [1, 3, 5]}}
    camera = copy.deepcopy(full)
    camera["params"]["areaParameters"] = [{"HandleRefId": str(i)} for i in range(3)]
    camera["parentTransform"] = {"HandleRefId": "3"}
    target = {"$type": "entTargetPointComponent", "id": "1735899277719298048",
              "name": cname("TargetPoint7121"), "syntheticTargetField": 72}
    entity = {"Header": {"DataType": "CR2W", "ArchiveFileName": ENTITY_RESOURCE, "syntheticHeader": 4},
              "Data": {"Version": 195, "RootChunk": {"$type": "entEntityTemplate",
                  "compiledData": {"Data": {"Version": 4, "Sections": 7, "CruidIndex": 0,
                      "CruidDict": {"0": "0", "1": full["id"], "2": target["id"]},
                      "Chunks": [copy.deepcopy(owner), full, copy.deepcopy(target)]}},
                  "components": [camera, target], "entity": {"HandleId": "4", "Data": copy.deepcopy(owner)},
                  "includes": [], "syntheticUnrelatedRoot": {"keep": [False, "yes"]}},
                  "EmbeddedFiles": []}}
    texture = {"Header": {"DataType": "CR2W", "ArchiveFileName": TEXTURE_RESOURCE},
               "Data": {"RootChunk": {"$type": "DynamicTexture", "width": 960, "height": 720,
                   "dataFormat": "RGBA_Uint8_SRGB", "mipChain": 0, "samplesCount": 1,
                   "scaleToViewport": 1, "generator": None, "syntheticTextureProperty": [17, "preserve"]},
                   "EmbeddedFiles": []}}
    return entity, texture


def walk(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk(child)


def graph_signature(document, omit_secondary=False):
    """Independent value+alias proof; only component order is normalized.

    Unwrapping to plain Data alone loses shared-handle identity. Keep each edge
    as its ID token and compare all definition payloads separately instead.
    """
    definitions = {}

    def visit(value):
        if isinstance(value, dict):
            if "HandleId" in value:
                token = value["HandleId"]
                if set(value) != {"HandleId", "Data"} or token in definitions:
                    raise AssertionError("Duplicate/malformed handle definition")
                definitions[token] = None
                definitions[token] = visit(value["Data"])
                return {"$handle": token}
            if "HandleRefId" in value:
                if set(value) != {"HandleRefId"}:
                    raise AssertionError("Malformed handle reference")
                return {"$handle": value["HandleRefId"]}
            return {key: visit(child) for key, child in value.items()}
        if isinstance(value, list):
            return [visit(child) for child in value]
        return value

    body = visit(document)
    root = body["Data"]["RootChunk"]
    package = root["compiledData"]["Data"]
    right_id = "1570861139923375313"
    root["components"] = {c["id"]: c for c in root["components"]
                          if not omit_secondary or c["id"] != right_id}
    package["Chunks"] = {c.get("id", "owner"): c for c in package["Chunks"]
                         if not omit_secondary or c.get("id") != right_id}
    del package["CruidDict"]  # Ordered map is asserted independently below.
    return body, definitions


class EditorDualAssetsTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(GENERATOR.is_file(), "Missing same-owner second editor RTT asset generator")
        spec = importlib.util.spec_from_file_location("gen_editor_dual_assets", GENERATOR)
        self.gen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.gen)
        self.entity, self.texture = fixtures()

    def roots(self, entity=None):
        root = (entity or self.entity)["Data"]["RootChunk"]
        return root, root["compiledData"]["Data"]

    def set_camera_field(self, field, value):
        root, package = self.roots()
        root["components"][0][field] = copy.deepcopy(value)
        package["Chunks"][1][field] = copy.deepcopy(value)

    def test_original_graph_and_all_unknown_data_remain_unchanged(self):
        before_entity, before_texture = copy.deepcopy(self.entity), copy.deepcopy(self.texture)
        candidate, right_texture = self.gen.build_candidate(self.entity, self.texture)
        self.assertEqual(self.entity, before_entity)
        self.assertEqual(self.texture, before_texture)
        self.assertEqual(graph_signature(candidate, omit_secondary=True), graph_signature(before_entity))
        self.assertEqual(right_texture["Data"], before_texture["Data"])

    def test_second_camera_has_own_key_target_id_and_is_disabled(self):
        candidate, right_texture = self.gen.build_candidate(self.entity, self.texture)
        root, package = self.roots(candidate)
        right, primary = root["components"][0], root["components"][2]
        self.assertEqual(right["name"], cname("cpvr_editor_right"))
        self.assertEqual(right["virtualCameraName"], cname("cpvr_editor_right"))
        self.assertEqual(right["dynamicTextureRes"], resource(RIGHT_TEXTURE))
        self.assertEqual(right["isEnabled"], 0)
        handles = self.gen._handles(candidate)
        self.assertEqual(self.gen._resolved(right, handles), self.gen._resolved(package["Chunks"][1], handles))
        self.assertEqual(right["id"], package["CruidDict"]["1"])
        self.assertNotIn(right["id"], (c["id"] for c in root["components"][1:]))
        differences = {key for key in right if right[key] != primary[key]}
        self.assertEqual(differences, {"name", "virtualCameraName", "dynamicTextureRes", "id"})
        self.assertEqual(right_texture["Header"]["ArchiveFileName"], RIGHT_TEXTURE)

    def test_original_area_and_binding_definitions_are_reused_once(self):
        candidate, _ = self.gen.build_candidate(self.entity, self.texture)
        root, package = self.roots(candidate)
        definitions = [node["HandleId"] for node in walk(candidate) if "HandleId" in node]
        self.assertEqual(sorted(definitions), ["0", "1", "2", "3", "4"])
        self.assertEqual(root["components"][0]["parentTransform"], {"HandleRefId": "3"})
        self.assertEqual(root["components"][0]["params"]["areaParameters"],
                         [{"HandleRefId": str(i)} for i in range(3)])
        self.assertEqual([p["HandleId"] for p in package["Chunks"][1]["params"]["areaParameters"]], ["0", "1", "2"])
        self.assertEqual(package["Chunks"][1]["parentTransform"]["HandleId"], "3")
        self.assertEqual(package["Chunks"][3]["parentTransform"], {"HandleRefId": "3"})

    def test_default_keeps_original_primary_last_and_indices_coherent(self):
        candidate, _ = self.gen.build_candidate(self.entity, self.texture)
        root, package = self.roots(candidate)
        expected = ["1570861139923375313", "1735899277719298048", "1735897481869320192"]
        self.assertEqual([c["id"] for c in root["components"]], expected)
        self.assertEqual([c["id"] for c in package["Chunks"][1:]], expected)
        self.assertEqual(package["CruidDict"], {"0": "0", "1": expected[0], "2": expected[1], "3": expected[2]})
        self.assertTrue(all(c.get("isEnabled", 0) == 0 for c in root["components"]))

    def test_default_has_no_reference_before_definition(self):
        candidate, _ = self.gen.build_candidate(self.entity, self.texture)
        seen = set()
        for node in walk(candidate):
            if "HandleId" in node:
                self.assertNotIn(node["HandleId"], seen)
                seen.add(node["HandleId"])
            if "HandleRefId" in node:
                self.assertIn(node["HandleRefId"], seen, "Streaming resolver cannot read this forward reference")
        self.assertEqual(seen, {"0", "1", "2", "3", "4"})

    def test_graph_proof_detects_equal_value_alias_rebinding_and_payload_changes(self):
        candidate, _ = self.gen.build_candidate(self.entity, self.texture)
        changed = copy.deepcopy(candidate)
        root, package = self.roots(changed)
        full_binding = next(copy.deepcopy(node) for node in walk(candidate) if node.get("HandleId") == "3")
        full_binding["HandleId"] = "5"
        package["Chunks"][1]["parentTransform"] = full_binding
        root["components"][0]["parentTransform"] = {"HandleRefId": "5"}
        self.assertNotEqual(graph_signature(changed), graph_signature(candidate))
        changed = copy.deepcopy(candidate)
        definition = next(node for node in walk(changed) if node.get("HandleId") == "0")
        definition["Data"]["syntheticValue"] += 1
        self.assertNotEqual(graph_signature(changed), graph_signature(candidate))

    def test_repeated_generation_is_deterministic(self):
        self.assertEqual(self.gen.build_candidate(self.entity, self.texture),
                         self.gen.build_candidate(self.entity, self.texture))

    def test_wrong_resource_owner_is_rejected(self):
        self.entity["Header"]["ArchiveFileName"] = r"base\other_camera.ent"
        with self.assertRaisesRegex(ValueError, "target_face"):
            self.gen.build_candidate(self.entity, self.texture)

    def test_wrong_entity_type_is_rejected(self):
        root, package = self.roots()
        root["entity"]["Data"]["$type"] = package["Chunks"][0]["$type"] = "PlayerPuppet"
        with self.assertRaisesRegex(ValueError, "owner"):
            self.gen.build_candidate(self.entity, self.texture)

    def test_wrong_rtt_identity_and_shared_target_are_rejected(self):
        for field, value in (("virtualCameraName", cname("world_camera")),
                             ("name", cname("some_other_camera")),
                             ("dynamicTextureRes", resource(RIGHT_TEXTURE)), ("isEnabled", 1)):
            with self.subTest(field=field):
                self.entity, self.texture = fixtures()
                self.set_camera_field(field, value)
                with self.assertRaises(ValueError):
                    self.gen.build_candidate(self.entity, self.texture)

    def test_extra_rtt_is_rejected(self):
        root, package = self.roots()
        duplicate = copy.deepcopy(root["components"][0])
        duplicate["id"] = "72"
        root["components"].append(copy.deepcopy(duplicate))
        package["Chunks"].append(duplicate)
        package["CruidDict"]["3"] = "72"
        with self.assertRaisesRegex(ValueError, "exactly"):
            self.gen.build_candidate(self.entity, self.texture)

    def test_mismatched_serialized_component_and_chunk_are_rejected(self):
        root, _ = self.roots()
        root["components"][0]["syntheticUnknownProperty"] = {"changed": True}
        with self.assertRaisesRegex(ValueError, "component.*chunk|chunk.*component"):
            self.gen.build_candidate(self.entity, self.texture)

    def test_missing_or_duplicate_handle_definitions_are_rejected(self):
        root, package = self.roots()
        root["components"][0]["parentTransform"] = {"HandleRefId": "99"}
        with self.assertRaisesRegex(ValueError, "handle"):
            self.gen.build_candidate(self.entity, self.texture)
        self.entity, self.texture = fixtures()
        root, package = self.roots()
        root["syntheticDuplicate"] = copy.deepcopy(package["Chunks"][1]["parentTransform"])
        with self.assertRaisesRegex(ValueError, "handle"):
            self.gen.build_candidate(self.entity, self.texture)

    def test_malformed_and_cyclic_handles_are_rejected(self):
        root, package = self.roots()
        package["Chunks"][1]["params"]["areaParameters"][0]["HandleId"] = "bad"
        with self.assertRaisesRegex(ValueError, "handle"):
            self.gen.build_candidate(self.entity, self.texture)
        self.entity, self.texture = fixtures()
        root, package = self.roots()
        package["Chunks"][1]["params"]["areaParameters"][0]["Data"]["cycle"] = {"HandleRefId": "0"}
        with self.assertRaisesRegex(ValueError, "handle"):
            self.gen.build_candidate(self.entity, self.texture)

    def test_cruid_map_mismatch_and_duplicate_ids_are_rejected(self):
        root, package = self.roots()
        package["CruidDict"]["1"] = "99"
        with self.assertRaisesRegex(ValueError, "CRUID"):
            self.gen.build_candidate(self.entity, self.texture)
        self.entity, self.texture = fixtures()
        root, package = self.roots()
        root["components"][1]["id"] = package["Chunks"][2]["id"] = root["components"][0]["id"]
        package["CruidDict"]["2"] = root["components"][0]["id"]
        with self.assertRaisesRegex(ValueError, "CRUID"):
            self.gen.build_candidate(self.entity, self.texture)

    def test_reserved_secondary_cruid_collision_is_rejected(self):
        root, package = self.roots()
        root["components"][1]["id"] = package["Chunks"][2]["id"] = self.gen.RIGHT_CRUID
        package["CruidDict"]["2"] = self.gen.RIGHT_CRUID
        with self.assertRaisesRegex(ValueError, "collision"):
            self.gen.build_candidate(self.entity, self.texture)

    def test_texture_resource_type_and_shape_are_validated(self):
        for key, value in (("$type", "BitmapTexture"), ("width", 0), ("samplesCount", 2)):
            with self.subTest(key=key):
                self.entity, self.texture = fixtures()
                self.texture["Data"]["RootChunk"][key] = value
                with self.assertRaisesRegex(ValueError, "texture"):
                    self.gen.build_candidate(self.entity, self.texture)

    def file_inputs(self, root):
        paths = root / "entity.json", root / "texture.json"
        for path, document in zip(paths, (self.entity, self.texture)):
            path.write_text(json.dumps(document), encoding="utf-8")
        hashes = tuple(hashlib.sha256(path.read_bytes()).hexdigest() for path in paths)
        return paths, hashes

    def test_hash_mismatch_leaves_output_absent(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, hashes = self.file_inputs(root)
            output = root / "audit/output"
            with self.assertRaisesRegex(ValueError, "SHA"):
                self.gen.generate_files(paths[0], "0" * 64, paths[1], hashes[1], output, root / "audit")
            self.assertFalse(output.exists())

    def test_existing_output_is_rejected_without_rewriting_any_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, hashes = self.file_inputs(root)
            output = root / "audit/output"
            output.mkdir(parents=True)
            marker = output / "keep.txt"
            marker.write_text("keep", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "fresh"):
                self.gen.generate_files(paths[0], hashes[0], paths[1], hashes[1], output, root / "audit")
            self.assertEqual(marker.read_text(encoding="utf-8"), "keep")
            self.assertEqual(list(output.iterdir()), [marker])

    def test_output_cannot_escape_audit_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, hashes = self.file_inputs(root)
            with self.assertRaisesRegex(ValueError, "audit"):
                self.gen.generate_files(paths[0], hashes[0], paths[1], hashes[1], root / "game", root / "audit")
            self.assertFalse((root / "game").exists())

    def test_audit_tree_records_exact_input_output_hashes_and_resource_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, hashes = self.file_inputs(root)
            output = root / "audit/output"
            receipt = self.gen.generate_files(paths[0], hashes[0], paths[1], hashes[1], output, root / "audit")
            self.assertEqual(receipt["status"], "SOURCE_ONLY_DISABLED_SECOND_RTT")
            self.assertEqual([row["sha256"].lower() for row in receipt["inputs"]], list(hashes))
            self.assertEqual({row["resource"] for row in receipt["outputs"]}, {ENTITY_RESOURCE, RIGHT_TEXTURE})
            for row in receipt["outputs"]:
                path = Path(row["path"])
                self.assertTrue(path.is_relative_to(output))
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), row["sha256"].lower())
                json.loads(path.read_text(encoding="utf-8"))
            self.assertFalse(receipt["installed"])
            self.assertFalse(receipt["game_launched"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
