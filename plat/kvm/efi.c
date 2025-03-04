/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <kvm/efi.h>
#include <uk/arch/paging.h>
#include <uk/libid.h>
#include <uk/plat/common/bootinfo.h>

extern struct ukplat_memregion_desc bpt_unmap_mrd;
static uk_efi_paddr_t uk_efi_alloc_max_paddr;

/* We must ensure backwards compatibility with !CONFIG_HAVE_PAGING */
#if CONFIG_HAVE_PAGING
static enum uk_efi_alloc_type uk_efi_alloc_type = UK_EFI_ALLOCATE_ANY_PAGES;
#else  /* !CONFIG_HAVE_PAGING */
static enum uk_efi_alloc_type uk_efi_alloc_type = UK_EFI_ALLOCATE_MAX_ADDRESS;
#endif /* !CONFIG_HAVE_PAGING */
#if CONFIG_LIBUKDEBUG_PRINTD
#include <stdio.h>
#endif /* CONFIG_LIBUKDEBUG_PRINTD */

static struct uk_efi_runtime_services *uk_efi_rs;
static struct uk_efi_boot_services *uk_efi_bs;
static struct uk_efi_sys_tbl *uk_efi_st;
static uk_efi_hndl_t uk_efi_sh;

static __u8 uk_efi_mat_present;

/* As per UEFI specification, the call to the GetMemoryMap routine following
 * the dummy one, must have a surplus amount of memory region descriptors in
 * size. Usually, 2 to 4 is enough, but allocate 10, just in case.
 */
#define UK_EFI_MAXPATHLEN					4096
#define UK_EFI_ABS_FNAME(f)					"\\EFI\\BOOT\\"f
#define UK_EFI_SURPLUS_MEM_DESC_COUNT				10

#define EFI_STUB_CMDLINE_FNAME	CONFIG_KVM_BOOT_PROTO_EFI_STUB_CMDLINE_FNAME
#define EFI_STUB_INITRD_FNAME	CONFIG_KVM_BOOT_PROTO_EFI_STUB_INITRD_FNAME
#define EFI_STUB_DTB_FNAME	CONFIG_KVM_BOOT_PROTO_EFI_STUB_DTB_FNAME

#define UK_EFI_MAX_FMT_STR_LEN					256

#if CONFIG_LIBUKDEBUG_PRINTD
static __sz ascii_to_utf16(const char *str, char *str16, __sz max_len16);
static void uk_efi_printf(const char *str, ...)
{
	char fmt_str[UK_EFI_MAX_FMT_STR_LEN];
	char str_tmp[UK_EFI_MAX_FMT_STR_LEN];
	__s16 str16[UK_EFI_MAX_FMT_STR_LEN];
	va_list ap;

	sprintf(fmt_str, "dbg: [%s] <%s @ %4u> %s\r", uk_libname_self(),
		STRINGIFY(__BASENAME__), __LINE__, str);

	va_start(ap, str);
	vsprintf(str_tmp, fmt_str, ap);
	va_end(ap);

	ascii_to_utf16(str_tmp, (char *)str16, UK_EFI_MAX_FMT_STR_LEN - 1);
	uk_efi_st->con_out->output_string(uk_efi_st->con_out, str16);
}
#define uk_efi_pr_debug					uk_efi_printf
/* UEFI for proper \n, we must also use CRLF */
#define UK_EFI_CRASH(...)					\
	do {							\
		uk_efi_printf(__VA_ARGS__);			\
		uk_efi_do_crash();				\
	} while (0)
#else /* !CONFIG_LIBUKDEBUG_PRINTD */
#define uk_efi_pr_debug(...)
#define UK_EFI_CRASH(str)				uk_efi_do_crash()
#endif /* !CONFIG_LIBUKDEBUG_PRINTD */

void uk_efi_jmp_to_kern(void) __noreturn;

/* Overlysimplified conversion from ASCII to UTF-16 */
static __sz ascii_to_utf16(const char *str, char *str16, __sz max_len16)
{
	__sz i = 0;

	while (str[i >> 1]) {
		if (unlikely(i == max_len16))
			return __SZ_MAX;

		str16[i] = str[i >> 1];
		str16[i + 1] = '\0';
		i += 2;
	}

	str16[i] = str16[i + 1] = '\0';

	return i + 2;
}

/* Overlysimplified conversion from UTF-16 to ASCII */
static __sz utf16_to_ascii(const char *str16, char *str, __sz max_len)
{
	__sz i = 0;

	while (*str16) {
		if (unlikely(i == max_len))
			return __SZ_MAX;

		str[i++] = *str16;
		str16 += 2;
	}

	str[i] = '\0';

	return i + 1;
}

static void uk_efi_do_crash(void)
{
	const char reset_data[] = "UK EFI SYSTEM CRASH";

	uk_efi_rs->reset_system(UK_EFI_RESET_SHUTDOWN, UK_EFI_SUCCESS,
				sizeof(reset_data), (void *)reset_data);
}

static void uk_efi_cls(void)
{
	uk_efi_st->con_out->clear_screen(uk_efi_st->con_out);
}

/* Initialize global variables */
static void uk_efi_init_vars(uk_efi_hndl_t self_hndl,
				    struct uk_efi_sys_tbl *sys_tbl)
{
	uk_efi_st = sys_tbl;
	uk_efi_bs = sys_tbl->boot_services;
	uk_efi_rs = sys_tbl->runtime_services;
	uk_efi_sh = self_hndl;

	uk_efi_alloc_max_paddr = bpt_unmap_mrd.pbase + bpt_unmap_mrd.len;
}

/* Convert an EFI Memory Descriptor to a ukplat_memregion_desc */
static int uk_efi_md_to_bi_mrd(struct uk_efi_mem_desc *const md,
			       struct ukplat_memregion_desc *const mrd)
{
	__paddr_t start, end;

	switch (md->type) {
	case UK_EFI_RESERVED_MEMORY_TYPE:
	case UK_EFI_ACPI_RECLAIM_MEMORY:
	case UK_EFI_UNUSABLE_MEMORY:
	case UK_EFI_ACPI_MEMORY_NVS:
	case UK_EFI_PAL_CODE:
	case UK_EFI_PERSISTENT_MEMORY:
		mrd->type = UKPLAT_MEMRT_RESERVED;
		mrd->flags = UKPLAT_MEMRF_READ | UKPLAT_MEMRF_MAP;

		break;
	case UK_EFI_MEMORY_MAPPED_IO:
	case UK_EFI_MEMORY_MAPPED_IO_PORT_SPACE:
		mrd->type = UKPLAT_MEMRT_RESERVED;
		mrd->flags = UKPLAT_MEMRF_READ | UKPLAT_MEMRF_WRITE |
			     UKPLAT_MEMRF_MAP;

		break;
	case UK_EFI_RUNTIME_SERVICES_CODE:
	case UK_EFI_RUNTIME_SERVICES_DATA:
		/* Already added through uk_efi_rt_md_to_bi_mrds() if a MAT
		 * has been found, dictated by whether uk_efi_mat_present != 0.
		 * Otherwise, add these instead.
		 */
		if (unlikely(uk_efi_mat_present))
			return -EEXIST;

		mrd->type = UKPLAT_MEMRT_RESERVED;
		/* A MAT would have provided us with proper, high granularity
		 * memory attributes, but now we cannot be sure of anything as
		 * Runtime Services related memory descriptors usually have
		 * useless and inaccurate flags. Therefore, just give all
		 * permissions to avoid crashes generated by explicit firmware
		 * calls.
		 */
		mrd->flags = UKPLAT_MEMRF_READ | UKPLAT_MEMRF_WRITE |
			     UKPLAT_MEMRF_MAP;

		break;
	case UK_EFI_LOADER_CODE:
	case UK_EFI_LOADER_DATA:
		/* Already added through mkbootinfo.py and relocated through
		 * do_uk_reloc
		 */
		return -EEXIST;
	case UK_EFI_BOOT_SERVICES_CODE:
	case UK_EFI_BOOT_SERVICES_DATA:
	case UK_EFI_CONVENTIONAL_MEMORY:
		/* These are freed after ExitBootServices is called */
		mrd->type = UKPLAT_MEMRT_FREE;

		mrd->flags = UKPLAT_MEMRF_READ | UKPLAT_MEMRF_WRITE;

		break;
	default:
		/* Memory type unknown */
		return -EINVAL;
	}

	/* Ignore zero-page */
	start = MAX(md->physical_start, __PAGE_SIZE);
	end = md->physical_start + md->number_of_pages * UK_EFI_PAGE_SIZE;
	if (unlikely(end <= start || end - start < __PAGE_SIZE))
		return -ENOMEM;

	mrd->pbase = start;
	mrd->vbase = start;
	mrd->len = end - start;

	return 0;
}

static void uk_efi_get_mmap_and_exit_bs(struct uk_efi_mem_desc **map,
					uk_efi_uintn_t *map_sz,
					uk_efi_uintn_t *desc_sz)
{
	uk_efi_uintn_t map_key;
	uk_efi_status_t status;
	__u8 retries = 0;
	__u32 desc_ver;

uk_efi_get_mmap_retry:
	if (retries) {
		if (unlikely(retries > 1))
			UK_EFI_CRASH("Failed to exit Boot Services second time\n");

		/* Free the memory map previously allocated */
		status = uk_efi_bs->free_pages((uk_efi_paddr_t)*map,
					       DIV_ROUND_UP(*map_sz,
							    PAGE_SIZE));
		if (unlikely(status != UK_EFI_SUCCESS))
			UK_EFI_CRASH("Failed to free previous memory map\n");
	}

	/* As the UEFI Spec says:
	 * If the MemoryMap buffer is too small, the EFI_BUFFER_TOO_SMALL
	 * error code is returned and the MemoryMapSize value contains the
	 * size of the buffer needed to contain the current memory map. The
	 * actual size of the buffer allocated for the consequent call to
	 * GetMemoryMap() should be bigger then the value returned in
	 * MemoryMapSize, since allocation of the new buffer may potentially
	 * increase memory map size.
	 */
	*map_sz = 0;  /* force EFI_BUFFER_TOO_SMALL */
	*map = NULL;
	status = uk_efi_bs->get_memory_map(map_sz, *map, &map_key,
					   desc_sz, &desc_ver);
	if (unlikely(status != UK_EFI_BUFFER_TOO_SMALL))
		UK_EFI_CRASH("Failed to call initial dummy get_memory_map\n");

	/* Make sure the actual allocated buffer is bigger */
	*map_sz += *desc_sz * UK_EFI_SURPLUS_MEM_DESC_COUNT;
	*map = (struct uk_efi_mem_desc *)uk_efi_alloc_max_paddr;
	status = uk_efi_bs->allocate_pages(uk_efi_alloc_type,
					   UK_EFI_LOADER_DATA,
					   DIV_ROUND_UP(*map_sz, PAGE_SIZE),
					   (uk_efi_paddr_t *)map);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to allocate memory for map\n");

	/* Now we call it for real */
	status = uk_efi_bs->get_memory_map(map_sz, *map, &map_key,
					   desc_sz, &desc_ver);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to get memory map\n");

	/* We now exit Boot Services since we no longer need it. */
	/* In case of exit failure, we obtain the memory map again, since the
	 * memory map may have been changed.
	 */
	status = uk_efi_bs->exit_boot_services(uk_efi_sh, map_key);
	if (unlikely(status != UK_EFI_SUCCESS)) {
		retries++;
		uk_efi_pr_debug("ExitBootServices failed, retrying GetMemoryMap\n");
		goto uk_efi_get_mmap_retry;
	}
}

/* Runtime Services memory regions in the Memory Attribute Table have a higher
 * granularity regarding sizes and permissions: the ones resulted from
 * GetMemoryMap only differentiate between Runtime Services Data/Code, while
 * the MAT also differentiates between permissions of the Runtime Services'
 * PE sections (Runtime Services can basically be thought of as loaded Portable
 * Executable format drivers).
 *
 * NOTE: Apparently, MAT is somewhat optional, so if none is found, we fallback
 *	on the Runtime Services memory descriptors we got from GetMemoryMap().
 */
static void uk_efi_rt_md_to_bi_mrds(struct ukplat_memregion_desc **rt_mrds,
				    __u32 *const rt_mrds_count)
{
	struct uk_efi_mem_attr_tbl *mat = NULL;
	struct ukplat_memregion_desc *rt_mrd;
	struct uk_efi_mem_desc *mat_md;
	struct uk_efi_cfg_tbl *ct;
	uk_efi_status_t status;
	uk_efi_uintn_t i;
	__sz desc_sz;

	/* Search for the MAT in UEFI System Table's Configuration Tables */
	for (i = 0; i < uk_efi_st->number_of_table_entries; i++) {
		ct = &uk_efi_st->configuration_table[i];

		if (!memcmp(&ct->vendor_guid,
			    UK_EFI_MEMORY_ATTRIBUTES_TABLE_GUID,
			    sizeof(ct->vendor_guid))) {
			mat = ct->vendor_table;
			uk_efi_mat_present = 1;

			break;
		}
	}
	if (!mat)
		return;

	desc_sz = mat->descriptor_size;
	*rt_mrds_count = mat->number_of_entries;
	status = uk_efi_bs->allocate_pool(UK_EFI_LOADER_DATA,
					  *rt_mrds_count * sizeof(**rt_mrds),
					  (void **)rt_mrds);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to allocate memory for Memory Sub-region Descriptors\n");

	/* Convert the EFI Runtime Services Memory descriptors to
	 * ukplat_memregion_desc's
	 */
	mat_md = (struct uk_efi_mem_desc *)mat->entry;
	for (i = 0; i < *rt_mrds_count; i++) {
		if (!(mat_md->attribute & UK_EFI_MEMORY_RUNTIME))
			continue;

		rt_mrd = *rt_mrds + i;
		rt_mrd->pbase = mat_md->physical_start;
		rt_mrd->len = mat_md->number_of_pages * UK_EFI_PAGE_SIZE;
		rt_mrd->vbase = rt_mrd->pbase;
		rt_mrd->type = UKPLAT_MEMRT_RESERVED;
		rt_mrd->flags = UKPLAT_MEMRF_MAP;
		if (mat_md->attribute & UK_EFI_MEMORY_XP)
			if (mat_md->attribute & UK_EFI_MEMORY_RO)
				rt_mrd->flags |= UKPLAT_MEMRF_READ;
			else
				rt_mrd->flags |= UKPLAT_MEMRF_READ |
						 UKPLAT_MEMRF_WRITE;
		else
			rt_mrd->flags |= UKPLAT_MEMRF_READ |
					 UKPLAT_MEMRF_EXECUTE;

		mat_md = (struct uk_efi_mem_desc *)((__u8 *)mat_md + desc_sz);
	}
}

static void uk_efi_setup_bootinfo_mrds(struct ukplat_bootinfo *bi)
{
	struct ukplat_memregion_desc mrd = {0}, *rt_mrds;
	struct uk_efi_mem_desc *map_start, *map_end, *md;
	uk_efi_uintn_t map_sz, desc_sz;
	__u32 rt_mrds_count = 0, i;
	uk_efi_status_t status;
	int rc;

#if defined(__X86_64__)
	rc = ukplat_memregion_list_insert_legacy_hi_mem(&bi->mrds);
	if (unlikely(rc < 0))
		UK_EFI_CRASH("Failed to insert legacy high memory region\n");
#endif

	/* Fetch the Runtime Services memory regions from the MAT */
	uk_efi_rt_md_to_bi_mrds(&rt_mrds, &rt_mrds_count);
	for (i = 0; i < rt_mrds_count; i++) {
		rc = ukplat_memregion_list_insert(&bi->mrds, &rt_mrds[i]);
		if (unlikely(rc < 0))
			UK_EFI_CRASH("Failed to insert rt_mrd\n");
	}

	/* We no longer need the list of Runtime Services memory regions */
	status = uk_efi_bs->free_pool(rt_mrds);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to free rt_mrds\n");

	/* Get memory map through GetMemoryMap and also exit Boot service.
	 * NOTE: after exiting, EFI printing provided by BS is not available
	 * anymore, so UK_CRASH should be used instead.
	 */
	uk_efi_get_mmap_and_exit_bs(&map_start, &map_sz, &desc_sz);

	map_end = (struct uk_efi_mem_desc *)((__u8 *)map_start + map_sz);
	for (md = map_start; md < map_end;
	     md = (struct uk_efi_mem_desc *)((__u8 *)md + desc_sz)) {
		if (uk_efi_md_to_bi_mrd(md, &mrd) < 0)
			continue;

		rc = ukplat_memregion_list_insert(&bi->mrds,  &mrd);
		if (unlikely(rc < 0))
			UK_CRASH("Failed to insert mrd\n");
	}

	ukplat_memregion_list_coalesce(&bi->mrds);

#if defined(__X86_64__)
	rc = ukplat_memregion_alloc_sipi_vect();
	if (unlikely(rc))
		UK_CRASH("Failed to insert SIPI vector region\n");
#endif
}

static struct uk_efi_ld_img_hndl *uk_efi_get_uk_img_hndl(void)
{
	static struct uk_efi_ld_img_hndl *uk_img_hndl;
	uk_efi_status_t status;

	/* Cache the image handle as we might need it later */
	if (uk_img_hndl)
		return uk_img_hndl;

	status = uk_efi_bs->handle_protocol(uk_efi_sh,
					    UK_EFI_LOADED_IMAGE_PROTOCOL_GUID,
					    (void **)&uk_img_hndl);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to handle loaded image protocol\n");

	return uk_img_hndl;
}

/* Read a file from a device, given a file name */
static void uk_efi_read_file(uk_efi_hndl_t dev_h, const char *file_name,
			     char **buf, __sz *len)
{
	struct uk_efi_file_proto *volume, *file_hndl;
	struct uk_efi_simple_fs_proto *sfs_proto;
	struct uk_efi_file_info_id *file_info;
	__s16 file_name16[UK_EFI_MAXPATHLEN];
	__sz len16, file_info_len;
	uk_efi_status_t status;

	/* The device must have a filesystem related driver attached to it */
	status = uk_efi_bs->handle_protocol(dev_h,
					    UK_EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
					    &sfs_proto);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to handle Simple Filesystem Protocol\n");

	/* For each block device that supports FAT12/16/32 firmware
	 * automatically creates handles for it. So now we basically open
	 * such partition
	 */
	status = sfs_proto->open_volume(sfs_proto, &volume);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to open Volume\n");

	/* UEFI only knows UTF-16 */
	len16 = ascii_to_utf16(file_name, (char *)file_name16,
			       UK_EFI_MAXPATHLEN - 1);
	if (unlikely(len16 > UK_EFI_MAXPATHLEN))
		UK_EFI_CRASH("File path too long\n");

	status = volume->open(volume, &file_hndl, file_name16,
			      UK_EFI_FILE_MODE_READ,
			      UK_EFI_FILE_READ_ONLY | UK_EFI_FILE_HIDDEN);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to open file\n");

	/* Just like GetMemoryMap, we first need to do a dummy call */
	file_info_len = 0;
	file_info = NULL;
	status = file_hndl->get_info(file_hndl, UK_EFI_FILE_INFO_ID_GUID,
				     &file_info_len, file_info);
	if (unlikely(status != UK_EFI_BUFFER_TOO_SMALL))
		UK_EFI_CRASH("Dummy call to get_info failed\n");

	status = uk_efi_bs->allocate_pool(UK_EFI_LOADER_DATA, file_info_len,
					  (void **)&file_info);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to allocate memory for file_info\n");

	status = file_hndl->get_info(file_hndl, UK_EFI_FILE_INFO_ID_GUID,
				     &file_info_len, file_info);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to get file_info\n");

	*len = file_info->file_size;
	*buf = (char *)uk_efi_alloc_max_paddr;
	status = uk_efi_bs->allocate_pages(uk_efi_alloc_type,
					   UK_EFI_LOADER_DATA,
					   DIV_ROUND_UP(*len, PAGE_SIZE),
					   (uk_efi_paddr_t *)buf);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to allocate memory for file contents\n");

	status = file_hndl->read(file_hndl, len, *buf);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to read file\n");

	status = uk_efi_bs->free_pool(file_info);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to free file_info\n");

	(*buf)[*len] = '\0';
}

static void uk_efi_setup_bootinfo_cmdl(struct ukplat_bootinfo *bi)
{
	struct ukplat_memregion_desc mrd = {0};
	struct uk_efi_ld_img_hndl *uk_img_hndl;
	uk_efi_status_t status;
	char *cmdl = NULL;
	__sz len;
	int rc;

	uk_img_hndl = uk_efi_get_uk_img_hndl();

	/* We can either have the command line provided by the user when this
	 * very specific instance of the image was launched, in which case this
	 * one takes priority, or we can have it provided through
	 * CONFIG_KVM_BOOT_PROTO_EFI_STUB_CMDLINE_PATH as a path on the same
	 * device.
	 */
	if (uk_img_hndl->load_options && uk_img_hndl->load_options_size) {
		len = (uk_img_hndl->load_options_size >> 1) + 1;

		cmdl = (char *)uk_efi_alloc_max_paddr;
		status = uk_efi_bs->allocate_pages(uk_efi_alloc_type,
						   UK_EFI_LOADER_DATA,
						   DIV_ROUND_UP(len, PAGE_SIZE),
						   (uk_efi_paddr_t *)&cmdl);
		if (unlikely(status != UK_EFI_SUCCESS))
			UK_EFI_CRASH("Failed to allocate memory for cmdl\n");

		/* Update actual size */
		len = utf16_to_ascii(uk_img_hndl->load_options, cmdl, len - 1);
		if (unlikely(len == __SZ_MAX))
			UK_EFI_CRASH("Conversion from UTF-16 to ASCII of cmdl "
				     "overflowed. This shouldn't be possible\n");
	} else if (sizeof(EFI_STUB_CMDLINE_FNAME) > 1) {
		uk_efi_read_file(uk_img_hndl->device_handle,
				 UK_EFI_ABS_FNAME(EFI_STUB_CMDLINE_FNAME),
				 (char **)&cmdl, &len);
	}

	if (!cmdl)
		return;

	mrd.pbase = (__paddr_t)cmdl;
	mrd.vbase = (__vaddr_t)cmdl;
	mrd.len = len;
	mrd.type = UKPLAT_MEMRT_CMDLINE;
	mrd.flags = UKPLAT_MEMRF_READ | UKPLAT_MEMRF_MAP;
	rc = ukplat_memregion_list_insert(&bi->mrds, &mrd);
	if (unlikely(rc < 0))
		UK_EFI_CRASH("Failed to insert cmdl mrd\n");

	bi->cmdline = (__u64)cmdl;
	bi->cmdline_len = len;
}

static void uk_efi_setup_bootinfo_initrd(struct ukplat_bootinfo *bi)
{
	struct ukplat_memregion_desc mrd = {0};
	struct uk_efi_ld_img_hndl *uk_img_hndl;
	char *initrd;
	__sz len;
	int rc;

	if (sizeof(EFI_STUB_INITRD_FNAME) <= 1)
		return;

	uk_img_hndl = uk_efi_get_uk_img_hndl();

	uk_efi_read_file(uk_img_hndl->device_handle,
			 UK_EFI_ABS_FNAME(EFI_STUB_INITRD_FNAME),
			 (char **)&initrd, &len);

	mrd.pbase = (__paddr_t)initrd;
	mrd.vbase = (__vaddr_t)initrd;
	mrd.len = len;
	mrd.type = UKPLAT_MEMRT_INITRD;
	mrd.flags = UKPLAT_MEMRF_READ | UKPLAT_MEMRF_MAP;
	rc = ukplat_memregion_list_insert(&bi->mrds, &mrd);
	if (unlikely(rc < 0))
		UK_EFI_CRASH("Failed to insert initrd mrd\n");
}

static void uk_efi_setup_bootinfo_dtb(struct ukplat_bootinfo *bi)
{
	struct ukplat_memregion_desc mrd = {0};
	struct uk_efi_ld_img_hndl *uk_img_hndl;
	char *dtb;
	__sz len;
	int rc;

	if (sizeof(EFI_STUB_DTB_FNAME) <= 1)
		return;

	uk_img_hndl = uk_efi_get_uk_img_hndl();

	uk_efi_read_file(uk_img_hndl->device_handle,
			 UK_EFI_ABS_FNAME(EFI_STUB_DTB_FNAME),
			 (char **)&dtb, &len);

	mrd.pbase = (__paddr_t)dtb;
	mrd.vbase = (__vaddr_t)dtb;
	mrd.len = len;
	mrd.type = UKPLAT_MEMRT_DEVICETREE;
	mrd.flags = UKPLAT_MEMRF_READ | UKPLAT_MEMRF_MAP;
	rc = ukplat_memregion_list_insert(&bi->mrds, &mrd);
	if (unlikely(rc < 0))
		UK_EFI_CRASH("Failed to insert dtb mrd\n");

	bi->dtb = (__u64)dtb;
}

static void uk_efi_setup_bootinfo(void)
{
	const char bl[] = "EFI_STUB";
	struct ukplat_bootinfo *bi;
	const char bp[] = "EFI";

	bi = ukplat_bootinfo_get();
	if (unlikely(!bi))
		UK_EFI_CRASH("Failed to get bootinfo\n");

	memcpy(bi->bootloader, bl, sizeof(bl));
	memcpy(bi->bootprotocol, bp, sizeof(bp));
	uk_efi_setup_bootinfo_cmdl(bi);
	uk_efi_setup_bootinfo_initrd(bi);
	uk_efi_setup_bootinfo_dtb(bi);
	uk_efi_setup_bootinfo_mrds(bi);

	bi->efi_st = (__u64)uk_efi_st;
}

/* Sect 4. of TCG Platform Reset Attack Mitigation Specification Version 1.10
 * Rev. 17
 */
static void uk_efi_reset_attack_mitigation_enable(void)
{
#ifdef CONFIG_KVM_BOOT_PROTO_EFI_STUB_RST_ATK_MITIGATION
	/* The UTF-16 encoding of the "MemoryOverwriteRequestControl" string */
	char var_name[] = "M\0e\0m\0o\0r\0y\0O\0v\0e\0r\0w\0r\0i\0t\0e\0R\0e"
			  "\0q\0u\0e\0s\0t\0C\0o\0n\0t\0r\0o\0l\0";
	uk_efi_uintn_t data_sz;
	uk_efi_status_t status;
	__u8 enable = 1;

	status = uk_efi_rs->get_variable((__s16 *)var_name,
					 MEMORY_ONLY_RESET_CONTROL_GUID,
					 NULL, &data_sz, NULL);
	/* There is either no such variable in the firmware database, or no
	 * variable storage is supported
	 */
	if (status == UK_EFI_UNSUPPORTED || status == UK_EFI_NOT_FOUND)
		return;
	else if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to get MemoryOverwriteRequestControl variable\n");

	status = uk_efi_rs->set_variable((__s16 *)var_name,
					 MEMORY_ONLY_RESET_CONTROL_GUID,
					 UK_EFI_VARIABLE_NON_VOLATILE	     |
					 UK_EFI_VARIABLE_BOOTSERVICE_ACCESS  |
					 UK_EFI_VARIABLE_RUNTIME_ACCESS,
					 sizeof(enable), &enable);
	if (unlikely(status != UK_EFI_SUCCESS))
		UK_EFI_CRASH("Failed to enable reset attack mitigation\n");
#endif
}

void __uk_efi_api __noreturn uk_efi_main(uk_efi_hndl_t self_hndl,
					 struct uk_efi_sys_tbl *sys_tbl)
{
	uk_efi_init_vars(self_hndl, sys_tbl);
	uk_efi_cls();
	uk_efi_reset_attack_mitigation_enable();

	/* uk_efi_setup_bootinfo must be called last, since it will exit Boot
	 * Service after obtaining EFI memory map
	 */
	uk_efi_setup_bootinfo();

	/* Jump to arch specific post-EFI entry */
	uk_efi_jmp_to_kern();
}
