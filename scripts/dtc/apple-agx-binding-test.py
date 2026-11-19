#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Validate G17 GPU compatibility and resource contracts with the real binding."""

from pathlib import Path
import unittest

from dtschema import DTValidator

ROOT = Path(__file__).resolve().parents[2]
BINDING = ROOT / "Documentation/devicetree/bindings/gpu/apple,agx.yaml"


def g17c(fallback=False):
    compatible = ["apple,agx-g17c", "apple,agx-g17s"]
    if fallback:
        compatible.append("apple,agx-t6050")
    return {
        "compatible": compatible,
        "reg": [[0x23, 0x2600000, 0, 0x88000], [0x23, 0, 0, 0x4000000]],
        "reg-names": ["asc", "sgx"],
        "mboxes": [[1], [2]],
        "power-domains": [[3]],
        "memory-region": [[4], [5], [6], [7], [8], [9]],
        "memory-region-names": [
            "ttbs", "pagetables", "handoff", "fw-data", "l2-pagetables", "fw-text"
        ],
        "operating-points-v2": [[10]],
        "apple,cs-opp": [[11]],
        "apple,afr-opp": [[12]],
        "apple,csafr-min-sram-microvolt": [0],
        "apple,aon-ptd": [0] * 9,
        "status": ["disabled"],
    }


def raw_t6050():
    return {
        "compatible": ["apple,agx-t6050"],
        "reg": [[0x23, 0, 0, 0x4000000], [0x23, 0x2600000, 0, 0x88000]],
        "interrupts": [[0, 0, irq, 4] for irq in range(8)],
        "power-domains": [[1]],
        "status": ["disabled"],
    }


class GPUContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.validator = DTValidator([str(BINDING)])

    def errors(self, node):
        return list(self.validator.iter_errors(node, filter=["gpu/apple,agx.yaml"]))

    def assert_valid(self, node):
        errors = self.errors(node)
        self.assertFalse(errors, "\n".join(error.message for error in errors))

    def assert_invalid(self, node, property_name):
        errors = self.errors(node)
        self.assertTrue(errors, "invalid GPU description unexpectedly accepted")
        def context(error):
            yield error
            for child in error.context:
                yield from context(child)

        details = [child for error in errors for child in context(error)]
        self.assertTrue(
            any(property_name in error.path or property_name in error.message for error in details),
            "\n".join(error.message for error in errors),
        )

    def test_g17c_modern_and_loader_fallback(self):
        for fallback in (False, True):
            with self.subTest(fallback=fallback):
                self.assert_valid(g17c(fallback))

    def test_compatibles_are_exact_and_ordered(self):
        for compatible in (
            ["apple,agx-g17c", "apple,agx-g17s", "apple,agx-t8140"],
            ["apple,agx-g17s", "apple,agx-g17c", "apple,agx-t6050"],
            ["apple,agx-g17c", "apple,agx-t6050"],
            ["apple,agx-g17c", "apple,agx-g17s", "apple,agx-t6050", "apple,agx-g13s"],
        ):
            with self.subTest(compatible=compatible):
                node = g17c(True)
                node["compatible"] = compatible
                self.assert_invalid(node, "compatible")

    def test_g17c_resource_contract_is_not_relaxed(self):
        for fallback in (False, True):
            for name, value in (
                ("reg", [[0x23, 0, 0, 0x4000000]]),
                ("reg-names", ["sgx", "asc"]),
                ("mboxes", [[1]]),
                ("memory-region", [[4], [5], [6]]),
                ("memory-region-names", ["ttbs", "pagetables", "handoff"]),
            ):
                with self.subTest(fallback=fallback, property=name):
                    node = g17c(fallback)
                    node[name] = value
                    self.assert_invalid(node, name)

    def test_required_g17c_handoff_properties_remain_required(self):
        for fallback in (False, True):
            for name in (
                "power-domains", "operating-points-v2", "apple,cs-opp", "apple,afr-opp",
                "apple,csafr-min-sram-microvolt", "apple,aon-ptd",
            ):
                with self.subTest(fallback=fallback, property=name):
                    node = g17c(fallback)
                    del node[name]
                    self.assert_invalid(node, name)

    def test_g17s_existing_single_register_form(self):
        node = g17c()
        node["compatible"] = ["apple,agx-g17s"]
        node["reg"] = [[0x23, 0, 0, 0x4000000]]
        node["reg-names"] = ["sgx"]
        for name in (
            "operating-points-v2", "apple,cs-opp", "apple,afr-opp",
            "apple,csafr-min-sram-microvolt", "apple,aon-ptd",
        ):
            del node[name]
        self.assert_valid(node)

    def test_raw_t6050_remains_disabled_with_interrupt_inventory(self):
        self.assert_valid(raw_t6050())
        for name, value in (("status", ["okay"]), ("interrupts", [[0, 0, 1, 4]])):
            with self.subTest(property=name):
                node = raw_t6050()
                node[name] = value
                self.assert_invalid(node, name)

    def test_raw_t6050_does_not_accept_coprocessor_only_handoff(self):
        node = g17c()
        node["compatible"] = ["apple,agx-t6050"]
        self.assert_invalid(node, "interrupts")


if __name__ == "__main__":
    unittest.main()
