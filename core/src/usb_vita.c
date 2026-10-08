/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — USB host enumeration + PS Vita UVC/UAC detection.
 *
 * Adapted from the Ghostcontrol core's gc_usb.c ugen enumeration pattern
 * (reference only): scan /dev/ugenX.Y, read the device + full config
 * descriptors via USB_GET_* ioctls, and classify interfaces. We extend the
 * classifier to recognize the Vita's Video/Audio interfaces instead of HID
 * controllers.
 *
 * Runs as a root payload via elfldr: /dev/ugen* is fully accessible here
 * (the sandboxed native app cannot open these — that is the whole reason
 * USB work lives in the payload).
 */
#include "usb_vita.h"
#include "uvc_protocol.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_endian.h>

#define VD_LOG(...) do { fprintf(stderr, "[VITA5] " __VA_ARGS__); } while (0)
#ifndef VD_USB_ENUM_TRACE
#define VD_USB_ENUM_TRACE 0
#endif
#define VD_ENUM_LOG(...) do { if (VD_USB_ENUM_TRACE) VD_LOG(__VA_ARGS__); } while (0)

static int is_video_streaming_iface(uint8_t class_code, uint8_t subclass)
{
    return class_code == USB_CLASS_VIDEO && subclass == UVC_SC_VIDEOSTREAMING;
}

static int is_video_control_iface(uint8_t class_code, uint8_t subclass)
{
    return class_code == USB_CLASS_VIDEO && subclass == UVC_SC_VIDEOCONTROL;
}

static int is_audio_streaming_iface(uint8_t class_code, uint8_t subclass)
{
    /* UAC 1.0: AudioStreaming (0x01/0x02) or AudioControl (0x01/0x01).
     * VitaUSBStream exposes these; base udcd-uvc does not. */
    return class_code == USB_CLASS_AUDIO &&
           (subclass == 0x01 || subclass == 0x02);
}

static int is_pad_iface(uint8_t class_code, uint8_t subclass, uint8_t protocol)
{
    /* Vendor-specific pad interface published by vita-side/input-receiver:
     * class 0xFF, subclass 0x50 ('P'), protocol 0x01 (pad wire format v1). */
    return class_code == 0xFF && subclass == 0x50 && protocol == 0x01;
}

/* Collect bulk/interrupt endpoint addresses under an interface. */
static void collect_endpoints(const uint8_t *iface, unsigned iface_len,
                              uint8_t *in_eps, unsigned *in_count,
                              uint8_t *out_eps, unsigned *out_count)
{
    unsigned pos = iface[0]; /* skip the interface descriptor itself */
    while (pos + 2 <= iface_len) {
        const uint8_t *d = iface + pos;
        if (d[0] < 2 || pos + d[0] > iface_len) break;
        if (d[1] == USB_DT_INTERFACE) break; /* next interface */
        if (d[1] == USB_DT_ENDPOINT && d[0] >= 7) {
            uint8_t addr = d[2];
            uint8_t attr = d[3] & 0x03;
            if (addr & 0x80) {
                if (*in_count < VD_USB_MAX_ENDPOINTS) in_eps[(*in_count)++] = addr;
            } else {
                if (*out_count < VD_USB_MAX_ENDPOINTS) out_eps[(*out_count)++] = addr;
            }
            (void)attr;
        }
        pos += d[0];
    }
}

/* Find the isochronous IN endpoint under one interface (any alt setting).
 * Endpoint descriptor layout: bLength@0, bDescriptorType@1 (0x05),
 * bEndpointAddress@2, bmAttributes@3 (transfer type in the low 2 bits:
 * 0x01 = isochronous), wMaxPacketSize LE@4-5. Returns 1 when found.
 * collect_endpoints() only records addresses and only runs for alt 0, so the
 * audio isoc endpoint (alt 1 here) needs this dedicated scan. */
static int find_isoc_in_endpoint(const uint8_t *iface, unsigned iface_len,
                                 uint8_t *addr_out, uint16_t *maxpkt_out)
{
    unsigned pos = iface[0]; /* skip the interface descriptor itself */
    while (pos + 2 <= iface_len) {
        const uint8_t *d = iface + pos;
        if (d[0] < 2 || pos + d[0] > iface_len) break;
        if (d[1] == USB_DT_INTERFACE) break; /* next interface */
        if (d[1] == USB_DT_ENDPOINT && d[0] >= 7) {
            uint8_t addr = d[2];
            uint8_t attr = d[3] & 0x03;
            if ((addr & 0x80) && attr == 0x01) { /* IN + isochronous */
                *addr_out = addr;
                *maxpkt_out = (uint16_t)(d[4] | ((uint16_t)d[5] << 8));
                return 1;
            }
        }
        pos += d[0];
    }
    return 0;
}

/* Pull the class-specific VideoStreaming descriptors for one interface and
 * parse them. The VS descriptors (0x24) sit immediately after the interface
 * descriptor and before/around its endpoints. `iface_offset` is the offset of
 * the interface descriptor in `config`; `iface_len` is the full extent of this
 * interface (descriptor + all children). We walk from just after the interface
 * descriptor to the end of the extent, collecting all 0x24 blobs. */
static void parse_vs_descriptors(const uint8_t *config, unsigned config_len,
                                 unsigned iface_offset, unsigned iface_len,
                                 uint8_t interface_number,
                                 VdUsbVitaDevice *dev)
{
    /* Gather contiguous class-specific (0x24) descriptors following the
     * interface descriptor as one blob for the UVC parser. */
    uint8_t blob[4096];
    unsigned blob_len = 0;
    const unsigned extent_end = iface_offset + iface_len;
    /* Start right after the interface descriptor itself (its bLength). */
    unsigned pos = iface_offset + config[iface_offset];
    while (pos + 3 <= extent_end && pos + 3 <= config_len) {
        const uint8_t *d = config + pos;
        if (d[0] < 2 || pos + d[0] > config_len) break;
        if (d[1] == USB_DT_INTERFACE || d[1] == USB_DT_IAD) break;
        if (d[1] == USB_DT_CS_INTERFACE) {
            if (blob_len + d[0] > sizeof(blob)) break;
            memcpy(blob + blob_len, d, d[0]);
            blob_len += d[0];
        }
        pos += d[0];
    }
    if (blob_len == 0) return;

    VdUvcStreaming streaming;
    if (vd_uvc_parse_streaming(blob, blob_len, interface_number, &streaming) == 0) {
        if (dev->uvc.interface_number == 0xFF) {
            dev->uvc = streaming;
            dev->has_video = 1;
            VD_LOG("UVC VS iface %u: ep=0x%02x, %u format(s)\n",
                   interface_number, streaming.endpoint_address,
                   streaming.format_count);
            for (unsigned fi = 0; fi < streaming.format_count; ++fi) {
                const VdUvcFormat *f = &streaming.formats[fi];
                for (unsigned fr = 0; fr < f->frame_count; ++fr) {
                    VD_LOG("  fmt%u frame%u: %ux%u nv12=%d\n",
                           f->format_index, f->frames[fr].frame_index,
                           f->frames[fr].width, f->frames[fr].height,
                           f->is_nv12);
                }
            }
        }
    }
}

/* Inspect one ugen path. Returns 0 if it looks like a Vita UVC/UAC device. */
int vd_usb_inspect_vita(const char *path, VdUsbVitaDevice *dev)
{
    if (!path || !dev) return -1;
    memset(dev, 0, sizeof(*dev));
    dev->uvc.interface_number = 0xFF;
    dev->uac.interface_number = 0xFF;
    snprintf(dev->path, sizeof(dev->path), "%s", path);

    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        VD_ENUM_LOG("ENUM %s open failed errno=%d\n", path, errno);
        return -1;
    }

    struct usb_device_descriptor desc;
    memset(&desc, 0, sizeof(desc));
    if (ioctl(fd, USB_GET_DEVICE_DESC, &desc) != 0) {
        VD_ENUM_LOG("ENUM %s USB_GET_DEVICE_DESC failed errno=%d\n", path, errno);
        close(fd);
        return -1;
    }
    dev->vid = UGETW(desc.idVendor);
    dev->pid = UGETW(desc.idProduct);
    dev->device_class = desc.bDeviceClass;
    VD_ENUM_LOG("ENUM %s vid=%04x pid=%04x class=%02x\n", path, dev->vid, dev->pid,
                dev->device_class);

    int config = 0;
    if (ioctl(fd, USB_GET_CONFIG, &config) != 0 || config < 0 || config > 255) {
        VD_ENUM_LOG("ENUM %s USB_GET_CONFIG failed value=%d errno=%d\n", path, config, errno);
        close(fd); return -1;
    }
    VD_ENUM_LOG("ENUM %s config_index=%d\n", path, config);

    uint8_t data[8192];
    struct usb_gen_descriptor request;
    memset(&request, 0, sizeof(request));
    request.ugd_data = data;
    request.ugd_maxlen = sizeof(data);
    request.ugd_config_index = (uint8_t)config;
    if (ioctl(fd, USB_GET_FULL_DESC, &request) != 0 ||
        request.ugd_actlen > sizeof(data)) {
        VD_ENUM_LOG("ENUM %s USB_GET_FULL_DESC failed actlen=%u errno=%d\n", path,
                    request.ugd_actlen, errno);
        close(fd); return -1;
    }
    const unsigned total = request.ugd_actlen;
    VD_ENUM_LOG("ENUM %s full_descriptor_bytes=%u\n", path, total);

    /* Walk top-level descriptors; for each interface, gather its endpoints
     * and (for video) its class-specific VS descriptors. */
    unsigned pos = 0;
    while (pos + 2 <= total) {
        const uint8_t *d = data + pos;
        if (d[0] < 2 || pos + d[0] > total) break;

        if (d[1] == USB_DT_INTERFACE && d[0] >= 9) {
            uint8_t iface_number = d[2];
            uint8_t alt          = d[3];
            uint8_t class_code   = d[5];
            uint8_t subclass     = d[6];
            uint8_t protocol     = d[7];
            (void)protocol;

            /* Find this interface's extent (up to next interface/IAD). */
            unsigned extent = pos;
            while (extent + 2 <= total) {
                const uint8_t *e = data + extent;
                if (e[0] < 2 || extent + e[0] > total) break;
                if (extent != pos && (e[1] == USB_DT_INTERFACE || e[1] == USB_DT_IAD))
                    break;
                extent += e[0];
            }
            const unsigned iface_len = extent - pos;

            if (alt == 0 && is_video_streaming_iface(class_code, subclass)) {
                collect_endpoints(data + pos, iface_len,
                                  dev->video_in_eps, &dev->video_in_count,
                                  dev->video_out_eps, &dev->video_out_count);
                parse_vs_descriptors(data, total, pos, iface_len,
                                     iface_number, dev);
                if (dev->uvc.interface_number == 0xFF) dev->uvc.interface_number = iface_number;
            } else if (alt == 0 && is_video_control_iface(class_code, subclass)) {
                dev->video_control_interface = iface_number;
                dev->has_video_control = 1;
            } else if (is_audio_streaming_iface(class_code, subclass)) {
                /* Alt 0 behavior is unchanged (address lists + uac iface).
                 * The isoc IN endpoint lives on the ACTIVE alt (alt 1 on
                 * VitaUSBStream; alt 0 is zero-bandwidth), so scan every alt
                 * for it and record the endpoint address + wMaxPacketSize. */
                if (alt == 0) {
                    collect_endpoints(data + pos, iface_len,
                                      dev->audio_in_eps, &dev->audio_in_count,
                                      dev->audio_out_eps, &dev->audio_out_count);
                    dev->uac.interface_number = iface_number;
                    dev->has_audio = 1;
                }
                if (subclass == 0x02) /* AudioStreaming (0x01/0x02) */
                    dev->audio_stream_interface = iface_number;
                {
                    uint8_t isoc_ep = 0;
                    uint16_t isoc_maxpkt = 0;
                    if (find_isoc_in_endpoint(data + pos, iface_len,
                                              &isoc_ep, &isoc_maxpkt)) {
                        dev->audio_isoc_ep = isoc_ep;
                        dev->audio_isoc_maxpkt = isoc_maxpkt;
                        dev->audio_isoc_alt = alt;
                        dev->audio_stream_interface = iface_number;
                        VD_LOG("UAC AS iface %u alt %u: isoc IN ep=0x%02x maxpkt=%u\n",
                               iface_number, alt, isoc_ep, isoc_maxpkt);
                    }
                }
            } else if (alt == 0 && is_pad_iface(class_code, subclass, protocol)) {
                /* Vendor pad interface (vita-side/input-receiver). Number and
                 * endpoint address are both discovered here - the plugin
                 * appends this interface after the gadget's own ones, so they
                 * depend on the stream plugin build (VitaUSBStream: UVC + UAC,
                 * pad lands on the next free number). */
                uint8_t pad_in[VD_USB_MAX_ENDPOINTS];
                uint8_t pad_out[VD_USB_MAX_ENDPOINTS];
                unsigned pad_in_count = 0, pad_out_count = 0;

                collect_endpoints(data + pos, iface_len,
                                  pad_in, &pad_in_count,
                                  pad_out, &pad_out_count);
                if (pad_out_count > 0) {
                    dev->pad_interface = iface_number;
                    dev->pad_out_ep = pad_out[0];
                    dev->has_pad = 1;
                    VD_LOG("pad iface %u: bulk OUT ep=0x%02x\n",
                           iface_number, pad_out[0]);
                }
            }
            pos = extent;
            continue;
        }
        pos += d[0];
    }

    close(fd);
    VD_ENUM_LOG("ENUM %s parsed video=%d video_in=%u formats=%u audio=%d\n", path,
                dev->has_video, dev->video_in_count, dev->uvc.format_count, dev->has_audio);

    /* A Vita UVC device must expose at least one VideoStreaming interface with
     * a bulk IN endpoint and a parseable NV12 (or MJPEG) format. */
    if (dev->has_video && dev->video_in_count > 0 &&
        dev->uvc.interface_number != 0xFF) {
        dev->is_vita = 1;
        return 0;
    }
    return -1;
}

/* Scan all ugen nodes and fill the first `capacity` Vita devices found.
 * Returns the number of Vita UVC devices detected. */
size_t vd_usb_scan_vita(VdUsbVitaDevice *devices, size_t capacity)
{
    DIR *directory = opendir("/dev");
    if (!directory) {
        VD_ENUM_LOG("ENUM opendir(/dev) failed errno=%d\n", errno);
        return 0;
    }
    size_t count = 0;
    unsigned entries = 0;
    struct dirent *entry;
    while (count < capacity && (entry = readdir(directory))) {
        unsigned bus = 0, address = 0;
        char extra = 0;
        int matched = sscanf(entry->d_name, "ugen%u.%u%c", &bus, &address, &extra);
        if (entries++ < 8 || strncmp(entry->d_name, "ugen", 4) == 0)
            VD_ENUM_LOG("ENUM entry=%.40s matched=%d bus=%u addr=%u\n", entry->d_name,
                        matched, bus, address);
        if (matched != 2 || address <= 1)
            continue;
        char path[32];
        int length = snprintf(path, sizeof(path), "/dev/ugen%u.%u", bus, address);
        if (length < 0 || (size_t)length >= sizeof(path)) continue;
        if (vd_usb_inspect_vita(path, &devices[count]) == 0) {
            VD_LOG("Vita UVC device at %s (vid=%04x pid=%04x)\n",
                   path, devices[count].vid, devices[count].pid);
            ++count;
        }
    }
    closedir(directory);
    VD_ENUM_LOG("ENUM scan entries=%u detected=%u\n", entries, (unsigned)count);
    return count;
}
