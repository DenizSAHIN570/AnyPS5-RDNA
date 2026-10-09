import argparse
import csv
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import progress

DEFAULT_MAP = HERE / "data" / "gfx1013-to-gfx12.csv"
STATUS_ORDER = ("identical", "renumbered", "renamed", "removed", "unresolved", "upstream-extra", "sentinel", "unknown")
TRANSLATOR_WORK = ("removed", "unresolved", "upstream-extra", "unknown")
FEATURE_NOTES = {
    "sdwa": "no SDWA on gfx12: bfe/perm/op_sel sequences",
    "dpp16": "DPP16 exists on gfx12 (VOP1/VOP2/VOPC, also VOP3)",
    "dpp8": "DPP8 exists on gfx12",
    "dpp8_fetch_inactive": "DPP8 FI exists on gfx12",
    "vintrp": "no VINTRP on gfx12: ds_param_load + VINTERP",
    "s_waitcnt_any": "regenerated per gfx12 counter",
    "s_getpc_b64": "replace with the guest PC as a constant",
    "s_setpc_b64": "code addresses must be resolved by the translator",
    "s_swappc_b64": "calls (fetch shaders): inline or translate the callee",
    "s_call_b64": "calls: inline or translate the callee",
    "s_setreg": "hwreg ids and MODE bits differ on gfx12",
    "s_getreg": "hwreg ids differ on gfx12",
    "s_dcache_discard": "removed on gfx12",
    "v_mad_legacy_f32": "removed on gfx12",
    "v_mac_legacy_f32": "removed on gfx12",
    "v_mul_lo_i32": "v_mul_lo_u32 on gfx12",
    "buffer_glc": "cache policy becomes TH/SCOPE; atomic return is TH bit 0",
    "buffer_slc": "cache policy becomes TH/SCOPE",
    "buffer_dlc": "cache policy becomes TH/SCOPE",
    "mimg_nsa": "gfx12 VIMAGE/VSAMPLE allow at most 5 address VGPRs",
    "literal_constant": "literals exist on gfx12",
}


def load_map(path):
    with path.open(newline="") as stream:
        return {row["name"]: row for row in csv.DictReader(stream)}


def export_kind(target):
    if target <= 7:
        return f"mrt{target}"
    if target == 8:
        return "mrtz"
    if target == 9:
        return "null"
    if 12 <= target <= 16:
        return f"pos{target - 12}"
    if target == 20:
        return "prim"
    if 32 <= target <= 63:
        return "param"
    return f"target {target}"


def enum_table(instruction_map):
    enum = re_enum()
    by_camel = {progress.camel(name): name for name in instruction_map}
    table = []
    for value, name in enumerate(enum):
        isa = progress.OPCODE_ALIASES.get(name) or by_camel.get(name)
        if name in progress.OPCODE_SENTINELS:
            status = "sentinel"
        elif isa is None:
            status = "upstream-extra"
        elif isa in instruction_map:
            status = instruction_map[isa]["status"]
        else:
            status = "unknown"
        table.append({"value": value, "enum": name, "isa": isa, "status": status, "row": instruction_map.get(isa) if isa else None})
    return table


def re_enum():
    enum = re.search(r"enum class RdnaOpcode[^{]*\{(.*?)\};", progress.OPCODES.read_text(), re.S).group(1)
    return re.findall(r"^\s*([A-Z]\w*)\s*[,=]", enum, re.M)


def build_report(census, instruction_map):
    table = enum_table(instruction_map)
    opcodes = []
    by_status = Counter()
    opcodes_by_status = Counter()
    for entry in census["opcodes"]:
        value = entry["value"]
        if value >= len(table):
            info = {"enum": f"#{value}", "isa": None, "status": "unknown", "row": None}
        else:
            info = table[value]
        row = info["row"] or {}
        record = {
            "value": value,
            "enum": info["enum"],
            "isa": info["isa"],
            "family": entry["family"],
            "count": entry["count"],
            "shaders": entry["shaders"],
            "status": info["status"],
            "target": row.get("target") or None,
            "targetEncoding": row.get("targetEncoding") or None,
            "replacement": row.get("replacement") or None,
        }
        opcodes.append(record)
        by_status[info["status"]] += entry["count"]
        opcodes_by_status[info["status"]] += 1
    shaders = census["shaders"]
    exports = Counter()
    for target, count in census.get("exportTargets", {}).items():
        exports[export_kind(int(target))] += count
    total = sum(by_status.values())
    return {
        "shaders": len(shaders),
        "decodeFailures": [s["name"] for s in shaders if s.get("decodeError")],
        "unsupportedInstructions": sum(s.get("unsupported", 0) for s in shaders),
        "unsupportedReasons": census.get("unsupportedReasons", {}),
        "instructions": total,
        "instructionsByStatus": {s: by_status[s] for s in STATUS_ORDER if by_status[s]},
        "opcodesByStatus": {s: opcodes_by_status[s] for s in STATUS_ORDER if opcodes_by_status[s]},
        "families": census.get("families", {}),
        "features": census.get("features", {}),
        "exports": dict(exports),
        "opcodes": sorted(opcodes, key=lambda r: (-r["count"], r["value"])),
    }


def percent(part, whole):
    return f"{100.0 * part / whole:.1f}%" if whole else "-"


def markdown(report):
    lines = ["# Shader census against gfx12", ""]
    lines.append(f"- Shaders: {report['shaders']} ({len(report['decodeFailures'])} failed to decode)")
    lines.append(f"- Instructions: {report['instructions']}, distinct opcodes: {len(report['opcodes'])}")
    lines.append(f"- Instructions upstream's decoder marks unsupported: {report['unsupportedInstructions']}")
    lines += ["", "## gfx12 status", "", "| Status | Instructions | Share | Distinct opcodes |", "|---|---|---|---|"]
    for status, count in report["instructionsByStatus"].items():
        lines.append(f"| {status} | {count} | {percent(count, report['instructions'])} | {report['opcodesByStatus'].get(status, 0)} |")
    work = [r for r in report["opcodes"] if r["status"] in TRANSLATOR_WORK]
    lines += ["", "## Translator work: opcodes with no direct gfx12 form", ""]
    if work:
        lines += ["| Opcode | ISA name | Count | Shaders | Status | Replacement |", "|---|---|---|---|---|---|"]
        for r in work:
            lines.append(f"| {r['enum']} | {r['isa'] or '-'} | {r['count']} | {r['shaders']} | {r['status']} | {r['replacement'] or '-'} |")
    else:
        lines.append("None.")
    lines += ["", "## Encoding features", "", "| Feature | Count | gfx12 |", "|---|---|---|"]
    for name, count in sorted(report["features"].items(), key=lambda item: -item[1]):
        lines.append(f"| {name} | {count} | {FEATURE_NOTES.get(name, '-')} |")
    if report["exports"]:
        lines += ["", "## Exports", "", "| Target | Count |", "|---|---|"]
        for name, count in sorted(report["exports"].items()):
            lines.append(f"| {name} | {count} |")
        if report["exports"].get("param"):
            lines.append("")
            lines.append("`exp param` has no gfx12 form: parameters go to the attribute ring.")
    lines += ["", "## All opcodes", "", "| Opcode | ISA name | Family | Count | Shaders | Status | gfx12 |", "|---|---|---|---|---|---|---|"]
    for r in report["opcodes"]:
        lines.append(f"| {r['enum']} | {r['isa'] or '-'} | {r['family']} | {r['count']} | {r['shaders']} | {r['status']} | {r['target'] or r['replacement'] or '-'} |")
    if report["decodeFailures"]:
        lines += ["", "## Decode failures", ""] + [f"- {name}" for name in report["decodeFailures"]]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description="Join a shader census (shader_census JSON) with the gfx1013 to gfx12 opcode table.")
    parser.add_argument("census", type=Path)
    parser.add_argument("--map", type=Path, default=DEFAULT_MAP)
    parser.add_argument("--markdown", type=Path, help="write the report here instead of stdout")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    census = json.loads(args.census.read_text())
    if census.get("version") != 1:
        sys.exit(f"{args.census}: unsupported census version {census.get('version')}")
    report = build_report(census, load_map(args.map))
    text = markdown(report)
    if args.markdown:
        args.markdown.write_text(text)
    else:
        sys.stdout.write(text)
    if args.json:
        args.json.write_text(json.dumps(report, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
