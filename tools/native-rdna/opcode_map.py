import argparse
import csv
import hashlib
import json
import re
import sys
import xml.etree.ElementTree as ElementTree
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
sys.path.insert(0, str(TOOLS))
import progress

TABLEGEN_FILES = ("SOPInstructions.td", "SMInstructions.td", "VOP1Instructions.td", "VOP2Instructions.td", "VOP3Instructions.td",
                  "VOP3PInstructions.td", "VOPCInstructions.td", "VOPDInstructions.td", "VINTERPInstructions.td", "DSInstructions.td",
                  "DSDIRInstructions.td", "BUFInstructions.td", "FLATInstructions.td", "MIMGInstructions.td", "EXPInstructions.td", "SIInstructions.td")

REPLACEMENTS = (
    (r"^S_CMPK_", "s_cmp_* with the immediate as a literal"),
    (r"^V_(MAD|MAC)_F32$|^V_MAD(AK|MK)_F32$", "unfused v_mul_f32 + v_add_f32; v_fma_f32 is not bit-exact"),
    (r"^V_(MAD|MAC|FMAC)_LEGACY_F32$", "v_mul_dx9_zero_f32 + v_add_f32 (fmac: v_fma_dx9_zero_f32)"),
    (r"^V_MUL_LO_I32$", "v_mul_lo_u32 (same low 32 bits)"),
    (r"^S_(BUFFER_)?(STORE|ATOMIC)_|^S_SCRATCH_", "vector memory (global/buffer) from one active lane"),
    (r"^S_DCACHE_DISCARD", "drop: gfx12 has no scalar stores to discard"),
    (r"^DS_.*_SRC2_", "emulate: per-lane DS operation with src2 addressing"),
    (r"^DS_GWS_|^DS_ORDERED_COUNT$", "emulate: gfx12 has no GWS/GDS"),
    (r"^DS_CMPST_", "ds_cmpstore_* with data operands swapped"),
    (r"^DS_WRAP_RTN_B32$", "emulate with a DS compare-and-swap loop"),
    (r"^(GLOBAL|SCRATCH|BUFFER)_LOAD_LDS_|^BUFFER_LOAD_.*_LDS", "vector load + ds_store (no LDS DMA on gfx12)"),
    (r"FCMPSWAP", "compare-and-swap loop with float compare"),
    (r"^(FLAT|GLOBAL)_ATOMIC_(MIN|MAX)_F64$|^(BUFFER|FLAT|GLOBAL)_ATOMIC_F(MIN|MAX)_X2$", "64-bit compare-and-swap loop"),
    (r"^IMAGE_GATHER4.*_O$", "fold the texel offset into the coordinates (needs the image size) or use a gather with offset where gfx12 keeps one"),
    (r"^IMAGE_(GATHER(4|8)H_PCK|LOAD(_MIP)?_(BY|PCK)[24])$", "emulate with image_load and bit packing (PS5 packed loads)"),
    (r"^IMAGE_STORE(_MIP)?_(BY|PCK)[24]$", "emulate with bit unpacking and image_store (PS5 packed stores)"),
    (r"^S_GET_WAVEID_IN_WORKGROUP$", "s_getreg HW_ID / a wave-index SGPR set up by the driver (UNCONFIRMED)"),
    (r"^S_GL1_INV$", "global_inv with SE scope (gfx12 has no separate GL1 invalidate)"),
    (r"^IMAGE_SAMPLE(_C)?_CD", "image_sample(_c)_d with the same derivatives; coarse-derivative precision UNCONFIRMED"),
    (r"^BUFFER_GL[01]_INV$", "global_inv with the matching scope"),
    (r"^S_WAITCNT", "regenerated per gfx12 counter (s_wait_loadcnt/storecnt/samplecnt/bvhcnt/expcnt/dscnt/kmcnt)"),
    (r"^S_BARRIER$", "s_barrier_signal -1 + s_barrier_wait -1"),
    (r"^S_CBRANCH_CDBG", "drop (debugger branches)"),
    (r"^S_INST_PREFETCH$", "drop or s_prefetch_inst (hint only)"),
    (r"^S_SUBVECTOR_LOOP_", "emulate with explicit EXEC halves"),
    (r"^S_FF0_", "s_not + s_ctz"),
    (r"^S_ENDPGM_ORDERED_PS_DONE$", "s_endpgm; ordered PS done handling UNCONFIRMED"),
    (r"^V_CMPX?_(F|TRU|T)_", "constant result (0 or all lanes); cmpx also writes EXEC"),
    (r"^V_CLREXCP$", "drop; exception state semantics UNCONFIRMED"),
    (r"^V_INTERP_", "ds_param_load + v_interp_p10/p2 (VINTERP)"),
    (r"^EXP", "exp pos/prim/mrt/mrtz remain; exp param becomes attribute-ring stores; compr and vm bits are gone"),
    (r"^V_DOT", "not on gfx1013"),
    (r"^S_MEMTIME$|^S_MEMREALTIME$", "s_getreg SHADER_CYCLES / s_sendmsg_rtn GET_REALTIME"),
)

RULE_RENAMES = (
    (r"^(V_(?:MIN|MAX)(?:3)?_F(?:16|32|64))$", lambda m: m.group(1).replace("_F", "_NUM_F", 1)),
    (r"^(V_PK_(?:MIN|MAX)_F16)$", lambda m: m.group(1).replace("_F16", "_NUM_F16")),
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_isa_xml(path):
    instructions = defaultdict(list)
    aliases = {}
    architecture = None
    for _, element in ElementTree.iterparse(path, events=("end",)):
        if element.tag == "ArchitectureName" and architecture is None:
            architecture = (element.text or "").strip()
        if element.tag != "Instruction":
            continue
        name = (element.findtext("InstructionName") or "").strip()
        for alias in element.findall("AliasedInstructionNames/InstructionName"):
            aliases[(alias.text or "").strip()] = name
        for encoding in element.iter("InstructionEncoding"):
            encoding_name = (encoding.findtext("EncodingName") or "").strip()
            opcode = encoding.find("Opcode")
            if opcode is None or not encoding_name:
                continue
            radix = int(opcode.get("Radix", "10"))
            value = int((opcode.text or "0").strip(), radix)
            entry = (encoding_name, value)
            if entry not in instructions[name]:
                instructions[name].append(entry)
        element.clear()
    if not instructions:
        raise SystemExit(f"{path}: no instructions found; is this an AMD machine-readable ISA XML file?")
    return architecture, dict(instructions), aliases


def strip_comments(text):
    return re.sub(r"//[^\n]*", "", text)


def split_top_level(body):
    parts, current, depth = [], "", 0
    for character in body:
        if character == "<":
            depth += 1
        elif character == ">":
            depth -= 1
        if character == "," and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += character
    parts.append(current)
    return parts


def parse_tablegen(directory):
    statement = re.compile(r"^\s*defm\s+([A-Z][A-Z0-9_]*)\s*:\s*(.*?);", re.S | re.M)
    generations = defaultdict(set)
    renames = defaultdict(dict)
    for file_name in TABLEGEN_FILES:
        path = directory / file_name
        if not path.exists():
            raise SystemExit(f"{path} not found: --llvm-amdgpu must name llvm/lib/Target/AMDGPU")
        for match in statement.finditer(strip_comments(path.read_text(encoding="utf-8"))):
            name, body = match.group(1), match.group(2)
            for part in split_top_level(body):
                part = part.strip()
                class_match = re.match(r"([A-Za-z0-9_]+)\s*(<.*)?$", part, re.S)
                if not class_match:
                    continue
                cls, args = class_match.group(1), class_match.group(2) or ""
                found = {int(g) for g in re.findall(r"(?:^|_)gfx(\d+)(?=_|$)", cls)}
                found |= {int(g) for g in re.findall(r"GFX(\d+)Gen", args)}
                if "Real" not in cls and not found:
                    continue
                lower = re.findall(r'"([a-z0-9_]+)"', args)
                upper = re.findall(r'"([A-Z][A-Z0-9_]+)"', args)
                pseudo = upper[0] if upper else name
                for generation in found:
                    generation = {1010: 10, 1030: 10, 1200: 12}.get(generation, generation)
                    generations[pseudo].add(generation)
                    if lower:
                        renames[pseudo][generation] = lower[-1].upper()
                    elif pseudo != name:
                        renames[pseudo][generation] = name
    return generations, renames


def replacement_note(name):
    for pattern, note in REPLACEMENTS:
        if re.search(pattern, name):
            return note
    return None


def rule_rename(name):
    for pattern, rename in RULE_RENAMES:
        match = re.match(pattern, name)
        if match:
            return rename(match)
    return None


def source_inventory():
    names = {}
    for line in progress.ISA.read_text().splitlines():
        if line and not line.startswith("#"):
            name, encoding = line.split()
            names[name] = encoding
    return names


def classify(name, source_entries, target, aliases, generations, renames):
    record = {"name": name}
    if source_entries:
        record["sourceEncoding"], record["sourceOpcode"] = source_entries[0]
    candidates = [(name, "same")]
    if name in aliases:
        candidates.append((aliases[name], "amd-alias"))
    llvm_name = renames.get(name, {}).get(12)
    if llvm_name:
        candidates.append((llvm_name, "llvm"))
    rule = rule_rename(name)
    if rule:
        candidates.append((rule, "rule"))
    for candidate, origin in candidates:
        if candidate in target:
            encoding, opcode = target[candidate][0]
            record.update(target=candidate, targetEncoding=encoding, targetOpcode=opcode, renameSource=origin if candidate != name else None)
            if candidate != name:
                record["status"] = "renamed"
            elif source_entries and (encoding, opcode) == source_entries[0]:
                record["status"] = "identical"
            else:
                record["status"] = "renumbered"
            return record
    has_gfx12 = 12 in generations.get(name, set())
    record["status"] = "unresolved" if has_gfx12 else "removed"
    note = replacement_note(name)
    if note:
        record["replacement"] = note
    return record


def enum_names():
    enum = re.search(r"enum class RdnaOpcode[^{]*\{(.*?)\};", progress.OPCODES.read_text(), re.S).group(1)
    return re.findall(r"^\s*([A-Z]\w*)\s*[,=]", enum, re.M)


def enum_to_isa(names, isa_names):
    by_camel = {progress.camel(name): name for name in isa_names}
    mapping = {}
    for opcode in names:
        mapping[opcode] = progress.OPCODE_ALIASES.get(opcode) or by_camel.get(opcode)
    return mapping


def build(isa_dir, llvm_dir):
    paths = {key: isa_dir / f"amdgpu_isa_{key}.xml" for key in ("rdna1", "rdna2", "rdna4")}
    for path in paths.values():
        if not path.exists():
            raise SystemExit(f"{path} not found: --isa-xml must hold the files of AMD's machine-readable ISA zip")
    parsed = {key: parse_isa_xml(path) for key, path in paths.items()}
    generations, renames = parse_tablegen(llvm_dir)
    inventory = source_inventory()
    rdna1, rdna2 = parsed["rdna1"][1], parsed["rdna2"][1]
    target, aliases = parsed["rdna4"][1], parsed["rdna4"][2]
    records = []
    for name in sorted(inventory):
        source_entries = rdna1.get(name) or rdna2.get(name) or []
        record = classify(name, source_entries, target, aliases, generations, renames)
        record["inventoryEncoding"] = inventory[name]
        record["sourceSpec"] = "rdna1" if name in rdna1 else "rdna2" if name in rdna2 else None
        records.append(record)
    by_name = {r["name"]: r for r in records}
    names = enum_names()
    mapping = enum_to_isa(names, inventory)
    enum = []
    for value, opcode in enumerate(names):
        isa = mapping.get(opcode)
        entry = {"value": value, "enum": opcode, "isa": isa}
        if opcode in progress.OPCODE_SENTINELS:
            entry["status"] = "sentinel"
        elif isa is None:
            entry["status"] = "upstream-extra"
        else:
            entry["status"] = by_name[isa]["status"]
            for key in ("target", "replacement"):
                if key in by_name[isa]:
                    entry[key] = by_name[isa][key]
        enum.append(entry)
    return {
        "version": 1,
        "sources": {key: {"file": paths[key].name, "sha256": sha256(paths[key]), "architecture": parsed[key][0]} for key in paths},
        "summary": dict(Counter(r["status"] for r in records)),
        "instructions": records,
        "enum": enum,
    }


def write_csv(path, records):
    fields = ["name", "status", "inventoryEncoding", "sourceSpec", "sourceEncoding", "sourceOpcode", "target", "targetEncoding", "targetOpcode", "renameSource", "replacement"]
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for record in records:
            writer.writerow(record)


def main():
    parser = argparse.ArgumentParser(description="Map the gfx1013 instruction inventory to gfx12 (RDNA4) from AMD's machine-readable ISA and LLVM's renames.")
    parser.add_argument("--isa-xml", type=Path, required=True, help="directory with amdgpu_isa_rdna1.xml, rdna2.xml and rdna4.xml")
    parser.add_argument("--llvm-amdgpu", type=Path, required=True, help="llvm/lib/Target/AMDGPU of an LLVM checkout")
    parser.add_argument("--json", type=Path, help="write the full map here")
    parser.add_argument("--csv", type=Path, help="write the instruction table here")
    args = parser.parse_args()
    result = build(args.isa_xml, args.llvm_amdgpu)
    if args.json:
        args.json.write_text(json.dumps(result, indent=1) + "\n")
    if args.csv:
        write_csv(args.csv, result["instructions"])
        sidecar = args.csv.with_suffix(".sources.json")
        sidecar.write_text(json.dumps({"generator": "tools/native-rdna/opcode_map.py", "inventory": "tools/rdna_isa.txt", "license": "Derived from the AMD Machine-Readable GPU ISA Specification, https://gpuopen.com/machine-readable-isa/. Copyright (c) 2026 Advanced Micro Devices, Inc., or its affiliates. MIT License.", "sources": result["sources"], "summary": result["summary"]}, indent=2) + "\n")
    total = len(result["instructions"])
    print(f"{total} instructions: " + ", ".join(f"{status} {count}" for status, count in sorted(result["summary"].items())))
    unresolved = [r["name"] for r in result["instructions"] if r["status"] == "unresolved"]
    if unresolved:
        print(f"unresolved (LLVM has a gfx12 form, the RDNA4 XML has no matching name): {' '.join(unresolved[:40])}{' ...' if len(unresolved) > 40 else ''}")
    removed = [r for r in result["instructions"] if r["status"] == "removed" and "replacement" not in r]
    if removed:
        print(f"removed without a replacement note: {' '.join(r['name'] for r in removed[:40])}{' ...' if len(removed) > 40 else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
