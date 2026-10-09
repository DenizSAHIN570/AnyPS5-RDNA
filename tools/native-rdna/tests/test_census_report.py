import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import census_report


def enum_value(name):
    return census_report.re_enum().index(name)


class CensusReportTests(unittest.TestCase):
    def setUp(self):
        self.map = {
            "V_MOV_B32": {"name": "V_MOV_B32", "status": "identical", "target": "V_MOV_B32", "targetEncoding": "ENC_VOP1", "replacement": ""},
            "S_GETPC_B64": {"name": "S_GETPC_B64", "status": "renumbered", "target": "S_GETPC_B64", "targetEncoding": "ENC_SOP1", "replacement": ""},
            "V_MAD_LEGACY_F32": {"name": "V_MAD_LEGACY_F32", "status": "removed", "target": "", "targetEncoding": "", "replacement": "v_mul_dx9_zero_f32 + v_add_f32"},
        }
        self.census = {
            "version": 1,
            "shaders": [{"name": "a.ash", "unsupported": 1, "decodeError": None}, {"name": "b.ash", "unsupported": 0, "decodeError": "bad"}],
            "opcodes": [
                {"value": enum_value("VMovB32"), "family": "VOP1", "count": 10, "shaders": 2},
                {"value": enum_value("SGetpcB64"), "family": "SOP1", "count": 3, "shaders": 1},
                {"value": enum_value("VMadLegacyF32"), "family": "VOP3", "count": 2, "shaders": 1},
                {"value": 99999, "family": "VOP3", "count": 1, "shaders": 1},
            ],
            "families": {"VOP1": 10},
            "features": {"sdwa": 4, "vintrp": 2},
            "exportTargets": {"0": 1, "12": 1, "32": 3},
            "unsupportedReasons": {"x": 1},
        }

    def test_report_joins_census_and_map(self):
        report = census_report.build_report(self.census, self.map)
        self.assertEqual(report["shaders"], 2)
        self.assertEqual(report["decodeFailures"], ["b.ash"])
        self.assertEqual(report["unsupportedInstructions"], 1)
        self.assertEqual(report["instructions"], 16)
        self.assertEqual(report["instructionsByStatus"], {"identical": 10, "renumbered": 3, "removed": 2, "unknown": 1})
        self.assertEqual(report["exports"], {"mrt0": 1, "pos0": 1, "param": 3})
        removed = [r for r in report["opcodes"] if r["status"] == "removed"][0]
        self.assertEqual(removed["isa"], "V_MAD_LEGACY_F32")
        self.assertIn("dx9_zero", removed["replacement"])

    def test_markdown_lists_translator_work(self):
        text = census_report.markdown(census_report.build_report(self.census, self.map))
        self.assertIn("| VMadLegacyF32 | V_MAD_LEGACY_F32 | 2 | 1 | removed |", text)
        self.assertIn("`exp param` has no gfx12 form", text)
        self.assertIn("no SDWA on gfx12", text)
        self.assertIn("## Decode failures", text)

    def test_committed_map_covers_the_inventory(self):
        rows = census_report.load_map(census_report.DEFAULT_MAP)
        inventory = [l.split()[0] for l in census_report.progress.ISA.read_text().splitlines() if l and not l.startswith("#")]
        self.assertEqual(set(inventory), set(rows))
        statuses = {row["status"] for row in rows.values()}
        self.assertLessEqual(statuses, {"identical", "renumbered", "renamed", "removed", "unresolved"})
        self.assertEqual(rows["S_GETPC_B64"]["targetOpcode"], "71")
        self.assertEqual(rows["BUFFER_LOAD_DWORD"]["target"], "BUFFER_LOAD_B32")
        for row in rows.values():
            if row["status"] == "removed":
                self.assertTrue(row["replacement"], row["name"])

    def test_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            census = Path(tmp) / "census.json"
            census.write_text(json.dumps(self.census))
            mapping = Path(tmp) / "map.csv"
            with mapping.open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=["name", "status", "target", "targetEncoding", "replacement"])
                writer.writeheader()
                writer.writerows(self.map.values())
            out = Path(tmp) / "report.md"
            data = Path(tmp) / "report.json"
            sys.argv = ["census_report.py", str(census), "--map", str(mapping), "--markdown", str(out), "--json", str(data)]
            self.assertEqual(census_report.main(), 0)
            self.assertIn("# Shader census against gfx12", out.read_text())
            self.assertEqual(json.loads(data.read_text())["instructions"], 16)


if __name__ == "__main__":
    unittest.main()
