/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SHMEM_FS_H
#define __SHMEM_FS_H

#include <linux/file.h>
#include <linux/swap.h>
#include <linux/mempolicy.h>
#include <linux/pagemap.h>
#include <linux/percpu_counter.h>
#include <linux/xattr.h>
#include <linux/fs_parser.h>
#include <linux/userfaultfd_k.h>
#include <linux/bits.h>

struct swap_iocb;

/* inode in-kernel data */

#ifdef CONFIG_TMPFS_QUOTA
#define SHMEM_MAXQUOTAS 2
#endif

/* Suppress pre-accounting of the entire object size. */
#define SHMEM_F_NORESERVE	BIT(0)
/* Disallow swapping. */
#define SHMEM_F_LOCKED		BIT(1)
/*
 * Disallow growing, shrinking, or hole punching in the inode. Combined with
 * folio pinning, makes sure the inode's mapping stays fixed.
 *
 * In some ways similar to F_SEAL_GROW | F_SEAL_SHRINK, but can be removed and
 * isn't directly visible to userspace.
 */
#define SHMEM_F_MAPPING_FROZEN	BIT(2)
/* This inode has used sub-native shared-page validity. */
#define SHMEM_F_UFFD_SUBPAGE	BIT(3)

struct shmem_inode_info {
	spinlock_t		lock;
	unsigned int		seals;		/* shmem seals */
	unsigned long		flags;
	unsigned long		alloced;	/* data pages alloced to file */
	unsigned long		swapped;	/* subtotal assigned to swap */
	union {
	    struct offset_ctx	dir_offsets;	/* stable directory offsets */
	    struct {
		struct list_head shrinklist;	/* shrinkable hpage inodes */
		struct list_head swaplist;	/* chain of maybes on swap */
	    };
	};
	struct timespec64	i_crtime;	/* file creation time */
	struct shared_policy	policy;		/* NUMA memory alloc policy */
	struct list_head        xattrs;		/* list of xattrs */
	pgoff_t			fallocend;	/* highest fallocate endindex */
	unsigned int		fsflags;	/* for FS_IOC_[SG]ETFLAGS */
	atomic_t		stop_eviction;	/* hold when working on inode */
#if defined(CONFIG_SHMEM) && defined(CONFIG_MM_SUBPAGE) && defined(CONFIG_USERFAULTFD)
	/* Swapped missing 4K units, keyed by native cache index. */
	struct xarray		uffd_missing;
#endif
#ifdef CONFIG_TMPFS_QUOTA
	struct dquot __rcu	*i_dquot[MAXQUOTAS];
#endif
	struct inode		vfs_inode;
};

#define SHMEM_FL_USER_VISIBLE		(FS_FL_USER_VISIBLE | FS_CASEFOLD_FL)
#define SHMEM_FL_USER_MODIFIABLE \
	(FS_IMMUTABLE_FL | FS_APPEND_FL | FS_NODUMP_FL | FS_NOATIME_FL | FS_CASEFOLD_FL)
#define SHMEM_FL_INHERITED		(FS_NODUMP_FL | FS_NOATIME_FL | FS_CASEFOLD_FL)

struct shmem_quota_limits {
	qsize_t usrquota_bhardlimit; /* Default user quota block hard limit */
	qsize_t usrquota_ihardlimit; /* Default user quota inode hard limit */
	qsize_t grpquota_bhardlimit; /* Default group quota block hard limit */
	qsize_t grpquota_ihardlimit; /* Default group quota inode hard limit */
};

struct shmem_sb_info {
	unsigned long max_blocks;   /* How many blocks are allowed */
	struct percpu_counter used_blocks;  /* How many are allocated */
	unsigned long max_inodes;   /* How many inodes are allowed */
	unsigned long free_ispace;  /* How much ispace left for allocation */
	raw_spinlock_t stat_lock;   /* Serialize shmem_sb_info changes */
	umode_t mode;		    /* Mount mode for root directory */
	unsigned char huge;	    /* Whether to try for hugepages */
	kuid_t uid;		    /* Mount uid for root directory */
	kgid_t gid;		    /* Mount gid for root directory */
	bool full_inums;	    /* If i_ino should be uint or ino_t */
	bool noswap;		    /* ignores VM reclaim / swap requests */
	ino_t next_ino;		    /* The next per-sb inode number to use */
	ino_t __percpu *ino_batch;  /* The next per-cpu inode number to use */
	struct mempolicy *mpol;     /* default memory policy for mappings */
	spinlock_t shrinklist_lock;   /* Protects shrinklist */
	struct list_head shrinklist;  /* List of shinkable inodes */
	unsigned long shrinklist_len; /* Length of shrinklist */
	struct shmem_quota_limits qlimits; /* Default quota limits */
	struct simple_xattr_cache xa_cache;
};

static inline struct shmem_inode_info *SHMEM_I(struct inode *inode)
{
	return container_of(inode, struct shmem_inode_info, vfs_inode);
}

/*
 * Functions in mm/shmem.c called directly from elsewhere:
 */
extern const struct fs_parameter_spec shmem_fs_parameters[];
extern void shmem_init(void);
extern int shmem_init_fs_context(struct fs_context *fc);
struct file *shmem_file_setup(const char *name, loff_t size, vma_flags_t flags);
struct file *shmem_kernel_file_setup(const char *name, loff_t size, vma_flags_t vma_flags);
extern struct file *shmem_file_setup_with_mnt(struct vfsmount *mnt,
		const char *name, loff_t size, vma_flags_t flags);
int shmem_zero_setup(struct vm_area_struct *vma);
int shmem_zero_setup_desc(struct vm_area_desc *desc);
extern unsigned long shmem_get_unmapped_area(struct file *, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags);
extern int shmem_lock(struct file *file, int lock, struct ucounts *ucounts);
#ifdef CONFIG_SHMEM
struct folio *shmem_alloc_collapse_folio(struct mm_struct *mm, struct inode *inode,
				       pgoff_t index, unsigned int order);
bool shmem_mapping(const struct address_space *mapping);
#else
static inline struct folio *shmem_alloc_collapse_folio(struct mm_struct *mm,
		struct inode *inode, pgoff_t index, unsigned int order)
{
	return ERR_PTR(-EOPNOTSUPP);
}
static inline bool shmem_mapping(const struct address_space *mapping)
{
	return false;
}
#endif /* CONFIG_SHMEM */
#if defined(CONFIG_SHMEM) && defined(CONFIG_MM_SUBPAGE) && defined(CONFIG_USERFAULTFD)
/* Return a locked cache folio and caller reference; new backing is all missing. */
struct folio *shmem_uffd_prepare_folio(struct vm_area_struct *vma,
				    unsigned long address, bool *new);
void shmem_uffd_abort_folio(struct folio *folio, struct vm_area_struct *vma, bool new);
bool shmem_uffd_has_missing_native_page(struct folio *folio);
bool shmem_uffd_range_missing(struct folio *folio, pgoff_t index,
			      unsigned int offset, unsigned long length);
/* Resident masks require no allocation; inode metadata persists only in swap. */
void shmem_uffd_copy_page(struct page *dst, struct page *src);
void shmem_uffd_migrate(struct folio *dst, struct folio *src);
/* Locked, uptodate cache folio. */
unsigned long shmem_uffd_missing_mask(struct folio *folio, pgoff_t index);
int shmem_uffd_set_missing(struct folio *folio, pgoff_t index,
			   unsigned long missing);
/* Clear populated units; no allocation. Caller holds the folio lock. */
void shmem_uffd_mark_present(struct folio *folio, pgoff_t index,
			     unsigned long present);
/* Mark exactly the byte range exposed by a mapping; folio lock held. */
void shmem_uffd_mark_mapped(struct folio *folio, unsigned long offset,
			    unsigned long length);
/* i_pages lock held; deletion, not replacement by swap or migration. */
void shmem_uffd_forget(struct address_space *mapping, pgoff_t start,
		       unsigned long nr);
#else
static inline void shmem_uffd_copy_page(struct page *dst, struct page *src) { }
static inline void shmem_uffd_migrate(struct folio *dst, struct folio *src) { }
static inline bool shmem_uffd_has_missing_native_page(struct folio *folio) { return false; }
static inline bool shmem_uffd_range_missing(struct folio *folio, pgoff_t index,
		unsigned int offset, unsigned long length) { return false; }
static inline void shmem_uffd_mark_mapped(struct folio *folio,
		unsigned long offset, unsigned long length) { }
static inline void shmem_uffd_forget(struct address_space *mapping,
				   pgoff_t start, unsigned long nr) { }
#endif

void shmem_unlock_mapping(struct address_space *mapping);
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping,
					pgoff_t index, gfp_t gfp_mask);
int shmem_writeout(struct folio *folio, struct swap_iocb **plug,
		struct list_head *folio_list);
void shmem_truncate_range(struct inode *inode, loff_t start, uoff_t end);
int shmem_unuse(unsigned int type);

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
unsigned long shmem_allowable_huge_orders(struct inode *inode,
				struct vm_area_struct *vma, pgoff_t index,
				loff_t write_end, bool shmem_huge_force);
bool shmem_hpage_pmd_enabled(void);
#else
static inline unsigned long shmem_allowable_huge_orders(struct inode *inode,
				struct vm_area_struct *vma, pgoff_t index,
				loff_t write_end, bool shmem_huge_force)
{
	return 0;
}

static inline bool shmem_hpage_pmd_enabled(void)
{
	return false;
}
#endif

#ifdef CONFIG_SHMEM
extern unsigned long shmem_swap_usage(struct vm_area_struct *vma);
extern void shmem_uncharge(struct inode *inode, long pages);
#else
static inline unsigned long shmem_swap_usage(struct vm_area_struct *vma)
{
	return 0;
}

static inline void shmem_uncharge(struct inode *inode, long pages)
{
}
#endif
unsigned long shmem_partial_swap_usage_bytes(struct address_space *mapping,
					    u64 start, u64 end);
extern unsigned long shmem_partial_swap_usage(struct address_space *mapping,
						pgoff_t start, pgoff_t end);

/* Flag allocation requirements to shmem_get_folio */
enum sgp_type {
	SGP_READ,	/* don't exceed i_size, don't allocate page */
	SGP_NOALLOC,	/* similar, but fail on hole or use fallocated page */
	SGP_CACHE,	/* don't exceed i_size, may allocate page */
	SGP_WRITE,	/* may exceed i_size, may allocate !Uptodate page */
	SGP_FALLOC,	/* like SGP_WRITE, but make existing page Uptodate */
};

int shmem_get_folio(struct inode *inode, pgoff_t index, loff_t write_end,
		struct folio **foliop, enum sgp_type sgp);
struct folio *shmem_read_folio_gfp(struct address_space *mapping,
		pgoff_t index, gfp_t gfp);

static inline struct folio *shmem_read_folio(struct address_space *mapping,
		pgoff_t index)
{
	return shmem_read_folio_gfp(mapping, index, mapping_gfp_mask(mapping));
}

static inline struct page *shmem_read_mapping_page(
				struct address_space *mapping, pgoff_t index)
{
	return shmem_read_mapping_page_gfp(mapping, index,
					mapping_gfp_mask(mapping));
}

static inline bool shmem_file(struct file *file)
{
	if (!IS_ENABLED(CONFIG_SHMEM))
		return false;
	if (!file || !file->f_mapping)
		return false;
	return shmem_mapping(file->f_mapping);
}

/* Must be called with inode lock taken exclusive. */
static inline void shmem_freeze(struct inode *inode, bool freeze)
{
	if (freeze)
		SHMEM_I(inode)->flags |= SHMEM_F_MAPPING_FROZEN;
	else
		SHMEM_I(inode)->flags &= ~SHMEM_F_MAPPING_FROZEN;
}

/*
 * If fallocate(FALLOC_FL_KEEP_SIZE) has been used, there may be pages
 * beyond i_size's notion of EOF, which fallocate has committed to reserving:
 * which split_huge_page() must therefore not delete.  This use of a single
 * "fallocend" per inode errs on the side of not deleting a reservation when
 * in doubt: there are plenty of cases when it preserves unreserved pages.
 */
static inline pgoff_t shmem_fallocend(struct inode *inode, pgoff_t eof)
{
	return max(eof, SHMEM_I(inode)->fallocend);
}

extern bool shmem_charge(struct inode *inode, long pages);

/*
 * Used space is stored as unsigned 64-bit value in bytes but
 * quota core supports only signed 64-bit values so use that
 * as a limit
 */
#define SHMEM_QUOTA_MAX_SPC_LIMIT 0x7fffffffffffffffLL /* 2^63-1 */
#define SHMEM_QUOTA_MAX_INO_LIMIT 0x7fffffffffffffffLL

#ifdef CONFIG_TMPFS_QUOTA
extern const struct dquot_operations shmem_quota_operations;
extern struct quota_format_type shmem_quota_format;
#endif /* CONFIG_TMPFS_QUOTA */

#endif
