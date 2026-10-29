#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check enabled guest peripheral drivers against a resolved kernel config."""
import argparse
import json
from pathlib import Path
import re
import libfdt

# This is a reviewed mapping of guest consumers, not a replacement for Kconfig.
# An unlisted compatible is reported as unaudited, never as unsupported.
DRIVERS = {
    "aurora,apif-mmio": ("AURORA_APIF_MMIO",),
    "apple,dart-apif": ("APPLE_DART_APIF", "AURORA_APIF_MMIO"),
    "apple,sart-apif": ("APPLE_SART", "APPLE_SART_APIF", "AURORA_APIF_MMIO"),
    "apple,nvme-apif": ("NVME_APPLE", "AURORA_APIF_MMIO"),
    "apple,apif-pcie": ("PCIE_APPLE_APIF", "PCI", "PCI_MSI"),
    "apple,t8140-qspi": ("SPI", "SPI_MEM", "SPI_APPLE_QSPI"),
    "apple,t6050-qspi": ("SPI", "SPI_MEM", "SPI_APPLE_QSPI"),
    "jedec,spi-nor": ("MTD", "MTD_SPI_NOR", "SPI_MEM"),
    "fixed-partitions": ("MTD", "MTD_OF_PARTS"),
    "fixed-clock": ("COMMON_CLK",),
    "syscon": ("MFD_SYSCON",),
    "apple,asc-mailbox-v4": ("APPLE_MAILBOX",),
    "apple,t6050-ascwrap-v6": ("APPLE_MAILBOX",),
    "apple,t6050-x1n1-ascwrap-v6": ("APPLE_MAILBOX",),
    "apple,t6050-pmp-v2": ("APPLE_PMP",),
    "apple,t6050-dwc3": ("USB_DWC3", "USB_DWC3_APPLE", "USB_XHCI_HCD", "USB_XHCI_PLATFORM", "USB_ROLE_SWITCH"),
    "apple,t6050-spmi": ("SPMI", "SPMI_APPLE"),
    "apple,sn201202x": ("TYPEC", "TYPEC_SN201202X", "TYPEC_TPS6598X_CORE"),
    "apple,t8142-display-crossbar": ("MULTIPLEXER", "MUX_APPLE_DPXBAR"),
    "apple,t6050-combo-phy": ("PHY_APPLE_T6050_DP", "TYPEC", "APPLE_RTKIT", "RESET_CONTROLLER"),
    "apple,t6050-dp-phy": ("PHY_APPLE_T6050_DP",),
    "apple,t6050-dcp": ("DRM", "DRM_APPLE", "APPLE_RTKIT"),
    "apple,t6050-dcpext": ("DRM", "DRM_APPLE", "APPLE_RTKIT"),
    "apple,display-subsystem": ("DRM_APPLE",),
    "apple,pinctrl": ("PINCTRL", "PINCTRL_APPLE_GPIO"),
    "apple,pmgr-pwrstate": ("APPLE_PMGR_PWRSTATE", "PM_GENERIC_DOMAINS"),
    "apple,t8103-pmgr-pwrstate": ("APPLE_PMGR_PWRSTATE", "PM_GENERIC_DOMAINS"),
    "apple,t8103-pmgr": ("APPLE_PMGR_MISC",),
    "apple,dockchannel": ("APPLE_DOCKCHANNEL",),
    "apple,dockchannel-hid": ("HID_DOCKCHANNEL",),
    "apple,rtk-helper-asc4": ("APPLE_RTKIT_HELPER",),
    "apple,s5l-fpwm": ("PWM", "PWM_APPLE"),
    "pwm-leds": ("LEDS_CLASS", "LEDS_PWM", "PWM"),
    "apple,wdt": ("WATCHDOG", "APPLE_WATCHDOG"),
    "apple,t6050-cluster-cpufreq": ("CPU_FREQ", "ARM_APPLE_SOC_CPUFREQ"),
    "apple,t6050-smc": ("MFD_MACSMC",),
    "apple,smc-pcio-gpio": ("GPIO_MACSMC",),
    "apple,smc-hwmon": ("SENSORS_MACSMC_HWMON",),
    "apple,t8122-aic3": ("APPLE_AIC",),
    "arm,armv8-timer": ("ARM_ARCH_TIMER",),
    "apple,s5l-uart": ("SERIAL_SAMSUNG", "SERIAL_SAMSUNG_CONSOLE"),
    "pci106b,1903": ("BT", "BT_HCICENTAURI", "APPLE_CENTAURI"),
    "pci106b,1902": ("APPLE_CENTAURI_WIFI", "APPLE_CENTAURI"),
    "gpu,t6050": ("DRM_ASAHI", "AURORA_APIF_GPU"),
    "apple,agx-t6050": ("DRM_ASAHI", "AURORA_APIF_GPU"),
}
# Declarative/container nodes whose compatible does not imply another driver.
METADATA = {"fixed-clock", "simple-bus", "simple-mfd", "syscon", "operating-points-v2",
            "arm,armv8", "framebuffer", "usb-c-connector", "apple,panel-mini-led"}


def parse_config(text):
    return dict(re.findall(r"^(CONFIG_[A-Za-z0-9_]+)=(.+)$", text, re.M))


def strings(tree, node, name):
    value = tree.getprop(node, name, quiet=(libfdt.NOTFOUND,))
    return [] if isinstance(value, int) else bytes(value).rstrip(b"\0").decode().split("\0")


def audit(blob, config, builtin=False):
    tree = libfdt.Fdt(blob)
    values = parse_config(config)
    result = {"matched": [], "missing": [], "unaudited": []}
    accepted = ("y",) if builtin else ("y", "m")

    def visit(node, parent_enabled=True):
        status = strings(tree, node, "status")
        enabled = parent_enabled and (not status or status in (["okay"], ["ok"]))
        if enabled:
            compatibles = strings(tree, node, "compatible")
            matched = next((compatible for compatible in compatibles if compatible in DRIVERS), None)
            path = tree.get_path(node)
            if matched:
                symbols = {"CONFIG_" + symbol: values.get("CONFIG_" + symbol, "n")
                           for symbol in DRIVERS[matched]}
                result["matched"].append({"node": path, "compatible": matched, "symbols": symbols})
                for symbol, value in symbols.items():
                    if value not in accepted:
                        result["missing"].append({"node": path, "symbol": symbol, "value": value})
            elif node and compatibles and not any(c in METADATA for c in compatibles):
                result["unaudited"].append({"node": path, "compatible": compatibles})
        child = tree.first_subnode(node, quiet=(libfdt.NOTFOUND,))
        while child >= 0:
            visit(child, enabled)
            child = tree.next_subnode(child, quiet=(libfdt.NOTFOUND,))

    visit(0)
    return result


def validate(blob, config, builtin=False):
    result = audit(blob, config, builtin)
    if result["missing"]:
        missing = "; ".join(f"{entry['node']}: {entry['symbol']}={entry['value']}"
                            for entry in result["missing"])
        raise ValueError("enabled guest peripherals lack kernel support: " + missing)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dtb", type=Path)
    parser.add_argument("config", type=Path)
    parser.add_argument("--built-in", action="store_true", help="require y rather than accepting loadable modules")
    args = parser.parse_args()
    result = audit(args.dtb.read_bytes(), args.config.read_text(), args.built_in)
    print(json.dumps(result, indent=2))
    return bool(result["missing"])


if __name__ == "__main__":
    raise SystemExit(main())
