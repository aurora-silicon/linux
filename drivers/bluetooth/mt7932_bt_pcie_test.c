// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the checks mt7932_bt_pcie applies to HCI packets and to the
 * board file before either reaches the controller or the HCI core. Built as
 * part of mt7932_bt_pcie.c, so the tests reach its static helpers.
 */
#include <kunit/test.h>

static void bt7932_hci_length_command_test(struct kunit *test)
{
	/* HCI_Reset (0x0c03) and a command carrying two parameter bytes. */
	static const u8 reset[] = { 0x03, 0x0c, 0x00 };
	static const u8 command[] = { 0x1a, 0xfc, 0x02, 0x11, 0x22, 0x33 };

	KUNIT_EXPECT_TRUE(test, bt7932_hci_length(HCI_COMMAND_PKT, reset, sizeof(reset)));
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_COMMAND_PKT, reset, 2));
	KUNIT_EXPECT_TRUE(test, bt7932_hci_length(HCI_COMMAND_PKT, command, 5));
	/* One byte short of, and one byte past, the declared length. */
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_COMMAND_PKT, command, 4));
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_COMMAND_PKT, command, 6));
}

static void bt7932_hci_length_event_test(struct kunit *test)
{
	/* Command Complete for HCI_Reset with status 0, then one spare byte. */
	static const u8 event[] = { 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00, 0xff };

	KUNIT_EXPECT_TRUE(test, bt7932_hci_length(HCI_EVENT_PKT, event, 6));
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_EVENT_PKT, event, 5));
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_EVENT_PKT, event, 7));
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_EVENT_PKT, event, 1));
}

static void bt7932_hci_length_data_test(struct kunit *test)
{
	/* ACL and ISO lengths are 16-bit; ISO keeps two flag bits on top. */
	static const u8 acl[] = { 0x01, 0x20, 0x02, 0x00, 0xaa, 0xbb };
	static const u8 sco[] = { 0x01, 0x00, 0x01, 0xcc };
	static const u8 iso[] = { 0x01, 0x00, 0x01, 0xc0, 0xdd };
	static const u8 long_acl[] = { 0x01, 0x20, 0x00, 0x01 };

	KUNIT_EXPECT_TRUE(test, bt7932_hci_length(HCI_ACLDATA_PKT, acl, sizeof(acl)));
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_ACLDATA_PKT, acl, 5));
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_ACLDATA_PKT, acl, 3));
	/* A 256-byte ACL payload must not be satisfied by its header alone. */
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_ACLDATA_PKT, long_acl, sizeof(long_acl)));
	KUNIT_EXPECT_TRUE(test, bt7932_hci_length(HCI_SCODATA_PKT, sco, sizeof(sco)));
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_SCODATA_PKT, sco, 3));
	KUNIT_EXPECT_TRUE(test, bt7932_hci_length(HCI_ISODATA_PKT, iso, sizeof(iso)));
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_ISODATA_PKT, iso, 4));
}

static void bt7932_hci_length_type_test(struct kunit *test)
{
	static const u8 packet[] = { 0x00, 0x00, 0x00, 0x00 };

	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(HCI_VENDOR_PKT, packet, sizeof(packet)));
	KUNIT_EXPECT_FALSE(test, bt7932_hci_length(0, packet, sizeof(packet)));
}

#define BT7932_TEST_PTX_SIZE 198

/* A well-formed container with synthetic record contents. */
static void bt7932_test_ptx(u8 *data)
{
	static const u32 types[] = { 0x101, 0x201, 0x301, 0x401 };
	static const u32 sizes[] = { 14, 44, 44, 0 };
	u32 offset = 96, sum, i, j;

	memset(data, 0, BT7932_TEST_PTX_SIZE);
	memcpy(data, "BLOB", 4);
	put_unaligned_le32(96, data + 4);
	put_unaligned_le16(1, data + 8);
	put_unaligned_le16(ARRAY_SIZE(types), data + 10);
	for (i = 0; i < ARRAY_SIZE(types); i++) {
		u8 *record = data + 16 + i * 20;

		for (j = 0, sum = 0; j < sizes[i]; j++) {
			data[offset + j] = 0x40 + i * 16 + j;
			sum += data[offset + j];
		}
		put_unaligned_le32(types[i], record);
		put_unaligned_le32(offset, record + 4);
		put_unaligned_le32(sizes[i], record + 8);
		put_unaligned_le32(sum, record + 12);
		offset += sizes[i];
	}
}

static void bt7932_validate_ptx_test(struct kunit *test)
{
	u8 *data = kunit_kzalloc(test, BT7932_TEST_PTX_SIZE + 1, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, data);
	bt7932_test_ptx(data);
	KUNIT_EXPECT_EQ(test, bt7932_validate_ptx(data, BT7932_TEST_PTX_SIZE), 0);
	KUNIT_EXPECT_EQ(test, bt7932_validate_ptx(data, BT7932_TEST_PTX_SIZE - 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, bt7932_validate_ptx(data, BT7932_TEST_PTX_SIZE + 1), -EINVAL);

	/* A payload byte that no longer matches its record's checksum. */
	data[120]++;
	KUNIT_EXPECT_EQ(test, bt7932_validate_ptx(data, BT7932_TEST_PTX_SIZE), -EINVAL);

	bt7932_test_ptx(data);
	data[0] = 'b';
	KUNIT_EXPECT_EQ(test, bt7932_validate_ptx(data, BT7932_TEST_PTX_SIZE), -EINVAL);

	/* A record of the wrong type in the first slot. */
	bt7932_test_ptx(data);
	put_unaligned_le32(0x201, data + 16);
	KUNIT_EXPECT_EQ(test, bt7932_validate_ptx(data, BT7932_TEST_PTX_SIZE), -EINVAL);

	/* A record length other than the one its type has. */
	bt7932_test_ptx(data);
	put_unaligned_le32(45, data + 16 + 2 * 20 + 8);
	KUNIT_EXPECT_EQ(test, bt7932_validate_ptx(data, BT7932_TEST_PTX_SIZE), -EINVAL);
}

static struct kunit_case bt7932_test_cases[] = {
	KUNIT_CASE(bt7932_hci_length_command_test),
	KUNIT_CASE(bt7932_hci_length_event_test),
	KUNIT_CASE(bt7932_hci_length_data_test),
	KUNIT_CASE(bt7932_hci_length_type_test),
	KUNIT_CASE(bt7932_validate_ptx_test),
	{}
};

static struct kunit_suite bt7932_test_suite = {
	.name = "mt7932-bt-pcie",
	.test_cases = bt7932_test_cases,
};

kunit_test_suite(bt7932_test_suite);
