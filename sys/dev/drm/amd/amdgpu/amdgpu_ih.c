/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <drm/drmP.h>
#include "amdgpu.h"
#include "amdgpu_ih.h"

/**
 * amdgpu_ih_ring_init - initialize the IH state
 *
 * @adev: amdgpu_device pointer
 * @ih: ih ring to initialize
 * @ring_size: ring size to allocate
 * @use_bus_addr: true when we can use dma_alloc_coherent
 *
 * Initializes the IH state and allocates a buffer
 * for the IH ring buffer.
 * Returns 0 for success, errors for failure.
 */
int amdgpu_ih_ring_init(struct amdgpu_device *adev, struct amdgpu_ih_ring *ih,
			unsigned ring_size, bool use_bus_addr)
{
	u32 rb_bufsz;
	int r;

	/* Align ring size */
	rb_bufsz = order_base_2(ring_size / 4);
	ring_size = (1 << rb_bufsz) * 4;
	ih->ring_size = ring_size;
	ih->ptr_mask = ih->ring_size - 1;
	ih->rptr = 0;
	ih->use_bus_addr = use_bus_addr;

	if (use_bus_addr) {
		if (ih->ring)
			return 0;

		/* add 8 bytes for the rptr/wptr shadows and
		 * add them to the end of the ring allocation.
		 */
		ih->ring = dma_alloc_coherent(adev->dev, ih->ring_size + 8,
					      &ih->rb_dma_addr, GFP_KERNEL);
		if (ih->ring == NULL)
			return -ENOMEM;

		memset((void *)ih->ring, 0, ih->ring_size + 8);
		ih->wptr_offs = (ih->ring_size / 4) + 0;
		ih->rptr_offs = (ih->ring_size / 4) + 1;
	} else {
		r = amdgpu_device_wb_get(adev, &ih->wptr_offs);
		if (r)
			return r;

		r = amdgpu_device_wb_get(adev, &ih->rptr_offs);
		if (r) {
			amdgpu_device_wb_free(adev, ih->wptr_offs);
			return r;
		}

		r = amdgpu_bo_create_kernel(adev, ih->ring_size, PAGE_SIZE,
					    AMDGPU_GEM_DOMAIN_GTT,
					    &ih->ring_obj, (u64*)&ih->gpu_addr,
					    (void **)&ih->ring);
		if (r) {
			amdgpu_device_wb_free(adev, ih->rptr_offs);
			amdgpu_device_wb_free(adev, ih->wptr_offs);
			return r;
		}
	}
	return 0;
}

/**
 * amdgpu_ih_ring_fini - tear down the IH state
 *
 * @adev: amdgpu_device pointer
 * @ih: ih ring to tear down
 *
 * Tears down the IH state and frees buffer
 * used for the IH ring buffer.
 */
void amdgpu_ih_ring_fini(struct amdgpu_device *adev, struct amdgpu_ih_ring *ih)
{
	if (ih->use_bus_addr) {
		if (!ih->ring)
			return;

		/* add 8 bytes for the rptr/wptr shadows and
		 * add them to the end of the ring allocation.
		 */
		dma_free_coherent(adev->dev, ih->ring_size + 8,
				  (void *)ih->ring, ih->rb_dma_addr);
		ih->ring = NULL;
	} else {
		amdgpu_bo_free_kernel(&ih->ring_obj, (u64*)&ih->gpu_addr,
				      (void **)&ih->ring);
		amdgpu_device_wb_free(adev, ih->wptr_offs);
		amdgpu_device_wb_free(adev, ih->rptr_offs);
	}
}

/**
 * amdgpu_ih_process - interrupt handler
 *
 * @adev: amdgpu_device pointer
 * @ih: ih ring to process
 *
 * Interrupt hander (VI), walk the IH ring.
 * Returns irq process return code.
 */
int amdgpu_ih_process(struct amdgpu_device *adev, struct amdgpu_ih_ring *ih,
		      void (*callback)(struct amdgpu_device *adev,
				       struct amdgpu_ih_ring *ih))
{
	u32 wptr;

	if (!ih->enabled || adev->shutdown)
		return IRQ_NONE;

	wptr = amdgpu_ih_get_wptr(adev);

restart_ih:
	/* is somebody else already processing irqs? */
	if (atomic_xchg(&ih->lock, 1))
		return IRQ_NONE;

#if 0
	DRM_DEBUG("%s: rptr %d, wptr %d\n", __func__, ih->rptr, wptr);
#endif

	/* Order reading of wptr vs. reading of IH ring data */
	rmb();

	while (ih->rptr != wptr) {
		callback(adev, ih);
		ih->rptr &= ih->ptr_mask;
	}

	amdgpu_ih_set_rptr(adev);
	atomic_set(&ih->lock, 0);

	/* make sure wptr hasn't changed while processing */
	wptr = amdgpu_ih_get_wptr(adev);
	if (wptr != ih->rptr)
		goto restart_ih;

	return IRQ_HANDLED;
}

