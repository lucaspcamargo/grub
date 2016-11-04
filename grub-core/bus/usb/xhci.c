/* xhci.h - xHCI driver */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2016 Hiddn Security AS
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Simple xHCI driver for GRUB. No interrupts, just polling. No 64-bit
 * addressing support.
 *
 * [spec] http://www.intel.com/content/www/us/en/io/universal-serial-bus/extensible-host-controler-interface-usb-xhci.html
 */

#include <grub/dl.h>
#include <grub/mm.h>
#include <grub/usb.h>
#include <grub/usbtrans.h>
#include <grub/misc.h>
#include <grub/pci.h>
#include <grub/cpu/pci.h>
#include <grub/cpu/io.h>
#include <grub/time.h>
#include <grub/loader.h>
#include <grub/disk.h>
#include <grub/cache.h>

GRUB_MOD_LICENSE ("GPLv3+");

/* Host Controller Capability Registers. Section 5.3 in [spec]. */
enum
{
  GRUB_XHCI_CAP_CAPLENGTH  = 0x00, /* 1 byte */
  /* 1 byte reserved */
  GRUB_XHCI_CAP_HCIVERSION = 0x02, /* 2 bytes */
  GRUB_XHCI_CAP_HCSPARAMS1 = 0x04, /* 4 bytes */
  GRUB_XHCI_CAP_HCSPARAMS2 = 0x08, /* 4 bytes */
  GRUB_XHCI_CAP_HCSPARAMS3 = 0x0c, /* 4 bytes */
  GRUB_XHCI_CAP_HCCPARAMS1 = 0x10, /* 4 bytes */
  GRUB_XHCI_CAP_DBOFF      = 0x14, /* 4 bytes */
  GRUB_XHCI_CAP_RTSOFF     = 0x18, /* 4 bytes */
  GRUB_XHCI_CAP_HCCPARAMS2 = 0x1c, /* 4 bytes */
  /* (CAPLENGTH - 0x20) bytes reserved */
};

/* Host Controller Operational Registers. Section 5.4 in [spec]. */
enum
{
  GRUB_XHCI_OPER_USBCMD   = 0x00,
  GRUB_XHCI_OPER_USBSTS   = 0x04,
  GRUB_XHCI_OPER_PAGESIZE = 0x08,
  /* 0x0c - 0x13 reserved */
  GRUB_XHCI_OPER_DNCTRL   = 0x14,
  GRUB_XHCI_OPER_CRCR     = 0x18,
  /* 0x20 - 0x2f reserved */
  GRUB_XHCI_OPER_DCBAAP   = 0x30,
  GRUB_XHCI_OPER_CONFIG   = 0x38,
  /* 0x3c - 0x3ff reserved */
  /* 0x400 - 0x13ff Port Register Set 1-MaxPorts */
};

/* USB Command Register (USBCMD) bits. Section 5.4.1 in [spec]. */
enum
{
  GRUB_XHCI_OPER_USBCMD_RUNSTOP    = (1 <<  0),
  GRUB_XHCI_OPER_USBCMD_HCRST      = (1 <<  1), /* host controller reset */
  GRUB_XHCI_OPER_USBCMD_INTE       = (1 <<  2), /* interrupter enable */
  GRUB_XHCI_OPER_USBCMD_HSEE       = (1 <<  3), /* host system error enable */
  /* bit 6:4 reserved */
  GRUB_XHCI_OPER_USBCMD_LHCRST     = (1 <<  7), /* light host controller reset */
  GRUB_XHCI_OPER_USBCMD_CSS        = (1 <<  8), /* controller save state */
  GRUB_XHCI_OPER_USBCMD_CRS        = (1 <<  9), /* controller restore state */
  GRUB_XHCI_OPER_USBCMD_EWE        = (1 << 10), /* enable wrap event */
  /* ... */
};

/* USB Status Register (USBSTS) bits. Section 5.4.2 in [spec]. */
enum
{
  GRUB_XHCI_USBSTS_HCH  = (1 <<  0), /* host controller halted */
  /* reserved */
  GRUB_XHCI_USBSTS_HSE  = (1 <<  2), /* host system error */
  GRUB_XHCI_USBSTS_EINT = (1 <<  3), /* event interrupt */
  GRUB_XHCI_USBSTS_PCD  = (1 <<  4), /* port change detect */
  /* 7:5 reserved */
  GRUB_XHCI_USBSTS_SSS  = (1 <<  8), /* save state status */
  GRUB_XHCI_USBSTS_RSS  = (1 <<  9), /* restore state status */
  GRUB_XHCI_USBSTS_SRE  = (1 << 10), /* save/restore error */
  GRUB_XHCI_USBSTS_CNR  = (1 << 11), /* controller not ready */
  GRUB_XHCI_USBSTS_HCE  = (1 << 12), /* host controller error */
  /* 31:13 reserved */
};

/* Offset relative to Operational Base */
#define GRUB_XHCI_PORTSC(port) (0x400 + (0x10 * (port - 1)))

#define GRUB_XHCI_ADDR_MEM_MASK	(~0xff)
#define GRUB_XHCI_POINTER_MASK	(~0x1f)

/* USB Legacy Support Capability (USBLEGSUP) bits. Section 7.1.1 in [spec]. */
enum
{
  GRUB_XHCI_USBLEGSUP_BIOS_OWNED = (1 << 16),
  GRUB_XHCI_USBLEGSUP_OS_OWNED = (1 << 24)
};

/* Operational register PORTSC bits */
enum
{
  GRUB_XHCI_PORTSC_CCS = (1 << 0), /* current connect status */
  GRUB_XHCI_PORTSC_PED = (1 << 1), /* port enabled/disabled */
  /* reserved */
  GRUB_XHCI_PORT_ENABLED = (1 << 2),
  GRUB_XHCI_PORT_ENABLED_CH = (1 << 3),
  GRUB_XHCI_PORT_OVERCUR = (1 << 4),
  GRUB_XHCI_PORT_OVERCUR_CH = (1 << 5),
  GRUB_XHCI_PORT_RESUME = (1 << 6),
  GRUB_XHCI_PORT_SUSPEND = (1 << 7),
  GRUB_XHCI_PORT_RESET = (1 << 8),
  GRUB_XHCI_PORT_LINE_STAT = (3 << 10),
  GRUB_XHCI_PORT_POWER = (1 << 12),
  GRUB_XHCI_PORT_OWNER = (1 << 13),
  GRUB_XHCI_PORT_INDICATOR = (3 << 14),
  GRUB_XHCI_PORT_TEST = (0xf << 16),
  GRUB_XHCI_PORT_WON_CONN_E = (1 << 20),
  GRUB_XHCI_PORT_WON_DISC_E = (1 << 21),
  GRUB_XHCI_PORT_WON_OVER_E = (1 << 22),

  GRUB_XHCI_PORT_LINE_SE0 = (0 << 10),
  GRUB_XHCI_PORT_LINE_K = (1 << 10),
  GRUB_XHCI_PORT_LINE_J = (2 << 10),
  GRUB_XHCI_PORT_LINE_UNDEF = (3 << 10),
  GRUB_XHCI_PORT_LINE_LOWSP = GRUB_XHCI_PORT_LINE_K,	/* K state means low speed */
  GRUB_XHCI_PORT_WMASK = ~(GRUB_XHCI_PORTSC_PED
			   | GRUB_XHCI_PORT_ENABLED_CH
			   | GRUB_XHCI_PORT_OVERCUR_CH)
};

/* Operational register CONFIGFLAGS bits */
enum
{
  GRUB_XHCI_CF_XHCI_OWNER = (1 << 0)
};

/* Queue Head & Transfer Descriptor constants */
#define GRUB_XHCI_HPTR_OFF       5	/* Horiz. pointer bit offset */
enum
{
  GRUB_XHCI_HPTR_TYPE_MASK = (3 << 1),
  GRUB_XHCI_HPTR_TYPE_ITD = (0 << 1),
  GRUB_XHCI_HPTR_TYPE_QH = (1 << 1),
  GRUB_XHCI_HPTR_TYPE_SITD = (2 << 1),
  GRUB_XHCI_HPTR_TYPE_FSTN = (3 << 1)
};

enum
{
  GRUB_XHCI_C = (1 << 27),
  GRUB_XHCI_MAXPLEN_MASK = (0x7ff << 16),
  GRUB_XHCI_H = (1 << 15),
  GRUB_XHCI_DTC = (1 << 14),
  GRUB_XHCI_SPEED_MASK = (3 << 12),
  GRUB_XHCI_SPEED_FULL = (0 << 12),
  GRUB_XHCI_SPEED_LOW = (1 << 12),
  GRUB_XHCI_SPEED_HIGH = (2 << 12),
  GRUB_XHCI_SPEED_RESERVED = (3 << 12),
  GRUB_XHCI_EP_NUM_MASK = (0xf << 8),
  GRUB_XHCI_DEVADDR_MASK = 0x7f,
  GRUB_XHCI_TARGET_MASK = (GRUB_XHCI_EP_NUM_MASK | GRUB_XHCI_DEVADDR_MASK)
};

enum
{
  GRUB_XHCI_MAXPLEN_OFF = 16,
  GRUB_XHCI_SPEED_OFF = 12,
  GRUB_XHCI_EP_NUM_OFF = 8
};

enum
{
  GRUB_XHCI_MULT_MASK = (3 << 30),
  GRUB_XHCI_MULT_RESERVED = (0 << 30),
  GRUB_XHCI_MULT_ONE = (1 << 30),
  GRUB_XHCI_MULT_TWO = (2 << 30),
  GRUB_XHCI_MULT_THREE = (3 << 30),
  GRUB_XHCI_DEVPORT_MASK = (0x7f << 23),
  GRUB_XHCI_HUBADDR_MASK = (0x7f << 16),
  GRUB_XHCI_CMASK_MASK = (0xff << 8),
  GRUB_XHCI_SMASK_MASK = (0xff << 0),
};

enum
{
  GRUB_XHCI_MULT_OFF = 30,
  GRUB_XHCI_DEVPORT_OFF = 23,
  GRUB_XHCI_HUBADDR_OFF = 16,
  GRUB_XHCI_CMASK_OFF = 8,
  GRUB_XHCI_SMASK_OFF = 0,
};

#define GRUB_XHCI_TERMINATE      (1<<0)

#define GRUB_XHCI_TOGGLE         (1<<31)

enum
{
  GRUB_XHCI_TOTAL_MASK = (0x7fff << 16),
  GRUB_XHCI_CERR_MASK = (3 << 10),
  GRUB_XHCI_CERR_0 = (0 << 10),
  GRUB_XHCI_CERR_1 = (1 << 10),
  GRUB_XHCI_CERR_2 = (2 << 10),
  GRUB_XHCI_CERR_3 = (3 << 10),
  GRUB_XHCI_PIDCODE_OUT = (0 << 8),
  GRUB_XHCI_PIDCODE_IN = (1 << 8),
  GRUB_XHCI_PIDCODE_SETUP = (2 << 8),
  GRUB_XHCI_STATUS_MASK = 0xff,
  GRUB_XHCI_STATUS_ACTIVE = (1 << 7),
  GRUB_XHCI_STATUS_HALTED = (1 << 6),
  GRUB_XHCI_STATUS_BUFERR = (1 << 5),
  GRUB_XHCI_STATUS_BABBLE = (1 << 4),
  GRUB_XHCI_STATUS_TRANERR = (1 << 3),
  GRUB_XHCI_STATUS_MISSDMF = (1 << 2),
  GRUB_XHCI_STATUS_SPLITST = (1 << 1),
  GRUB_XHCI_STATUS_PINGERR = (1 << 0)
};

enum
{
  GRUB_XHCI_TOTAL_OFF = 16,
  GRUB_XHCI_CERR_OFF = 10
};

#define GRUB_XHCI_BUFPTR_MASK    (0xfffff<<12)
#define GRUB_XHCI_QHTDPTR_MASK   0xffffffe0

#define GRUB_XHCI_TD_BUF_PAGES   5

#define GRUB_XHCI_BUFPAGELEN     0x1000
#define GRUB_XHCI_MAXBUFLEN      0x5000


/** Capability register length */
#define XHCI_CAP_CAPLENGTH 0x00

/** Host controller interface version number */
#define XHCI_CAP_HCIVERSION 0x02

/** Structural parameters 1 */
#define XHCI_CAP_HCSPARAMS1 0x04

/** Number of device slots */
#define XHCI_HCSPARAMS1_SLOTS(params) ( ( (params) >> 0 ) & 0xff )

/** Number of interrupters */
#define XHCI_HCSPARAMS1_INTRS(params) ( ( (params) >> 8 ) & 0x3ff )

/** Number of ports */
#define XHCI_HCSPARAMS1_PORTS(params) ( ( (params) >> 24 ) & 0xff )

/** Structural parameters 2 */
#define XHCI_CAP_HCSPARAMS2 0x08

/** Number of page-sized scratchpad buffers */
#define XHCI_HCSPARAMS2_SCRATCHPADS(params) \
	( ( ( (params) >> 16 ) & 0x3e0 ) | ( ( (params) >> 27 ) & 0x1f ) )

/** Capability parameters */
#define XHCI_CAP_HCCPARAMS1 0x10

static grub_uint32_t
pci_config_read (grub_pci_device_t dev, unsigned int reg)
{
  grub_pci_address_t addr;
  addr = grub_pci_make_address (dev, reg);
  return grub_pci_read (addr);
}


static grub_uint8_t
pci_config_read8 (grub_pci_device_t dev, unsigned int reg)
{
  grub_pci_address_t addr;
  addr = grub_pci_make_address (dev, reg);
  return grub_pci_read_byte (addr);
}

static grub_uint16_t
pci_config_read16 (grub_pci_device_t dev, unsigned int reg)
{
  grub_pci_address_t addr;
  addr = grub_pci_make_address (dev, reg);
  return grub_le_to_cpu16 (grub_pci_read_word (addr) );
}

static grub_uint32_t
pci_config_read32 (grub_pci_device_t dev, unsigned int reg)
{
  grub_pci_address_t addr;
  addr = grub_pci_make_address (dev, reg);
  return grub_le_to_cpu32 (grub_pci_read (addr) );
}

/** Number of registers per port */
#define NUM_PORT_REGS 4

/** bit 1:0 is Rsvd */
#define DBOFF_MASK (~0x3)

/** bit 4:0 is Rsvd */
#define RTSOFF_MASK (~0x1f)

/** Capability registers */
struct xhci_cap_regs {
  /* These are read only, so we don't need volatile */
  const grub_uint8_t caplength;
  const grub_uint8_t rsvd1;
  const grub_uint16_t hciversion;
  const grub_uint32_t hcsparams1;
  const grub_uint32_t hcsparams2;
  const grub_uint32_t hcsparams3;
  const grub_uint32_t hccparams1;
  const grub_uint32_t dboff;
  const grub_uint32_t rtsoff;
  const grub_uint32_t hccparams2;
  /* Reserved up to (caplength - 0x20) */
};

/** Operational registers */
struct xhci_oper_regs {
  /** USB Command */
  volatile grub_uint32_t usbcmd;
  /** USB Status */
  volatile grub_uint32_t usbsts;
  /* Page Size */
  volatile grub_uint32_t pagesize;
  /** Reserved */
  grub_uint32_t rsvdz1;
  grub_uint32_t rsvdz2;
  /** Device Notification Control */
  volatile grub_uint32_t dnctrl;
  /** Command Ring Control */
  volatile grub_uint32_t crcr;
  /** Reserved 0x20-0x2F */
  grub_uint32_t rsvdz3[4];
  /** Device Context Base Address Array Pointer */
  volatile grub_uint32_t dcbaap;
  /** Configure */
  volatile grub_uint32_t config;
  /** Reserved 0x03c-0x3ff */
  grub_uint32_t rsvdz4[241];
  /** Port Register Set 1-MaxPorts (0x400-0x13ff) */
  volatile grub_uint32_t reserved[1024];
};

/** Runtime registers */
struct xhci_run_regs {
  const volatile grub_uint32_t microframe_index;
};

/** Port Register Set */
//struct xhci_port_reg_set {
//  volatile grub_uint32_t portsc;
//  volatile grub_uint32_t portpmsc;
//  volatile grub_uint32_t portli;
//  volatile grub_uint32_t porthlpmc;
//};

#define MAX_DOORBELL_ENTRIES 256

/** Doorbell array registers */
struct xhci_doorbell_regs {
  volatile grub_uint32_t doorbell[MAX_DOORBELL_ENTRIES];
};

struct grub_xhci
{
  volatile struct xhci_cap_regs *cap_regs;
  volatile struct xhci_oper_regs *oper_regs;
  volatile struct xhci_run_regs *run_regs;
  volatile struct xhci_doorbell_regs *db_regs;

  /* valid range 1-255 */
  grub_uint8_t max_device_slots;
  /* valid range 1-255 */
  grub_uint8_t max_ports;

  /* linked list */
  struct grub_xhci *next;


  /* DEPRECATED STUFF BELOW */

  volatile void *regs;     /* Start of registers (same addr as capability) */
  /* Pointers to specific register areas */
  volatile grub_uint8_t *cap;	   /* Capability registers */
  volatile grub_uint8_t *oper;	   /* Operational registers */
  volatile grub_uint8_t *runtime;  /* Runtime registers */
  //volatile grub_uint8_t *doorbell; /* Doorbell Array */

  unsigned int slots;  /* number of device slots */
  unsigned int ports;  /* number of ports */

  /* grub stuff */
  volatile grub_uint32_t *iobase_cap;	   /* Capability registers */
  volatile grub_uint32_t *iobase_oper;	   /* Operational registers */
  //unsigned int reset;
};

static struct grub_xhci *xhci_list;

/* xHCI capability registers access functions */
static inline grub_uint32_t
grub_xhci_cap_read32 (struct grub_xhci *xhci, grub_uint32_t off)
{
  return grub_le_to_cpu32 (*((volatile grub_uint32_t *) xhci->cap +
		       (off / sizeof (grub_uint32_t))));
}

static inline grub_uint8_t
mmio_read8 (const volatile grub_uint8_t *addr)
{
  return *addr;
}

static inline grub_uint16_t
mmio_read16 (const volatile grub_uint16_t *addr)
{
  return grub_le_to_cpu16 (*addr);
}

static inline grub_uint32_t
mmio_read32 (const volatile grub_uint32_t *addr)
{
  return grub_le_to_cpu32 (*addr);
}

static inline void
mmio_write8 (volatile grub_uint8_t *addr, grub_uint8_t val)
{
  *addr = val;
}

static inline void
mmio_write16 (volatile grub_uint16_t *addr, grub_uint16_t val)
{
  *addr = grub_cpu_to_le16 (val);
}

static inline void
mmio_write32 (volatile grub_uint32_t *addr, grub_uint32_t val)
{
  *addr = grub_cpu_to_le32 (val);
}

enum xhci_portrs_type
{
  PORTSC = 0,
  PORTPMSC = 4,
  PORTLI = 8,
  PORTHLPMC = 12,
};

/* Read Port Register Set n of given type */
static inline grub_uint32_t
xhci_read_portrs(struct grub_xhci *xhci, unsigned int port, enum xhci_portrs_type type)
{
  grub_uint8_t *addr;

  if (port > xhci->max_ports)
  {
    grub_dprintf ("xhci", "too big port number\n");
    return ~0;
  }

  addr = (grub_uint8_t*)xhci->oper_regs + 0x400 + (0x10 * (port - 1)) + type;
  return mmio_read32 ((grub_uint32_t *)addr);
}

/* Halt if xHCI HC not halted */
static grub_usb_err_t
grub_xhci_halt (struct grub_xhci *xhci)
{
  (void)xhci;
  //grub_uint64_t maxtime;
  //unsigned int is_halted;

  grub_dprintf ("xhci", "grub_xhci_halt enter\n");
  //is_halted = grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBSTS) & GRUB_XHCI_USBSTS_HCH;
  //if (is_halted == 0)
  //  {
  //    grub_xhci_oper_write32 (xhci, GRUB_XHCI_OPER_USBCMD,
  //      		      ~GRUB_XHCI_OPER_USBCMD_RUNSTOP
  //      		      & grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD));
  //    /* Ensure command is written */
  //    grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD);
  //    maxtime = grub_get_time_ms () + 16000; /* spec says 16ms max */
  //    while (((grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBSTS)
  //             & GRUB_XHCI_USBSTS_HCH) == 0)
  //           && (grub_get_time_ms () < maxtime));
  //    if ((grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBSTS)
  //         & GRUB_XHCI_USBSTS_HCH) == 0)
  //      return GRUB_USB_ERR_TIMEOUT;
  //  }

  return GRUB_USB_ERR_NONE;
}

/* xHCI HC reset */
static grub_usb_err_t
grub_xhci_reset (struct grub_xhci *xhci)
{
  //grub_uint64_t maxtime;
  (void)xhci;

  grub_dprintf ("xhci", "grub_xhci_reset enter\n");

  //sync_all_caches (xhci);

  //grub_xhci_oper_write32 (xhci, GRUB_XHCI_OPER_USBCMD,
  //      		  GRUB_XHCI_OPER_USBCMD_HCRST
  //      		  | grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD));
  ///* Ensure command is written */
  //grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD);
  ///* XXX: How long time could take reset of HC ? */
  //maxtime = grub_get_time_ms () + 16000;
  //while (((grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD)
  //         & GRUB_XHCI_OPER_USBCMD_HCRST) != 0)
  //       && (grub_get_time_ms () < maxtime));
  //if ((grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD)
  //     & GRUB_XHCI_OPER_USBCMD_HCRST) != 0)
  //  return GRUB_USB_ERR_TIMEOUT;

  return GRUB_USB_ERR_NONE;
}


#if 0

static void
sync_all_caches (struct grub_xhci *xhci)
{
  (void)xhci;
  grub_dprintf("xhci", "sync_all_caches enter\n");
  return;
#if 0
  if (!xhci)
    return;
  if (xhci->td_virt)
    grub_arch_sync_dma_caches (xhci->td_virt, sizeof (struct grub_xhci_td) *
			       GRUB_XHCI_N_TD);
  if (xhci->qh_virt)
    grub_arch_sync_dma_caches (xhci->qh_virt, sizeof (struct grub_xhci_qh) *
			       GRUB_XHCI_N_QH);
  if (xhci->framelist_virt)
    grub_arch_sync_dma_caches (xhci->framelist_virt, 4096);
#endif
}

static inline grub_uint16_t
grub_xhci_cap_read16 (struct grub_xhci *xhci, grub_uint32_t addr)
{
  return
    grub_le_to_cpu16 (*((volatile grub_uint16_t *) xhci->iobase_cap +
		       (addr / sizeof (grub_uint16_t))));
}

static inline grub_uint8_t
grub_xhci_cap_read8 (struct grub_xhci *xhci, grub_uint32_t addr)
{
  return *((volatile grub_uint8_t *) xhci->iobase_cap + addr);
}

/* Operational registers access functions */
static inline grub_uint32_t
grub_xhci_oper_read32 (struct grub_xhci *xhci, grub_uint32_t addr)
{
  return
    grub_le_to_cpu32 (*
		      ((volatile grub_uint32_t *) xhci->iobase_oper +
		       (addr / sizeof (grub_uint32_t))));
}

static inline void
grub_xhci_oper_write32 (struct grub_xhci *xhci, grub_uint32_t addr,
			grub_uint32_t value)
{
  *((volatile grub_uint32_t *) xhci->iobase_oper + (addr / sizeof (grub_uint32_t))) =
    grub_cpu_to_le32 (value);
}

static inline grub_uint32_t
grub_xhci_port_read (struct grub_xhci *xhci, grub_uint32_t port)
{
  return grub_xhci_oper_read32 (xhci, GRUB_XHCI_PORTSC(port));
}

static inline void
grub_xhci_port_resbits (struct grub_xhci *xhci, grub_uint32_t port,
			grub_uint32_t bits)
{
  grub_xhci_oper_write32 (xhci, GRUB_XHCI_PORTSC(port),
			  grub_xhci_port_read (xhci,
					       port) & GRUB_XHCI_PORT_WMASK &
			  ~(bits));
  grub_xhci_port_read (xhci, port);
}

static inline void
grub_xhci_port_setbits (struct grub_xhci *xhci, grub_uint32_t port,
			grub_uint32_t bits)
{
  grub_xhci_oper_write32 (xhci, GRUB_XHCI_PORTSC(port),
			  (grub_xhci_port_read (xhci, port) &
			   GRUB_XHCI_PORT_WMASK) | bits);
  grub_xhci_port_read (xhci, port);
}


#endif

static grub_err_t
grub_xhci_restore_hw (void)
{
  struct grub_xhci *xhci;
  //grub_uint32_t n_ports;
  //int i;

  grub_dprintf("xhci", "grub_xhci_restore_hw enter\n");
  /* We should re-enable all xHCI HW similarly as on inithw */
  for (xhci = xhci_list; xhci; xhci = xhci->next)
    {
      /* Check if xHCI is halted and halt it if not */
      if (grub_xhci_halt (xhci) != GRUB_USB_ERR_NONE)
	grub_error (GRUB_ERR_TIMEOUT, "restore_hw: xHCI halt timeout");

      /* Reset xHCI */
      if (grub_xhci_reset (xhci) != GRUB_USB_ERR_NONE)
	grub_error (GRUB_ERR_TIMEOUT, "restore_hw: xHCI reset timeout");

      /* Setup some xHCI registers and enable xHCI */
//      grub_xhci_oper_write32 (xhci, GRUB_XHCI_OPER_USBCMD,
//			      GRUB_XHCI_OPER_USBCMD_RUNSTOP |
//			      grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD));

      /* Now should be possible to power-up and enumerate ports etc. */
	  /* Power on all ports */
    }

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_xhci_fini_hw (int noreturn __attribute__ ((unused)))
{
  struct grub_xhci *xhci;

  grub_dprintf ("xhci", "grub_xhci_fini_hw enter\n");

  /* We should disable all xHCI HW to prevent any DMA access etc. */
  for (xhci = xhci_list; xhci; xhci = xhci->next)
    {
      /* Disable both lists */
      //grub_xhci_oper_write32 (xhci, GRUB_XHCI_OPER_USBCMD,
       // ~(GRUB_XHCI_CMD_AS_ENABL | GRUB_XHCI_CMD_PS_ENABL)
        //& grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD));

      /* Check if xHCI is halted and halt it if not */
      //grub_xhci_halt (xhci);

      /* Reset xHCI */
      //grub_xhci_reset (xhci);
    }

  return GRUB_ERR_NONE;
}

static grub_usb_err_t
grub_xhci_cancel_transfer (grub_usb_controller_t dev,
			   grub_usb_transfer_t transfer)
{
  struct grub_xhci *xhci = dev->data;
  struct grub_xhci_transfer_controller_data *cdata =
    transfer->controller_data;
  (void)cdata;
  (void)xhci;
  grub_dprintf ("xhci", "grub_xhci_cancel_transfer: begin\n");
  return GRUB_USB_ERR_NONE;

#if 0
  grub_size_t actual;
  int i;
  grub_uint64_t maxtime;
  grub_uint32_t qh_phys;

  sync_all_caches (xhci);

  grub_uint32_t interrupt =
    cdata->qh_virt->ep_cap & GRUB_XHCI_SMASK_MASK;

  /* QH can be active and should be de-activated and halted */

  grub_dprintf ("xhci", "cancel_transfer: begin\n");

  /* First check if xHCI is running - if not, there is no problem */
  /* to cancel any transfer. Or, if transfer is asynchronous, check */
  /* if AL is enabled - if not, transfer can be canceled also. */
  if (((grub_xhci_oper_read32 (xhci, GRUB_XHCI_STATUS) &
      GRUB_XHCI_ST_HC_HALTED) != 0) ||
    (!interrupt && ((grub_xhci_oper_read32 (xhci, GRUB_XHCI_STATUS) &
      (GRUB_XHCI_ST_AS_STATUS | GRUB_XHCI_ST_PS_STATUS)) == 0)))
    {
      grub_xhci_pre_finish_transfer (transfer);
      grub_free (cdata);
      sync_all_caches (xhci);
      grub_dprintf ("xhci", "cancel_transfer: end - xHCI not running\n");
      return GRUB_USB_ERR_NONE;
    }

  /* xHCI and (AL or SL) are running. What to do? */
  /* Try to Halt QH via de-scheduling QH. */
  /* Find index of previous QH */
  qh_phys = grub_dma_virt2phys(cdata->qh_virt, xhci->qh_chunk);
  for (i = 0; i < GRUB_XHCI_N_QH; i++)
    {
      if ((grub_le_to_cpu32(xhci->qh_virt[i].qh_hptr)
        & GRUB_XHCI_QHTDPTR_MASK) == qh_phys)
        break;
    }
  if (i == GRUB_XHCI_N_QH)
    {
      grub_printf ("%s: prev not found, queues are corrupt\n", __func__);
      return GRUB_USB_ERR_UNRECOVERABLE;
    }
  /* Unlink QH from AL */
  xhci->qh_virt[i].qh_hptr = cdata->qh_virt->qh_hptr;

  sync_all_caches (xhci);

  /* If this is an interrupt transfer, we just wait for the periodic
   * schedule to advance a few times and then assume that the xHCI
   * controller has read the updated QH. */
  if (cdata->qh_virt->ep_cap & GRUB_XHCI_SMASK_MASK)
    {
      grub_millisleep(20);
    }
  else
    {
      /* For the asynchronous schedule we use the advance doorbell to find
       * out when the xHCI controller has read the updated QH. */

      /* Ring the doorbell */
      grub_xhci_oper_write32 (xhci, GRUB_XHCI_OPER_USBCMD,
                              GRUB_XHCI_CMD_AS_ADV_D
                              | grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD));
      /* Ensure command is written */
      grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD);
      /* Wait answer with timeout */
      maxtime = grub_get_time_ms () + 2;
      while (((grub_xhci_oper_read32 (xhci, GRUB_XHCI_STATUS)
               & GRUB_XHCI_ST_AS_ADVANCE) == 0)
             && (grub_get_time_ms () < maxtime));

      /* We do not detect the timeout because if timeout occurs, it most
       * probably means something wrong with xHCI - maybe stopped etc. */

      /* Shut up the doorbell */
      grub_xhci_oper_write32 (xhci, GRUB_XHCI_OPER_USBCMD,
                              ~GRUB_XHCI_CMD_AS_ADV_D
                              & grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD));
      grub_xhci_oper_write32 (xhci, GRUB_XHCI_STATUS,
                              GRUB_XHCI_ST_AS_ADVANCE
                              | grub_xhci_oper_read32 (xhci, GRUB_XHCI_STATUS));
      /* Ensure command is written */
      grub_xhci_oper_read32 (xhci, GRUB_XHCI_STATUS);
    }

  /* Now is QH out of AL and we can do anything with it... */
  grub_xhci_pre_finish_transfer (transfer);

  /* "Free" the QH - link it to itself */
  cdata->qh_virt->ep_char = 0;
  cdata->qh_virt->qh_hptr =
    grub_cpu_to_le32 ((grub_dma_virt2phys (cdata->qh_virt,
                                           xhci->qh_chunk)
                       & GRUB_XHCI_POINTER_MASK) | GRUB_XHCI_HPTR_TYPE_QH);

  grub_free (cdata);

  grub_dprintf ("xhci", "cancel_transfer: end\n");

  sync_all_caches (xhci);

#endif
  return GRUB_USB_ERR_NONE;
}

static grub_usb_speed_t
grub_xhci_detect_dev (grub_usb_controller_t dev, int port, int *changed)
{
  struct grub_xhci *xhci = (struct grub_xhci *) dev->data;
  grub_uint32_t status, line_state;
  grub_uint32_t is_connected;

  (void)port;
  (void)changed;
  (void)xhci;
  (void)line_state;
  (void)status;
  static int state;

  grub_dprintf ("xhci", "grub_xhci_detect_dev port=%d\n", port);
  grub_dprintf ("xhci", "PORTSC(port=%d): 0x%08x\n",
      port, xhci_read_portrs (xhci, port, PORTSC));

  is_connected = xhci_read_portrs (xhci, port, PORTSC) & 1;
  if (is_connected)
  {
    //grub_dprintf ("xhci", "IS CONNECTED!!!!\n");
    //grub_millisleep (10000);
  }

  grub_millisleep (1000);

  switch (state) {
    case 0:
      state = 0;
      *changed = 1;
      return GRUB_USB_SPEED_SUPER;

    case 1:
      state = 2;
      *changed = 0;
      return GRUB_USB_SPEED_NONE;

    case 2:
      state = 0;
      *changed = 1;
      return GRUB_USB_SPEED_SUPER;
      break;

    default:
      break;
  }

  return GRUB_USB_SPEED_NONE;

#if 0
  status = grub_xhci_port_read (xhci, port);

  /* Connect Status Change bit - it detects change of connection */
  if (status & GRUB_XHCI_PORTSC_PED)
    {
      *changed = 1;
      /* Reset bit Connect Status Change */
      grub_xhci_port_setbits (xhci, port, GRUB_XHCI_PORTSC_PED);
    }
  else
    *changed = 0;

  if (!(status & GRUB_XHCI_PORTSC_CCS))
    {				/* We should reset related "reset" flag in not connected state */
      xhci->reset &= ~(1 << port);
      return GRUB_USB_SPEED_NONE;
    }
  /* Detected connected state, so we should return speed.
   * But we can detect only LOW speed device and only at connection
   * time when PortEnabled=FALSE. FULL / HIGH speed detection is made
   * later by xHCI-specific reset procedure.
   * Another thing - if detected speed is LOW at connection time,
   * we should change port ownership to companion controller.
   * So:
   * 1. If we detect connected and enabled and xHCI-owned port,
   * we can say it is HIGH speed.
   * 2. If we detect connected and not xHCI-owned port, we can say
   * NONE speed, because such devices are not handled by xHCI.
   * 3. If we detect connected, not enabled but reset port, we can say
   * NONE speed, because it means FULL device connected to port and
   * such devices are not handled by xHCI.
   * 4. If we detect connected, not enabled and not reset port, which
   * has line state != "K", we will say HIGH - it could be FULL or HIGH
   * device, we will see it later after end of xHCI-specific reset
   * procedure.
   * 5. If we detect connected, not enabled and not reset port, which
   * has line state == "K", we can say NONE speed, because LOW speed
   * device is connected and we should change port ownership. */
  if ((status & GRUB_XHCI_PORT_ENABLED) != 0)	/* Port already enabled, return high speed. */
    return GRUB_USB_SPEED_HIGH;
  if ((status & GRUB_XHCI_PORT_OWNER) != 0)	/* xHCI is not port owner */
    return GRUB_USB_SPEED_NONE;	/* xHCI driver is ignoring this port. */
  if ((xhci->reset & (1 << port)) != 0)	/* Port reset was done = FULL speed */
    return GRUB_USB_SPEED_NONE;	/* xHCI driver is ignoring this port. */
  else				/* Port connected but not enabled - test port speed. */
    {
      line_state = status & GRUB_XHCI_PORT_LINE_STAT;
      if (line_state != GRUB_XHCI_PORT_LINE_LOWSP)
	return GRUB_USB_SPEED_HIGH;
      /* Detected LOW speed device, we should change
       * port ownership.
       * XXX: Fix it!: There should be test if related companion
       * controler is available ! And what to do if it does not exist ? */
      grub_xhci_port_setbits (xhci, port, GRUB_XHCI_PORT_OWNER);
      return GRUB_USB_SPEED_NONE;	/* Ignore this port */
      /* Note: Reset of PORT_OWNER bit is done by xHCI HW when
       * device is really disconnected from port.
       * Don't do PORT_OWNER bit reset by SW when not connected signal
       * is detected in port register ! */
    }

#endif
  return GRUB_USB_SPEED_NONE;
}

static grub_usb_err_t
grub_xhci_portstatus (grub_usb_controller_t dev,
		      unsigned int port, unsigned int enable)
{
  (void)dev;
  (void)port;
  (void)enable;
  grub_dprintf ("xhci", "grub_xhci_portstatus enter (port=%d, enable=%d)\n",
      port, enable);
  return GRUB_USB_ERR_NONE;

#if 0
  struct grub_xhci *xhci = (struct grub_xhci *) dev->data;
  grub_uint64_t endtime;

  grub_dprintf ("xhci", "portstatus: xHCI USBSTS: %08x\n",
		grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBSTS));
  grub_dprintf ("xhci",
		"portstatus: begin, iobase_cap=%p, port=%d, status=0x%02x\n",
		xhci->iobase_cap, port, grub_xhci_port_read (xhci, port));

  /* In any case we need to disable port:
   * - if enable==false - we should disable port
   * - if enable==true we will do the reset and the specification says
   *   PortEnable should be FALSE in such case */
  /* Disable the port and wait for it. */
  grub_xhci_cap_read32 (xhci, GRUB_XHCI_PORTSC(port));
  endtime = grub_get_time_ms () + 1000;
  while (grub_xhci_port_read (xhci, port) & GRUB_XHCI_PORT_ENABLED)
    if (grub_get_time_ms () > endtime)
      return GRUB_USB_ERR_TIMEOUT;

  if (!enable)			/* We don't need reset port */
    {
      grub_dprintf ("xhci", "portstatus: Disabled.\n");
      grub_dprintf ("xhci", "portstatus: end, status=0x%02x\n",
		    grub_xhci_port_read (xhci, port));
      return GRUB_USB_ERR_NONE;
    }

  grub_dprintf ("xhci", "portstatus: enable\n");

  grub_boot_time ("Resetting port %d", port);

  /* Now we will do reset - if HIGH speed device connected, it will
   * result in Enabled state, otherwise port remains disabled. */
  /* Set RESET bit for 50ms */
  grub_xhci_port_setbits (xhci, port, GRUB_XHCI_PORT_RESET);
  grub_millisleep (50);

  /* Reset RESET bit and wait for the end of reset */
  grub_xhci_port_resbits (xhci, port, GRUB_XHCI_PORT_RESET);
  endtime = grub_get_time_ms () + 1000;
  while (grub_xhci_port_read (xhci, port) & GRUB_XHCI_PORT_RESET)
    if (grub_get_time_ms () > endtime)
      return GRUB_USB_ERR_TIMEOUT;
  grub_boot_time ("Port %d reset", port);
  /* Remember "we did the reset" - needed by detect_dev */
  xhci->reset |= (1 << port);
  /* Test if port enabled, i.xhci. HIGH speed device connected */
  if ((grub_xhci_port_read (xhci, port) & GRUB_XHCI_PORT_ENABLED) != 0)	/* yes! */
    {
      grub_dprintf ("xhci", "portstatus: Enabled!\n");
      /* "Reset recovery time" (USB spec.) */
      grub_millisleep (10);
    }
  else				/* no... */
    {
      /* FULL speed device connected - change port ownership.
       * It results in disconnected state of this xHCI port. */
      grub_xhci_port_setbits (xhci, port, GRUB_XHCI_PORT_OWNER);
      return GRUB_USB_ERR_BADDEVICE;
    }

  /* XXX: Fix it! There is possible problem - we can say to calling
   * function that we lost device if it is FULL speed onlu via
   * return value <> GRUB_ERR_NONE. It (maybe) displays also error
   * message on screen - but this situation is not error, it is normal
   * state! */

  grub_dprintf ("xhci", "portstatus: end, status=0x%02x\n",
		grub_xhci_port_read (xhci, port));

#endif
  return GRUB_USB_ERR_NONE;
}

static int
grub_xhci_hubports (grub_usb_controller_t dev)
{
  struct grub_xhci *xhci = (struct grub_xhci *) dev->data;
  grub_uint32_t hcsparams1;
  unsigned int nports = 0;

  hcsparams1 = mmio_read32 (&xhci->cap_regs->hcsparams1);
  xhci->max_device_slots = hcsparams1 & 0xff;
  xhci->max_ports = (hcsparams1 >> 24) & 0xff;
  nports = xhci->max_ports;
  grub_dprintf ("xhci", "grub_xhci_hubports nports=%d\n", nports);

  //grub_dprintf ("xhci", "grub_xhci_hubports force nports=0 (prevent hang)\n");
  //nports = 0;
  return nports;
}

static grub_usb_err_t
grub_xhci_check_transfer (grub_usb_controller_t dev,
			  grub_usb_transfer_t transfer, grub_size_t * actual)
{
  struct grub_xhci *xhci = dev->data;
  struct grub_xhci_transfer_controller_data *cdata =
    transfer->controller_data;
  (void)cdata;
  (void)xhci;
  (void)actual;

  grub_dprintf ("xhci", "grub_xhci_check_transfer enter\n");
  return GRUB_USB_ERR_NONE;
#if 0
  grub_uint32_t token, token_ftd;

  sync_all_caches (xhci);

  grub_dprintf ("xhci",
		"check_transfer: xHCI STATUS=%08x, cdata=%p, qh=%p\n",
		grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBSTS),
		cdata, cdata->qh_virt);
  grub_dprintf ("xhci", "check_transfer: qh_hptr=%08x, ep_char=%08x\n",
		grub_le_to_cpu32 (cdata->qh_virt->qh_hptr),
		grub_le_to_cpu32 (cdata->qh_virt->ep_char));
  grub_dprintf ("xhci", "check_transfer: ep_cap=%08x, td_current=%08x\n",
		grub_le_to_cpu32 (cdata->qh_virt->ep_cap),
		grub_le_to_cpu32 (cdata->qh_virt->td_current));
  grub_dprintf ("xhci", "check_transfer: next_td=%08x, alt_next_td=%08x\n",
		grub_le_to_cpu32 (cdata->qh_virt->td_overlay.next_td),
		grub_le_to_cpu32 (cdata->qh_virt->td_overlay.alt_next_td));
  grub_dprintf ("xhci", "check_transfer: token=%08x, buffer[0]=%08x\n",
		grub_le_to_cpu32 (cdata->qh_virt->td_overlay.token),
		grub_le_to_cpu32 (cdata->qh_virt->td_overlay.buffer_page[0]));

  /* Check if xHCI is running and AL is enabled */
  if ((grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBSTS)
       & GRUB_XHCI_USBSTS_HCH) != 0)
    return grub_xhci_parse_notrun (dev, transfer, actual);
  if ((grub_xhci_oper_read32 (xhci, GRUB_XHCI_STATUS)
       & (GRUB_XHCI_ST_AS_STATUS | GRUB_XHCI_ST_PS_STATUS)) == 0)
    return grub_xhci_parse_notrun (dev, transfer, actual);

  token = grub_le_to_cpu32 (cdata->qh_virt->td_overlay.token);
  /* If the transfer consist from only one TD, we should check */
  /* if the TD was really executed and deactivated - to prevent */
  /* false detection of transfer finish. */
  token_ftd = grub_le_to_cpu32 (cdata->td_first_virt->token);

  /* Detect QH halted */
  if ((token & GRUB_XHCI_STATUS_HALTED) != 0)
    return grub_xhci_parse_halt (dev, transfer, actual);

  /* Detect QH not active - QH is not active and no next TD */
  if (token && ((token & GRUB_XHCI_STATUS_ACTIVE) == 0)
	&& ((token_ftd & GRUB_XHCI_STATUS_ACTIVE) == 0))
    {
      /* It could be finish at all or short packet condition */
      if ((grub_le_to_cpu32 (cdata->qh_virt->td_overlay.next_td)
	   & GRUB_XHCI_TERMINATE) &&
	  ((grub_le_to_cpu32 (cdata->qh_virt->td_current)
	    & GRUB_XHCI_QHTDPTR_MASK) == cdata->td_last_phys))
	/* Normal finish */
	return grub_xhci_parse_success (dev, transfer, actual);
      else if ((token & GRUB_XHCI_TOTAL_MASK) != 0)
	/* Short packet condition */
	/* But currently we don't handle it - higher level will do it */
	return grub_xhci_parse_success (dev, transfer, actual);
    }

#endif
  return GRUB_USB_ERR_WAIT;
}


static grub_usb_err_t
grub_xhci_setup_transfer (grub_usb_controller_t dev,
			  grub_usb_transfer_t transfer)
{
  (void)dev;
  (void)transfer;
  grub_dprintf ("xhci", "grub_xhci_setup_transfer enter\n");
  /* pretend we managed to start sending data */
  return GRUB_USB_ERR_NONE;

#if 0
  struct grub_xhci *xhci = (struct grub_xhci *) dev->data;
  grub_xhci_td_t td = NULL;
  grub_xhci_td_t td_prev = NULL;
  int i;
  struct grub_xhci_transfer_controller_data *cdata;
  grub_uint32_t status;

  sync_all_caches (xhci);

  /* Check if xHCI is running and AL is enabled */
  status = grub_xhci_oper_read32 (xhci, GRUB_XHCI_STATUS);
  if ((status & GRUB_XHCI_ST_HC_HALTED) != 0)
    /* XXX: Fix it: Currently we don't do anything to restart xHCI */
    {
      grub_dprintf ("xhci", "setup_transfer: halted, status = 0x%x\n",
		    status);
      return GRUB_USB_ERR_INTERNAL;
    }
  status = grub_xhci_oper_read32 (xhci, GRUB_XHCI_STATUS);
  if ((status
       & (GRUB_XHCI_ST_AS_STATUS | GRUB_XHCI_ST_PS_STATUS)) == 0)
    /* XXX: Fix it: Currently we don't do anything to restart xHCI */
    {
      grub_dprintf ("xhci", "setup_transfer: no AS/PS, status = 0x%x\n",
		    status);
      return GRUB_USB_ERR_INTERNAL;
    }

  /* Allocate memory for controller transfer data.  */
  cdata = grub_malloc (sizeof (*cdata));
  if (!cdata)
    return GRUB_USB_ERR_INTERNAL;
  cdata->td_first_virt = NULL;

  /* Allocate a queue head for the transfer queue.  */
  cdata->qh_virt = 0; //grub_xhci_find_qh (xhci, transfer);
  if (!cdata->qh_virt)
    {
      grub_dprintf ("xhci", "setup_transfer: no QH\n");
      grub_free (cdata);
      return GRUB_USB_ERR_INTERNAL;
    }

  /* To detect short packet we need some additional "alternate" TD,
   * allocate it first. */
  cdata->td_alt_virt = 0; //grub_xhci_alloc_td (xhci);
  if (!cdata->td_alt_virt)
    {
      grub_dprintf ("xhci", "setup_transfer: no TDs\n");
      grub_free (cdata);
      return GRUB_USB_ERR_INTERNAL;
    }
  /* Fill whole alternate TD by zeros (= inactive) and set
   * Terminate bits and Halt bit */
  grub_memset ((void *) cdata->td_alt_virt, 0, sizeof (struct grub_xhci_td));
  cdata->td_alt_virt->next_td = grub_cpu_to_le32_compile_time (GRUB_XHCI_TERMINATE);
  cdata->td_alt_virt->alt_next_td = grub_cpu_to_le32_compile_time (GRUB_XHCI_TERMINATE);
  cdata->td_alt_virt->token = grub_cpu_to_le32_compile_time (GRUB_XHCI_STATUS_HALTED);

  /* Allocate appropriate number of TDs and set */
  for (i = 0; i < transfer->transcnt; i++)
    {
      grub_usb_transaction_t tr = &transfer->transactions[i];

      td = grub_xhci_transaction (xhci, tr->pid, tr->toggle, tr->size,
				  tr->data, cdata->td_alt_virt);

      if (!td)			/* de-allocate and free all */
	{
	  grub_size_t actual = 0;

	  if (cdata->td_first_virt)
	    grub_xhci_free_tds (xhci, cdata->td_first_virt, NULL, &actual);

	  grub_free (cdata);
	  grub_dprintf ("xhci", "setup_transfer: no TD\n");
	  return GRUB_USB_ERR_INTERNAL;
	}

      /* Register new TD in cdata or previous TD */
      if (!cdata->td_first_virt)
	cdata->td_first_virt = td;
      else
	{
	  td_prev->link_td = grub_dma_virt2phys (td, xhci->td_chunk);
	  td_prev->next_td =
	    grub_cpu_to_le32 (grub_dma_virt2phys (td, xhci->td_chunk));
	}
      td_prev = td;
    }

  /* Remember last TD */
  cdata->td_last_virt = td;
  cdata->td_last_phys = grub_dma_virt2phys (td, xhci->td_chunk);
  /* Last TD should not have set alternate TD */
  cdata->td_last_virt->alt_next_td = grub_cpu_to_le32_compile_time (GRUB_XHCI_TERMINATE);

  grub_dprintf ("xhci", "setup_transfer: cdata=%p, qh=%p\n",
		cdata,cdata->qh_virt);
  grub_dprintf ("xhci", "setup_transfer: td_first=%p, td_alt=%p\n",
		cdata->td_first_virt,
		cdata->td_alt_virt);
  grub_dprintf ("xhci", "setup_transfer: td_last=%p\n",
		cdata->td_last_virt);

  /* Start transfer: */
  /* Unlink possible alternate pointer in QH */
  cdata->qh_virt->td_overlay.alt_next_td =
    grub_cpu_to_le32_compile_time (GRUB_XHCI_TERMINATE);
  /* Link new TDs with QH via next_td */
  cdata->qh_virt->td_overlay.next_td =
    grub_cpu_to_le32 (grub_dma_virt2phys
		      (cdata->td_first_virt, xhci->td_chunk));
  /* Reset Active and Halted bits in QH to activate Advance Queue,
   * i.e. reset token */
  cdata->qh_virt->td_overlay.token = grub_cpu_to_le32_compile_time (0);

  sync_all_caches (xhci);

  /* Finito */
  transfer->controller_data = cdata;

#endif
  //return GRUB_USB_ERR_TIMEOUT;
  return GRUB_USB_ERR_NONE;
}


static int
grub_xhci_iterate (grub_usb_controller_iterate_hook_t hook, void *hook_data)
{
  struct grub_xhci *xhci;
  struct grub_usb_controller dev;
  (void)dev;

  grub_dprintf ("xhci", "grub_xhci_iterate enter\n");
  for (xhci = xhci_list; xhci; xhci = xhci->next)
    {
      dev.data = xhci;
      if (hook (&dev, hook_data))
          return 1;
    }

  return 0;
}

static int
grub_xhci_dump_cap(struct grub_xhci *xhci)
{
  grub_dprintf ("xhci", "CAPLENGTH=%d\n",
      mmio_read8 (&xhci->cap_regs->caplength));

  grub_dprintf ("xhci", "HCIVERSION=0x%04x\n",
      mmio_read16 (&xhci->cap_regs->hciversion));

  grub_dprintf ("xhci", "HCSPARAMS1=0x%08x\n",
      mmio_read32 (&xhci->cap_regs->hcsparams1));

  grub_dprintf ("xhci", "HCSPARAMS2=0x%08x\n",
      mmio_read32 (&xhci->cap_regs->hcsparams2));

  grub_dprintf ("xhci", "HCSPARAMS3=0x%08x\n",
      mmio_read32 (&xhci->cap_regs->hcsparams3));

  grub_dprintf ("xhci", "HCCPARAMS1=0x%08x\n",
      mmio_read32 (&xhci->cap_regs->hccparams1));

  grub_dprintf ("xhci", "DBOFF=0x%08x\n",
      mmio_read32 (&xhci->cap_regs->dboff) & DBOFF_MASK);

  grub_dprintf ("xhci", "RTSOFF=0x%08x\n",
      mmio_read32 (&xhci->cap_regs->rtsoff) & RTSOFF_MASK);

  grub_dprintf ("xhci", "HCCPARAMS2=0x%08x\n",
      mmio_read32 (&xhci->cap_regs->hccparams2));

  return 0;
}

static int
grub_xhci_dump_oper(struct grub_xhci *xhci)
{
  grub_uint32_t val32;

  grub_dprintf ("xhci", "USBCMD=0x%08x\n",
      mmio_read32 (&xhci->oper_regs->usbcmd));

  grub_dprintf ("xhci", "USBSTS=0x%08x\n",
      mmio_read32 (&xhci->oper_regs->usbsts));

  val32 = mmio_read32 (&xhci->oper_regs->pagesize);
  grub_dprintf ("xhci", "PAGESIZE=%d (%d bytes)\n",
      val32, 1 << (val32 + 12));

  grub_dprintf ("xhci", "DNCTRL=0x%08x\n",
      mmio_read32 (&xhci->oper_regs->dnctrl));

  grub_dprintf ("xhci", "CRCR=0x%08x\n",
      mmio_read32 (&xhci->oper_regs->crcr));

  grub_dprintf ("xhci", "DCBAAP=0x%08x\n",
      mmio_read32 (&xhci->oper_regs->dcbaap));

  grub_dprintf ("xhci", "CONFIG=0x%08x\n",
      mmio_read32 (&xhci->oper_regs->config));

  return 0;
}

static int
grub_xhci_init (struct grub_xhci *xhci, volatile void *mmio_base_addr)
{
  //grub_int32_t hcsparams1;
  //grub_uint32_t hcsparams2;
  //grub_uint32_t hccparams1;
  //grub_uint32_t pagesize;

  //hcsparams1 = grub_xhci_cap_read32 (xhci, GRUB_XHCI_CAP_HCSPARAMS1);
  //nports = ((hcsparams1 >> 24) & 0xff);
  //xhci->slots = ((hcsparams1 >> 24) & 0xff);

  /* Locate capability, operational, runtime, and doorbell registers */
  xhci->cap_regs = mmio_base_addr;
  xhci->oper_regs = (struct xhci_oper_regs *)
    ((grub_uint8_t *)xhci->cap_regs + mmio_read8 (&xhci->cap_regs->caplength));
  xhci->db_regs = (struct xhci_doorbell_regs *)
    ((grub_uint8_t *)xhci->cap_regs + (mmio_read32 (&xhci->cap_regs->dboff) & DBOFF_MASK));
  xhci->run_regs = (struct xhci_run_regs *)
    ((grub_uint8_t *)xhci->cap_regs + (mmio_read32 (&xhci->cap_regs->rtsoff) & RTSOFF_MASK));

  grub_xhci_dump_cap(xhci);
  grub_xhci_dump_oper(xhci);

#if 0
  rtsoff = readl ( xhci->cap + XHCI_CAP_RTSOFF );
  dboff = readl ( xhci->cap + XHCI_CAP_DBOFF );
  xhci->op = ( xhci->cap + caplength );
  xhci->run = ( xhci->cap + rtsoff );
  xhci->db = ( xhci->cap + dboff );
  DBGC2 ( xhci, "XHCI %s cap %08lx op %08lx run %08lx db %08lx\n",
      xhci->name, virt_to_phys ( xhci->cap ),
      virt_to_phys ( xhci->op ), virt_to_phys ( xhci->run ),
      virt_to_phys ( xhci->db ) );

  /* Read structural parameters 1 */
  hcsparams1 = readl ( xhci->cap + XHCI_CAP_HCSPARAMS1 );
  xhci->slots = XHCI_HCSPARAMS1_SLOTS ( hcsparams1 );
  xhci->intrs = XHCI_HCSPARAMS1_INTRS ( hcsparams1 );
  xhci->ports = XHCI_HCSPARAMS1_PORTS ( hcsparams1 );
  DBGC ( xhci, "XHCI %s has %d slots %d intrs %d ports\n",
      xhci->name, xhci->slots, xhci->intrs, xhci->ports );

  /* Read structural parameters 2 */
  hcsparams2 = readl ( xhci->cap + XHCI_CAP_HCSPARAMS2 );
  xhci->scratchpads = XHCI_HCSPARAMS2_SCRATCHPADS ( hcsparams2 );
  DBGC2 ( xhci, "XHCI %s needs %d scratchpads\n",
      xhci->name, xhci->scratchpads );

  /* Read capability parameters 1 */
  hccparams1 = readl ( xhci->cap + XHCI_CAP_HCCPARAMS1 );
  xhci->addr64 = XHCI_HCCPARAMS1_ADDR64 ( hccparams1 );
  xhci->csz_shift = XHCI_HCCPARAMS1_CSZ_SHIFT ( hccparams1 );
  xhci->xecp = XHCI_HCCPARAMS1_XECP ( hccparams1 );

  /* Read page size */
  pagesize = readl ( xhci->op + XHCI_OP_PAGESIZE );
  xhci->pagesize = XHCI_PAGESIZE ( pagesize );
  assert ( xhci->pagesize != 0 );
  assert ( ( ( xhci->pagesize ) & ( xhci->pagesize - 1 ) ) == 0 );
  DBGC2 ( xhci, "XHCI %s page size %zd bytes\n",
      xhci->name, xhci->pagesize );

  /*** GRUB CODE BELOW ***/

  grub_uint32_t base, base_h;
  //grub_uint32_t n_ports;
  grub_uint8_t caplen;
  grub_pci_address_t addr;
  //grub_uint32_t hccparams1;
  int status;

  /*
   * Map MMIO registers
   */
  addr = grub_pci_make_address (dev, GRUB_PCI_REG_BAR0);
  base = grub_pci_read (addr);
  addr = grub_pci_make_address (dev, GRUB_PCI_REG_BAR1);
  base_h = grub_pci_read (addr);
  /* Stop if registers are mapped above 4G - GRUB does not currently
     work with registers mapped above 4G */
  if (((base & GRUB_PCI_ADDR_MEM_TYPE_MASK) != GRUB_PCI_ADDR_MEM_TYPE_32)
      && (base_h != 0))
    {
      grub_dprintf ("xhci",
                      "xHCI grub_xhci_pci_iter: registers above 4G are not supported\n");
      return 0;
    }
  base &= GRUB_PCI_ADDR_MEM_MASK;
  if (!base)
    {
      grub_dprintf ("xhci",
                      "xHCI: xHCI is not mapped\n");
      return 0;
    }

  grub_dprintf ("xhci", "xHCI grub_xhci_pci_iter: 32-bit xHCI OK\n");

  /* Allocate memory for the controller and fill basic values. */
  xhci = grub_zalloc (sizeof (*xhci));
  if (!xhci)
    return 1;

  xhci->iobase_cap = grub_pci_device_map_range (dev,
                  (base & GRUB_XHCI_ADDR_MEM_MASK),
                  0x100); /* PCI config space is 256 bytes */
  grub_dprintf ("xhci", "xHCI grub_xhci_pci_iter: iobase of CAP: %08x\n",
		(xhci->iobase_cap));

  /* Determine base address of xHCI operational registers */
  caplen = grub_xhci_cap_read8 (xhci, GRUB_XHCI_CAP_CAPLENGTH);
#ifndef GRUB_HAVE_UNALIGNED_ACCESS
  if (caplen & (sizeof (grub_uint32_t) - 1))
    {
      grub_dprintf ("xhci", "Unaligned caplen\n");
      return 0;
    }
  xhci->iobase_oper = ((volatile grub_uint32_t *) xhci->iobase_cap
	       + (caplen / sizeof (grub_uint32_t)));
#else
  xhci->iobase_oper = (volatile grub_uint32_t *)
    ((grub_uint8_t *) xhci->iobase_cap + caplen);
#endif

  /* TODO: initialize the various "rings" and TRBs */

  /* Determine and change ownership. */

  /* TODO: Check if handover is supported */
  addr = grub_pci_make_address (dev, GRUB_XHCI_CAP_HCCPARAMS1);
  hccparams1 = grub_pci_read(addr);
  xecp = hccparams1 >> 16;
  if (xecp)
    {
      addr = grub_pci_make_address (dev, 0);
      addr += sizeof(uint32_t) *
      hccparams1 = grub_pci_read(addr);
    }

  /* XECP offset valid in HCCPARAMS1 */
  /* Ownership can be changed via XECP only */
  if (eecp_offset >= 0x40)
  {
    grub_pci_address_t pciaddr_eecp;
    pciaddr_eecp = grub_pci_make_address (dev, eecp_offset);

    usblegsup = grub_pci_read (pciaddr_eecp);
    if (usblegsup & GRUB_XHCI_USBLEGSUP_BIOS_OWNED)
      {
        grub_boot_time ("Taking ownership of xHCI controller");
        grub_dprintf ("xhci",
                      "xHCI grub_xhci_pci_iter: xHCI owned by: BIOS\n");
        /* Ownership change - set OS_OWNED bit */
        grub_pci_write (pciaddr_eecp, usblegsup | GRUB_XHCI_USBLEGSUP_OS_OWNED);
        /* Ensure PCI register is written */
        grub_pci_read (pciaddr_eecp);

        /* Wait for finish of ownership change, xHCI specification
         * says it can take up to 16 ms
         */
        maxtime = grub_get_time_ms () + 1000;
        while ((grub_pci_read (pciaddr_eecp) & GRUB_XHCI_USBLEGSUP_BIOS_OWNED)
               && (grub_get_time_ms () < maxtime));
        if (grub_pci_read (pciaddr_eecp) & GRUB_XHCI_USBLEGSUP_BIOS_OWNED)
          {
            grub_dprintf ("xhci",
                          "xHCI grub_xhci_pci_iter: xHCI change ownership timeout");
            /* Change ownership in "hard way" - reset BIOS ownership */
            grub_pci_write (pciaddr_eecp, GRUB_XHCI_USBLEGSUP_OS_OWNED);
            /* Ensure PCI register is written */
            grub_pci_read (pciaddr_eecp);
          }
      }
    else if (usblegsup & GRUB_XHCI_USBLEGSUP_OS_OWNED)
      /* XXX: What to do in this case - nothing ? Can it happen ? */
      grub_dprintf ("xhci", "xHCI grub_xhci_pci_iter: xHCI owned by: OS\n");
    else
      {
        grub_dprintf ("xhci",
                      "xHCI grub_xhci_pci_iter: xHCI owned by: NONE\n");
        /* XXX: What to do in this case ? Can it happen ?
         * Is code below correct ? */
        /* Ownership change - set OS_OWNED bit */
        grub_pci_write (pciaddr_eecp, GRUB_XHCI_USBLEGSUP_OS_OWNED);
        /* Ensure PCI register is written */
        grub_pci_read (pciaddr_eecp);
      }

    /* Disable SMI, just to be sure.  */
    pciaddr_eecp = grub_pci_make_address (dev, eecp_offset + 4);
    grub_pci_write (pciaddr_eecp, 0);
    /* Ensure PCI register is written */
    grub_pci_read (pciaddr_eecp);
  }

  grub_dprintf ("xhci", "inithw: xHCI grub_xhci_pci_iter: ownership OK\n");

  /* Now we can setup xHCI (maybe...) */

  /* Check if xHCI is halted and halt it if not */
  if (grub_xhci_halt (xhci) != GRUB_USB_ERR_NONE)
    {
      grub_error (GRUB_ERR_TIMEOUT,
		  "xHCI grub_xhci_pci_iter: xHCI halt timeout");
      goto fail;
    }

  grub_dprintf ("xhci", "xHCI grub_xhci_pci_iter: halted OK\n");

  /* Reset xHCI */
  if (grub_xhci_reset (xhci) != GRUB_USB_ERR_NONE)
    {
      grub_error (GRUB_ERR_TIMEOUT,
		  "xHCI grub_xhci_pci_iter: xHCI reset timeout");
      goto fail;
    }

  grub_dprintf ("xhci", "xHCI grub_xhci_pci_iter: reset OK\n");

  /* Now should be possible to power-up and enumerate ports etc. */
  if ((grub_xhci_cap_read32 (xhci, GRUB_XHCI_EHCC_SPARAMS)
       & GRUB_XHCI_SPARAMS_PPC) != 0)
    {				/* xHCI has port powering control */
      /* Power on all ports */
      n_ports = grub_xhci_cap_read32 (xhci, GRUB_XHCI_EHCC_SPARAMS)
	& GRUB_XHCI_SPARAMS_N_PORTS;
      for (i = 0; i < (int) n_ports; i++)
	grub_xhci_oper_write32 (xhci, GRUB_XHCI_PORTSC(port),
				GRUB_XHCI_PORT_POWER
				| grub_xhci_oper_read32 (xhci,
							 GRUB_XHCI_PORTSC(i)));
    }

  /* Ensure all commands are written */
  grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD);

  /* Enable xHCI */
  grub_xhci_oper_write32 (xhci, GRUB_XHCI_OPER_USBCMD,
			  GRUB_XHCI_OPER_USBCMD_RUNSTOP
			  | grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD));

  /* Ensure command is written */
  grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD);

  /* Link to xhci now that initialisation is successful.  */
  xhci->next = xhci_list;
  xhci_list = xhci;

  sync_all_caches (xhci);

  grub_dprintf ("xhci", "xHCI grub_xhci_pci_iter: OK at all\n");

  grub_dprintf ("xhci",
		"xHCI grub_xhci_pci_iter: iobase of oper. regs: %08x\n",
		((unsigned int)xhci->iobase_oper));
  grub_dprintf ("xhci", "xHCI grub_xhci_pci_iter: USBCMD: %08x\n",
		grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBCMD));
  grub_dprintf ("xhci", "xHCI grub_xhci_pci_iter: USBSTS: %08x\n",
		grub_xhci_oper_read32 (xhci, GRUB_XHCI_OPER_USBSTS));

#endif
  return 0;

//fail:
//  if (xhci)
//    {
////      if (xhci->td_chunk)
////	grub_dma_free ((void *) xhci->td_chunk);
////      if (xhci->qh_chunk)
////	grub_dma_free ((void *) xhci->qh_chunk);
////      if (xhci->framelist_chunk)
////	grub_dma_free (xhci->framelist_chunk);
//    }
  grub_free (xhci);
  return 0;
}

#if 0
static int
grub_pci_config_read (grub_pci_device_t dev, int reg)
{
  return *(volatile grub_uint32_t *) config_addr (addr);
}
#endif

/*
 * Read PCI BAR
 *
 * @v pci		PCI device
 * @v reg		PCI register number
 * @ret bar		Base address register
 *
 * Reads the specified PCI base address register, including the flags
 * portion.  64-bit BARs will be handled automatically.  If the value
 * of the 64-bit BAR exceeds the size of an unsigned long (i.e. if the
 * high dword is non-zero on a 32-bit platform), then the value
 * returned will be zero plus the flags for a 64-bit BAR.  Unreachable
 * 64-bit BARs are therefore returned as uninitialised 64-bit BARs.
 */
static unsigned long pci_bar ( struct grub_pci_device *dev, unsigned int reg ) {
  grub_uint32_t low;
  grub_uint32_t high;

  low = pci_config_read (*dev, reg);
  if ( ( low & (GRUB_PCI_ADDR_SPACE_IO|GRUB_PCI_ADDR_MEM_TYPE_MASK))
      == GRUB_PCI_ADDR_MEM_TYPE_64 )
    {
      high = pci_config_read (*dev, reg + 4);
      if ( high )
        {
          if ( sizeof ( unsigned long ) > sizeof ( grub_uint32_t ) ) {
            return ( ( ( grub_uint64_t ) high << 32 ) | low );
          }
        else
          {
            grub_dprintf ("xhci", "unhandled 64-bit BAR\n");
            return GRUB_PCI_ADDR_MEM_TYPE_64;
          }
        }
    }
  return low;
}

/**
 * Find the start of a PCI BAR
 *
 * @v pci		PCI device
 * @v reg		PCI register number
 * @ret start		BAR start address
 *
 * Reads the specified PCI base address register, and returns the
 * address portion of the BAR (i.e. without the flags).
 *
 * If the address exceeds the size of an unsigned long (i.e. if a
 * 64-bit BAR has a non-zero high dword on a 32-bit machine), the
 * return value will be zero.
 */
static unsigned long pci_bar_start ( struct grub_pci_device *dev, unsigned int reg )
{
  unsigned long bar;

  bar = pci_bar (dev, reg);
  if ( bar & GRUB_PCI_ADDR_SPACE_IO )
    {
      return ( bar & ~GRUB_PCI_ADDR_IO_MASK );
    }
  else
    {
      return ( bar & ~GRUB_PCI_ADDR_MEM_MASK );
    }
}


/* PCI iteration function... */
static int
grub_xhci_pci_iter (grub_pci_device_t dev,
                    grub_pci_id_t pciid __attribute__ ((unused)),
		    void *data __attribute__ ((unused)))
{
  int err;
  struct grub_xhci *xhci;
  grub_uint32_t class_code;
  grub_uint32_t addr;
  grub_uint32_t base;
  volatile grub_uint32_t *mmio_base_addr;
  grub_uint32_t base_h;

  /* Exit if not USB3.0 xHCI controller */
  /* TODO: endianness (PCI regs are little-endian) */
  class_code = pci_config_read (dev, GRUB_PCI_REG_CLASS) >> 8;
  if (class_code != 0x0c0330)
    return 0;

  grub_dprintf ("xhci", "found xHCI controller on bus %d device %d "
      "function %d: device|vendor ID 0x%08x\n", dev.bus, dev.device,
      dev.function, pciid);

  /* Determine xHCI MMIO registers base address */
  addr = grub_pci_make_address (dev, GRUB_PCI_REG_ADDRESS_REG0);
  base = grub_pci_read (addr);
  addr = grub_pci_make_address (dev, GRUB_PCI_REG_ADDRESS_REG1);
  base_h = grub_pci_read (addr);
  /* Stop if registers are mapped above 4G - GRUB does not currently
   * work with registers mapped above 4G */
  if (((base & GRUB_PCI_ADDR_MEM_TYPE_MASK) != GRUB_PCI_ADDR_MEM_TYPE_32)
      && (base_h != 0))
    {
      grub_dprintf ("xhci", "registers above 4G are not supported\n");
      return 0;
    }
  base &= GRUB_PCI_ADDR_MEM_MASK;
  if (!base)
    {
      grub_dprintf ("xhci", "xHCI is not mapped (broken PC firmware)\n");
      return 0;
    }

  /* Set bus master - needed for coreboot, VMware, broken BIOSes etc. */
  addr = grub_pci_make_address (dev, GRUB_PCI_REG_COMMAND);
  grub_pci_write_word(addr,
    		  GRUB_PCI_COMMAND_MEM_ENABLED
    		  | GRUB_PCI_COMMAND_BUS_MASTER
    		  | grub_pci_read_word(addr));

  grub_dprintf ("ehci", "xHCI 32-bit MMIO regs OK\n");

  mmio_base_addr = grub_pci_device_map_range (dev,
      (base & GRUB_XHCI_ADDR_MEM_MASK),
      0x100); /* PCI config space is 256 bytes */

  grub_dprintf ("xhci", "Start of MMIO area (BAR0): 0x%08x\n",
      (unsigned int)mmio_base_addr);

  xhci = grub_malloc (sizeof (*xhci));
  if (!xhci)
    {
      grub_dprintf ("xhci", "out of memory\n");
      return GRUB_USB_ERR_INTERNAL;
    }

  err = grub_xhci_init (xhci, mmio_base_addr);
  if (err)
  {
    grub_free(xhci);
    return err;
  }

  /* Build list of xHCI controllers */
  xhci->next = xhci_list;
  xhci_list = xhci;

  return 0;
}

static struct grub_usb_controller_dev usb_controller_dev = {
  .name = "xhci",
  .iterate = grub_xhci_iterate,
  .setup_transfer = grub_xhci_setup_transfer, /* give data to HW, let it go */
  .check_transfer = grub_xhci_check_transfer, /* check if HW has completed transfer, polled by USB framework (see usbtrans.c) */
  .cancel_transfer = grub_xhci_cancel_transfer, /* called if/when check_transfer has failed over a period of time */
  .hubports = grub_xhci_hubports,
  .portstatus = grub_xhci_portstatus,
  .detect_dev = grub_xhci_detect_dev,
  /* estimated max. count of TDs for one bulk transfer */
  .max_bulk_tds = 16, //GRUB_EHCI_N_TD * 3 / 4
};

GRUB_MOD_INIT (xhci)
{
  grub_dprintf ("xhci", "[loading]\n");

  /* TODO: anything to compile time check? */
  //COMPILE_TIME_ASSERT (sizeof (struct grub_xhci_FOO) == 64);

  grub_stop_disk_firmware ();

  grub_boot_time ("Initing xHCI hardware");
  grub_pci_iterate (grub_xhci_pci_iter, NULL);
  grub_boot_time ("Registering xHCI driver");
  grub_usb_controller_dev_register (&usb_controller_dev);
  grub_boot_time ("xHCI driver registered");
  grub_dprintf ("xhci", "xHCI driver is registered, register preboot hook\n");
  grub_loader_register_preboot_hook (grub_xhci_fini_hw, grub_xhci_restore_hw,
				     GRUB_LOADER_PREBOOT_HOOK_PRIO_DISK);
  grub_dprintf ("xhci", "GRUB_MOD_INIT completed\n");
}

GRUB_MOD_FINI (xhci)
{
  grub_dprintf ("xhci", "[unloading]\n");
  grub_xhci_fini_hw (0);
  grub_usb_controller_dev_unregister (&usb_controller_dev);
}
