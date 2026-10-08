/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * input_receiver.c - VITA5: Vita-side USB pad input receiver.
 *
 * The PS5 is the USB HOST; the PS Vita is the USB DEVICE/gadget (SceUdcd).
 * Video flows Vita->PS5 through the UVC interfaces (VideoControl=0,
 * VideoStreaming=1, bulk IN 0x81). This plugin adds a THIRD, vendor-specific
 * interface (interface number 2) with one BULK OUT endpoint (0x02) to the
 * SAME USB configuration, so the PS5 host can write 28-byte pad reports
 * (see include/pad_wire.h) over the same cable. Each report is decoded and
 * injected into the Vita input system.
 *
 * INPUT INJECTION API (see README.md for evidence):
 *   ksceCtrlSetButtonEmulation() / ksceCtrlSetAnalogEmulation()
 *   (psp2kern/ctrl.h, SceCtrlForDriver_stub, NIDs 0x1E750326 / 0x06577FE8)
 *   The exact call pattern is taken from xerpi's ds4vita, a shipped kernel
 *   plugin that feeds an external pad into SceCtrl the same way:
 *     ksceCtrlSetButtonEmulation(0, 0, buttons, buttons, 32);
 *     ksceCtrlSetAnalogEmulation(0, 0, lx, ly, rx, ry,
 *                                lx, ly, rx, ry, 1);
 *
 * BUILD MODES:
 *   INPUT_RECEIVER_STANDALONE (default) - builds input_receiver.skprx which
 *     hooks ksceUdcdRegister() and swaps in extended configuration
 *     descriptor tables when the UVC gadget driver registers.
 *   INPUT_RECEIVER_INTEGRATED - built inside a patched vita-udcd-uvc tree
 *     (integration/vita-udcd-uvc-input.patch); the host plugin calls
 *     input_receiver_init(&endpoints[2]).
 *
 * THIS FILE IS VITA KERNEL CODE AND HAS NOT BEEN COMPILED OR RUN ON THIS
 * HOST (vitasdk is not installed here). Only the wire decoder
 * (src/pad_wire.c) is host-tested.
 */

#include <psp2kern/types.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/udcd.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>

#include <psp2/touch.h>
#include <stdarg.h>

#include "pad_wire.h"
#include "input_receiver.h"

#if !defined(INPUT_RECEIVER_STANDALONE) && !defined(INPUT_RECEIVER_INTEGRATED)
#define INPUT_RECEIVER_STANDALONE 1
#endif

/* Diagnostics file (read over FTP): the Vita's kernel log is not reachable
 * remotely, so the plugin appends its breadcrumbs to this file instead. */
#define IR_LOG_PATH "ur0:/tai/input_receiver.log"

/* v1.7: the pad channel rides a vendor control request on the gadget's own
 * EP0 (see input_process_request_hook). The descriptor-superset approach is
 * kept below for reference but disabled: on hardware, SceUdcd kept serving
 * the gadget's ORIGINAL configuration tables no matter how the driver's
 * configuration pointers were rewritten at registration (wire wTotalLength/
 * bNumInterfaces stayed at the originals), so the appended interface never
 * reached the host. Set to 1 to experiment with it again. */
#define INPUT_RECEIVER_DESCRIPTOR_PATCH 0

/* Start a fresh diagnostics file (called once from module_start). */
static void ir_log_reset(void)
{
	SceUID fd = ksceIoOpen(IR_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);

	if (fd < 0)
		return;
	ksceIoWrite(fd, "== input_receiver diagnostics ==\n", 32);
	ksceIoClose(fd);
}

/* Append one formatted line to the diagnostics file. */
static void ir_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	SceUID fd;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if ((unsigned int)n >= sizeof(buf))
		n = (int)sizeof(buf) - 1;
	fd = ksceIoOpen(IR_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0)
		return;
	ksceIoWrite(fd, buf, (SceSize)n);
	ksceIoClose(fd);
}

/* Per-hop event counters, incremented even in contexts where file I/O is
 * impossible (the EP0 completion callback and the processRequest hook); the
 * worker thread dumps them. */
static volatile int g_cnt_req;
static volatile int g_cnt_arm;
static volatile int g_cnt_done;
static int g_log_emu;
static int g_log_wire;

/* Telemetry: which stage of the touch pipeline actually runs. (Declared
 * here, before the worker thread that dumps them.) */
static volatile unsigned int g_touch_chunks_seen;
static volatile unsigned int g_touch_records_ok;
static volatile unsigned int g_touch_records_bad;
static volatile unsigned int g_touch_reads_seen;
static volatile unsigned int g_touch_injected;
/* Vendor control requests seen by the processRequest hook (all shapes). */
static volatile unsigned int g_vendor_reqs;

/* ------------------------------------------------------------------------- */
/* USB layout constants                                                      */
/* ------------------------------------------------------------------------- */

/** Vendor-specific input interface number: assigned dynamically at hook
 *  time (see input_build_superset). Base vita-udcd-uvc exposes two
 *  interfaces (VideoControl=0, VideoStreaming=1) but VitaUSBStream also
 *  carries the UAC audio interfaces (2, 3), so a fixed number would collide
 *  with an existing interface. The host finds this interface by class/subclass
 *  (0xFF/0x50), never by number. */
#define INPUT_INTERFACE_CLASS    0xFF /* USB_CLASS_VENDOR_SPEC */
#define INPUT_INTERFACE_SUBCLASS 0x50 /* 'P' (pad) */
#define INPUT_INTERFACE_PROTOCOL 0x01 /* pad wire format v1 */

/** Pad OUT endpoint address (bulk OUT). Endpoint NUMBER matters: the gadget
 *  driver's own endpoints are numbered from their descriptor addresses, and a
 *  queued request on a pre-filled number binds to whatever SceUdcd later puts
 *  on that number. Live probe of VitaUSBStream (tools/probe_report.txt):
 *  EP1 IN = video 0x81, EP3 IN = audio isoc 0x83. Numbers 0 (EP0), 1 and 3
 *  are taken - EP2 OUT is free in both directions and matches the wire
 *  layout documented in README.md. */
#define INPUT_ENDPOINT_ADDRESS   (USB_ENDPOINT_OUT | 0x02)
#define INPUT_ENDPOINT_NUMBER    2

/** Wire size added to the configuration descriptor wTotalLength:
 *  one interface descriptor (9) + one endpoint descriptor (7). */
#define INPUT_DESCRIPTOR_DELTA   (USB_DT_INTERFACE_SIZE + USB_DT_ENDPOINT_SIZE)

/* Driver name of xerpi's base vita-udcd-uvc UVC_DRIVER_NAME. Kept as one
 * match criterion, but NOT the only one: forks of the gadget (VitaUSBStream)
 * may rename their SceUdcdDriver, and a name-only check then fails silently
 * and the patch never applies (seen live: the vendor interface was simply
 * absent from the enumerated config). The reliable signal is the driver's
 * own configuration exposing a Video (class 0x0E) interface. */
#define INPUT_UVC_DRIVER_NAME    "VITAUVC00"

/* ------------------------------------------------------------------------- */
/* SceCtrl mapping checks                                                    */
/*                                                                           */
/* The wire button layout mirrors SceCtrlButtons for bits 0..16 (see         */
/* pad_wire.h). Assert that against the real SDK enum at compile time.       */
/* ------------------------------------------------------------------------- */

PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_SELECT   == SCE_CTRL_SELECT,   ctrl_select);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_L3       == SCE_CTRL_L3,       ctrl_l3);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_R3       == SCE_CTRL_R3,       ctrl_r3);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_START    == SCE_CTRL_START,    ctrl_start);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_UP       == SCE_CTRL_UP,       ctrl_up);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_RIGHT    == SCE_CTRL_RIGHT,    ctrl_right);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_DOWN     == SCE_CTRL_DOWN,     ctrl_down);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_LEFT     == SCE_CTRL_LEFT,     ctrl_left);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_LTRIGGER == SCE_CTRL_LTRIGGER, ctrl_ltrigger);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_RTRIGGER == SCE_CTRL_RTRIGGER, ctrl_rtrigger);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_L1       == SCE_CTRL_L1,       ctrl_l1);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_R1       == SCE_CTRL_R1,       ctrl_r1);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_TRIANGLE == SCE_CTRL_TRIANGLE, ctrl_triangle);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_CIRCLE   == SCE_CTRL_CIRCLE,   ctrl_circle);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_CROSS    == SCE_CTRL_CROSS,    ctrl_cross);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_SQUARE   == SCE_CTRL_SQUARE,   ctrl_square);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_CTRL_HOME     == SCE_CTRL_PSBUTTON, ctrl_home);

/* ------------------------------------------------------------------------- */
/* Injection parameters (mirroring ds4vita)                                  */
/* ------------------------------------------------------------------------- */

#define INPUT_CTRL_PORT            0
#define INPUT_CTRL_SLOT            0
/** Button emulation duration in sampling counts (~0.5 s at 60 Hz sampling).
 *  Reports keep refreshing it; if they stop, the emulation expires and the
 *  thread also explicitly resets it (stuck-input failsafe). */
#define INPUT_EMULATION_MAKE_COUNT 32
#define INPUT_ANALOG_MAKE_COUNT    1
#define INPUT_STALL_RESET_US       500000 /* 500 ms without a report */

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static SceUdcdEndpoint *g_input_ep;
static int g_run;
static int g_rx_armed;
static int g_injected;
static SceUID g_thread_uid = -1;
static SceUID g_evflag_uid = -1;
static uint64_t g_last_report_time;
static uint64_t g_last_telemetry_us;

/* RX buffer for one wire report. Invalidate the dcache around DMA use
 * (same pattern as vita-udcd-uvc's usb_ep0_enqueue_recv). */
static unsigned char g_rx_buf[PAD_WIRE_SIZE] __attribute__((aligned(64)));

/* Single-slot mailbox of RAW WIRE BYTES: filled by the USB completion
 * callback, drained (decoded) by the worker thread. */
static unsigned char g_mailbox[PAD_WIRE_SIZE];
static volatile int g_mailbox_valid;

/* Chunked SETUP-only reassembly buffer (see input_process_request_hook). */
static uint8_t g_chunk_buf[PAD_WIRE_SIZE];
static uint32_t g_chunk_mask;

static void input_rx_on_complete(SceUdcdDeviceRequest *req);

/* ------------------------------------------------------------------------- */
/* Input injection (ds4vita call pattern)                                    */
/* ------------------------------------------------------------------------- */

static void input_reset_injection(void)
{
	ksceCtrlSetButtonEmulation(INPUT_CTRL_PORT, INPUT_CTRL_SLOT, 0, 0,
				   INPUT_EMULATION_MAKE_COUNT);
	ksceCtrlSetAnalogEmulation(INPUT_CTRL_PORT, INPUT_CTRL_SLOT,
				   0x80, 0x80, 0x80, 0x80,
				   0x80, 0x80, 0x80, 0x80, 0);
	g_injected = 0;
}

static void input_inject_report(const pad_wire_report *r)
{
	uint32_t buttons = pad_wire_buttons_to_ctrl(r->buttons);
	uint8_t lx = pad_wire_stick_to_u8(r->left_x);
	uint8_t ly = pad_wire_stick_to_u8(r->left_y);
	uint8_t rx = pad_wire_stick_to_u8(r->right_x);
	uint8_t ry = pad_wire_stick_to_u8(r->right_y);
	int moved;

	buttons = pad_wire_apply_triggers(buttons, r->l2, r->r2,
					  PAD_WIRE_TRIGGER_THRESHOLD);

	moved = (lx != 0x80) || (ly != 0x80) || (rx != 0x80) || (ry != 0x80) ||
		(r->l2 > PAD_WIRE_TRIGGER_THRESHOLD) ||
		(r->r2 > PAD_WIRE_TRIGGER_THRESHOLD);

	{
		int ret_b = ksceCtrlSetButtonEmulation(INPUT_CTRL_PORT, INPUT_CTRL_SLOT,
						       buttons, buttons,
						       INPUT_EMULATION_MAKE_COUNT);
		int ret_a = ksceCtrlSetAnalogEmulation(INPUT_CTRL_PORT, INPUT_CTRL_SLOT,
						       lx, ly, rx, ry, lx, ly, rx, ry,
						       INPUT_ANALOG_MAKE_COUNT);
		if (g_log_emu < 8) {
			g_log_emu++;
			ir_log("emu: buttons=0x%x ret=%d analog ret=%d\n",
			       (unsigned)buttons, ret_b, ret_a);
		}
	}

	g_injected = 1;
	g_last_report_time = ksceKernelGetSystemTimeWide();

	if (buttons != 0 || moved)
		ksceKernelPowerTick(0); /* keep the Vita awake while playing */
}

/* ------------------------------------------------------------------------- */
/* USB receive pipeline                                                      */
/* ------------------------------------------------------------------------- */

static int input_arm_rx(void)
{
	static SceUdcdDeviceRequest req;
	int ret;

	/* The pad report rides the data stage of a vendor control OUT
	 * (0x40/0x50) on the gadget's OWN EP0 endpoint struct - the same
	 * pattern vita-udcd-uvc uses for its 34-byte UVC SET_CUR requests.
	 * This runs from the processRequest hook when such a request arrives. */
	if (g_input_ep == NULL || !g_run)
		return -1;

	req = (SceUdcdDeviceRequest){
		.endpoint = g_input_ep,
		.data = g_rx_buf,
		.attributes = 0,
		.size = PAD_WIRE_SIZE,
		.isControlRequest = 0,
		.onComplete = &input_rx_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	ksceKernelDcacheInvalidateRange(g_rx_buf, PAD_WIRE_SIZE);

	ret = ksceUdcdReqRecv(&req);
	g_rx_armed = (ret >= 0) ? 1 : 0;
	if (g_cnt_arm < 12)
		g_cnt_arm++;
	return ret;
}

static void input_rx_on_complete(SceUdcdDeviceRequest *req)
{
	if (g_cnt_done < 12)
		g_cnt_done++;

	if (!g_run)
		return;

	if (req->returnCode < 0) {
		/* Cancelled (detach / ClearFIFO / endpoint halt). The worker
		 * thread re-arms once the endpoint is usable again. */
		g_rx_armed = 0;
		ksceKernelSetEventFlag(g_evflag_uid, 1);
		return;
	}

	if (req->transmitted == PAD_WIRE_SIZE) {
		ksceKernelDcacheInvalidateRange(g_rx_buf, PAD_WIRE_SIZE);
		memcpy(g_mailbox, g_rx_buf, PAD_WIRE_SIZE);
		g_mailbox_valid = 1;
	}

	/* The next report arms its own EP0 recv from the processRequest
	 * hook; nothing to re-arm here. */
	ksceKernelSetEventFlag(g_evflag_uid, 1);
}

static int input_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	while (g_run) {
		unsigned int out_bits;
		int ret;

		/* Telemetry dump from the worker: the only context where file
		 * I/O is safe. Hooks only bump counters (v2.0.3). */
		{
			uint64_t now = ksceKernelGetSystemTimeWide();

			if (g_last_telemetry_us == 0)
				g_last_telemetry_us = now;
			else if (now - g_last_telemetry_us > 5000000ull) {
				g_last_telemetry_us = now;
				ir_log("telemetry: vendor=%u touch_chunks=%u ok=%u bad=%u reads=%u injected=%u chain=%d/%d/%d",
				       g_vendor_reqs, g_touch_chunks_seen,
				       g_touch_records_ok, g_touch_records_bad,
				       g_touch_reads_seen, g_touch_injected,
				       g_cnt_req, g_cnt_arm, g_cnt_done);
			}
		}

		/* Each vendor control request arms its own data-stage recv. */

		ret = ksceKernelWaitEventFlagCB(g_evflag_uid, 1,
						SCE_EVENT_WAITOR |
						SCE_EVENT_WAITCLEAR_PAT,
						&out_bits,
						(SceUInt32[]){100000});

		if (ret == 0 && g_mailbox_valid) {
			pad_wire_report r;
			int dec;

			g_mailbox_valid = 0;
			dec = pad_wire_decode(g_mailbox, PAD_WIRE_SIZE, &r);
			if (g_log_wire < 12) {
				g_log_wire++;
				ir_log("chain: req=%d arm=%d done=%d\n",
				       g_cnt_req, g_cnt_arm, g_cnt_done);
				ir_log("wire: %02x %02x %02x %02x %02x %02x %02x %02x "
				       "%02x %02x %02x %02x %02x %02x %02x %02x\n",
				       g_mailbox[0], g_mailbox[1], g_mailbox[2], g_mailbox[3],
				       g_mailbox[4], g_mailbox[5], g_mailbox[6], g_mailbox[7],
				       g_mailbox[8], g_mailbox[9], g_mailbox[10], g_mailbox[11],
				       g_mailbox[12], g_mailbox[13], g_mailbox[14], g_mailbox[15]);
				ir_log("decode=%d buttons=0x%x\n", dec,
				       dec == 0 ? (unsigned)r.buttons : 0u);
			}
			if (dec == 0)
				input_inject_report(&r);
		} else if (ret == (int)0x80028005) {
			/* SCE_KERNEL_ERROR_WAIT_TIMEOUT: no report for
			 * 100 ms. Drop the emulation if the stream stalled so
			 * no button stays stuck. */
			if (g_injected &&
			    (ksceKernelGetSystemTimeWide() - g_last_report_time) >
			        INPUT_STALL_RESET_US)
				input_reset_injection();
		}
	}

	input_reset_injection();
	return 0;
}

int input_receiver_init(SceUdcdEndpoint *input_ep)
{
	int ret;

	/* Standalone mode passes NULL: g_input_ep is bound to the gadget's own
	 * EP0 endpoint when the UVC driver registers. Mode A passes the UVC
	 * plugin's pad endpoint directly. */
	if (input_ep != NULL)
		g_input_ep = input_ep;
	g_run = 1;
	g_rx_armed = 0;
	g_mailbox_valid = 0;
	g_last_report_time = ksceKernelGetSystemTimeWide();

	g_evflag_uid = ksceKernelCreateEventFlag("vd5_input", 0, 0, NULL);
	if (g_evflag_uid < 0)
		return g_evflag_uid;

	g_thread_uid = ksceKernelCreateThread("vd5_input_thread", input_thread,
					      0x3C, 0x2000, 0, 0x10000, 0);
	if (g_thread_uid < 0) {
		ret = g_thread_uid;
		ksceKernelDeleteEventFlag(g_evflag_uid);
		g_evflag_uid = -1;
		return ret;
	}

	ret = ksceKernelStartThread(g_thread_uid, 0, NULL);
	if (ret < 0) {
		ksceKernelDeleteThread(g_thread_uid);
		ksceKernelDeleteEventFlag(g_evflag_uid);
		g_thread_uid = -1;
		g_evflag_uid = -1;
		g_run = 0;
		return ret;
	}

	return 0;
}

void input_receiver_term(void)
{
	g_run = 0;

	if (g_input_ep != NULL && g_rx_armed) {
		ksceUdcdClearFIFO(g_input_ep);
		ksceUdcdReqCancelAll(g_input_ep);
	}
	g_rx_armed = 0;

	if (g_evflag_uid >= 0)
		ksceKernelSetEventFlag(g_evflag_uid, 1);
	if (g_thread_uid >= 0) {
		ksceKernelWaitThreadEnd(g_thread_uid, NULL, NULL);
		ksceKernelDeleteThread(g_thread_uid);
		g_thread_uid = -1;
	}
	if (g_evflag_uid >= 0) {
		ksceKernelDeleteEventFlag(g_evflag_uid);
		g_evflag_uid = -1;
	}

	input_reset_injection();
	g_input_ep = NULL;
}

#ifdef INPUT_RECEIVER_STANDALONE

/* ------------------------------------------------------------------------- */
/* Standalone mode: descriptor extension + ksceUdcdRegister hook             */
/*                                                                           */
/* A USB gadget has exactly one configuration descriptor owned by one        */
/* registered SceUdcdDriver (the UVC plugin's). A second, separately         */
/* registered driver cannot add interfaces to it. Instead we hook            */
/* ksceUdcdRegister() (SceUdcdForDriver NID 0x4E55244D) and, before the      */
/* registration proceeds, replace the driver's configuration tables with     */
/* superset copies that add interface 2 + endpoint 0x02. The UVC plugin's    */
/* own endpoints[] array (used for the video bulk IN 0x81 transfers) is      */
/* left untouched so video streaming is unaffected.                          */
/* ------------------------------------------------------------------------- */

static tai_hook_ref_t ksceUdcdRegister_ref;
static int g_patched;

#if INPUT_RECEIVER_DESCRIPTOR_PATCH
/* Interface 2 endpoint descriptor: bulk OUT 0x02. */
static SceUdcdEndpointDescriptor input_endpdesc_hi = {
	.bLength          = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType  = USB_DT_ENDPOINT,
	.bEndpointAddress = INPUT_ENDPOINT_ADDRESS, /* OUT 0x02 */
	.bmAttributes     = USB_ENDPOINT_TYPE_BULK,
	.wMaxPacketSize   = 0x200,                  /* hi-speed */
	.bInterval        = 0x00,
	.extra            = NULL,
	.extraLength      = 0
};

static SceUdcdEndpointDescriptor input_endpdesc_full = {
	.bLength          = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType  = USB_DT_ENDPOINT,
	.bEndpointAddress = INPUT_ENDPOINT_ADDRESS,
	.bmAttributes     = USB_ENDPOINT_TYPE_BULK,
	.wMaxPacketSize   = 0x40,                   /* full-speed */
	.bInterval        = 0x00,
	.extra            = NULL,
	.extraLength      = 0
};

/* The OUT endpoint as seen by ksceUdcdReqRecv().
 *
 * NOTE (unverified assumption): this struct is NOT part of the UVC driver's
 * SceUdcdEndpoint endpoints[] array (that array has exactly 2 slots, EP0 +
 * video IN, and cannot be extended from outside). We therefore pre-fill
 * endpointNumber with the descriptor's endpoint address (2) ourselves
 * instead of letting SceUdcd fill it in. Whether SceUdcd accepts
 * ksceUdcdReqRecv() on an endpoint struct outside the registered driver's
 * list is not verified on hardware - see README "Unverified". */
static SceUdcdEndpoint input_endpoint = {
	USB_ENDPOINT_OUT,             /* direction */
	INPUT_ENDPOINT_NUMBER,        /* driverEndpointNumber */
	INPUT_ENDPOINT_NUMBER,        /* endpointNumber (pre-filled, see above) */
	0                             /* transmittedBytes */
};

#define INPUT_MAX_INTERFACES 8

static tai_hook_ref_t ksceUdcdRegister_ref;

typedef struct {
	SceUdcdInterfaceDescriptor ifaces[INPUT_MAX_INTERFACES + 1]; /* + terminator */
	SceUdcdInterfaceSettings settings[INPUT_MAX_INTERFACES];
	SceUdcdConfigDescriptor confdesc;
	SceUdcdConfiguration config;
} InputGadgetExt;

static InputGadgetExt g_ext_hi;
static InputGadgetExt g_ext_full;
static SceUdcdInterface g_ext_iface;
static int g_patched;

/*
 * Build a superset of an existing SceUdcdConfiguration: copy its interface
 * descriptors by value (their `extra`/`endpoints` pointers keep pointing at
 * the UVC plugin's own class descriptors and endpoint descriptors, which
 * stay in place and on the wire byte-identical), append the vendor input
 * interface, and fix up wTotalLength / bNumInterfaces.
 */
static SceUdcdConfiguration *input_build_superset(InputGadgetExt *ext,
						  SceUdcdConfiguration *orig,
						  SceUdcdEndpointDescriptor *input_ep_desc)
{
	SceUdcdConfigDescriptor *orig_conf = orig->configDescriptors;
	SceUdcdInterfaceDescriptor *src;
	unsigned int count = 0;
	unsigned int next_ifnum = 0;
	unsigned int i;

	memset(ext, 0, sizeof(*ext));

	/* Copy the existing interface descriptors (VideoControl, VideoStreaming,
	 * and on VitaUSBStream also AudioControl, AudioStreaming including its
	 * alternate setting). They are laid out as a contiguous,
	 * 0-length-terminated array. If they cannot all be represented, return
	 * NULL and leave the gadget's config untouched - a truncated config
	 * would break enumeration. */
	for (src = orig->interfaceDescriptors;
	     src != NULL && src->bLength != 0;
	     src++) {
		if (count >= INPUT_MAX_INTERFACES - 1)
			return NULL;
		ext->ifaces[count++] = *src;
		/* Next FREE interface number: descriptor count is not interface
		 * count (an interface may have several alternate settings), so
		 * track the highest bInterfaceNumber seen. */
		if ((unsigned int)src->bInterfaceNumber + 1u > next_ifnum)
			next_ifnum = (unsigned int)src->bInterfaceNumber + 1u;
	}
	if (count == 0)
		return NULL;

	/* Append the vendor input interface with the next free interface number
	 * (the gadget may expose more than the base two interfaces). */
	ext->ifaces[count] = (SceUdcdInterfaceDescriptor){
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		(uint8_t)next_ifnum,      /* bInterfaceNumber */
		0,                        /* bAlternateSetting */
		1,                        /* bNumEndpoints */
		INPUT_INTERFACE_CLASS,
		INPUT_INTERFACE_SUBCLASS,
		INPUT_INTERFACE_PROTOCOL,
		0,                        /* iInterface */
		input_ep_desc,            /* endpoints */
		NULL, 0                   /* extra */
	};
	count++;
	/* ext->ifaces[count] stays zeroed: array terminator */

	/* Interface settings: synthesize one xerpi-style entry per interface
	 * descriptor, {&iface, its own bAlternateSetting, 1}. Never read the
	 * gadget's own settings table: its length is unknown (v1.3 copied it
	 * and the appended interface never reached the wire), and the
	 * self-consistent synthesized table is exactly the layout
	 * vita-udcd-uvc itself uses. */
	for (i = 0; i < count; i++)
		ext->settings[i] = (SceUdcdInterfaceSettings){
			&ext->ifaces[i], (unsigned int)ext->ifaces[i].bAlternateSetting, 1
		};

	/* Configuration descriptor: same fields as the original, plus one
	 * interface descriptor (9 bytes) and one endpoint descriptor (7). */
	ext->confdesc = *orig_conf;
	ext->confdesc.wTotalLength += INPUT_DESCRIPTOR_DELTA;
	ext->confdesc.bNumInterfaces += 1;
	ext->confdesc.settings = &ext->settings[0];

	ext->config = (SceUdcdConfiguration){
		&ext->confdesc,
		&ext->settings[0],
		&ext->ifaces[0],
		orig->endpointDescriptors
	};

	ir_log("superset: %u entries, appended iface %u, wTotalLength +%u\n",
	       count, (unsigned)next_ifnum, (unsigned)INPUT_DESCRIPTOR_DELTA);

	return &ext->config;
}
#endif /* INPUT_RECEIVER_DESCRIPTOR_PATCH */

static SceUdcdDriver *g_patched_driver;

/* Does this configuration expose a Video-class (UVC) interface? The walk is
 * bounded and stops at the first entry that is not a valid interface
 * descriptor, so a non-standard layout can never run off into the weeds. */
static int config_has_video_iface(SceUdcdConfiguration *cfg)
{
	SceUdcdInterfaceDescriptor *d;
	int i;

	if (cfg == NULL || cfg->interfaceDescriptors == NULL)
		return 0;
	for (i = 0, d = cfg->interfaceDescriptors; i < 16; i++, d++) {
		if (d->bLength != USB_DT_INTERFACE_SIZE || d->bDescriptorType != USB_DT_INTERFACE)
			break;
		if (d->bInterfaceClass == 0x0E)
			return 1;
	}
	return 0;
}

/* The gadget driver this receiver attaches to: xerpi's base name, or any
 * driver whose configuration carries UVC interfaces (forks may rename). */
static int is_uvc_gadget_driver(SceUdcdDriver *driver)
{
	if (driver == NULL)
		return 0;
	if (driver->driverName != NULL &&
	    strcmp(driver->driverName, INPUT_UVC_DRIVER_NAME) == 0)
		return 1;
	return config_has_video_iface(driver->configuration_hi) ||
	       config_has_video_iface(driver->configuration);
}

/* Bounded, validated dump of a configuration's interface descriptors into the
 * diagnostics file. Only entries that look exactly like an interface
 * descriptor are read; the walk stops at the first anything-else. */
static void ir_describe_config(const char *tag, SceUdcdConfiguration *cfg)
{
	SceUdcdInterfaceDescriptor *d;
	int i;

	if (cfg == NULL || cfg->interfaceDescriptors == NULL) {
		ir_log("  %s: config=%p without interfaceDescriptors\n", tag, (void *)cfg);
		return;
	}
	for (i = 0, d = cfg->interfaceDescriptors; i < 16; i++, d++) {
		if (d->bLength != USB_DT_INTERFACE_SIZE || d->bDescriptorType != USB_DT_INTERFACE)
			break;
		ir_log("  %s[%d]: num=%u alt=%u class=%02x sub=%02x proto=%02x eps=%u\n",
		       tag, i, (unsigned)d->bInterfaceNumber, (unsigned)d->bAlternateSetting,
		       (unsigned)d->bInterfaceClass, (unsigned)d->bInterfaceSubClass,
		       (unsigned)d->bInterfaceProtocol, (unsigned)d->bNumEndpoints);
	}
	ir_log("  %s: %d interface entries\n", tag, i);
}

/* Chunked SETUP-only channel: EP0 OUT DATA STAGES DO NOT DELIVER on this
 * firmware (proven by the UVC probe SET_CUR -> GET_CUR round-trip test:
 * payload/eptest.c - even the gadget's own 34-byte SET_CUR data never
 * arrives). SETUP-only control transfers, however, demonstrably reach
 * processRequest. So the 28-byte pad report rides SEVEN no-data control
 * OUTs (bRequest 0x50..0x56 = chunk index), 4 bytes per request carried
 * in wValue/wIndex - legal vendor semantics and no data stage anywhere.
 * The interface-recipient shape (0x21) is kept; 0x40 is accepted too. */
#define INPUT_CTRL_BMREQUEST   0x21
#define INPUT_CTRL_BMREQUEST_ALT 0x40 /* accepted for older host builds */
#define INPUT_CTRL_BREQUEST    0x50
#define INPUT_CTRL_CHUNKS      7 /* 7 * 4 bytes = PAD_WIRE_SIZE */

/* Touch records ride their own chunk family: bRequest 0x58..0x5B,
 * 4 chunks * 4 bytes = PAD_WIRE_TOUCH_SIZE. Chunk 0 resyncs. */
#define INPUT_TOUCH_BREQUEST   0x58
#define INPUT_TOUCH_CHUNKS     4

/* ------------------------------------------------------------------------- */
/* Touch injection (SceTouch* hooks)                                         */
/*                                                                           */
/* Mechanism confirmed from vitacompanion's kernel/touch_patch.c: hook the   */
/* SceTouch read exports and append synthetic SceTouchReport entries to     */
/* every sample the game reads. Fingers 0/1 from the wire record map onto    */
/* injection slots 0/1 (synthetic ids 0x70/0x71).                           */
/* ------------------------------------------------------------------------- */

#define TOUCH_SYNTHETIC_ID_BASE  0x70
#define TOUCH_FRONT_MAX_REPORTS  6
#define TOUCH_REAR_MAX_REPORTS   4

typedef struct {
	volatile int active;
	volatile unsigned int x;
	volatile unsigned int y;
} TouchSlot;

static TouchSlot g_touch_slots[2];
static volatile unsigned int g_touch_port; /* 0 = front, 1 = rear */
static uint8_t g_touch_buf[PAD_WIRE_TOUCH_SIZE];
static uint32_t g_touch_mask;

static void touch_apply_record(const pad_wire_touch *t)
{
	g_touch_port = t->port;
	g_touch_slots[0].x = t->f0_x;
	g_touch_slots[0].y = t->f0_y;
	g_touch_slots[0].active = (t->f0_active != 0);
	g_touch_slots[1].x = t->f1_x;
	g_touch_slots[1].y = t->f1_y;
	g_touch_slots[1].active = (t->f1_active != 0);
}

/* Append our active slots to one buffer of SceTouchData (kernel pointers). */
static unsigned int touch_patch_data(unsigned int port, SceTouchData *data,
				     unsigned int count)
{
	unsigned int max_reports =
		(port == PAD_WIRE_TOUCH_PORT_FRONT) ? TOUCH_FRONT_MAX_REPORTS
						    : TOUCH_REAR_MAX_REPORTS;
	unsigned int i, injected = 0;

	if (data == NULL || count == 0)
		return 0;

	/* Counter-only in hook context: file I/O from the SceTouch read hook
	 * races the gadget's timing-critical paths. Telemetry is dumped by the
	 * worker thread instead (v2.0.3). */
	if (g_touch_reads_seen < 4000000000u)
		g_touch_reads_seen++;

	if (port != g_touch_port)
		return 0;

	for (i = 0; i < count; i++) {
		SceTouchData *cur = &data[i];
		int s;

		for (s = 0; s < 2; s++) {
			SceTouchReport *r;

			if (!g_touch_slots[s].active ||
			    cur->reportNum >= max_reports)
				continue;
			r = &cur->report[cur->reportNum++];
			r->id = (uint8_t)(TOUCH_SYNTHETIC_ID_BASE + s);
			r->force = 0x80;
			r->x = (int16_t)g_touch_slots[s].x;
			r->y = (int16_t)g_touch_slots[s].y;
			memset(r->reserved, 0, sizeof(r->reserved));
			r->info = 0;
			injected++;
		}
	}

	if (injected != 0) {
		g_touch_injected += injected;
		ksceKernelPowerTick(0);
	}
	return injected;
}

/* Same for the *_Ext entry points, which receive USER pointers. */
static unsigned int touch_patch_user(unsigned int port,
				     SceTouchData *user_data,
				     unsigned int count)
{
	unsigned int i, injected = 0;

	if (user_data == NULL || count == 0 || count > 8)
		return 0;

	for (i = 0; i < count; i++) {
		SceTouchData sample;
		unsigned int n;

		if (ksceKernelCopyFromUser(&sample, &user_data[i],
					   sizeof(sample)) < 0)
			break;
		n = touch_patch_data(port, &sample, 1);
		if (n == 0)
			continue;
		if (ksceKernelCopyToUser(&user_data[i], &sample,
					 sizeof(sample)) < 0)
			break;
		injected += n;
	}
	return injected;
}

static tai_hook_ref_t touch_peek_ref;
static tai_hook_ref_t touch_peek_region_ref;
static tai_hook_ref_t touch_read_ref;
static tai_hook_ref_t touch_read_region_ref;
static tai_hook_ref_t touch_peek_region_ext_ref;
static tai_hook_ref_t touch_read_region_ext_ref;
static SceUID touch_hook_ids[4] = {-1, -1, -1, -1};
static SceUID touch_ext_hook_ids[2] = {-1, -1};

static int touch_peek_hook(unsigned int port, SceTouchData *data,
			   unsigned int count)
{
	int result = TAI_CONTINUE(int, touch_peek_ref, port, data, count);

	if (result > 0)
		touch_patch_data(port, data, (unsigned int)result);
	return result;
}

static int touch_peek_region_hook(unsigned int port, SceTouchData *data,
				  unsigned int count, int region)
{
	int result = TAI_CONTINUE(int, touch_peek_region_ref, port, data, count,
				  region);

	if (result > 0)
		touch_patch_data(port, data, (unsigned int)result);
	return result;
}

static int touch_read_hook(unsigned int port, SceTouchData *data,
			   unsigned int count)
{
	int result = TAI_CONTINUE(int, touch_read_ref, port, data, count);

	if (result > 0)
		touch_patch_data(port, data, (unsigned int)result);
	return result;
}

static int touch_read_region_hook(unsigned int port, SceTouchData *data,
				  unsigned int count, int region)
{
	int result = TAI_CONTINUE(int, touch_read_region_ref, port, data, count,
				  region);

	if (result > 0)
		touch_patch_data(port, data, (unsigned int)result);
	return result;
}

static int touch_peek_region_ext_hook(unsigned int port, SceTouchData *data,
				      unsigned int count, int region)
{
	int result = TAI_CONTINUE(int, touch_peek_region_ext_ref, port, data,
				  count, region);

	if (result > 0)
		touch_patch_user(port, data, (unsigned int)result);
	return result;
}

static int touch_read_region_ext_hook(unsigned int port, SceTouchData *data,
				      unsigned int count, int region)
{
	int result = TAI_CONTINUE(int, touch_read_region_ext_ref, port, data,
				  count, region);

	if (result > 0)
		touch_patch_user(port, data, (unsigned int)result);
	return result;
}

static void touch_release_hooks(void)
{
	int i;

	for (i = 1; i >= 0; --i) {
		if (touch_ext_hook_ids[i] >= 0) {
			taiHookReleaseForKernel(touch_ext_hook_ids[i],
						i == 0 ? touch_peek_region_ext_ref
						       : touch_read_region_ext_ref);
			touch_ext_hook_ids[i] = -1;
		}
	}
	for (i = 3; i >= 0; --i) {
		static tai_hook_ref_t *refs[4] = {
			&touch_peek_ref, &touch_peek_region_ref,
			&touch_read_ref, &touch_read_region_ref
		};
		if (touch_hook_ids[i] >= 0) {
			taiHookReleaseForKernel(touch_hook_ids[i], *refs[i]);
			touch_hook_ids[i] = -1;
		}
	}
}

static int touch_install_hooks(void)
{
	/* NIDs verified against vitacompanion's kernel/main.c (working
	 * touch injection on real hardware). */
	static const uint32_t nids[4] = {
		0xBAD1960B, /* sceTouchPeek */
		0x9B3F7207, /* sceTouchPeekRegion */
		0x70C8AACE, /* sceTouchRead */
		0x9A91F624  /* sceTouchReadRegion */
	};
	static void *const functions[4] = {
		(void *)touch_peek_hook, (void *)touch_peek_region_hook,
		(void *)touch_read_hook, (void *)touch_read_region_hook
	};
	tai_hook_ref_t *refs[4] = {
		&touch_peek_ref, &touch_peek_region_ref,
		&touch_read_ref, &touch_read_region_ref
	};
	tai_module_info_t info;
	const char *module_name = "SceTouch";
	int i;

	info.size = sizeof(info);
	if (taiGetModuleInfoForKernel(KERNEL_PID, module_name, &info) < 0) {
		module_name = "SceTouchDummy";
		info.size = sizeof(info);
		if (taiGetModuleInfoForKernel(KERNEL_PID, module_name, &info) < 0)
			return -1;
	}

	for (i = 0; i < 4; ++i) {
		touch_hook_ids[i] = taiHookFunctionExportForKernel(
			KERNEL_PID, refs[i], module_name, TAI_ANY_LIBRARY,
			nids[i], functions[i]);
		if (touch_hook_ids[i] < 0) {
			touch_release_hooks();
			return touch_hook_ids[i];
		}
	}

	/* The *_Ext entry points live in a named library. */
	{
		static const uint32_t ext_nids[2] = {
			0x2CF6D7E2, /* sceTouchPeekRegionExt */
			0x9F0ACAF9  /* sceTouchReadRegionExt */
		};
		static void *const ext_functions[2] = {
			(void *)touch_peek_region_ext_hook,
			(void *)touch_read_region_ext_hook
		};
		tai_hook_ref_t *ext_refs[2] = {
			&touch_peek_region_ext_ref, &touch_read_region_ext_ref
		};

		for (i = 0; i < 2; ++i) {
			touch_ext_hook_ids[i] = taiHookFunctionExportForKernel(
				KERNEL_PID, ext_refs[i], module_name,
				0x3E4F4A81, /* SCE_TOUCH_USER_LIBRARY_NID */
				ext_nids[i], ext_functions[i]);
		}
	}
	return 0;
}

static int (*g_orig_process_request)(int recipient, int arg,
				     SceUdcdEP0DeviceRequest *req, void *user_data);

static int input_process_request_hook(int recipient, int arg,
				      SceUdcdEP0DeviceRequest *req, void *user_data)
{
	if (arg >= 0 && req != NULL &&
	    (req->bmRequestType == INPUT_CTRL_BMREQUEST ||
	     req->bmRequestType == INPUT_CTRL_BMREQUEST_ALT)) {
		/* Counter-only in hook context (file I/O dropped writes here
		 * anyway); the worker thread dumps it (v2.0.3). */
		g_vendor_reqs++;
	}
	if (arg >= 0 && req != NULL &&
	    (req->bmRequestType == INPUT_CTRL_BMREQUEST ||
	     req->bmRequestType == INPUT_CTRL_BMREQUEST_ALT) &&
	    ((req->bRequest >= INPUT_CTRL_BREQUEST &&
	      req->bRequest < INPUT_CTRL_BREQUEST + INPUT_CTRL_CHUNKS) ||
	     (req->bRequest >= INPUT_TOUCH_BREQUEST &&
	      req->bRequest < INPUT_TOUCH_BREQUEST + INPUT_TOUCH_CHUNKS))) {
		unsigned int idx;

		if (req->bRequest >= INPUT_TOUCH_BREQUEST) {
			/* Touch record: 4 chunks x 4 bytes. */
			pad_wire_touch t;

			idx = (unsigned int)(req->bRequest - INPUT_TOUCH_BREQUEST);
			if (idx == 0) {
				g_touch_mask = 0;
				memset(g_touch_buf, 0, sizeof(g_touch_buf));
			}
			g_touch_buf[idx * 4u + 0u] = (uint8_t)(req->wValue & 0xFF);
			g_touch_buf[idx * 4u + 1u] = (uint8_t)(req->wValue >> 8);
			g_touch_buf[idx * 4u + 2u] = (uint8_t)(req->wIndex & 0xFF);
			g_touch_buf[idx * 4u + 3u] = (uint8_t)(req->wIndex >> 8);
			g_touch_mask |= 1u << idx;
			if (g_touch_chunks_seen < 4000000000u)
				g_touch_chunks_seen++;

			if (g_touch_mask == ((1u << INPUT_TOUCH_CHUNKS) - 1u)) {
				g_touch_mask = 0;
				if (pad_wire_touch_decode(g_touch_buf,
							  PAD_WIRE_TOUCH_SIZE,
							  &t) == 0) {
					g_touch_records_ok++;
					touch_apply_record(&t);
				} else {
					g_touch_records_bad++;
				}
			}
			return 0;
		}

		/* Pad report: 7 chunks x 4 bytes. */
		idx = (unsigned int)(req->bRequest - INPUT_CTRL_BREQUEST);

		if (idx == 0) {
			/* Chunk 0 is the report boundary: drop any partial state
			 * left by a torn transfer so reports never mix. */
			g_chunk_mask = 0;
			memset(g_chunk_buf, 0, PAD_WIRE_SIZE);
		}
		g_chunk_buf[idx * 4u + 0u] = (uint8_t)(req->wValue & 0xFF);
		g_chunk_buf[idx * 4u + 1u] = (uint8_t)(req->wValue >> 8);
		g_chunk_buf[idx * 4u + 2u] = (uint8_t)(req->wIndex & 0xFF);
		g_chunk_buf[idx * 4u + 3u] = (uint8_t)(req->wIndex >> 8);
		g_chunk_mask |= 1u << idx;

		if (g_chunk_mask == ((1u << INPUT_CTRL_CHUNKS) - 1u)) {
			g_chunk_mask = 0;
			memcpy(g_mailbox, g_chunk_buf, PAD_WIRE_SIZE);
			g_mailbox_valid = 1;
			if (g_cnt_req < 12)
				g_cnt_req++;
			if (g_evflag_uid >= 0)
				ksceKernelSetEventFlag(g_evflag_uid, 1);
		}
		return 0;
	}

	if (g_orig_process_request != NULL)
		return g_orig_process_request(recipient, arg, req, user_data);
	return -1;
}

static int ksceUdcdRegister_hook(SceUdcdDriver *driver)
{
	int matched = is_uvc_gadget_driver(driver);

	ir_log("register: driver='%s' matched=%d already_patched=%d hi=%p full=%p\n",
	       (driver != NULL && driver->driverName != NULL) ? driver->driverName : "(null)",
	       matched, g_patched,
	       (driver != NULL) ? (void *)driver->configuration_hi : NULL,
	       (driver != NULL) ? (void *)driver->configuration : NULL);
	if (driver != NULL) {
		ir_describe_config("hi", driver->configuration_hi);
		ir_describe_config("full", driver->configuration);
	}

	if (driver != NULL && !g_patched && matched) {
		if (driver->endpoints != NULL && driver->numEndpoints >= 1) {
			/* Attach to the gadget's own EP0 endpoint struct (the same
			 * one vita-udcd-uvc queues its EP0 data stages on) and
			 * wrap the control-request handler. No descriptor changes:
			 * the stream gadget's configuration stays pristine. */
			g_input_ep = &driver->endpoints[0];
			g_orig_process_request = driver->processRequest;
			driver->processRequest = &input_process_request_hook;
			g_patched_driver = driver;
			g_patched = 1;
			ir_log("attached: processRequest wrapped, EP0=%p\n", (void *)g_input_ep);
		} else {
			ir_log("attach failed: driver has no endpoints array (n=%d)\n",
			       driver->numEndpoints);
		}
	}

	return TAI_CONTINUE(int, ksceUdcdRegister_ref, driver);
}

static SceUID ksceUdcdRegister_hook_uid = -1;

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	int ret;

	(void)argc;
	(void)args;

	ir_log_reset();
	ir_log("module_start v2.0.3 (descriptor-patch=%d)", INPUT_RECEIVER_DESCRIPTOR_PATCH);

	/* Touch injection hooks are independent of the USB gadget. */
	{
		int trc = touch_install_hooks();

		ir_log("touch hooks install rc=%d", trc);
	}

	/* Must be installed BEFORE the UVC gadget plugin registers its
	 * driver (taiHEN loads kplugins in config.txt order). */
	ksceUdcdRegister_hook_uid =
		taiHookFunctionExportForKernel(KERNEL_PID, &ksceUdcdRegister_ref,
					       "SceUdcd", TAI_ANY_LIBRARY,
					       0x4E55244D, /* ksceUdcdRegister */
					       ksceUdcdRegister_hook);
	ir_log("module_start: hook uid=%d\n", ksceUdcdRegister_hook_uid);
	if (ksceUdcdRegister_hook_uid < 0)
		return SCE_KERNEL_START_FAILED;

	ret = input_receiver_init(NULL); /* standalone: EP0 bound at registration */
	ir_log("module_start: init ret=%d\n", ret);
	if (ret < 0) {
		taiHookReleaseForKernel(ksceUdcdRegister_hook_uid,
					ksceUdcdRegister_ref);
		ksceUdcdRegister_hook_uid = -1;
		return SCE_KERNEL_START_FAILED;
	}

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	touch_release_hooks();
	(void)argc;
	(void)args;

	input_receiver_term();

	if (g_patched_driver != NULL) {
		g_patched_driver->processRequest = g_orig_process_request;
		g_patched_driver = NULL;
		g_orig_process_request = NULL;
	}

	if (ksceUdcdRegister_hook_uid >= 0) {
		taiHookReleaseForKernel(ksceUdcdRegister_hook_uid,
					ksceUdcdRegister_ref);
		ksceUdcdRegister_hook_uid = -1;
	}

	return SCE_KERNEL_STOP_SUCCESS;
}

#endif /* INPUT_RECEIVER_STANDALONE */
