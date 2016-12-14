#ifndef XHCI_IO_H
#define XHCI_IO_H

#include <stdint.h> /* uint32_t */
#include <stddef.h> /* size_t */
#include <stdarg.h> /* va_list */

uint32_t le_to_cpu32(uint32_t val);

uint32_t cpu_to_le32(uint32_t val);

uint64_t cpu_to_le64(uint64_t val);

int xhci_debug_enabled(void);

void xhci_mdelay(unsigned int delay);

int xhci_printf (const char *fmt, ...);

int xhci_snprintf (char *str, size_t n, const char *fmt, ...);

int xhci_vprintf (const char *fmt, va_list ap);

void xhci_debug(const char *fmt, ...);

void *xhci_calloc(size_t nmemb, size_t size);

void *xhci_malloc(size_t size);

void *xhci_memset(void *s, int c, size_t n);

void xhci_free(void *ptr);

void *xhci_dma_alloc(size_t align, size_t size);

void *dma_memalign(size_t align, size_t size);

uintptr_t xhci_dma_get_phys(void *ptr);

uintptr_t virt_to_phys(volatile void *ptr);

void *phys_to_virt(uintptr_t phys);

uint8_t mmio_read8 (const volatile uint8_t *addr);

uint16_t mmio_read16 (const volatile uint16_t *addr);

uint32_t mmio_read32 (const volatile uint32_t *addr);

uint64_t mmio_read64 (const volatile uint64_t *addr);

void mmio_write8 (volatile uint8_t *addr, uint8_t val);

void mmio_write16 (volatile uint16_t *addr, uint16_t val);

void mmio_write32 (volatile uint32_t *addr, uint32_t val);

void mmio_write64 (volatile uint64_t *addr, uint64_t val);

#endif /* XHCI_IO_H */
