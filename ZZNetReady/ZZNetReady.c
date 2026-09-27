/*
 * ZZNetReady - Boot-time check: ZZ9000 present and Ethernet link ready?
 *
 * For S:User-Startup, to decide whether to start the network stack.
 * Needs no TCP/IP stack. Link readiness needs firmware with
 * ETH_CONFIG_CAP_LINK_STATE (merged 2026-09-27); on older firmware
 * the tool exits OK so the old always-start behaviour is kept.
 *
 * Template:  TIMEOUT/N,QUIET/S     (TIMEOUT in seconds, default 5)
 *
 * Return codes:
 *   0  (OK)    Link ready - start the network
 *              (also returned if the firmware can't report link state,
 *               so older firmware keeps the old always-start behaviour)
 *   5  (WARN)  ZZ9000 present, no link within TIMEOUT
 *   20 (FAIL)  No ZZ9000 found (or bad arguments)
 *
 * User-Startup example:
 *   ZZNetReady QUIET
 *   If WARN
 *     ; no card or no link - skip the stack this boot
 *   Else
 *     Run SYS:Network/... ; or your stack's start script
 *   EndIf
 *
 * Draft and tool by Kavanoz; register contract reviewed against the
 * firmware read paths and ZZ9000Net.device's own AutoConfig discovery.
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/configvars.h>

/* Classic NDK ordering: the library base is declared between the plain
 * headers and the proto headers, which reference it. */
struct ExpansionBase *ExpansionBase = NULL;

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>

/* AutoConfig IDs, matching ZZ9000Net.device (net/device.c): the Z3
 * register board is product 4 (5 is the Z3 FastRAM board), Z2 is 3. */
#define ZZ_MANUFACTURER  0x6D6E
#define ZZ_PROD_Z3       4
#define ZZ_PROD_Z2       3

/* REG_ZZ_ETH_CONFIG / ZZNET_ETH_CONFIG (firmware ethernet.h,
 * drivers net/mcast.h). Read-side bits added by the link-ready
 * firmware change: capability marker and negotiated-and-running. */
#define REG_ETH_CONFIG   0x8A
#define CAP_LINK_STATE   0x0002
#define LINK_READY       0x0100

#define POLL_TICKS       10         /* 0.2 s */

int main(void)
{
    LONG args[2] = { 0, 0 };        /* TIMEOUT/N, QUIET/S */
    struct RDArgs *rda;
    struct ConfigDev *cd = NULL;
    volatile UWORD *eth_config;
    LONG timeout = 5, waited = 0;
    BOOL quiet;
    UWORD v;

    if ((rda = ReadArgs((UBYTE *)"TIMEOUT/N,QUIET/S", args, NULL)) == NULL)
    {
        PrintFault(IoErr(), (UBYTE *)"ZZNetReady");
        return RETURN_FAIL;
    }
    if (args[0])
        timeout = *(LONG *)args[0];
    quiet = (args[1] != 0);
    FreeArgs(rda);

    if ((ExpansionBase = (struct ExpansionBase *)OpenLibrary(
        (UBYTE *)"expansion.library", 37)) != NULL)
    {
        cd = FindConfigDev(NULL, ZZ_MANUFACTURER, ZZ_PROD_Z3);
        if (cd == NULL)
            cd = FindConfigDev(NULL, ZZ_MANUFACTURER, ZZ_PROD_Z2);
        CloseLibrary((struct Library *)ExpansionBase);
    }

    if (cd == NULL)
    {
        if (!quiet) PutStr((UBYTE *)"No ZZ9000 found\n");
        return RETURN_FAIL;
    }

    eth_config = (volatile UWORD *)((UBYTE *)cd->cd_BoardAddr + REG_ETH_CONFIG);
    v = *eth_config;

    if (!(v & CAP_LINK_STATE))
    {
        if (!quiet)
            PutStr((UBYTE *)"ZZ9000 found; firmware can't report link, assuming ready\n");
        return RETURN_OK;
    }

    /* Negotiation takes a few seconds after the card powers up, so give
     * it TIMEOUT seconds on a cold boot. A warm reboot sees it at once. */
    while (!(v & LINK_READY))
    {
        if (waited >= timeout * TICKS_PER_SECOND ||
            (SetSignal(0, 0) & SIGBREAKF_CTRL_C))
        {
            if (!quiet) PutStr((UBYTE *)"ZZ9000 found; no Ethernet link\n");
            return RETURN_WARN;
        }
        Delay(POLL_TICKS);
        waited += POLL_TICKS;
        v = *eth_config;
    }

    if (!quiet) PutStr((UBYTE *)"ZZ9000 found; Ethernet link ready\n");
    return RETURN_OK;
}
