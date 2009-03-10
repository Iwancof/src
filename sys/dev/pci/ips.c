/*	$OpenBSD: ips.c,v 1.54 2009/03/10 09:16:40 grange Exp $	*/

/*
 * Copyright (c) 2006, 2007, 2009 Alexander Yurchenko <grange@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
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

/*
 * IBM (Adaptec) ServeRAID controllers driver.
 */

#include "bio.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/sensors.h>
#include <sys/timeout.h>
#include <sys/queue.h>

#include <machine/bus.h>

#include <scsi/scsi_all.h>
#include <scsi/scsi_disk.h>
#include <scsi/scsiconf.h>

#include <dev/biovar.h>

#include <dev/pci/pcidevs.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

/* Debug levels */
#define IPS_D_ERR	0x0001	/* errors */
#define IPS_D_INFO	0x0002	/* information */
#define IPS_D_XFER	0x0004	/* transfers */

#ifdef IPS_DEBUG
#define DPRINTF(a, b)	do { if (ips_debug & (a)) printf b; } while (0)
int ips_debug = IPS_D_ERR;
#else
#define DPRINTF(a, b)
#endif

#define IPS_MAXDRIVES		8
#define IPS_MAXCHANS		4
#define IPS_MAXTARGETS		15
#define IPS_MAXCHUNKS		16
#define IPS_MAXCMDS		128

#define IPS_MAXFER		(64 * 1024)
#define IPS_MAXSGS		16
#define IPS_MAXCMDSZ		(IPS_CMDSZ + IPS_MAXSGS * IPS_SGSZ)

#define IPS_CMDSZ		sizeof(struct ips_cmd)
#define IPS_SGSZ		sizeof(struct ips_sg)
#define IPS_SECSZ		512
#define IPS_NVRAMPGSZ		128
#define IPS_SQSZ		(IPS_MAXCMDS * sizeof(u_int32_t))

#define	IPS_TIMEOUT		5	/* seconds */

/* Command codes */
#define IPS_CMD_READ		0x02
#define IPS_CMD_WRITE		0x03
#define IPS_CMD_DCDB		0x04
#define IPS_CMD_GETADAPTERINFO	0x05
#define IPS_CMD_FLUSH		0x0a
#define IPS_CMD_REBUILDSTATUS	0x0c
#define IPS_CMD_ERRORTABLE	0x17
#define IPS_CMD_GETDRIVEINFO	0x19
#define IPS_CMD_RESETCHAN	0x1a
#define IPS_CMD_DOWNLOAD	0x20
#define IPS_CMD_RWBIOSFW	0x22
#define IPS_CMD_READCONF	0x38
#define IPS_CMD_GETSUBSYS	0x40
#define IPS_CMD_CONFIGSYNC	0x58
#define IPS_CMD_READ_SG		0x82
#define IPS_CMD_WRITE_SG	0x83
#define IPS_CMD_DCDB_SG		0x84
#define IPS_CMD_EXT_DCDB	0x95
#define IPS_CMD_EXT_DCDB_SG	0x96
#define IPS_CMD_RWNVRAMPAGE	0xbc
#define IPS_CMD_GETVERINFO	0xc6
#define IPS_CMD_FFDC		0xd7
#define IPS_CMD_SG		0x80
#define IPS_CMD_RWNVRAM		0xbc

/* Register definitions */
#define IPS_REG_HIS		0x08	/* host interrupt status */
#define IPS_REG_HIS_SCE			0x01	/* status channel enqueue */
#define IPS_REG_HIS_EN			0x80	/* enable interrupts */
#define IPS_REG_CCSA		0x10	/* command channel system address */
#define IPS_REG_CCC		0x14	/* command channel control */
#define IPS_REG_CCC_SEM			0x0008	/* semaphore */
#define IPS_REG_CCC_START		0x101a	/* start command */
#define IPS_REG_SQH		0x20	/* status queue head */
#define IPS_REG_SQT		0x24	/* status queue tail */
#define IPS_REG_SQE		0x28	/* status queue end */
#define IPS_REG_SQS		0x2c	/* status queue start */

#define IPS_REG_OIS		0x30	/* outbound interrupt status */
#define IPS_REG_OIS_PEND		0x0008	/* interrupt is pending */
#define IPS_REG_OIM		0x34	/* outbound interrupt mask */
#define IPS_REG_OIM_DS			0x0008	/* disable interrupts */
#define IPS_REG_IQP		0x40	/* inbound queue port */
#define IPS_REG_OQP		0x44	/* outbound queue port */

#define IPS_REG_STAT_ID(x)	(((x) >> 8) & 0xff)
#define IPS_REG_STAT_BASIC(x)	(((x) >> 16) & 0xff)
#define IPS_REG_STAT_GSC(x)	(((x) >> 16) & 0x0f)
#define IPS_REG_STAT_EXT(x)	(((x) >> 24) & 0xff)

#define IPS_IOSIZE		128	/* max space size to map */

/* Command frame */
struct ips_cmd {
	u_int8_t	code;
	u_int8_t	id;
	u_int8_t	drive;
	u_int8_t	sgcnt;
	u_int32_t	lba;
	u_int32_t	sgaddr;
	u_int16_t	seccnt;
	u_int8_t	seg4g;
	u_int8_t	esg;
	u_int32_t	ccsar;
	u_int32_t	cccr;
};

/* Scatter-gather array element */
struct ips_sg {
	u_int32_t	addr;
	u_int32_t	size;
};

/* Data frames */
struct ips_adapterinfo {
	u_int8_t	drivecnt;
	u_int8_t	miscflag;
	u_int8_t	sltflag;
	u_int8_t	bstflag;
	u_int8_t	pwrchgcnt;
	u_int8_t	wrongaddrcnt;
	u_int8_t	unidentcnt;
	u_int8_t	nvramdevchgcnt;
	u_int8_t	firmware[8];
	u_int8_t	bios[8];
	u_int32_t	drivesize[IPS_MAXDRIVES];
	u_int8_t	cmdcnt;
	u_int8_t	maxphysdevs;
	u_int16_t	flashrepgmcnt;
	u_int8_t	defunctdiskcnt;
	u_int8_t	rebuildflag;
	u_int8_t	offdrivecnt;
	u_int8_t	critdrivecnt;
	u_int16_t	confupdcnt;
	u_int8_t	blkflag;
	u_int8_t	__reserved;
	u_int16_t	deaddisk[IPS_MAXCHANS * (IPS_MAXTARGETS + 1)];
};

struct ips_driveinfo {
	u_int8_t	drivecnt;
	u_int8_t	__reserved[3];
	struct ips_drive {
		u_int8_t	id;
		u_int8_t	__reserved;
		u_int8_t	raid;
		u_int8_t	state;
#define IPS_DS_FREE	0x00
#define IPS_DS_OFFLINE	0x02
#define IPS_DS_ONLINE	0x03
#define IPS_DS_DEGRADED	0x04
#define IPS_DS_SYS	0x06
#define IPS_DS_CRS	0x24

		u_int32_t	seccnt;
	}		drive[IPS_MAXDRIVES];
};

struct ips_conf {
	u_int8_t	ldcnt;
	u_int8_t	day;
	u_int8_t	month;
	u_int8_t	year;
	u_int8_t	initid[4];
	u_int8_t	hostid[12];
	u_int8_t	time[8];
	u_int32_t	useropt;
	u_int16_t	userfield;
	u_int8_t	rebuildrate;
	u_int8_t	__reserved1;

	struct ips_hw {
		u_int8_t	board[8];
		u_int8_t	cpu[8];
		u_int8_t	nchantype;
		u_int8_t	nhostinttype;
		u_int8_t	compression;
		u_int8_t	nvramtype;
		u_int32_t	nvramsize;
	}		hw;

	struct ips_ld {
		u_int16_t	userfield;
		u_int8_t	state;
		u_int8_t	raidcacheparam;
		u_int8_t	chunkcnt;
		u_int8_t	stripesize;
		u_int8_t	params;
		u_int8_t	__reserved;
		u_int32_t	size;

		struct ips_chunk {
			u_int8_t	channel;
			u_int8_t	target;
			u_int16_t	__reserved;
			u_int32_t	startsec;
			u_int32_t	seccnt;
		}		chunk[IPS_MAXCHUNKS];
	}		ld[IPS_MAXDRIVES];

	struct ips_dev {
		u_int8_t	initiator;
		u_int8_t	params;
		u_int8_t	miscflag;
		u_int8_t	state;
#define IPS_DVS_PRESENT	0x81
#define IPS_DVS_REBUILD	0x02
#define IPS_DVS_SPARE	0x04
#define IPS_DVS_MEMBER	0x08

		u_int32_t	seccnt;
		u_int8_t	devid[28];
	}		dev[IPS_MAXCHANS][IPS_MAXTARGETS + 1];

	u_int8_t	reserved[512];
};

struct ips_pg5 {
	u_int32_t	signature;
	u_int8_t	__reserved1;
	u_int8_t	slot;
	u_int16_t	type;
	u_int8_t	bioshi[4];
	u_int8_t	bioslo[4];
	u_int16_t	__reserved2;
	u_int8_t	__reserved3;
	u_int8_t	os;
	u_int8_t	driverhi[4];
	u_int8_t	driverlo[4];
	u_int8_t	__reserved4[100];
};

struct ips_info {
	struct ips_adapterinfo	adapter;
	struct ips_driveinfo	drive;
	struct ips_conf		conf;
	struct ips_pg5		pg5;
};

/* Command control block */
struct ips_softc;
struct ips_ccb {
	int			c_id;		/* command id */
	int			c_flags;	/* flags */
#define IPS_CCB_READ	0x0001
#define IPS_CCB_WRITE	0x0002
#define IPS_CCB_POLL	0x0004
#define IPS_CCB_RUN	0x0008

	void *			c_cmdva;	/* command frame virt addr */
	paddr_t			c_cmdpa;	/* command frame phys addr */
	bus_dmamap_t		c_dmam;		/* data buffer DMA map */
	struct scsi_xfer *	c_xfer;		/* corresponding SCSI xfer */
	int			c_stat;		/* status word copy */
	int			c_estat;	/* ext status word copy */

	void			(*c_done)(struct ips_softc *,	/* cmd done */
				    struct ips_ccb *);		/* callback */

	TAILQ_ENTRY(ips_ccb)	c_link;		/* queue link */
};

/* CCB queue */
TAILQ_HEAD(ips_ccbq, ips_ccb);

/* DMA-able chunk of memory */
struct dmamem {
	bus_dma_tag_t		dm_tag;
	bus_dmamap_t		dm_map;
	bus_dma_segment_t	dm_seg;
	bus_size_t		dm_size;
	void *			dm_vaddr;
#define dm_paddr dm_seg.ds_addr
};

struct ips_softc {
	struct device		sc_dev;

	struct scsi_link	sc_scsi_link;
	struct scsibus_softc *	sc_scsibus;

	struct ksensordev	sc_sensordev;
	struct ksensor *	sc_sensors;

	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	bus_dma_tag_t		sc_dmat;

	const struct ips_chipset *sc_chip;

	struct ips_info *	sc_info;
	struct dmamem		sc_infom;

	int			sc_nunits;

	struct dmamem		sc_cmdm;

	struct ips_ccb *	sc_ccb;
	int			sc_nccbs;
	struct ips_ccbq		sc_ccbq_free;

	struct dmamem		sc_sqm;
	paddr_t			sc_sqtail;
	u_int32_t *		sc_sqbuf;
	int			sc_sqidx;
};

int	ips_match(struct device *, void *, void *);
void	ips_attach(struct device *, struct device *, void *);

int	ips_scsi_cmd(struct scsi_xfer *);
int	ips_scsi_ioctl(struct scsi_link *, u_long, caddr_t, int,
	    struct proc *);

int	ips_ioctl(struct device *, u_long, caddr_t);
int	ips_ioctl_inq(struct ips_softc *, struct bioc_inq *);
int	ips_ioctl_vol(struct ips_softc *, struct bioc_vol *);
int	ips_ioctl_disk(struct ips_softc *, struct bioc_disk *);

void	ips_sensors(void *);

int	ips_load(struct ips_softc *, struct ips_ccb *, struct scsi_xfer *);
int	ips_cmd(struct ips_softc *, struct ips_ccb *);
int	ips_poll(struct ips_softc *, struct ips_ccb *);
void	ips_done_xs(struct ips_softc *, struct ips_ccb *);
void	ips_done_mgmt(struct ips_softc *, struct ips_ccb *);
int	ips_intr(void *);
void	ips_timeout(void *);

int	ips_getadapterinfo(struct ips_softc *);
int	ips_getdriveinfo(struct ips_softc *);
int	ips_getconf(struct ips_softc *);
int	ips_getpg5(struct ips_softc *);
int	ips_flush(struct ips_softc *);

void	ips_copperhead_exec(struct ips_softc *, struct ips_ccb *);
void	ips_copperhead_init(struct ips_softc *);
void	ips_copperhead_intren(struct ips_softc *);
void	ips_copperhead_intrds(struct ips_softc *);
int	ips_copperhead_isintr(struct ips_softc *);
int	ips_copperhead_reset(struct ips_softc *);
u_int32_t ips_copperhead_status(struct ips_softc *);

void	ips_morpheus_exec(struct ips_softc *, struct ips_ccb *);
void	ips_morpheus_init(struct ips_softc *);
void	ips_morpheus_intren(struct ips_softc *);
void	ips_morpheus_intrds(struct ips_softc *);
int	ips_morpheus_isintr(struct ips_softc *);
int	ips_morpheus_reset(struct ips_softc *);
u_int32_t ips_morpheus_status(struct ips_softc *);

struct ips_ccb *ips_ccb_alloc(struct ips_softc *, int);
void	ips_ccb_free(struct ips_softc *, struct ips_ccb *, int);
struct ips_ccb *ips_ccb_get(struct ips_softc *);
void	ips_ccb_put(struct ips_softc *, struct ips_ccb *);

int	ips_dmamem_alloc(struct dmamem *, bus_dma_tag_t, bus_size_t);
void	ips_dmamem_free(struct dmamem *);

struct cfattach ips_ca = {
	sizeof(struct ips_softc),
	ips_match,
	ips_attach
};

struct cfdriver ips_cd = {
	NULL, "ips", DV_DULL
};

static struct scsi_adapter ips_scsi_adapter = {
	ips_scsi_cmd,
	scsi_minphys,
	NULL,
	NULL,
	ips_scsi_ioctl
};

static struct scsi_device ips_scsi_device = {
	NULL,
	NULL,
	NULL,
	NULL
};

static const struct pci_matchid ips_ids[] = {
	{ PCI_VENDOR_IBM,	PCI_PRODUCT_IBM_SERVERAID },
	{ PCI_VENDOR_IBM,	PCI_PRODUCT_IBM_SERVERAID2 },
	{ PCI_VENDOR_ADP2,	PCI_PRODUCT_ADP2_SERVERAID }
};

static const struct ips_chipset {
	enum {
		IPS_CHIP_COPPERHEAD = 0,
		IPS_CHIP_MORPHEUS
	}		ic_id;

	int		ic_bar;

	void		(*ic_exec)(struct ips_softc *, struct ips_ccb *);
	void		(*ic_init)(struct ips_softc *);
	void		(*ic_intren)(struct ips_softc *);
	void		(*ic_intrds)(struct ips_softc *);
	int		(*ic_isintr)(struct ips_softc *);
	int		(*ic_reset)(struct ips_softc *);
	u_int32_t	(*ic_status)(struct ips_softc *);
} ips_chips[] = {
	{
		IPS_CHIP_COPPERHEAD,
		0x14,
		ips_copperhead_exec,
		ips_copperhead_init,
		ips_copperhead_intren,
		ips_copperhead_intrds,
		ips_copperhead_isintr,
		ips_copperhead_reset,
		ips_copperhead_status
	},
	{
		IPS_CHIP_MORPHEUS,
		0x10,
		ips_morpheus_exec,
		ips_morpheus_init,
		ips_morpheus_intren,
		ips_morpheus_intrds,
		ips_morpheus_isintr,
		ips_morpheus_reset,
		ips_morpheus_status
	}
};

#define ips_exec(s, c)	(s)->sc_chip->ic_exec((s), (c))
#define ips_init(s)	(s)->sc_chip->ic_init((s))
#define ips_intren(s)	(s)->sc_chip->ic_intren((s))
#define ips_intrds(s)	(s)->sc_chip->ic_intrds((s))
#define ips_isintr(s)	(s)->sc_chip->ic_isintr((s))
#define ips_reset(s)	(s)->sc_chip->ic_reset((s))
#define ips_status(s)	(s)->sc_chip->ic_status((s))

static const char *ips_names[] = {
	NULL,
	NULL,
	"II",
	"onboard",
	"onboard",
	"3H",
	"3L",
	"4H",
	"4M",
	"4L",
	"4Mx",
	"4Lx",
	"5i",
	"5i",
	"6M",
	"6i",
	"7t",
	"7k",
	"7M"
};

int
ips_match(struct device *parent, void *match, void *aux)
{
	return (pci_matchbyid(aux, ips_ids,
	    sizeof(ips_ids) / sizeof(ips_ids[0])));
}

void
ips_attach(struct device *parent, struct device *self, void *aux)
{
	struct ips_softc *sc = (struct ips_softc *)self;
	struct pci_attach_args *pa = aux;
	struct ips_ccb ccb0;
	struct scsibus_attach_args saa;
	struct ips_adapterinfo *ai;
	struct ips_driveinfo *di;
	struct ips_pg5 *pg5;
	pcireg_t maptype;
	bus_size_t iosize;
	pci_intr_handle_t ih;
	const char *intrstr;
	int type, i;

	sc->sc_dmat = pa->pa_dmat;

	/* Identify chipset */
	if (PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_IBM_SERVERAID)
		sc->sc_chip = &ips_chips[IPS_CHIP_COPPERHEAD];
	else
		sc->sc_chip = &ips_chips[IPS_CHIP_MORPHEUS];

	/* Map registers */
	maptype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, sc->sc_chip->ic_bar);
	if (pci_mapreg_map(pa, sc->sc_chip->ic_bar, maptype, 0, &sc->sc_iot,
	    &sc->sc_ioh, NULL, &iosize, IPS_IOSIZE)) {
		printf(": can't map regs\n");
		return;
	}

	/* Initialize hardware */
	ips_init(sc);

	/* Allocate command buffer */
	if (ips_dmamem_alloc(&sc->sc_cmdm, sc->sc_dmat,
	    IPS_MAXCMDS * IPS_MAXCMDSZ)) {
		printf(": can't alloc cmd buffer\n");
		goto fail1;
	}

	/* Allocate info buffer */
	if (ips_dmamem_alloc(&sc->sc_infom, sc->sc_dmat,
	    sizeof(struct ips_info))) {
		printf(": can't alloc info buffer\n");
		goto fail2;
	}
	sc->sc_info = sc->sc_infom.dm_vaddr;
	ai = &sc->sc_info->adapter;
	di = &sc->sc_info->drive;
	pg5 = &sc->sc_info->pg5;

	/* Allocate status queue for the Copperhead chipset */
	if (sc->sc_chip->ic_id == IPS_CHIP_COPPERHEAD) {
		if (ips_dmamem_alloc(&sc->sc_sqm, sc->sc_dmat, IPS_SQSZ)) {
			printf(": can't alloc status queue\n");
			goto fail3;
		}
		sc->sc_sqtail = sc->sc_sqm.dm_paddr;
		sc->sc_sqbuf = sc->sc_sqm.dm_vaddr;
		sc->sc_sqidx = 0;
		bus_space_write_4(sc->sc_iot, sc->sc_ioh, IPS_REG_SQS,
		    sc->sc_sqm.dm_paddr);
		bus_space_write_4(sc->sc_iot, sc->sc_ioh, IPS_REG_SQE,
		    sc->sc_sqm.dm_paddr + IPS_SQSZ);
		bus_space_write_4(sc->sc_iot, sc->sc_ioh, IPS_REG_SQH,
		    sc->sc_sqm.dm_paddr + sizeof(u_int32_t));
		bus_space_write_4(sc->sc_iot, sc->sc_ioh, IPS_REG_SQT,
		    sc->sc_sqm.dm_paddr);
	}

	/* Bootstrap CCB queue */
	sc->sc_nccbs = 1;
	sc->sc_ccb = &ccb0;
	bzero(&ccb0, sizeof(ccb0));
	ccb0.c_cmdva = sc->sc_cmdm.dm_vaddr;
	ccb0.c_cmdpa = sc->sc_cmdm.dm_paddr;
	TAILQ_INIT(&sc->sc_ccbq_free);
	TAILQ_INSERT_TAIL(&sc->sc_ccbq_free, &ccb0, c_link);

	/* Get adapter info */
	if (ips_getadapterinfo(sc)) {
		printf(": can't get adapter info\n");
		goto fail4;
	}

	/* Get logical drives info */
	if (ips_getdriveinfo(sc)) {
		printf(": can't get ld info\n");
		goto fail4;
	}
	sc->sc_nunits = di->drivecnt;

	/* Get configuration */
	if (ips_getconf(sc)) {
		printf(": can't get config\n");
		goto fail4;
	}

	/* Read NVRAM page 5 for additional info */
	(void)ips_getpg5(sc);

	/* Initialize CCB queue */
	sc->sc_nccbs = ai->cmdcnt;
	if ((sc->sc_ccb = ips_ccb_alloc(sc, sc->sc_nccbs)) == NULL) {
		printf(": can't alloc ccb queue\n");
		goto fail4;
	}
	TAILQ_INIT(&sc->sc_ccbq_free);
	for (i = 0; i < sc->sc_nccbs; i++)
		TAILQ_INSERT_TAIL(&sc->sc_ccbq_free,
		    &sc->sc_ccb[i], c_link);

	/* Install interrupt handler */
	if (pci_intr_map(pa, &ih)) {
		printf(": can't map interrupt\n");
		goto fail5;
	}
	intrstr = pci_intr_string(pa->pa_pc, ih);
	if (pci_intr_establish(pa->pa_pc, ih, IPL_BIO, ips_intr, sc,
	    sc->sc_dev.dv_xname) == NULL) {
		printf(": can't establish interrupt");
		if (intrstr != NULL)
			printf(" at %s", intrstr);
		printf("\n");
		goto fail5;
	}
	printf(": %s\n", intrstr);

	/* Display adapter info */
	printf("%s: ServeRAID", sc->sc_dev.dv_xname);
	type = letoh16(pg5->type);
	if (type < sizeof(ips_names) / sizeof(ips_names[0]) && ips_names[type])
		printf(" %s", ips_names[type]);
	printf(", FW %c%c%c%c%c%c%c", ai->firmware[0], ai->firmware[1],
	    ai->firmware[2], ai->firmware[3], ai->firmware[4], ai->firmware[5],
	    ai->firmware[6]);
	printf(", BIOS %c%c%c%c%c%c%c", ai->bios[0], ai->bios[1], ai->bios[2],
	    ai->bios[3], ai->bios[4], ai->bios[5], ai->bios[6]);
	printf(", %d cmds, %d LD%s", sc->sc_nccbs, sc->sc_nunits,
	    (sc->sc_nunits == 1 ? "" : "s"));
	printf("\n");

	/* Attach SCSI bus */
	if (sc->sc_nunits > 0)
		sc->sc_scsi_link.openings = sc->sc_nccbs / sc->sc_nunits;
	sc->sc_scsi_link.adapter_target = sc->sc_nunits;
	sc->sc_scsi_link.adapter_buswidth = sc->sc_nunits;
	sc->sc_scsi_link.device = &ips_scsi_device;
	sc->sc_scsi_link.adapter = &ips_scsi_adapter;
	sc->sc_scsi_link.adapter_softc = sc;

	bzero(&saa, sizeof(saa));
	saa.saa_sc_link = &sc->sc_scsi_link;
	sc->sc_scsibus = (struct scsibus_softc *)config_found(self, &saa,
	    scsiprint);

	/* Enable interrupts */
	ips_intren(sc);

#if NBIO > 0
	/* Install ioctl handler */
	if (bio_register(&sc->sc_dev, ips_ioctl))
		printf("%s: no ioctl support\n", sc->sc_dev.dv_xname);
#endif

#ifndef SMALL_KERNEL
	/* Add sensors */
	if ((sc->sc_sensors = malloc(sizeof(struct ksensor) * sc->sc_nunits,
	    M_DEVBUF, M_NOWAIT | M_ZERO)) == NULL) {
		printf(": can't alloc sensors\n");
		return;
	}
	strlcpy(sc->sc_sensordev.xname, sc->sc_dev.dv_xname,
	    sizeof(sc->sc_sensordev.xname));
	for (i = 0; i < sc->sc_nunits; i++) {
		sc->sc_sensors[i].type = SENSOR_DRIVE;
		sc->sc_sensors[i].status = SENSOR_S_UNKNOWN;
		strlcpy(sc->sc_sensors[i].desc, ((struct device *)
		    sc->sc_scsibus->sc_link[i][0]->device_softc)->dv_xname,
		    sizeof(sc->sc_sensors[i].desc));
		sensor_attach(&sc->sc_sensordev, &sc->sc_sensors[i]);
	}
	if (sensor_task_register(sc, ips_sensors, 10) == NULL) {
		printf(": no sensors support\n");
		free(sc->sc_sensors, M_DEVBUF);
		return;
	}
	sensordev_install(&sc->sc_sensordev);
#endif	/* !SMALL_KERNEL */

	return;
fail5:
	ips_ccb_free(sc, sc->sc_ccb, sc->sc_nccbs);
fail4:
	if (sc->sc_chip->ic_id == IPS_CHIP_COPPERHEAD)
		ips_dmamem_free(&sc->sc_sqm);
fail3:
	ips_dmamem_free(&sc->sc_infom);
fail2:
	ips_dmamem_free(&sc->sc_cmdm);
fail1:
	bus_space_unmap(sc->sc_iot, sc->sc_ioh, iosize);
}

int
ips_scsi_cmd(struct scsi_xfer *xs)
{
	struct scsi_link *link = xs->sc_link;
	struct ips_softc *sc = link->adapter_softc;
	struct ips_driveinfo *di = &sc->sc_info->drive;
	struct ips_drive *drive;
	struct scsi_inquiry_data inq;
	struct scsi_read_cap_data rcd;
	struct scsi_sense_data sd;
	struct scsi_rw *rw;
	struct scsi_rw_big *rwb;
	struct ips_ccb *ccb;
	struct ips_cmd *cmd;
	int target = link->target;
	u_int32_t blkno, blkcnt;
	int code, error, flags, s;

	if (target >= sc->sc_nunits || link->lun != 0) {
		DPRINTF(IPS_D_INFO, ("%s: invalid scsi command, "
		    "target %d, lun %d\n", sc->sc_dev.dv_xname,
		    target, link->lun));
		xs->error = XS_DRIVER_STUFFUP;
		s = splbio();
		scsi_done(xs);
		splx(s);
		return (COMPLETE);
	}

	drive = &di->drive[target];
	xs->error = XS_NOERROR;

	/* Fake SCSI commands */
	switch (xs->cmd->opcode) {
	case READ_BIG:
	case READ_COMMAND:
	case WRITE_BIG:
	case WRITE_COMMAND:
		if (xs->cmdlen == sizeof(struct scsi_rw)) {
			rw = (void *)xs->cmd;
			blkno = _3btol(rw->addr) &
			    (SRW_TOPADDR << 16 | 0xffff);
			blkcnt = rw->length ? rw->length : 0x100;
		} else {
			rwb = (void *)xs->cmd;
			blkno = _4btol(rwb->addr);
			blkcnt = _2btol(rwb->length);
		}

		if (blkno >= letoh32(drive->seccnt) || blkno + blkcnt >
		    letoh32(drive->seccnt)) {
			DPRINTF(IPS_D_ERR, ("%s: invalid scsi command, "
			    "blkno %u, blkcnt %u\n", sc->sc_dev.dv_xname,
			    blkno, blkcnt));
			xs->error = XS_DRIVER_STUFFUP;
			s = splbio();
			scsi_done(xs);
			splx(s);
			break;
		}

		if (xs->flags & SCSI_DATA_IN) {
			code = IPS_CMD_READ;
			flags = IPS_CCB_READ;
		} else {
			code = IPS_CMD_WRITE;
			flags = IPS_CCB_WRITE;
		}
		if (xs->flags & SCSI_POLL)
			flags |= IPS_CCB_POLL;

		s = splbio();
		ccb = ips_ccb_get(sc);
		splx(s);
		if (ccb == NULL)
			return (NO_CCB);

		ccb->c_flags = flags;
		ccb->c_xfer = xs;
		ccb->c_done = ips_done_xs;

		cmd = ccb->c_cmdva;
		cmd->code = code;
		cmd->drive = target;
		cmd->lba = htole32(blkno);
		cmd->seccnt = htole16(blkcnt);

		if (ips_load(sc, ccb, xs)) {
			s = splbio();
			ips_ccb_put(sc, ccb);
			xs->error = XS_DRIVER_STUFFUP;
			scsi_done(xs);
			splx(s);
			return (COMPLETE);
		}

		if (cmd->sgcnt > 0)
			cmd->code |= IPS_CMD_SG;

		timeout_set(&xs->stimeout, ips_timeout, ccb);
		timeout_add_sec(&xs->stimeout, IPS_TIMEOUT);

		if ((error = ips_cmd(sc, ccb))) {
			if (error == ETIMEDOUT)
				xs->error = XS_TIMEOUT;
			else
				xs->error = XS_DRIVER_STUFFUP;

			s = splbio();
			scsi_done(xs);
			splx(s);
			return (COMPLETE);
		}

		if (flags & IPS_CCB_POLL)
			return (COMPLETE);
		else
			return (SUCCESSFULLY_QUEUED);
	case INQUIRY:
		bzero(&inq, sizeof(inq));
		inq.device = T_DIRECT;
		inq.version = 2;
		inq.response_format = 2;
		inq.additional_length = 32;
		strlcpy(inq.vendor, "IBM", sizeof(inq.vendor));
		snprintf(inq.product, sizeof(inq.product),
		    "LD%d RAID%d", target, drive->raid);
		strlcpy(inq.revision, "1.0", sizeof(inq.revision));
		memcpy(xs->data, &inq, MIN(xs->datalen, sizeof(inq)));
		break;
	case READ_CAPACITY:
		bzero(&rcd, sizeof(rcd));
		_lto4b(letoh32(drive->seccnt) - 1, rcd.addr);
		_lto4b(IPS_SECSZ, rcd.length);
		memcpy(xs->data, &rcd, MIN(xs->datalen, sizeof(rcd)));
		break;
	case REQUEST_SENSE:
		bzero(&sd, sizeof(sd));
		sd.error_code = SSD_ERRCODE_CURRENT;
		sd.flags = SKEY_NO_SENSE;
		memcpy(xs->data, &sd, MIN(xs->datalen, sizeof(sd)));
		break;
	case SYNCHRONIZE_CACHE:
		if (ips_flush(sc))
			xs->error = XS_DRIVER_STUFFUP;
		break;
	case PREVENT_ALLOW:
	case START_STOP:
	case TEST_UNIT_READY:
		break;
	default:
		DPRINTF(IPS_D_INFO, ("%s: unsupported scsi command 0x%02x\n",
		    sc->sc_dev.dv_xname, xs->cmd->opcode));
		xs->error = XS_DRIVER_STUFFUP;
	}

	s = splbio();
	scsi_done(xs);
	splx(s);

	return (COMPLETE);
}

int
ips_scsi_ioctl(struct scsi_link *link, u_long cmd, caddr_t addr, int flag,
    struct proc *p)
{
	return (ips_ioctl(link->adapter_softc, cmd, addr));
}

#if NBIO > 0
int
ips_ioctl(struct device *dev, u_long cmd, caddr_t addr)
{
	struct ips_softc *sc = (struct ips_softc *)dev;

	DPRINTF(IPS_D_INFO, ("%s: ioctl %lu\n", sc->sc_dev.dv_xname, cmd));

	switch (cmd) {
	case BIOCINQ:
		return (ips_ioctl_inq(sc, (struct bioc_inq *)addr));
	case BIOCVOL:
		return (ips_ioctl_vol(sc, (struct bioc_vol *)addr));
	case BIOCDISK:
		return (ips_ioctl_disk(sc, (struct bioc_disk *)addr));
	default:
		return (ENOTTY);
	}
}

int
ips_ioctl_inq(struct ips_softc *sc, struct bioc_inq *bi)
{
	struct ips_conf *conf = &sc->sc_info->conf;
	int i;

	strlcpy(bi->bi_dev, sc->sc_dev.dv_xname, sizeof(bi->bi_dev));
	bi->bi_novol = sc->sc_nunits;
	for (i = 0, bi->bi_nodisk = 0; i < sc->sc_nunits; i++)
		bi->bi_nodisk += conf->ld[i].chunkcnt;

	return (0);
}

int
ips_ioctl_vol(struct ips_softc *sc, struct bioc_vol *bv)
{
	struct ips_driveinfo *di = &sc->sc_info->drive;
	struct ips_conf *conf = &sc->sc_info->conf;
	struct ips_ld *ld;
	int vid = bv->bv_volid;
	struct device *dev;

	if (vid >= sc->sc_nunits)
		return (EINVAL);
	ld = &conf->ld[vid];

	switch (ld->state) {
	case IPS_DS_ONLINE:
		bv->bv_status = BIOC_SVONLINE;
		break;
	case IPS_DS_DEGRADED:
		bv->bv_status = BIOC_SVDEGRADED;
		break;
	case IPS_DS_OFFLINE:
		bv->bv_status = BIOC_SVOFFLINE;
		break;
	default:
		bv->bv_status = BIOC_SVINVALID;
	}

	bv->bv_size = (u_quad_t)letoh32(ld->size) * IPS_SECSZ;
	bv->bv_level = di->drive[vid].raid;
	bv->bv_nodisk = ld->chunkcnt;

	dev = sc->sc_scsibus->sc_link[vid][0]->device_softc;
	strlcpy(bv->bv_dev, dev->dv_xname, sizeof(bv->bv_dev));
	strlcpy(bv->bv_vendor, "IBM", sizeof(bv->bv_vendor));

	return (0);
}

int
ips_ioctl_disk(struct ips_softc *sc, struct bioc_disk *bd)
{
	struct ips_conf *conf = &sc->sc_info->conf;
	struct ips_ld *ld;
	struct ips_chunk *chunk;
	struct ips_dev *dev;
	int vid = bd->bd_volid, did = bd->bd_diskid;

	if (vid >= sc->sc_nunits)
		return (EINVAL);
	ld = &conf->ld[vid];

	if (did >= ld->chunkcnt)
		return (EINVAL);
	chunk = &ld->chunk[did];

	if (chunk->channel >= IPS_MAXCHANS || chunk->target >= IPS_MAXTARGETS)
		return (EINVAL);
	dev = &conf->dev[chunk->channel][chunk->target];

	bd->bd_channel = chunk->channel;
	bd->bd_target = chunk->target;
	bd->bd_lun = 0;
	bd->bd_size = (u_quad_t)letoh32(chunk->seccnt) * IPS_SECSZ;

	bzero(bd->bd_vendor, sizeof(bd->bd_vendor));
	memcpy(bd->bd_vendor, dev->devid, MIN(sizeof(bd->bd_vendor),
	    sizeof(dev->devid)));

	if (dev->state & IPS_DVS_PRESENT) {
		if (dev->state & IPS_DVS_REBUILD)
			bd->bd_status = BIOC_SDREBUILD;
		if (dev->state & IPS_DVS_SPARE)
			bd->bd_status = BIOC_SDHOTSPARE;
		if (dev->state & IPS_DVS_MEMBER)
			bd->bd_status = BIOC_SDONLINE;
	} else {
		bd->bd_status = BIOC_SDOFFLINE;
	}

	return (0);
}
#endif	/* NBIO > 0 */

#ifndef SMALL_KERNEL
void
ips_sensors(void *arg)
{
	struct ips_softc *sc = arg;
	struct ips_conf *conf = &sc->sc_info->conf;
	struct ips_ld *ld;
	int i;

	if (ips_getconf(sc)) {
		for (i = 0; i < sc->sc_nunits; i++) {
			sc->sc_sensors[i].value = 0;
			sc->sc_sensors[i].status = SENSOR_S_UNKNOWN;
		}
		return;
	}

	for (i = 0; i < sc->sc_nunits; i++) {
		ld = &conf->ld[i];
		switch (ld->state) {
		case IPS_DS_ONLINE:
			sc->sc_sensors[i].value = SENSOR_DRIVE_ONLINE;
			sc->sc_sensors[i].status = SENSOR_S_OK;
			break;
		case IPS_DS_DEGRADED:
			sc->sc_sensors[i].value = SENSOR_DRIVE_PFAIL;
			sc->sc_sensors[i].status = SENSOR_S_WARN;
			break;
		case IPS_DS_OFFLINE:
			sc->sc_sensors[i].value = SENSOR_DRIVE_FAIL;
			sc->sc_sensors[i].status = SENSOR_S_CRIT;
			break;
		default:
			sc->sc_sensors[i].value = 0;
			sc->sc_sensors[i].status = SENSOR_S_UNKNOWN;
		}
	}
}
#endif

int
ips_load(struct ips_softc *sc, struct ips_ccb *ccb, struct scsi_xfer *xs)
{
	struct ips_cmd *cmd = ccb->c_cmdva;
	struct ips_sg *sg;
	int nsegs, i;

	if (xs->datalen == 0)
		return (0);

	/* Map data buffer into DMA segments */
	if (bus_dmamap_load(sc->sc_dmat, ccb->c_dmam, xs->data, xs->datalen,
	    NULL, (xs->flags & SCSI_NOSLEEP ? BUS_DMA_NOWAIT : 0)))
		return (1);
	bus_dmamap_sync(sc->sc_dmat, ccb->c_dmam, 0,ccb->c_dmam->dm_mapsize,
	    xs->flags & SCSI_DATA_IN ? BUS_DMASYNC_PREREAD :
	    BUS_DMASYNC_PREWRITE);

	if ((nsegs = ccb->c_dmam->dm_nsegs) > IPS_MAXSGS)
		return (1);

	if (nsegs > 1) { 
		cmd->sgcnt = nsegs;
		cmd->sgaddr = htole32(ccb->c_cmdpa + IPS_CMDSZ);

		/* Fill in scatter-gather array */
		sg = (void *)(cmd + 1);
		for (i = 0; i < nsegs; i++) {
			sg[i].addr = htole32(ccb->c_dmam->dm_segs[i].ds_addr);
			sg[i].size = htole32(ccb->c_dmam->dm_segs[i].ds_len);
		}
	} else {
		cmd->sgcnt = 0;
		cmd->sgaddr = htole32(ccb->c_dmam->dm_segs[0].ds_addr);
	}

	return (0);
}

int
ips_cmd(struct ips_softc *sc, struct ips_ccb *ccb)
{
	struct ips_cmd *cmd = ccb->c_cmdva;
	int s, error = 0;

	DPRINTF(IPS_D_XFER, ("%s: cmd id %d, flags 0x%02x, code 0x%02x, "
	    "drive %d, sgcnt %d, lba %d, sgaddr 0x%08x, seccnt %d\n",
	    sc->sc_dev.dv_xname, ccb->c_id, ccb->c_flags, cmd->code,
	    cmd->drive, cmd->sgcnt, cmd->lba, cmd->sgaddr, cmd->seccnt));

	/* Pass command to hardware */
	cmd->id = ccb->c_id;
	ccb->c_flags |= IPS_CCB_RUN;
	s = splbio();
	ips_exec(sc, ccb);
	splx(s);

	if (ccb->c_flags & IPS_CCB_POLL) {
		/* Wait for command to complete */
		s = splbio();
		error = ips_poll(sc, ccb);
		splx(s);
	}

	return (error);
}

int
ips_poll(struct ips_softc *sc, struct ips_ccb *c)
{
	struct ips_ccb *ccb = NULL;
	u_int32_t status;
	int id, timeout;

	while (ccb != c) {
		for (timeout = 100; timeout-- > 0; delay(100)) {
			if ((status = ips_status(sc)) == 0xffffffff)
				continue;
			id = IPS_REG_STAT_ID(status);
			if (id >= sc->sc_nccbs) {
				DPRINTF(IPS_D_ERR, ("%s: invalid command "
				    "0x%02x\n", sc->sc_dev.dv_xname, id));
				continue;
			}
			break;
		}
		if (timeout < 0) {
			printf("%s: poll timeout\n", sc->sc_dev.dv_xname);
			return (ETIMEDOUT);
		}
		ccb = &sc->sc_ccb[id];
		ccb->c_stat = IPS_REG_STAT_GSC(status);
		ccb->c_estat = IPS_REG_STAT_EXT(status);
		ccb->c_done(sc, ccb);
	}

	return (0);
}

void
ips_done_xs(struct ips_softc *sc, struct ips_ccb *ccb)
{
	struct scsi_xfer *xs = ccb->c_xfer;
	int flags = ccb->c_flags;
	int error = 0;

	if ((flags & IPS_CCB_RUN) == 0) {
		printf("%s: cmd 0x%02x not run\n", sc->sc_dev.dv_xname,
		    ccb->c_id);
		return;
	}

	timeout_del(&xs->stimeout);

	if (flags & (IPS_CCB_READ | IPS_CCB_WRITE)) {
		bus_dmamap_sync(sc->sc_dmat, ccb->c_dmam, 0,
		    ccb->c_dmam->dm_mapsize, flags & IPS_CCB_READ ?
		    BUS_DMASYNC_POSTREAD : BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, ccb->c_dmam);
	}

	if (ccb->c_stat) {
		sc_print_addr(xs->sc_link);
		if (ccb->c_stat == 1) {
			printf("recovered error\n");
		} else {
			printf("error\n");
			error = 1;
		}
	}

	/* Release CCB */
	ips_ccb_put(sc, ccb);

	if (error)
		xs->error = XS_DRIVER_STUFFUP;
	else
		xs->resid = 0;
	xs->flags |= ITSDONE;
	scsi_done(xs);
}

void
ips_done_mgmt(struct ips_softc *sc, struct ips_ccb *ccb)
{
	int flags = ccb->c_flags;
	int error = 0;

	if ((flags & IPS_CCB_RUN) == 0) {
		printf("%s: cmd 0x%02x not run\n", sc->sc_dev.dv_xname,
		    ccb->c_id);
		return;
	}

	if (flags & (IPS_CCB_READ | IPS_CCB_WRITE))
		bus_dmamap_sync(sc->sc_dmat, sc->sc_infom.dm_map, 0,
		    sc->sc_infom.dm_map->dm_mapsize, flags & IPS_CCB_READ ?
		    BUS_DMASYNC_POSTREAD : BUS_DMASYNC_POSTWRITE);

	if (ccb->c_stat) {
		printf("%s: ", sc->sc_dev.dv_xname);
		if (ccb->c_stat == 1) {
			printf("recovered error\n");
		} else {
			printf("error\n");
			error = 1;
		}
	}

	/* Release CCB */
	ips_ccb_put(sc, ccb);
}

int
ips_intr(void *arg)
{
	struct ips_softc *sc = arg;
	struct ips_ccb *ccb;
	u_int32_t status;
	int id;

	if (!ips_isintr(sc))
		return (0);

	/* Process completed commands */
	while ((status = ips_status(sc)) != 0xffffffff) {
		DPRINTF(IPS_D_XFER, ("%s: intr status 0x%08x\n",
		    sc->sc_dev.dv_xname, status));

		id = IPS_REG_STAT_ID(status);
		if (id >= sc->sc_nccbs) {
			DPRINTF(IPS_D_ERR, ("%s: invalid command %d\n",
			    sc->sc_dev.dv_xname, id));
			continue;
		}
		ccb = &sc->sc_ccb[id];
		ccb->c_stat = IPS_REG_STAT_GSC(status);
		ccb->c_estat = IPS_REG_STAT_EXT(status);
		ccb->c_done(sc, ccb);
	}

	return (1);
}

void
ips_timeout(void *arg)
{
	struct ips_ccb *ccb = arg;
	struct scsi_xfer *xs = ccb->c_xfer;
	struct ips_softc *sc = xs->sc_link->adapter_softc;
	int s;

	/*
	 * Command never completed. Cleanup and recover.
	 */
	s = splbio();
	sc_print_addr(xs->sc_link);
	printf("timeout");
	DPRINTF(IPS_D_ERR, (", command 0x%02x", ccb->c_id));
	printf("\n");

	ips_ccb_put(sc, ccb);

	xs->error = XS_TIMEOUT;
	xs->flags |= ITSDONE;
	scsi_done(xs);

	ips_reset(sc);
	splx(s);
}

int
ips_getadapterinfo(struct ips_softc *sc)
{
	struct ips_ccb *ccb;
	struct ips_cmd *cmd;
	int s;

	s = splbio();
	ccb = ips_ccb_get(sc);
	splx(s);
	if (ccb == NULL)
		return (1);

	ccb->c_flags = IPS_CCB_READ | IPS_CCB_POLL;
	ccb->c_done = ips_done_mgmt;

	cmd = ccb->c_cmdva;
	cmd->code = IPS_CMD_GETADAPTERINFO;
	cmd->sgaddr = htole32(sc->sc_infom.dm_paddr + offsetof(struct ips_info,
	    adapter));

	return (ips_cmd(sc, ccb));
}

int
ips_getdriveinfo(struct ips_softc *sc)
{
	struct ips_ccb *ccb;
	struct ips_cmd *cmd;
	int s;

	s = splbio();
	ccb = ips_ccb_get(sc);
	splx(s);
	if (ccb == NULL)
		return (1);

	ccb->c_flags = IPS_CCB_READ | IPS_CCB_POLL;
	ccb->c_done = ips_done_mgmt;

	cmd = ccb->c_cmdva;
	cmd->code = IPS_CMD_GETDRIVEINFO;
	cmd->sgaddr = htole32(sc->sc_infom.dm_paddr + offsetof(struct ips_info,
	    drive));

	return (ips_cmd(sc, ccb));
}

int
ips_getconf(struct ips_softc *sc)
{
	struct ips_ccb *ccb;
	struct ips_cmd *cmd;
	int s;

	s = splbio();
	ccb = ips_ccb_get(sc);
	splx(s);
	if (ccb == NULL)
		return (1);

	ccb->c_flags = IPS_CCB_READ | IPS_CCB_POLL;
	ccb->c_done = ips_done_mgmt;

	cmd = ccb->c_cmdva;
	cmd->code = IPS_CMD_READCONF;
	cmd->sgaddr = htole32(sc->sc_infom.dm_paddr + offsetof(struct ips_info,
	    conf));

	return (ips_cmd(sc, ccb));
}

int
ips_getpg5(struct ips_softc *sc)
{
	struct ips_ccb *ccb;
	struct ips_cmd *cmd;
	int s;

	s = splbio();
	ccb = ips_ccb_get(sc);
	splx(s);
	if (ccb == NULL)
		return (1);

	ccb->c_flags = IPS_CCB_READ | IPS_CCB_POLL;
	ccb->c_done = ips_done_mgmt;

	cmd = ccb->c_cmdva;
	cmd->code = IPS_CMD_RWNVRAM;
	cmd->drive = 5;
	cmd->sgaddr = htole32(sc->sc_infom.dm_paddr + offsetof(struct ips_info,
	    pg5));

	return (ips_cmd(sc, ccb));
}

int
ips_flush(struct ips_softc *sc)
{
	struct ips_ccb *ccb;
	struct ips_cmd *cmd;
	int s;

	s = splbio();
	ccb = ips_ccb_get(sc);
	splx(s);
	if (ccb == NULL)
		return (1);

	ccb->c_flags = IPS_CCB_POLL;
	ccb->c_done = ips_done_mgmt;

	cmd = ccb->c_cmdva;
	cmd->code = IPS_CMD_FLUSH;

	return (ips_cmd(sc, ccb));
}

void
ips_copperhead_exec(struct ips_softc *sc, struct ips_ccb *ccb)
{
	u_int32_t reg;
	int timeout;

	for (timeout = 100; timeout-- > 0; delay(100)) {
		reg = bus_space_read_4(sc->sc_iot, sc->sc_ioh, IPS_REG_CCC);
		if ((reg & IPS_REG_CCC_SEM) == 0)
			break;
	}
	if (timeout < 0) {
		printf("%s: semaphore timeout\n", sc->sc_dev.dv_xname);
		return;
	}

	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IPS_REG_CCSA, ccb->c_cmdpa);
	bus_space_write_2(sc->sc_iot, sc->sc_ioh, IPS_REG_CCC,
	    IPS_REG_CCC_START);
}

void
ips_copperhead_init(struct ips_softc *sc)
{
	/* XXX: not implemented */
}

void
ips_copperhead_intren(struct ips_softc *sc)
{
	bus_space_write_1(sc->sc_iot, sc->sc_ioh, IPS_REG_HIS, IPS_REG_HIS_EN);
}

void
ips_copperhead_intrds(struct ips_softc *sc)
{
	bus_space_write_1(sc->sc_iot, sc->sc_ioh, IPS_REG_HIS, 0);
}

int
ips_copperhead_isintr(struct ips_softc *sc)
{
	u_int8_t reg;

	reg = bus_space_read_1(sc->sc_iot, sc->sc_ioh, IPS_REG_HIS);
	bus_space_write_1(sc->sc_iot, sc->sc_ioh, IPS_REG_HIS, reg);
	if (reg != 0xff && (reg & IPS_REG_HIS_SCE))
		return (1);

	return (0);
}

int
ips_copperhead_reset(struct ips_softc *sc)
{
	/* XXX: not implemented */
	return (0);
}

u_int32_t
ips_copperhead_status(struct ips_softc *sc)
{
	u_int32_t sqhead, sqtail, status;

	sqhead = bus_space_read_4(sc->sc_iot, sc->sc_ioh, IPS_REG_SQH);
	DPRINTF(IPS_D_XFER, ("%s: sqhead 0x%08x, sqtail 0x%08x\n",
	    sc->sc_dev.dv_xname, sqhead, sc->sc_sqtail));

	sqtail = sc->sc_sqtail + sizeof(u_int32_t);
	if (sqtail == sc->sc_sqm.dm_paddr + IPS_SQSZ)
		sqtail = sc->sc_sqm.dm_paddr;
	if (sqtail == sqhead)
		return (0xffffffff);

	sc->sc_sqtail = sqtail;
	if (++sc->sc_sqidx == IPS_MAXCMDS)
		sc->sc_sqidx = 0;
	status = letoh32(sc->sc_sqbuf[sc->sc_sqidx]);
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IPS_REG_SQT, sqtail);

	return (status);
}

void
ips_morpheus_exec(struct ips_softc *sc, struct ips_ccb *ccb)
{
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IPS_REG_IQP, ccb->c_cmdpa);
}

void
ips_morpheus_init(struct ips_softc *sc)
{
	/* XXX: not implemented */
}

void
ips_morpheus_intren(struct ips_softc *sc)
{
	u_int32_t reg;

	reg = bus_space_read_4(sc->sc_iot, sc->sc_ioh, IPS_REG_OIM);
	reg &= ~IPS_REG_OIM_DS;
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IPS_REG_OIM, reg);
}

void
ips_morpheus_intrds(struct ips_softc *sc)
{
	u_int32_t reg;

	reg = bus_space_read_4(sc->sc_iot, sc->sc_ioh, IPS_REG_OIM);
	reg |= IPS_REG_OIM_DS;
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IPS_REG_OIM, reg);
}

int
ips_morpheus_isintr(struct ips_softc *sc)
{
	u_int32_t reg;

	reg = bus_space_read_4(sc->sc_iot, sc->sc_ioh, IPS_REG_OIS);
	DPRINTF(IPS_D_XFER, ("%s: isintr 0x%08x\n", sc->sc_dev.dv_xname, reg));

	return (reg & IPS_REG_OIS_PEND);
}

int
ips_morpheus_reset(struct ips_softc *sc)
{
	/* XXX: not implemented */
	return (0);
}

u_int32_t
ips_morpheus_status(struct ips_softc *sc)
{
	u_int32_t reg;

	reg = bus_space_read_4(sc->sc_iot, sc->sc_ioh, IPS_REG_OQP);
	DPRINTF(IPS_D_XFER, ("%s: status 0x%08x\n", sc->sc_dev.dv_xname, reg));

	return (reg);
}

struct ips_ccb *
ips_ccb_alloc(struct ips_softc *sc, int n)
{
	struct ips_ccb *ccb;
	int i;

	if ((ccb = malloc(n * sizeof(*ccb), M_DEVBUF, M_NOWAIT|M_ZERO)) == NULL)
		return (NULL);

	for (i = 0; i < n; i++) {
		ccb[i].c_id = i;
		ccb[i].c_cmdva = (char *)sc->sc_cmdm.dm_vaddr +
		    i * IPS_MAXCMDSZ;
		ccb[i].c_cmdpa = sc->sc_cmdm.dm_paddr + i * IPS_MAXCMDSZ;
		if (bus_dmamap_create(sc->sc_dmat, IPS_MAXFER, IPS_MAXSGS,
		    IPS_MAXFER, 0, BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW,
		    &ccb[i].c_dmam))
			goto fail;
	}

	return (ccb);
fail:
	for (; i > 0; i--)
		bus_dmamap_destroy(sc->sc_dmat, ccb[i - 1].c_dmam);
	free(ccb, M_DEVBUF);
	return (NULL);
}

void
ips_ccb_free(struct ips_softc *sc, struct ips_ccb *ccb, int n)
{
	int i;

	for (i = 0; i < n; i++)
		bus_dmamap_destroy(sc->sc_dmat, ccb[i - 1].c_dmam);
	free(ccb, M_DEVBUF);
}

struct ips_ccb *
ips_ccb_get(struct ips_softc *sc)
{
	struct ips_ccb *ccb;

	if ((ccb = TAILQ_FIRST(&sc->sc_ccbq_free)) != NULL) {
		TAILQ_REMOVE(&sc->sc_ccbq_free, ccb, c_link);
		ccb->c_flags = 0;
		ccb->c_xfer = NULL;
		bzero(ccb->c_cmdva, sizeof(struct ips_cmd));
	}

	return (ccb);
}

void
ips_ccb_put(struct ips_softc *sc, struct ips_ccb *ccb)
{
	ccb->c_flags = 0;
	ccb->c_xfer = NULL;
	TAILQ_INSERT_TAIL(&sc->sc_ccbq_free, ccb, c_link);
}

int
ips_dmamem_alloc(struct dmamem *dm, bus_dma_tag_t tag, bus_size_t size)
{
	int nsegs;

	dm->dm_tag = tag;
	dm->dm_size = size;

	if (bus_dmamap_create(tag, size, 1, size, 0,
	    BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW, &dm->dm_map))
		return (1);
	if (bus_dmamem_alloc(tag, size, 0, 0, &dm->dm_seg, 1, &nsegs,
	    BUS_DMA_NOWAIT))
		goto fail1;
	if (bus_dmamem_map(tag, &dm->dm_seg, 1, size, (caddr_t *)&dm->dm_vaddr,
	    BUS_DMA_NOWAIT))
		goto fail2;
	if (bus_dmamap_load(tag, dm->dm_map, dm->dm_vaddr, size, NULL,
	    BUS_DMA_NOWAIT))
		goto fail3;

	return (0);

fail3:
	bus_dmamem_unmap(tag, dm->dm_vaddr, size);
fail2:
	bus_dmamem_free(tag, &dm->dm_seg, 1);
fail1:
	bus_dmamap_destroy(tag, dm->dm_map);
	return (1);
}

void
ips_dmamem_free(struct dmamem *dm)
{
	bus_dmamap_unload(dm->dm_tag, dm->dm_map);
	bus_dmamem_unmap(dm->dm_tag, dm->dm_vaddr, dm->dm_size);
	bus_dmamem_free(dm->dm_tag, &dm->dm_seg, 1);
	bus_dmamap_destroy(dm->dm_tag, dm->dm_map);
}
