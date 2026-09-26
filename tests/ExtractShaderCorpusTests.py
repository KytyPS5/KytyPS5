#!/usr/bin/env python3
"""Synthetic AGC metadata tests; uses only the standard library and no game data."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
import unittest


def load_extractor(path):
    # Importing the CLI module must not write Python cache files into tools/.
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("kyty_shader_corpus_extractor", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_compute_header(rsrc1_words):
    # SH offsets are relative to the AMD SH register base. COMPUTE_PGM_RSRC1
    # is SH0x212 (decimal530); the surrounding registers match a minimal CS.
    registers = [(0x207, 32), (0x208, 1), (0x209, 1)]
    registers += [(0x212, value) for value in rsrc1_words]
    registers.append((0x213, 0))
    registers_offset = 96
    specials_offset = registers_offset + len(registers) * 8
    header_size = specials_offset + 48
    data = bytearray(header_size)
    data[:8] = b"1234\x18\x00\x00\x00"
    struct.pack_into("<Q", data, 32, registers_offset - 32)
    struct.pack_into("<Q", data, 40, specials_offset - 40)
    struct.pack_into("<5I", data, 64, header_size, 4, 0, 0, 0)
    struct.pack_into("<3H3B", data, 84, 0, 0, 48, 0, 0, len(registers))
    for index, (offset, value) in enumerate(registers):
        struct.pack_into("<II", data, registers_offset + 8 * index, offset, value)
    struct.pack_into("<I", data, specials_offset + 16, 0x8000)  # declared wave32
    return bytes(data)


class ComputeHeaderFloatingPointTests(unittest.TestCase):
    extractor = None

    def profile(self, rsrc1_words):
        metadata = self.extractor.parse_header(make_compute_header(rsrc1_words), 0)
        self.assertTrue(self.extractor.add_compute_header_profile(metadata))
        self.assertFalse(metadata["metadata_complete"])
        self.assertFalse(metadata["runtime_context_complete"])
        self.assertEqual(metadata["metadata_provenance"], "agc_header_profile")
        # Exercise the actual JSON value types supplied to the native auditor.
        return json.loads(json.dumps(metadata))["compute"]

    def test_declared_fp_controls_survive_header_profile(self):
        cases = [(0xc0, False, False)]
        cases += [(0xc0 ^ (1 << bit), False, False) for bit in range(8)]
        cases += [(0xc0, True, False), (0xc0, False, True), (0xff, True, True),
                  (0, False, False)]
        for float_mode, ieee_mode, dx10_clamp in cases:
            # FP fields follow pm4.h's existing RSRC1 layout. Low VGPR bits
            # and DEBUG_MODE deliberately differ from the FP controls.
            rsrc1 = ((float_mode << 12) | (int(ieee_mode) << 23) |
                     (int(dx10_clamp) << 21) | (1 << 22) | 0x15)
            state = self.profile([rsrc1]).get("initial_fp_state")
            self.assertEqual(state, {"known": True, "float_mode": float_mode,
                                     "ieee_mode": ieee_mode, "dx10_clamp": dx10_clamp},
                             f"declared RSRC1 FP state lost: 0x{rsrc1:08x}")
            self.assertIs(type(state["known"]), bool)
            self.assertIs(type(state["ieee_mode"]), bool)
            self.assertIs(type(state["dx10_clamp"]), bool)
            self.assertIs(type(state["float_mode"]), int)

    def test_missing_rsrc1_does_not_invent_known_fp_state(self):
        state = self.profile([]).get("initial_fp_state")
        self.assertIsInstance(state, dict,
                              "missing RSRC1 must retain an explicit unknown FP state")
        self.assertIs(state.get("known"), False)

    def test_duplicate_rsrc1_rejects_ambiguous_fp_state(self):
        with self.assertRaises(ValueError):
            self.profile([0xc0 << 12, 0xc1 << 12])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--extractor", type=Path,
                        default=Path(__file__).resolve().parents[1] /
                                "tools" / "extract-shader-corpus.py")
    options, remaining = parser.parse_known_args()
    ComputeHeaderFloatingPointTests.extractor = load_extractor(options.extractor)
    unittest.main(argv=[sys.argv[0], *remaining])
