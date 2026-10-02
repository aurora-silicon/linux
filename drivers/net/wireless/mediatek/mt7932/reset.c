// SPDX-License-Identifier: GPL-2.0-only
/* J700 MT7932 mapped preparation and function-scoped reset.
 * Clean-room contracts: NEO_MT7932_DEVICE_RESET_UNLOAD_CONTRACT.md and
 * NEO_MT7932_FLR_PREPARATION_RESOLVED.md. No secondary-bus reset fallback.
 */
#include "mt7932.h"

int mt_dma_stop(struct mt7932 *m)
{
	u32 value;

	/* Call only after joining software producers and IRQ service. */
	mt_write(m, W + 0x204, 0);
	value = mt_read(m, W + 0x208);
	if (value == U32_MAX)
		return -ENODEV;
	mt_write(m, W + 0x208, value & ~0x18208005);
	return mt_poll(m, W + 0x208, 0x1820800f, 0, 100000);
}

static int mt_reset_identity(struct pci_dev *pdev)
{
	u32 id;

	if (pci_read_config_dword(pdev, PCI_VENDOR_ID, &id) ||
	    id != (0x7932 << 16 | PCI_VENDOR_ID_MEDIATEK))
		return -ENODEV;
	return 0;
}

int mt_function_reset(struct mt7932 *m)
{
	struct pci_dev *pdev = m->pdev;
	u32 fabric, aer_mask = 0;
	u16 command, status;
	int aer, ret;

	/* Probe/remove already hold the device lock. Serialize configuration
	 * access as PCI core reset does, but select FLR explicitly: the generic
	 * reset-method list can contain a bus reset with a different scope.
	 */
	device_lock_assert(&pdev->dev);
	ret = pcie_reset_flr(pdev, true);
	if (ret)
		return ret;
	ret = mt_reset_identity(pdev);
	if (ret)
		return ret;
	if (mt_read(m, 0xfe250) >> 16 != 0x7000)
		return -ENODEV;
	aer = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_ERR);
	if (aer && aer != 0x200)
		return -EOPNOTSUPP;
	pci_cfg_access_lock(pdev);
	if (aer) {
		ret = pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_MASK, &aer_mask);
		if (ret || aer_mask == U32_MAX) {
			ret = -EIO;
			goto unlock;
		}
		/* Preserve the OS mask and status evidence, not the vendor's global
		 * status-clear/enable literals. Only mask the reset-related errors.
		 */
		ret = pci_write_config_dword(pdev, aer + PCI_ERR_UNCOR_MASK,
					    aer_mask | 0x0057f010);
		if (ret) {
			ret = -EIO;
			goto unlock;
		}
	}
	mt_write(m, 0x73020, 0);
	usleep_range(1000, 1100);
	pci_clear_master(pdev);
	if (pci_read_config_word(pdev, PCI_COMMAND, &command) ||
	    command == U16_MAX || (command & PCI_COMMAND_MASTER)) {
		ret = -EIO;
		goto restore_aer;
	}
	if (!pci_wait_for_pending_transaction(pdev)) {
		ret = -ETIMEDOUT;
		goto restore_aer;
	}
	if (pcie_capability_read_word(pdev, PCI_EXP_DEVSTA, &status) ||
	    status == U16_MAX || (status & PCI_EXP_DEVSTA_TRPND)) {
		ret = -EIO;
		goto restore_aer;
	}
	ret = pci_save_state(pdev);
	if (ret)
		goto restore_aer;
	ret = pci_write_config_word(pdev, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE);
	if (!ret)
		ret = pcie_reset_flr(pdev, false);
	else
		ret = -EIO;
	/* Saved BME is already clear. Restore BAR/MSI state, never old DMA. */
	pci_restore_state(pdev);
	if (ret)
		goto restore_aer;
	ret = mt_reset_identity(pdev);
	if (ret)
		goto restore_aer;
	if (pci_read_config_word(pdev, PCI_COMMAND, &command) ||
	    command == U16_MAX || (command & PCI_COMMAND_MASTER) ||
	    pci_read_config_dword(pdev, 0x48c, &fabric) ||
	    fabric == U32_MAX || (fabric & 0x000f0000))
		ret = -EIO;
restore_aer:
	if (aer && pci_write_config_dword(pdev, aer + PCI_ERR_UNCOR_MASK, aer_mask))
		ret = ret ?: -EIO;
unlock:
	pci_cfg_access_unlock(pdev);
	return ret;
}
