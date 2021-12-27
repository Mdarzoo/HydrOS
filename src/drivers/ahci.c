#include <drivers/ahci.h>
#include <string/string.h>

/* the whole AHCI explanation can be found in the ahci.h header file,
 * the documentation for specific functions can be found here */

#define SATA_SIG_ATA 0x00000101   /* SATA drive                  */
#define SATA_SIG_ATAPI 0xEB140101 /* SATAPI drive                */
#define SATA_SIG_SEMB 0xC33C0101  /* enclosure management bridge */
#define SATA_SIG_PM 0x96690101    /* port multiplier             */

#define AHCI_DEV_NULL 0
#define AHCI_DEV_SATA 1
#define AHCI_DEV_SEMB 2
#define AHCI_DEV_PM 3
#define AHCI_DEV_SATAPI 4

#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PORT_DET_PRESENT 3

#define AHCI_BASE 0x400000 /* 4096 bytes */

int check_type(hba_port_t *port)
{
    uint32_t ssts = port->ssts; /* SATA status */

    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != HBA_PORT_DET_PRESENT)
        return AHCI_DEV_NULL;
    if (ipm != HBA_PORT_IPM_ACTIVE)
        return AHCI_DEV_NULL;

    switch (port->sig) /* signature */
    {
    case SATA_SIG_ATAPI:
        return AHCI_DEV_SATAPI;
    case SATA_SIG_SEMB:
        return AHCI_DEV_SEMB;
    case SATA_SIG_PM:
        return AHCI_DEV_PM;
    default:
        return AHCI_DEV_SATA;
    }
}

void probe_port(hba_mem_t *hba_addr)
{
    /* from https://wiki.osdev.org/AHCI#Detect_attached_SATA_devices 
     *
     *      the Port Implemented register (hba_mem.pi) is a 32 bit value
     *      and each bit represents a port. If the bit is set, the according
     *      port has a device attached, otherwise the port is free.  */

    uint32_t pi = hba_addr->pi; /* copy the port implemented register */
    for (uint8_t i = 0; i < 32; i++)
    {
        if (pi & 1) /* test the first bit */
        {
            int dt = check_type(&hba_addr->ports[i]);
            if (dt == AHCI_DEV_SATA)
                kprintf("[AHCI] SATA drive found, port = %u\n", i);
            else if (dt == AHCI_DEV_SATAPI)
                kprintf("[AHCI] SATAPI drive found, port = %u\n", i);
            else if (dt == AHCI_DEV_SEMB)
                kprintf("[AHCI] SEMB drive found, port = %u\n", i);
            else if (dt == AHCI_DEV_PM)
                kprintf("[AHCI] PM drive found, port = %u\n", i);
        }
        pi >>= 1; /* next port to the first bit */
    }
}

void start_cmd(hba_port_t *port)
{
    /* wait until CR (bit 15) is cleared */
    while (port->cmd & (1 << 15))
        ;

    /* cmd is the command and status register */
    port->cmd |= 1 << 4; /* set FRE */
    port->cmd |= 1;      /* set ST  */
}

void stop_cmd(hba_port_t *port)
{
    /* remember that ~ is NOT and & is AND
     *  0b101 &= ~0b001
     *  0b101 &= 0b110
     *  being the result 0b100 */

    /* cmd is the command and status register */
    port->cmd &= ~1;        /* clear ST  */
    port->cmd &= ~(1 << 4); /* clear FRE */

    /* wait until FR and CR are cleared */
    for (;;)
    {
        if (port->cmd & (1 << 14))
            continue;
        if (port->cmd & (1 << 15))
            continue;
        break;
    }
}

void port_rebase(hba_mem_t *hba_addr)
{
    /* (https://wiki.osdev.org/AHCI#AHCI_port_memory_space_initialization)
     *
     * before rebasing the port memory space, our kernel needs to wait for
     * pending commands */

    uint32_t pi = hba_addr->pi; /* copy the port implemented register */
    for (uint8_t i = 0; i < 32; i++)
    {
        if (pi & 1) /* test the first bit */
        {
            hba_port_t *port = &hba_addr->ports[i];
            stop_cmd(port); /* stop the command engine */

            /* clb is command list base address
             * command list base address: 1K*port */
            port->clb = AHCI_BASE + (i << 10);
            port->clbu = 0;
            memset((void *)&port->clb, 0, 1024);

            /* fb is FIS base address
             * FIS base address: 32K + 256*port */
            port->fb = AHCI_BASE + (32 << 10) + (i << 8);
            port->fbu = 0;
            memset((void *)&port->fb, 0, 256);

            hba_cmd_header_t *cmd_header = (hba_cmd_header_t *)(uint64_t)port->clb;
            for (uint8_t i = 0; i < 32; i++)
            {
                cmd_header[i].prdtl = 8;

                cmd_header[i].ctba = AHCI_BASE + (40 << 10) + (i << 13) + (i << 8);
                cmd_header[i].ctbau = 0;
                memset((void *)&cmd_header[i].ctba, 0, 256);
            }

            start_cmd(port); /* restart the command engine */
        }
        pi >>= 1; /* next port to the first bit */
    }
}

void init_ahci(uint64_t hba_addr)
{
    kprintf("[AHCI] ABAR = 0x%lx, reconfiguring AHCI memory\n", hba_addr);
    port_rebase((hba_mem_t *)hba_addr);
    kprintf("[AHCI] AHCI memory spaces configured, enumerating devices\n");
    probe_port((hba_mem_t *)hba_addr);
    kprintf("[AHCI] drive enumeration completed\n");
}