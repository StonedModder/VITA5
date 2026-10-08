/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * input_receiver.h - public API of the VITA5 Vita-side USB pad input
 * receiver kernel plugin.
 *
 * Two build modes (see README.md):
 *
 *  - INPUT_RECEIVER_STANDALONE (default):
 *      Builds input_receiver.skprx, a self-contained taiHEN kernel plugin.
 *      It hooks ksceUdcdRegister() so that when the UVC gadget plugin
 *      (vita-udcd-uvc, driver name "VITAUVC00") registers its SceUdcdDriver,
 *      the driver's configuration descriptor tables are replaced with a
 *      superset that adds a third interface (vendor class 0xFF) exposing one
 *      bulk OUT endpoint (0x02). Video interfaces 0/1 keep their exact
 *      descriptors and endpoints.
 *
 *  - INPUT_RECEIVER_INTEGRATED:
 *      Built together with a patched vita-udcd-uvc tree (see
 *      integration/vita-udcd-uvc-input.patch). The UVC plugin itself owns the
 *      extended descriptor tables and calls input_receiver_init() with its
 *      new OUT endpoint. This is the recommended, fully SceUdcd-tracked
 *      integration.
 */

#ifndef VITA5_INPUT_RECEIVER_H
#define VITA5_INPUT_RECEIVER_H

#include <psp2kern/udcd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the USB receive pipeline and the input-injection thread.
 *
 * @param input_ep  the SceUdcdEndpoint for the pad OUT endpoint
 *                  (bEndpointAddress 0x02). Must have been filled in by
 *                  SceUdcd (integrated mode: &endpoints[2] of the UVC
 *                  gadget driver).
 * @return 0 on success, < 0 on error.
 */
int input_receiver_init(SceUdcdEndpoint *input_ep);

/** Stop the pipeline, cancel queued USB requests, release emulation. */
void input_receiver_term(void);

#ifdef __cplusplus
}
#endif

#endif /* VITA5_INPUT_RECEIVER_H */
