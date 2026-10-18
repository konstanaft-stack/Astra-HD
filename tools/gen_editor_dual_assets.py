#!/usr/bin/env python3
"""Prepare an audit-only disabled second RTT on the original editor camera owner.

Inputs are explicit SHA-pinned WolvenKit JSON exports of target_face.ent and its
preview.dtex. This does not enable a camera, pack/deploy an archive, or establish
same-frame rendering/history isolation. The native owner/capture integration is
separate. No vanilla binary or complete vanilla JSON belongs in this repository.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import sys


ENTITY_RESOURCE = r"base\gameplay\gui\fullscreen\main_menu\target_face.ent"
SOURCE_TEXTURE = r"base\gameplay\gui\fullscreen\main_menu\preview.dtex"
RIGHT_TEXTURE = r"cpvr\editor\preview_right.dtex"
RIGHT_NAME = "cpvr_editor_right"
APPROVED_AUDIT_ROOT = (Path(__file__).resolve().parents[1] / "audit")


def _fnv1a64(text):
    value = 0xCBF29CE484222325
    for byte in text.encode("utf-8"):
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


RIGHT_CRUID = str(_fnv1a64("cpvr_editor_right.component.v1"))


def _walk(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from _walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk(child)


def _path(value):
    return str(value).replace("/", "\\").lower()


def _resource(value):
    try:
        return value["DepotPath"]["$value"]
    except (KeyError, TypeError):
        raise ValueError("Malformed resource reference") from None


def _document_root(document, expected_resource):
    try:
        header = document["Header"]
        source = _path(header["ArchiveFileName"])
        wanted = _path(expected_resource)
        if header["DataType"] != "CR2W" or not (source == wanted or source.endswith("\\" + wanted)):
            raise ValueError("Expected explicit " + expected_resource + " source resource")
        return document["Data"]["RootChunk"]
    except (KeyError, TypeError):
        raise ValueError("Malformed WolvenKit source document") from None


def _handle_token(value):
    if not isinstance(value, str) or not re.fullmatch(r"0|[1-9][0-9]*", value):
        raise ValueError("Malformed handle identity")
    return value


def _handles(document):
    definitions, references = {}, set()
    for node in _walk(document):
        if "HandleId" in node:
            token = _handle_token(node["HandleId"])
            if token in definitions or set(node) != {"HandleId", "Data"} or not isinstance(node["Data"], dict):
                raise ValueError("Duplicate or malformed handle definition")
            definitions[token] = node["Data"]
        if "HandleRefId" in node:
            if set(node) != {"HandleRefId"}:
                raise ValueError("Malformed handle reference")
            references.add(_handle_token(node["HandleRefId"]))
    if not references.issubset(definitions):
        raise ValueError("Undefined handle reference")
    # Resolve every definition as well as those currently referenced. Cycles
    # cannot be serialized into an independently verifiable cloned component.
    for token in definitions:
        _resolved({"HandleRefId": token}, definitions)
    return definitions


def _resolved(value, definitions, active=frozenset()):
    if isinstance(value, dict):
        if "HandleId" in value or "HandleRefId" in value:
            token = value.get("HandleId", value.get("HandleRefId"))
            if token in active:
                raise ValueError("Cyclic handle graph")
            return _resolved(definitions[token], definitions, active | {token})
        return {key: _resolved(child, definitions, active) for key, child in value.items()}
    if isinstance(value, list):
        return [_resolved(child, definitions, active) for child in value]
    return value


def _cruid(value):
    if not isinstance(value, str) or not re.fullmatch(r"[1-9][0-9]*", value) or int(value) >= 1 << 64:
        raise ValueError("Malformed CRUID")
    return value


def _validate_entity(document):
    root = _document_root(document, ENTITY_RESOURCE)
    try:
        package, components = root["compiledData"]["Data"], root["components"]
        chunks, cruids = package["Chunks"], package["CruidDict"]
        if (root["$type"] != "entEntityTemplate" or
                chunks[0]["$type"] != "gameuiEntityPreviewGameObject" or
                root["entity"]["Data"]["$type"] != "gameuiEntityPreviewGameObject"):
            raise ValueError("Expected original editor camera owner")
        if (len(components) != 2 or len(chunks) != 3 or
                [component["$type"] for component in components] !=
                ["entRenderToTextureCameraComponent", "entTargetPointComponent"]):
            raise ValueError("Expected exactly one original RTT and one target component")
        if set(cruids) != {"0", "1", "2"} or cruids["0"] != "0":
            raise ValueError("Unexpected original CRUID index map")
        ids = [_cruid(component["id"]) for component in components]
        if len(set(ids)) != len(ids) or any(cruids[str(i + 1)] != value for i, value in enumerate(ids)):
            raise ValueError("Original CRUID map or component IDs disagree")
        if RIGHT_CRUID in ids:
            raise ValueError("Reserved secondary CRUID collision")
        definitions = _handles(document)
        if _resolved(root["entity"]["Data"], definitions) != _resolved(chunks[0], definitions):
            raise ValueError("Serialized owner and compiled owner chunk disagree")
        for component, chunk in zip(components, chunks[1:]):
            if _resolved(component, definitions) != _resolved(chunk, definitions):
                raise ValueError("Serialized component and compiled chunk disagree")
        camera, target = components
        if (camera["name"]["$value"] != "RenderToTextureCamera1587" or
                camera["virtualCameraName"]["$value"] != "character_customization" or
                _path(_resource(camera["dynamicTextureRes"])) != _path(SOURCE_TEXTURE) or
                camera["isEnabled"] != 0 or target["name"]["$value"] != "TargetPoint7121"):
            raise ValueError("Original editor RTT identity, target or disabled state differs")
        binding = _resolved(camera["parentTransform"], definitions)
        if (binding["$type"] != "entHardTransformBinding" or
                binding["bindName"]["$value"] != "TargetPoint7121"):
            raise ValueError("Original editor RTT handle binding differs")
        # Every definition is preserved in the original graph. The appended
        # chunk must use references, never duplicate the original HandleIds.
        if any("HandleId" in node for node in _walk(camera)):
            raise ValueError("Serialized component must reference original handle definitions")
        return root, package, camera
    except (KeyError, IndexError, TypeError):
        raise ValueError("Malformed original editor owner/component graph") from None


def _validate_texture(document):
    texture = _document_root(document, SOURCE_TEXTURE)
    expected = {"$type": "DynamicTexture", "width": 960, "height": 720,
                "dataFormat": "RGBA_Uint8_SRGB", "mipChain": 0, "samplesCount": 1,
                "scaleToViewport": 1, "generator": None}
    if any(texture.get(key) != value for key, value in expected.items()):
        raise ValueError("Original preview texture type/shape/format contract differs")


def _definitions_at_first_use(document):
    """Keep shared IDs/values/alias edges while emitting definitions before refs.

    Moving the original primary to the final RTT position must not leave its
    definitions after right's references: WolvenKit resolves in document order.
    Traverse once, including compiledData and root.components in their real order.
    """
    definitions = _handles(document)
    if set(definitions) != {"0", "1", "2", "3", "4"}:
        raise ValueError("Unexpected original editor handle identity set")
    movable, emitted = {"0", "1", "2", "3"}, set()

    def visit(value):
        if isinstance(value, dict):
            token = value.get("HandleId", value.get("HandleRefId"))
            if token in movable:
                if token in emitted:
                    return {"HandleRefId": token}
                emitted.add(token)
                return {"HandleId": token, "Data": visit(copy.deepcopy(definitions[token]))}
            return {key: visit(child) for key, child in value.items()}
        if isinstance(value, list):
            return [visit(child) for child in value]
        return value

    result = visit(document)
    if emitted != movable or _handles(result) != definitions:
        raise ValueError("Handle definition placement altered original payload")
    return result


def build_candidate(entity_document, texture_document):
    """Pure disabled candidate with original primary last; preserve its graph."""
    _, _, original_camera = _validate_entity(entity_document)
    _validate_texture(texture_document)
    entity, texture = copy.deepcopy(entity_document), copy.deepcopy(texture_document)
    root, package = entity["Data"]["RootChunk"], entity["Data"]["RootChunk"]["compiledData"]["Data"]
    right = copy.deepcopy(original_camera)
    right["id"] = RIGHT_CRUID
    right["name"]["$value"] = right["virtualCameraName"]["$value"] = RIGHT_NAME
    right["dynamicTextureRes"]["DepotPath"]["$value"] = RIGHT_TEXTURE
    right["isEnabled"] = 0
    package["CruidDict"][str(len(package["Chunks"]))] = RIGHT_CRUID
    package["Chunks"].append(copy.deepcopy(right))
    root["components"].append(right)
    # Tested native activation depends on retaining the original primary as the
    # final RTT. Keep target's index and all original object/CRUID values fixed.
    root["components"][0], root["components"][-1] = root["components"][-1], root["components"][0]
    package["Chunks"][1], package["Chunks"][-1] = package["Chunks"][-1], package["Chunks"][1]
    package["CruidDict"]["1"], package["CruidDict"]["3"] = package["CruidDict"]["3"], package["CruidDict"]["1"]
    entity = _definitions_at_first_use(entity)
    texture["Header"]["ArchiveFileName"] = RIGHT_TEXTURE
    _handles(entity)  # No dangling, duplicated or cyclic definitions introduced.
    return entity, texture


def _read_pinned(path, expected_sha):
    path = Path(path)
    if not re.fullmatch(r"[a-fA-F0-9]{64}", expected_sha):
        raise ValueError("Explicit SHA256 input pin required")
    raw = path.read_bytes()
    actual = hashlib.sha256(raw).hexdigest().upper()
    if actual != expected_sha.upper():
        raise ValueError("Input SHA256 mismatch: " + str(path))
    return json.loads(raw.decode("utf-8-sig")), {"path": str(path.resolve()), "sha256": actual, "bytes": len(raw)}


def _audit_output(output, allowed_root):
    raw_output, raw_root = Path(output).absolute(), Path(allowed_root).absolute()
    for path in (raw_output, *raw_output.parents, raw_root, *raw_root.parents):
        if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
            raise ValueError("Linked audit output ancestor is not allowed")
    output, root = raw_output.resolve(), raw_root.resolve()
    if output == root or not output.is_relative_to(root):
        raise ValueError("Output must be inside the approved audit root")
    if output.exists():
        raise ValueError("Output must be fresh; existing files are never overwritten")
    return output


def generate_files(entity_json, entity_sha256, texture_json, texture_sha256, output,
                   allowed_output_root=APPROVED_AUDIT_ROOT):
    output = _audit_output(output, allowed_output_root)
    entity, entity_input = _read_pinned(entity_json, entity_sha256)
    texture, texture_input = _read_pinned(texture_json, texture_sha256)
    entity, texture = build_candidate(entity, texture)
    outputs = []
    for resource, document in ((ENTITY_RESOURCE, entity), (RIGHT_TEXTURE, texture)):
        path = output / "source/raw" / (resource.replace("\\", "/") + ".json")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        outputs.append({"resource": resource, "path": str(path),
                        "sha256": hashlib.sha256(path.read_bytes()).hexdigest().upper()})
    receipt = {"schema": 1, "status": "SOURCE_ONLY_DISABLED_SECOND_RTT",
               "inputs": [entity_input, texture_input], "outputs": outputs,
               "original_camera": "RenderToTextureCamera1587", "secondary_camera": RIGHT_NAME,
               "secondary_cruid": RIGHT_CRUID, "secondary_enabled": False,
               "installed": False, "game_launched": False,
               "native_pair_or_history_verified": False,
               "component_order": "secondary,target,primary",
               "serialization_normalization": "same-ID shared definitions at first use",
               "limitation": "WolvenKit FromJson flattens filenames; preserve these resource paths in a separate verified package tree before any later pack."}
    (output / "manifest.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entity-json", required=True, type=Path)
    parser.add_argument("--entity-sha256", required=True)
    parser.add_argument("--texture-json", required=True, type=Path)
    parser.add_argument("--texture-sha256", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        receipt = generate_files(args.entity_json, args.entity_sha256, args.texture_json,
                                 args.texture_sha256, args.output)
    except (ValueError, OSError) as error:
        print("ERROR: " + str(error), file=sys.stderr)
        return 2
    print(json.dumps({"status": receipt["status"], "outputs": len(receipt["outputs"]),
                      "secondary_cruid": RIGHT_CRUID, "installed": False}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
