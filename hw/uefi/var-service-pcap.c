/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/pcap.h"
#include "system/dma.h"

#include "hw/uefi/var-service.h"

/* range reserved for private use */
#define LINKTYPE_USER0    147
#define LINKTYPE_USER15   162

#define SNAPLEN   (64 * 1024)
#define TYPE_RESET       0x01
#define TYPE_REQUEST     0x02
#define TYPE_REPLY       0x03

void var_service_pcap_init(FILE *fp)
{
    struct pcap_hdr header = {
        .magic_number  = PCAP_MAGIC,
        .version_major = PCAP_MAJOR,
        .version_minor = PCAP_MINOR,
        .snaplen       = SNAPLEN,
        .network       = LINKTYPE_USER0,
    };

    fwrite(&header, sizeof(header), 1, fp);
    fflush(fp);
}

static void var_service_pcap_packet(FILE *fp, uint32_t type, void *buffer, size_t size)
{
    struct pcaprec_hdr header;
    struct timeval tv;
    uint32_t orig_len = size + sizeof(uint32_t);
    uint32_t incl_len = MIN(orig_len, SNAPLEN);

    gettimeofday(&tv, NULL);
    header.ts_sec   = tv.tv_sec;
    header.ts_usec  = tv.tv_usec;
    header.incl_len = incl_len;
    header.orig_len = orig_len;

    fwrite(&header, sizeof(header), 1, fp);
    fwrite(&type, sizeof(type), 1, fp);
    if (buffer) {
        fwrite(buffer, incl_len - sizeof(type), 1, fp);
    }
    fflush(fp);
}

void var_service_pcap_reset(FILE *fp)
{
    var_service_pcap_packet(fp, TYPE_RESET, NULL, 0);
}

void var_service_pcap_request(FILE *fp, void *buffer, size_t size)
{
    var_service_pcap_packet(fp, TYPE_REQUEST, buffer, size);
}

void var_service_pcap_reply(FILE *fp, void *buffer, size_t size)
{
    var_service_pcap_packet(fp, TYPE_REPLY, buffer, size);
}
