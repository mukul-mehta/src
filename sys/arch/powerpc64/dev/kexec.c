/*	$OpenBSD: kexec.c,v 1.1 2020/07/16 19:37:58 kettenis Exp $	*/

/*
 * Copyright (c) 2019-2020 Visa Hankala
 * Copyright (c) 2020 Mark Kettenis <kettenis@openbsd.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/exec_elf.h>
#include <sys/malloc.h>
#include <sys/proc.h>

#include <uvm/uvm_extern.h>

#include <machine/kexec.h>
#include <machine/opal.h>

int	kexec_kexec(struct kexec_args *, struct proc *);
int	kexec_read(struct kexec_args *, void *, size_t, off_t);
void	kexec(paddr_t, paddr_t);

void
kexecattach(int num)
{
}

int
kexecopen(dev_t dev, int flags, int mode, struct proc *p)
{
	return (0);
}

int
kexecclose(dev_t dev, int flags, int mode, struct proc *p)
{
	return (0);
}

int
kexecioctl(dev_t dev, u_long cmd, caddr_t data, int flags, struct proc *p)
{
	int error = 0;

	switch (cmd) {
	case KIOC_KEXEC:
		error = suser(p);
		if (error != 0)
			break;
		error = kexec_kexec((struct kexec_args *)data, p);
		break;

	case KIOC_GETBOOTDUID:
		memcpy(data, bootduid, sizeof(bootduid));
		break;

	default:
		error = ENOTTY;
		break;
	}

	return error;
}

int
kexec_kexec(struct kexec_args *kargs, struct proc *p)
{
	extern paddr_t fdt_pa;
	struct kmem_pa_mode kp_kexec = {
		.kp_constraint = &no_constraint,
		.kp_boundary = SEGMENT_SIZE,
		.kp_maxseg = 1,
		.kp_zero = 1
	};
	Elf_Ehdr eh;
	Elf_Phdr *ph = NULL;
	Elf_Shdr *sh = NULL;
	vaddr_t start = VM_MAX_ADDRESS;
	vaddr_t end = 0;
	paddr_t start_pa;
	vsize_t align = 0;;
	caddr_t addr;
	size_t phsize, shsize, size;
	int error, i;

	/*
	 * Read the headers and validate them.
	 */
	error = kexec_read(kargs, &eh, sizeof(eh), 0);
	if (error != 0)
		goto fail;

	/* Load program headers. */
	ph = mallocarray(eh.e_phnum, sizeof(Elf_Phdr), M_TEMP, M_NOWAIT);
	if (ph == NULL) {
		error = ENOMEM;
		goto fail;
	}
	phsize = eh.e_phnum * sizeof(Elf_Phdr);
	error = kexec_read(kargs, ph, phsize, eh.e_phoff);
	if (error != 0)
		goto fail;

	/* Load section headers. */
	sh = mallocarray(eh.e_shnum, sizeof(Elf_Shdr), M_TEMP, M_NOWAIT);
	if (sh == NULL) {
		error = ENOMEM;
		goto fail;
	}
	shsize = eh.e_shnum * sizeof(Elf_Shdr);
	error = kexec_read(kargs, sh, shsize, eh.e_shoff);
	if (error != 0)
		goto fail;

	/*
	 * Allocate physical memory and load the segments.
	 */

	for (i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		start = MIN(start, ph[i].p_vaddr);
		align = MAX(align, ph[i].p_align);
		end = MAX(end, ph[i].p_vaddr + ph[i].p_memsz);
	}
	size = round_page(end) - start;

	kp_kexec.kp_align = align;
	addr = km_alloc(size, &kv_any, &kp_kexec, &kd_nowait);
	if (addr == NULL) {
		error = ENOMEM;
		goto fail;
	}

	for (i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;

		error = kexec_read(kargs, addr + (ph[i].p_vaddr - start),
		    ph[i].p_filesz, ph[i].p_offset);
		if (error != 0)
			goto fail;

		/* Clear any BSS. */
		if (ph[i].p_memsz > ph[i].p_filesz) {
			memset(addr + (ph[i].p_vaddr + ph[i].p_filesz) - start,
			    0, ph[i].p_memsz - ph[i].p_filesz);
		}
	}

	for (i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_OPENBSD_RANDOMIZE)
			continue;

		/* Assume that the segment is inside a LOAD segment. */
		arc4random_buf(addr + ph[i].p_vaddr - start, ph[i].p_filesz);
	}

	printf("launching kernel\n");

	config_suspend_all(DVACT_POWERDOWN);

	intr_disable();

	pmap_extract(pmap_kernel(), (vaddr_t)addr, &start_pa);
	kexec(start_pa + (eh.e_entry - start), fdt_pa);

	for (;;)
		continue;

fail:
	free(sh, M_TEMP, shsize);
	free(ph, M_TEMP, phsize);
	return error;
}

int
kexec_read(struct kexec_args *kargs, void *buf, size_t size, off_t off)
{
	if (off + size < off || off + size > kargs->klen)
		return ENOEXEC;
	return copyin(kargs->kimg + off, buf, size);
}
