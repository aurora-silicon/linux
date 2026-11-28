// SPDX-License-Identifier: GPL-2.0-only OR MIT
static const char original[] = "previous validated generation";
static const char header[] = "0123456789abcdef";
static const char body[] = "replacement opaque biometric generation";
static const char path[] = "/var/lib/apple-sep-catacomb-user.bin";
static const char master_path[] = "/var/lib/apple-sep-catacomb-master.bin";
static const char owner_path[] = "/var/lib/apple-sep-catacomb-owner.bin";
static int passed;
static void setup(void)
{
	fixture_reset();
	fixture_set(original, sizeof(original));
}
static void unchanged(void)
{
	assert(slots[SLOT_USER].exists &&
	       slots[SLOT_USER].file.len == sizeof(original) &&
	       !memcmp(slots[SLOT_USER].file.bytes, original, sizeof(original)));
	assert(!renames && !dir_syncs);
}
static void complete(void)
{
	assert(slots[SLOT_USER].exists &&
	       slots[SLOT_USER].file.len == sizeof(header) - 1 + sizeof(body));
	assert(!memcmp(slots[SLOT_USER].file.bytes, header,
		       sizeof(header) - 1));
	assert(!memcmp(slots[SLOT_USER].file.bytes + sizeof(header) - 1, body,
		       sizeof(body)));
}
static int replace(void)
{
	return sep_store_replace(path, header, sizeof(header) - 1, body,
				 sizeof(body));
}
static void published_mode(umode_t mode)
{
	assert(slots[SLOT_USER].dentry.inode.i_mode == (S_IFREG | mode));
}
static void done(const char *name)
{
	fixture_done();
	passed++;
	printf("PASS %s\n", name);
}
int main(void)
{
	struct {
		int stage, error;
		const char *name;
	} failures[] = {
		{ ALLOC, ENOMEM, "parent allocation" },
		{ CRED, ENOMEM, "kernel credential allocation" },
		{ PARENT_OPEN, EACCES, "parent open denied" },
		{ PARENT_OPEN, ENOENT, "parent missing" },
		{ TEMP_OPEN, ENOSPC, "temporary create ENOSPC" },
		{ TEMP_OPEN, EROFS, "temporary create EROFS" },
		{ CREATOR_MAP, EOVERFLOW, "temporary create unmapped creator ID" },
		{ TEMP_SYNC, EIO, "temporary fsync EIO" },
		{ TEMP_SYNC, ENOSPC, "temporary fsync ENOSPC" },
		{ MOUNT, EROFS, "mount write denied" },
		{ RENAME_LOCK, ENOMEM, "rename lookup allocation" },
		{ SECURITY_RENAME, EPERM, "path security rejects rename" },
		{ RENAME, EIO, "vfs rename EIO" },
		{ RENAME, EWOULDBLOCK, "vfs rename delegation" },
		{ RENAME, EBUSY, "vfs rename mountpoint" },
	};
	for (size_t i = 0; i < sizeof(failures) / sizeof(*failures); i++) {
		setup();
		fixture_fault(failures[i].stage, failures[i].error);
		assert(replace() == -failures[i].error);
		unchanged();
		done(failures[i].name);
	}
	setup();
	fixture_fault(DIR_NO_FSYNC, 1);
	assert(replace() == -EINVAL);
	unchanged();
	assert(!attempts && !temp_present && !events[0]);
	done("parent without directory fsync refused before temp create");
	setup();
	fixture_fault(DIR_NO_RENAME, 1);
	assert(replace() == -EPERM);
	unchanged();
	assert(!attempts && !temp_present && !events[0]);
	done("parent without rename callback refused before temp create");
	setup();
	fixture_write_fault(0, EIO, 0);
	assert(replace() == -EIO);
	unchanged();
	assert(!temp_present);
	done("header write EIO");
	setup();
	fixture_write_fault(7, ENOSPC, 0);
	assert(replace() == -ENOSPC);
	unchanged();
	done("partial header then ENOSPC");
	setup();
	fixture_write_fault(16, EIO, 0);
	assert(replace() == -EIO);
	unchanged();
	done("payload write EIO");
	setup();
	fixture_write_fault(22, EIO, 0);
	assert(replace() == -EIO);
	unchanged();
	done("partial payload then EIO");
	setup();
	fixture_write_fault(0, 0, 1);
	assert(replace() == -EIO);
	unchanged();
	done("zero-progress header");
	setup();
	fixture_write_fault(18, 0, 1);
	assert(replace() == -EIO);
	unchanged();
	done("zero-progress payload");
	setup();
	fixture_fault(TARGET_TYPE, 2);
	assert(replace() == -EINVAL);
	unchanged();
	done("directory target refused");
	setup();
	fixture_fault(TARGET_TYPE, 3);
	assert(replace() == -EINVAL);
	unchanged();
	done("symlink target refused");
	setup();
	fixture_fault(STALE_SOURCE, 1);
	assert(replace() == -EINVAL);
	unchanged();
	assert(temp_present && cleanup_warning);
	done("source moved; cleanup does not remove reused name");
	for (int i = CLEANUP_LOCK; i <= UNLINK; i++) {
		setup();
		fixture_write_fault(16, EIO, 0);
		fixture_fault(i, EPERM);
		assert(replace() == -EIO);
		unchanged();
		assert(temp_present && cleanup_warning);
		done(i == CLEANUP_LOCK	  ? "cleanup lookup failure" :
		     i == SECURITY_UNLINK ? "cleanup security failure" :
					    "cleanup unlink failure");
	}
	setup();
	fixture_collisions(1);
	assert(!replace());
	complete();
	assert(attempts == 2 && stale_unlinks == 1 &&
	       !slots[SLOT_USER].stale_present);
	assert(strstr(events, "SC"));
	done("interrupted temporary removed, never adopted");
	setup();
	fixture_collisions(2);
	assert(replace() == -EEXIST);
	unchanged();
	assert(attempts == 2 && stale_unlinks == 1 && !temp_present);
	done("temporary name recreated by another writer");
	setup();
	fixture_collisions(1);
	fixture_fault(STALE_REMOVE, EPERM);
	assert(replace() == -EPERM);
	unchanged();
	assert(attempts == 1 && !stale_unlinks &&
	       slots[SLOT_USER].stale_present && !temp_present);
	done("interrupted temporary removal denied");
	setup();
	fixture_collisions(1);
	fixture_fault(STALE_TYPE, 3);
	assert(replace() == -EEXIST);
	unchanged();
	assert(attempts == 1 && !stale_unlinks &&
	       slots[SLOT_USER].stale_present && !temp_present);
	done("non-regular entry at the temporary name left in place");
	setup();
	fixture_collisions(1);
	fixture_fault(STALE_SECURITY, EPERM);
	assert(replace() == -EPERM);
	unchanged();
	assert(attempts == 1 && !stale_unlinks &&
	       slots[SLOT_USER].stale_present && !temp_present);
	done("interrupted temporary unlink denied by security hook");
	setup();
	fixture_collisions(1);
	fixture_fault(STALE_UNLINK, EIO);
	assert(replace() == -EIO);
	unchanged();
	assert(attempts == 1 && !stale_unlinks &&
	       slots[SLOT_USER].stale_present && !temp_present);
	done("interrupted temporary unlink fails");
	setup();
	fixture_collisions(1);
	fixture_stale_vanish();
	assert(!replace());
	complete();
	assert(attempts == 2 && !stale_unlinks && !slots[SLOT_USER].stale_present);
	done("interrupted temporary gone before removal, created again");
	setup();
	fixture_fault(TEMP_CLOSE, EIO);
	assert(replace() == -EIO);
	unchanged();
	assert(!temp_present && unlinks == 1 && temp_syncs == 1);
	assert(strstr(events, "FLU"));
	done("temporary close error unlinks it before rename");
	{
		static const umode_t masks[] = { 0777, 0277 };

		for (size_t i = 0; i < sizeof(masks) / sizeof(*masks); i++) {
			setup();
			fixture_umask(masks[i]);
			assert(!replace());
			complete();
			published_mode(0600);
			assert(chmod_calls == 1 && strstr(events, "CMW"));
			done(i ? "umask 0277 temporary published 0600" :
				 "umask 0777 temporary published 0600");
		}
	}
	setup();
	fixture_umask(0022);
	assert(!replace());
	complete();
	published_mode(0600);
	assert(!chmod_calls && !strchr(events, 'M'));
	done("umask 0022 temporary published 0600 without chmod");
	setup();
	fixture_umask(0777);
	fixture_fault(TEMP_CHMOD, EACCES);
	assert(replace() == -EACCES);
	unchanged();
	assert(!chmod_calls && !write_calls && !temp_present && unlinks == 1);
	done("temporary chmod failure unlinks it before writing");
	{
		struct {
			int stage, error, expect;
			const char *name;
		} checks[] = {
			{ 0, 0, 0, "replaceable destination admitted" },
			{ DEST_MOUNT, 1, EBUSY, "bind-mounted destination refused" },
			{ DEST_TYPE, 3, EINVAL, "symlink destination refused as non-regular" },
			{ DEST_TYPE, 2, EINVAL, "directory destination refused" },
			{ DEST_LOOKUP, EACCES, EACCES, "destination lookup error refused" },
			{ DEST_MAY_DELETE, EPERM, EPERM,
			  "immutable or sticky destination refused" },
			{ MOUNT, EROFS, EROFS, "read-only parent mount refused" },
			{ DIR_NO_FSYNC, 1, EINVAL, "parent without directory fsync refused" },
			{ DIR_NO_RENAME, 1, EPERM, "parent without rename callback refused" },
			{ CREATOR_MAP, 1, EOVERFLOW, "unmapped creator ID refused" },
			{ DIR_PERM, EACCES, EACCES, "parent without write access refused" },
			{ DIR_APPEND, 1, EPERM, "append-only parent refused" },
			{ DIR_DEAD, 1, ENOENT, "dead parent refused" },
			{ PARENT_OPEN, ENOENT, ENOENT, "missing parent refused" },
			{ ALLOC, ENOMEM, ENOMEM, "replaceable parent allocation failure" },
			{ CRED, ENOMEM, ENOMEM, "replaceable credential allocation failure" },
		};
		for (size_t i = 0; i < sizeof(checks) / sizeof(*checks); i++) {
			setup();
			if (checks[i].stage)
				fixture_fault(checks[i].stage, checks[i].error);
			assert(sep_store_replaceable(path) == -checks[i].expect);
			unchanged();
			assert(!attempts && !temp_present && !stale_unlinks &&
			       !events[0]);
			done(checks[i].name);
		}
	}
	setup();
	fixture_slot_stale(SLOT_USER, 1, 1, 0, 0, 0, 0);
	assert(sep_store_replaceable(path) == 0);
	unchanged();
	assert(slots[SLOT_USER].stale_present && !stale_unlinks && !events[0]);
	done("replaceable admits removable regular reserved file without unlinking it");
	setup();
	fixture_slot_stale(SLOT_USER, 1, 2, 0, 0, 0, 0);
	assert(sep_store_replaceable(path) == -EEXIST);
	unchanged();
	assert(slots[SLOT_USER].stale_present && !stale_unlinks && !events[0]);
	done("replaceable refuses directory at reserved temporary name");
	setup();
	fixture_slot_stale(SLOT_USER, 1, 3, 0, 0, 0, 0);
	assert(sep_store_replaceable(path) == -EEXIST);
	unchanged();
	assert(slots[SLOT_USER].stale_present && !stale_unlinks && !events[0]);
	done("replaceable refuses symlink at reserved temporary name");
	setup();
	fixture_slot_stale(SLOT_USER, 1, 1, 1, 0, 0, 0);
	assert(sep_store_replaceable(path) == -EBUSY);
	unchanged();
	assert(slots[SLOT_USER].stale_present && !stale_unlinks && !events[0]);
	done("replaceable refuses bind-mounted reserved temporary name");
	setup();
	fixture_slot_stale(SLOT_USER, 1, 1, 0, EPERM, 0, 0);
	assert(sep_store_replaceable(path) == -EPERM);
	unchanged();
	assert(slots[SLOT_USER].stale_present && !stale_unlinks && !events[0]);
	done("replaceable refuses unremovable reserved regular file");
	setup();
	fixture_slot_stale(SLOT_USER, 1, 1, 0, 0, EPERM, 0);
	assert(sep_store_replaceable(path) == -EPERM);
	unchanged();
	assert(slots[SLOT_USER].stale_present && !stale_unlinks && !events[0]);
	done("replaceable refuses reserved file denied by security_path_unlink");
	setup();
	fixture_fault(DIR_NO_UNLINK, 1);
	assert(sep_store_replaceable(path) == 0);
	unchanged();
	assert(!stale_unlinks && !events[0]);
	done("replaceable ignores missing unlink callback when reserved name is free");
	setup();
	fixture_fault(DIR_NO_UNLINK, 1);
	fixture_slot_stale(SLOT_USER, 1, 1, 0, 0, 0, 0);
	assert(sep_store_replaceable(path) == -EPERM);
	unchanged();
	assert(slots[SLOT_USER].stale_present && !stale_unlinks && !events[0]);
	done("replaceable refuses reserved file when parent lacks unlink callback");
	setup();
	fixture_fault(STALE_REMOVE, EACCES);
	assert(sep_store_replaceable(path) == -EACCES);
	unchanged();
	assert(!stale_unlinks && !events[0]);
	done("replaceable propagates reserved lookup error");
	setup();
	assert(sep_store_replaceable("invalid") == -EINVAL);
	unchanged();
	done("replaceable rejects invalid path");
	setup();
	fixture_set_slot(SLOT_MASTER, original, sizeof(original));
	fixture_slot_mountpoint(SLOT_MASTER, 1);
	assert(sep_store_replaceable(path) == 0);
	assert(sep_store_replaceable(master_path) == -EBUSY);
	unchanged();
	done("replaceable checks the requested basename, not another slot");
	fixture_reset();
	assert(sep_store_replaceable(path) == -ENOENT);
	assert(!attempts && !events[0]);
	done("missing destination refused by replaceable");
	/* sep_store_publishable(): destination may be absent (fresh owner/master). */
	fixture_reset();
	assert(sep_store_publishable(owner_path) == 0);
	assert(!slots[SLOT_OWNER].exists && !attempts && !events[0]);
	done("publishable admits absent destination under healthy parent");
	fixture_reset();
	fixture_fault(DIR_APPEND, 1);
	assert(sep_store_publishable(owner_path) == -EPERM);
	assert(!slots[SLOT_OWNER].exists && !events[0]);
	done("publishable refuses absent destination under append-only parent");
	fixture_reset();
	fixture_fault(DIR_DEAD, 1);
	assert(sep_store_publishable(owner_path) == -ENOENT);
	assert(!slots[SLOT_OWNER].exists && !events[0]);
	done("publishable refuses absent destination under dead parent");
	fixture_reset();
	fixture_fault(DIR_PERM, EACCES);
	assert(sep_store_publishable(owner_path) == -EACCES);
	assert(!slots[SLOT_OWNER].exists && !events[0]);
	done("publishable refuses absent destination under unwritable parent");
	fixture_reset();
	fixture_fault(MOUNT, EROFS);
	assert(sep_store_publishable(owner_path) == -EROFS);
	assert(!slots[SLOT_OWNER].exists && !events[0]);
	done("publishable refuses absent destination on read-only mount");
	fixture_reset();
	fixture_fault(CREATOR_MAP, 1);
	assert(sep_store_publishable(owner_path) == -EOVERFLOW);
	assert(!slots[SLOT_OWNER].exists && !events[0]);
	done("publishable refuses absent destination with unmapped creator ID");
	fixture_reset();
	fixture_fault(DIR_NO_FSYNC, 1);
	assert(sep_store_publishable(owner_path) == -EINVAL);
	assert(!slots[SLOT_OWNER].exists && !events[0]);
	done("publishable refuses absent destination without directory fsync");
	fixture_reset();
	fixture_fault(DIR_NO_RENAME, 1);
	assert(sep_store_publishable(owner_path) == -EPERM);
	assert(!slots[SLOT_OWNER].exists && !events[0]);
	done("publishable refuses absent destination without rename callback");
	fixture_reset();
	fixture_slot_stale(SLOT_OWNER, 1, 2, 0, 0, 0, 0);
	assert(sep_store_publishable(owner_path) == -EEXIST);
	assert(!slots[SLOT_OWNER].exists && slots[SLOT_OWNER].stale_present &&
	       !stale_unlinks && !events[0]);
	done("publishable refuses absent destination with non-regular reserved temp");
	fixture_reset();
	fixture_set_slot(SLOT_MASTER, original, sizeof(original));
	fixture_slot_mountpoint(SLOT_MASTER, 1);
	assert(sep_store_publishable(master_path) == -EBUSY);
	assert(!events[0]);
	done("publishable refuses existing bind-mounted destination");
	setup();
	fixture_short(3);
	assert(!replace());
	complete();
	assert(write_calls > 2 && !temp_present && temp_syncs == 1 &&
	       dir_syncs == 1);
	assert(strstr(events, "FLRD"));
	done("short writes loop to durable replacement");
	setup();
	fixture_fault(DIR_SYNC, EIO);
	assert(replace() == -EIO);
	complete();
	assert(renames == 1 && !unlinks && commit_warning);
	done("directory fsync failure leaves complete new generation visible");
	setup();
	fixture_fault(DIR_SYNC, EIO);
	assert(replace() == -EIO);
	fixture_fault(DIR_SYNC, 0);
	assert(!replace());
	complete();
	assert(renames == 2 && !temp_present);
	done("retry after ambiguous directory fsync");
	fixture_reset();
	assert(!replace());
	complete();
	done("first creation durable");
	setup();
	assert(sep_store_replace("invalid", header, 16, body, sizeof(body)) ==
	       -EINVAL);
	unchanged();
	done("invalid path rejected before writes");
	{
		struct {
			int stage, error, expect;
			const char *name;
		} syncs[] = {
			{ 0, 0, 0, "parent sync of an open store file" },
			{ DIR_NO_FSYNC, 1, EINVAL, "parent sync without directory fsync" },
			{ DIR_SYNC, EIO, EIO, "parent sync EIO" },
			{ PARENT_OPEN, EACCES, EACCES, "parent sync open denied" },
			{ CRED, ENOMEM, ENOMEM, "parent sync credential allocation" },
		};

		for (size_t i = 0; i < sizeof(syncs) / sizeof(*syncs); i++) {
			setup();
			if (syncs[i].stage)
				fixture_fault(syncs[i].stage, syncs[i].error);
			slots[SLOT_USER].file.refs++;
			assert(sep_store_sync_parent(&slots[SLOT_USER].file) ==
			       -syncs[i].expect);
			slots[SLOT_USER].file.refs--;
			assert(!renames && !attempts &&
			       dir_syncs == (!syncs[i].stage ||
					     syncs[i].stage == DIR_SYNC));
			done(syncs[i].name);
		}
	}
	printf("C cases: %d passed, 0 failed\n", passed);
	return 0;
}
