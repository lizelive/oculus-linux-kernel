// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2002,2008-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/ptrace.h>

#include "kgsl_debugfs.h"
#include "kgsl_device.h"
#include "kgsl_sharedmem.h"

/*default log levels is error for everything*/
#define KGSL_LOG_LEVEL_MAX     7

struct dentry *kgsl_debugfs_dir;
static struct dentry *proc_d_debugfs;

static int _strict_set(void *data, u64 val)
{
	kgsl_sharedmem_set_noretry(val ? true : false);
	return 0;
}

static int _strict_get(void *data, u64 *val)
{
	*val = kgsl_sharedmem_get_noretry();
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(_strict_fops, _strict_get, _strict_set, "%llu\n");

static void kgsl_qdss_gfx_register_probe(struct kgsl_device *device)
{
	struct resource *res;

	res = platform_get_resource_byname(device->pdev, IORESOURCE_MEM,
							"qdss_gfx");

	if (res == NULL)
		return;

	device->qdss_gfx_virt = devm_ioremap(device->dev, res->start,
							resource_size(res));

	if (device->qdss_gfx_virt == NULL)
		dev_warn(device->dev, "qdss_gfx ioremap failed\n");
}

static int _isdb_set(void *data, u64 val)
{
	struct kgsl_device *device = data;

	if (device->qdss_gfx_virt == NULL)
		kgsl_qdss_gfx_register_probe(device);

	device->set_isdb_breakpoint = val ? true : false;
	return 0;
}

static int _isdb_get(void *data, u64 *val)
{
	struct kgsl_device *device = data;

	*val = device->set_isdb_breakpoint ? 1 : 0;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(_isdb_fops, _isdb_get, _isdb_set, "%llu\n");

void kgsl_device_debugfs_init(struct kgsl_device *device)
{
	struct dentry *snapshot_dir;

	if (IS_ERR_OR_NULL(kgsl_debugfs_dir))
		return;

	device->d_debugfs = debugfs_create_dir(device->name,
						       kgsl_debugfs_dir);
	snapshot_dir = debugfs_create_dir("snapshot", kgsl_debugfs_dir);
	debugfs_create_file("break_isdb", 0644, snapshot_dir, device,
		&_isdb_fops);
}

void kgsl_device_debugfs_close(struct kgsl_device *device)
{
	debugfs_remove_recursive(device->d_debugfs);
}

struct type_entry {
	int type;
	const char *str;
};

static const struct type_entry memtypes[] = { KGSL_MEM_TYPES };

static const char *memtype_str(int memtype)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(memtypes); i++)
		if (memtypes[i].type == memtype)
			return memtypes[i].str;
	return "unknown";
}

static char get_alignflag(const struct kgsl_memdesc *m)
{
	int align = kgsl_memdesc_get_align(m);

	if (align >= ilog2(SZ_1M))
		return 'L';
	else if (align >= ilog2(SZ_64K))
		return 'l';
	return '-';
}

static char get_cacheflag(const struct kgsl_memdesc *m)
{
	static const char table[] = {
		[KGSL_CACHEMODE_WRITECOMBINE] = '-',
		[KGSL_CACHEMODE_UNCACHED] = 'u',
		[KGSL_CACHEMODE_WRITEBACK] = 'b',
		[KGSL_CACHEMODE_WRITETHROUGH] = 't',
	};

	return table[kgsl_memdesc_get_cachemode(m)];
}

static int print_mem_entry(void *data, void *ptr)
{
	struct seq_file *s = data;
	struct kgsl_mem_entry *entry = ptr;
	char flags[10];
	char usage[16];
	struct kgsl_memdesc *m = &entry->memdesc;
	unsigned int usermem_type = kgsl_memdesc_usermem_type(m);
	int egl_surface_count = 0, egl_image_count = 0, total_count = 0;

	if (m->flags & KGSL_MEMFLAGS_SPARSE_VIRT)
		return 0;

	flags[0] = kgsl_memdesc_is_global(m) ?  'g' : '-';
	flags[1] = '-';
	flags[2] = !(m->flags & KGSL_MEMFLAGS_GPUREADONLY) ? 'w' : '-';
	flags[3] = get_alignflag(m);
	flags[4] = get_cacheflag(m);
	flags[5] = kgsl_memdesc_use_cpu_map(m) ? 'p' : '-';
	/*
	 * Show Y if at least one vma has this entry
	 * mapped (could be multiple)
	 */
	flags[6] = atomic_read(&entry->map_count) ? 'Y' : 'N';
	flags[7] = kgsl_memdesc_is_secured(m) ?  's' : '-';
	flags[8] = m->flags & KGSL_MEMFLAGS_SPARSE_PHYS ? 'P' : '-';
	flags[9] = '\0';

	kgsl_get_memory_usage(usage, sizeof(usage), m->flags);

	if (usermem_type == KGSL_MEM_ENTRY_ION)
		kgsl_get_egl_counts(entry, &egl_surface_count,
						&egl_image_count, &total_count);

	seq_printf(s, "%pK %pK %16llu %5d %9s %10s %16s %5d %16d %6d %6d",
			(uint64_t *)(uintptr_t) m->gpuaddr,
			/*
			 * Show zero for the useraddr - we can't reliably track
			 * that value for multiple vmas anyway
			 */
			0, m->size, entry->id, flags,
			memtype_str(usermem_type),
			usage, (m->sgt ? m->sgt->nents : 0),
			atomic_read(&entry->map_count),
			egl_surface_count, egl_image_count);

	if (entry->metadata)
		seq_printf(s, " %s", entry->metadata);

	seq_putc(s, '\n');

	return 0;
}

static struct kgsl_mem_entry *process_mem_seq_find(struct seq_file *s,
						void *ptr, loff_t pos)
{
	struct kgsl_mem_entry *entry = ptr;
	struct kgsl_process_private *private = s->private;
	int id = 0;

	loff_t temp_pos = 1;

	if (entry != SEQ_START_TOKEN)
		id = entry->id + 1;

	spin_lock(&private->mem_lock);
	for (entry = idr_get_next(&private->mem_idr, &id); entry;
		id++, entry = idr_get_next(&private->mem_idr, &id),
							temp_pos++) {
		if (temp_pos == pos && kgsl_mem_entry_get(entry)) {
			spin_unlock(&private->mem_lock);
			goto found;
		}
	}
	spin_unlock(&private->mem_lock);

	entry = NULL;
found:
	if (ptr != SEQ_START_TOKEN)
		kgsl_mem_entry_put(ptr);

	return entry;
}

static void *process_mem_seq_start(struct seq_file *s, loff_t *pos)
{
	loff_t seq_file_offset = *pos;

	if (seq_file_offset == 0)
		return SEQ_START_TOKEN;
	else
		return process_mem_seq_find(s, SEQ_START_TOKEN,
						seq_file_offset);
}

static void process_mem_seq_stop(struct seq_file *s, void *ptr)
{
	if (ptr && ptr != SEQ_START_TOKEN)
		kgsl_mem_entry_put(ptr);
}

static void *process_mem_seq_next(struct seq_file *s, void *ptr,
							loff_t *pos)
{
	++*pos;
	return process_mem_seq_find(s, ptr, 1);
}

static int process_mem_seq_show(struct seq_file *s, void *ptr)
{
	if (ptr == SEQ_START_TOKEN) {
		seq_printf(s, "%16s %16s %16s %5s %9s %10s %16s %5s %16s %6s %6s\n",
			"gpuaddr", "useraddr", "size", "id", "flags", "type",
			"usage", "sglen", "mapcount", "eglsrf", "eglimg");
		return 0;
	} else
		return print_mem_entry(s, ptr);
}

static const struct seq_operations process_mem_seq_fops = {
	.start = process_mem_seq_start,
	.stop = process_mem_seq_stop,
	.next = process_mem_seq_next,
	.show = process_mem_seq_show,
};

static int process_mem_open(struct inode *inode, struct file *file)
{
	int ret;
	pid_t pid = (pid_t) (unsigned long) inode->i_private;
	struct seq_file *s = NULL;
	struct kgsl_process_private *private = NULL;

	private = kgsl_process_private_find(pid);

	if (!private)
		return -ENODEV;

	ret = seq_open(file, &process_mem_seq_fops);
	if (ret)
		kgsl_process_private_put(private);
	else {
		s = file->private_data;
		s->private = private;
	}

	return ret;
}

static int process_mem_release(struct inode *inode, struct file *file)
{
	struct kgsl_process_private *private =
		((struct seq_file *)file->private_data)->private;

	if (private)
		kgsl_process_private_put(private);

	return seq_release(inode, file);
}

static const struct file_operations process_mem_fops = {
	.open = process_mem_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = process_mem_release,
};

static int print_sparse_mem_entry(int id, void *ptr, void *data)
{
	struct seq_file *s = data;
	struct kgsl_mem_entry *entry = ptr;
	struct kgsl_memdesc *m = &entry->memdesc;
	struct rb_node *node;

	if (!(m->flags & KGSL_MEMFLAGS_SPARSE_VIRT))
		return 0;

	spin_lock(&entry->bind_lock);
	node = rb_first(&entry->bind_tree);

	while (node != NULL) {
		struct sparse_bind_object *obj = rb_entry(node,
				struct sparse_bind_object, node);
		seq_printf(s, "%5d %16llx %16llx %16llx %16llx\n",
				entry->id, entry->memdesc.gpuaddr,
				obj->v_off, obj->size, obj->p_off);
		node = rb_next(node);
	}
	spin_unlock(&entry->bind_lock);

	seq_putc(s, '\n');

	return 0;
}

static int process_sparse_mem_print(struct seq_file *s, void *unused)
{
	struct kgsl_process_private *private = s->private;

	seq_printf(s, "%5s %16s %16s %16s %16s\n",
		   "v_id", "gpuaddr", "v_offset", "v_size", "p_offset");

	spin_lock(&private->mem_lock);
	idr_for_each(&private->mem_idr, print_sparse_mem_entry, s);
	spin_unlock(&private->mem_lock);

	return 0;
}

static int process_sparse_mem_open(struct inode *inode, struct file *file)
{
	int ret;
	pid_t pid = (pid_t) (unsigned long) inode->i_private;
	struct kgsl_process_private *private = NULL;

	private = kgsl_process_private_find(pid);

	if (!private)
		return -ENODEV;

	ret = single_open(file, process_sparse_mem_print, private);
	if (ret)
		kgsl_process_private_put(private);

	return ret;
}

static const struct file_operations process_sparse_mem_fops = {
	.open = process_sparse_mem_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = process_mem_release,
};

static int globals_print(struct seq_file *s, void *unused)
{
	kgsl_print_global_pt_entries(s);
	return 0;
}

static int globals_open(struct inode *inode, struct file *file)
{
	return single_open(file, globals_print, NULL);
}

static int globals_release(struct inode *inode, struct file *file)
{
	return single_release(inode, file);
}

static const struct file_operations global_fops = {
	.open = globals_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = globals_release,
};

/**
 * kgsl_process_init_debugfs() - Initialize debugfs for a process
 * @private: Pointer to process private structure created for the process
 *
 * kgsl_process_init_debugfs() is called at the time of creating the
 * process struct when a process opens kgsl device for the first time.
 * This function is not fatal - all we do is print a warning message if
 * the files can't be created
 */
void kgsl_process_init_debugfs(struct kgsl_process_private *private)
{
	unsigned char name[16];
	struct dentry *dentry;

	snprintf(name, sizeof(name), "%d", pid_nr(private->pid));

	private->debug_root = debugfs_create_dir(name, proc_d_debugfs);

	/*
	 * Both debugfs_create_dir() and debugfs_create_file() return
	 * ERR_PTR(-ENODEV) if debugfs is disabled in the kernel but return
	 * NULL on error when it is enabled. For both usages we need to check
	 * for ERROR or NULL and only print a warning on an actual failure
	 * (i.e. - when the return value is NULL)
	 */

	if (IS_ERR_OR_NULL(private->debug_root)) {
		WARN((private->debug_root == NULL),
			"Unable to create debugfs dir for %s\n", name);
		private->debug_root = NULL;
		return;
	}

	dentry = debugfs_create_file("mem", 0444, private->debug_root,
		(void *) ((unsigned long) pid_nr(private->pid)),
		&process_mem_fops);

	if (IS_ERR_OR_NULL(dentry))
		WARN((dentry == NULL),
			"Unable to create 'mem' file for %s\n", name);

	dentry = debugfs_create_file("sparse_mem", 0444, private->debug_root,
		(void *) ((unsigned long) pid_nr(private->pid)),
		&process_sparse_mem_fops);

	if (IS_ERR_OR_NULL(dentry))
		WARN((dentry == NULL),
			"Unable to create 'sparse_mem' file for %s\n", name);

}

static int print_mem_entry_page(struct seq_file *s, void *ptr)
{
	struct kgsl_mem_entry *entry = s->private;
	unsigned int page = *(loff_t *)ptr - 1;
	size_t page_offset = page << PAGE_SHIFT;
	void *kptr, *buf;

	const size_t rowsize = 32;
	size_t linelen = rowsize;
	size_t remaining = PAGE_SIZE;
	size_t i;

	unsigned char linebuf[32 * 3 + 2 + 32 + 1];

	/* Skip unallocated pages. */
	if (entry->memdesc.pages[page] == NULL)
		return 0;

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return 0;

	kptr = vm_map_ram(&entry->memdesc.pages[page], 1, -1,
			pgprot_writecombine(PAGE_KERNEL));
	if (!kptr)
		goto no_mapping;

	memcpy(buf, kptr, PAGE_SIZE);
	vm_unmap_ram(kptr, 1);

	for (i = 0; i < PAGE_SIZE; i += rowsize) {
		const size_t offset = page_offset + i + (kptr_restrict < 2 ?
				entry->memdesc.gpuaddr : 0);

		linelen = min(remaining, rowsize);
		remaining -= rowsize;

		hex_dump_to_buffer(buf + i, linelen, rowsize, 4,
				linebuf, sizeof(linebuf), true);

		seq_printf(s, "%016llx: %s\n", offset, linebuf);
	}

no_mapping:
	kfree(buf);

	return 0;
}

static void *mem_entry_seq_start(struct seq_file *s, loff_t *pos)
{
	struct kgsl_mem_entry *entry = s->private;
	struct task_struct *task = get_pid_task(entry->priv->pid, PIDTYPE_PID);
	bool may_access;

	/*
	 * First check if the dump request is coming from the process itself
	 * or from a privileged process (like root).
	 */
	if (!task)
		return NULL;

	may_access = ptrace_may_access(task, PTRACE_MODE_READ_FSCREDS);
	put_task_struct(task);

	if (!may_access)
		return ERR_PTR(-EACCES);

	/* If the entry is being freed or we fail to grab a ref bail out here. */
	if (entry->pending_free || kgsl_mem_entry_get(entry) == 0)
		return NULL;

	if (*pos == 0)
		return SEQ_START_TOKEN;
	else
		return (*pos <= entry->memdesc.page_count) ? pos : NULL;
}

static void *mem_entry_seq_next(struct seq_file *s, void *ptr, loff_t *pos)
{
	struct kgsl_mem_entry *entry = s->private;

	++(*pos);
	return (*pos <= entry->memdesc.page_count) ? pos : NULL;
}

static int mem_entry_seq_show(struct seq_file *s, void *ptr)
{
	if (ptr == SEQ_START_TOKEN)
		return print_mem_entry(s, s->private);
	else
		return print_mem_entry_page(s, ptr);
}

static void mem_entry_seq_stop(struct seq_file *s, void *ptr)
{
	struct kgsl_mem_entry *entry = s->private;

	kgsl_mem_entry_put_deferred(entry);
}

static const struct seq_operations mem_entry_seq_ops = {
	.start = mem_entry_seq_start,
	.next = mem_entry_seq_next,
	.show = mem_entry_seq_show,
	.stop = mem_entry_seq_stop,
};

static int mem_entry_open(struct inode *inode, struct file *file)
{
	const struct seq_operations *seq_ops = &mem_entry_seq_ops;
	struct kgsl_mem_entry *entry = inode->i_private;
	struct seq_file *m;
	int ret;

	ret = seq_open(file, seq_ops);
	if (ret < 0)
		return ret;
	m = file->private_data;
	m->private = entry;

	return ret;
}

static const struct file_operations mem_entry_fops = {
	.open = mem_entry_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

void kgsl_process_init_mem_entry_debugfs(struct kgsl_mem_entry *entry)
{
	struct dentry *dentry;
	unsigned char name[16];

	/* Don't bother making debugfs entries for memory with these flags */
	const uint64_t blocked_flags = KGSL_MEMFLAGS_USERMEM_MASK |
			KGSL_MEMFLAGS_SPARSE_VIRT | KGSL_MEMFLAGS_SECURE;

	if (IS_ERR_OR_NULL(entry->priv->debug_root) ||
			(entry->memdesc.flags & blocked_flags) ||
			entry->memdesc.pages == NULL ||
			entry->memdesc.page_count == 0)
		return;

	snprintf(name, sizeof(name), "%d", entry->id);

	dentry = debugfs_create_file(name, 0444, entry->priv->debug_root,
						entry, &mem_entry_fops);
	if (IS_ERR_OR_NULL(dentry)) {
		WARN((dentry == NULL),
			"Unable to create mem entry file for %d:%s\n",
			entry->priv->pid, name);
		entry->dentry_id = 0;
		return;
	}

	idr_preload(GFP_KERNEL);
	spin_lock(&entry->priv->mem_lock);
	entry->dentry_id = idr_alloc(&entry->priv->dentry_idr, dentry, 1, 0,
			GFP_NOWAIT);
	spin_unlock(&entry->priv->mem_lock);
	idr_preload_end();
}

void kgsl_core_debugfs_init(void)
{
	struct dentry *debug_dir;

	kgsl_debugfs_dir = debugfs_create_dir("kgsl", NULL);
	if (IS_ERR_OR_NULL(kgsl_debugfs_dir))
		return;

	debugfs_create_file("globals", 0444, kgsl_debugfs_dir, NULL,
		&global_fops);

	debug_dir = debugfs_create_dir("debug", kgsl_debugfs_dir);

	debugfs_create_file("strict_memory", 0644, debug_dir, NULL,
		&_strict_fops);

	proc_d_debugfs = debugfs_create_dir("proc", kgsl_debugfs_dir);
}

void kgsl_core_debugfs_close(void)
{
	debugfs_remove_recursive(kgsl_debugfs_dir);
}
