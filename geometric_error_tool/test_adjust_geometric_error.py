import json
import tempfile
import unittest
from pathlib import Path

from adjust_geometric_error import (
    GeometricErrorProcessor,
    INDEX_ONLY_GEOMETRIC_ERROR,
)


def leaf(name):
    return {
        "geometricError": 0.0,
        "content": {"uri": f"./Data/{name}.glb"},
    }


def paged_base(name, ge):
    return {
        "geometricError": ge,
        "content": {"uri": f"./Data/{name}.glb"},
        "children": [leaf(name + "_a"), leaf(name + "_b")],
    }


class GeometricErrorTests(unittest.TestCase):
    def write_tileset(self, root, name="tileset.json"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(
                {
                    "asset": {"version": "1.1"},
                    "geometricError": root["geometricError"],
                    "root": root,
                }
            ),
            encoding="utf-8",
        )
        return path

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def process(self, path):
        processor = GeometricErrorProcessor(path, 2.0, False, None)
        return processor.run()

    def test_pagedlod_recomputes_each_upper_level(self):
        root = {
            "geometricError": 24.025,
            "content": {"uri": "./Data/Tile_root.glb"},
            "children": [
                {
                    "geometricError": 15.5,
                    "content": {"uri": "./Data/mid.glb"},
                    "children": [paged_base("base", 10.0)],
                }
            ],
        }
        result = self.process(self.write_tileset(root))
        self.assertEqual(result.natural_ge, 40.0)

    def test_hlod_multiple_cells_and_parent(self):
        level_zero = {
            "geometricError": 18.6,
            "content": {"uri": "./Data/HLOD/L0_X0_Y0.glb"},
            "children": [paged_base("cell_a", 10.0), paged_base("cell_b", 12.0)],
        }
        root = {
            "geometricError": 28.83,
            "content": {"uri": "./Data/HLOD/root.glb"},
            "children": [level_zero],
        }
        result = self.process(self.write_tileset(root))
        self.assertAlmostEqual(result.natural_ge, 28.83)

    def test_hlod_single_cell_does_not_multiply_level_zero(self):
        root = {
            "geometricError": 10.0,
            "content": {"uri": "./Data/HLOD/L0_X0_Y0.glb"},
            "children": [paged_base("only_cell", 10.0)],
        }
        result = self.process(self.write_tileset(root))
        self.assertEqual(result.natural_ge, 10.0)

    def test_index_only_override_does_not_pollute_parent(self):
        index_child = {
            "geometricError": INDEX_ONLY_GEOMETRIC_ERROR,
            "children": [paged_base("cell_a", 10.0), paged_base("cell_b", 12.0)],
        }
        root = {
            "geometricError": 28.83,
            "content": {"uri": "./Data/HLOD/root.glb"},
            "children": [index_child],
        }
        result = self.process(self.write_tileset(root))
        self.assertAlmostEqual(result.natural_ge, 28.83)
        self.assertAlmostEqual(result.output_ge, 28.83)

    def test_external_reference_uses_natural_error(self):
        external_root = {
            "geometricError": INDEX_ONLY_GEOMETRIC_ERROR,
            "children": [paged_base("cell_a", 10.0), paged_base("cell_b", 12.0)],
        }
        self.write_tileset(external_root, "subtilesets/HLOD_L1_X0_Y0.json")
        root = {
            "geometricError": 18.6,
            "content": {"uri": "./Data/HLOD/root.glb"},
            "children": [
                {
                    "geometricError": 18.6,
                    "content": {
                        "uri": "./subtilesets/HLOD_L1_X0_Y0.json"
                    },
                }
            ],
        }
        result = self.process(self.write_tileset(root))
        self.assertAlmostEqual(result.natural_ge, 28.83)

    def test_hlod_and_pagedlod_use_distinct_factors(self):
        level_zero = {
            "geometricError": 18.6,
            "content": {"uri": "./Data/HLOD/L0_X0_Y0.glb"},
            "children": [paged_base("cell_a", 10.0), paged_base("cell_b", 12.0)],
        }
        root = {
            "geometricError": 28.83,
            "content": {"uri": "./Data/HLOD/root.glb"},
            "children": [level_zero],
        }
        processor = GeometricErrorProcessor(
            self.write_tileset(root), 3.0, False, None, hlod_factor=1.25
        )
        result = processor.run()
        self.assertAlmostEqual(result.natural_ge, 18.75)


if __name__ == "__main__":
    unittest.main()
