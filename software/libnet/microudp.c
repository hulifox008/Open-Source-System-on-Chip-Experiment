/*
 * Derived from Milkymist SoC (Software)
 * Copyright (C) 2012 William Heatley
 * Copyright (C) 2007, 2008, 2009, 2010, 2011 Sebastien Bourdeauducq
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <crc.h>
//#include <irq.h>
//#include <system.h>
#include <hw/minimac.h>
//#include <hw/sysctl.h>
//#include <hw/interrupts.h>

#include <net/microudp.h>

#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IP  0x0800

struct ethernet_header {
	unsigned char preamble[8];
	unsigned char destmac[6];
	unsigned char srcmac[6];
	unsigned short ethertype;
} __attribute__((packed));

static void fill_eth_header(struct ethernet_header *h, const unsigned char *destmac, const unsigned char *srcmac, unsigned short ethertype)
{
	int i;

	for(i=0;i<7;i++)
		h->preamble[i] = 0x55;
	h->preamble[7] = 0xd5;
	for(i=0;i<6;i++)
		h->destmac[i] = destmac[i];
	for(i=0;i<6;i++)
		h->srcmac[i] = srcmac[i];
	h->ethertype = ethertype;
}

#define ARP_HWTYPE_ETHERNET 0x0001
#define ARP_PROTO_IP        0x0800

#define ARP_OPCODE_REQUEST  0x0001
#define ARP_OPCODE_REPLY    0x0002

struct arp_frame {
	unsigned short hwtype;
	unsigned short proto;
	unsigned char hwsize;
	unsigned char protosize;
	unsigned short opcode;
	unsigned char sender_mac[6];
	unsigned int sender_ip;
	unsigned char target_mac[6];
	unsigned int target_ip;
	unsigned char padding[18];
} __attribute__((packed));

#define IP_IPV4			0x45
#define IP_DONT_FRAGMENT	0x4000
#define IP_TTL			64
#define IP_PROTO_UDP		0x11
#define IP_PROTO_TCP		0x06

struct ip_header {
	unsigned char version;
	unsigned char diff_services;
	unsigned short total_length;
	unsigned short identification;
	unsigned short fragment_offset;
	unsigned char ttl;
	unsigned char proto;
	unsigned short checksum;
	unsigned int src_ip;
	unsigned int dst_ip;
} __attribute__((packed));

struct udp_header {
	unsigned short src_port;
	unsigned short dst_port;
	unsigned short length;
	unsigned short checksum;
} __attribute__((packed));

struct tcp_header {
	unsigned short src_port;
	unsigned short dst_port;
	unsigned int seqno;
	unsigned int ackno;
	unsigned char tcpoffset;
	unsigned char flags;
	unsigned short wnd;
	unsigned short tcpchksum;
	unsigned short urgp;
	unsigned int optdata;
} __attribute__((packed));

struct udp_frame {
	struct udp_header header;
	char payload[];
} __attribute__((packed));

struct tcp_frame {
	struct tcp_header header;
	char payload[];
} __attribute__((packed));

struct ip_frame {
	struct ip_header header;
	union {
		struct tcp_frame tcp;
		struct udp_frame udp;
	} contents;
} __attribute__((packed));

struct ethernet_frame {
	struct ethernet_header eth_header;
	union {
		struct arp_frame arp;
		struct ip_frame ip;
	} contents;
} __attribute__((packed));

typedef union {
	struct ethernet_frame frame;
	unsigned char raw[1532];
} ethernet_buffer;


static int rxlen;
static ethernet_buffer *rxbuffer;
static ethernet_buffer *rxbuffer0;
static ethernet_buffer *rxbuffer1;
static int txlen;
static ethernet_buffer *txbuffer;

static void send_packet(void)
{
	unsigned int crc;
	
	crc = crc32(&txbuffer->raw[8], txlen-8);
	txbuffer->raw[txlen  ] = (crc & 0xff);
	txbuffer->raw[txlen+1] = (crc & 0xff00) >> 8;
	txbuffer->raw[txlen+2] = (crc & 0xff0000) >> 16;
	txbuffer->raw[txlen+3] = (crc & 0xff000000) >> 24;
	txlen += 4;
	CSR_MINIMAC_TXCOUNT = txlen;
	//while((irq_pending() & IRQ_ETHTX) == 0);
	//irq_ack(IRQ_ETHTX);
	// TODO: Use interrupts
	while (CSR_MINIMAC_TXCOUNT != 0);	// Set to 0 once packet has been sent
}

static unsigned char my_mac[6];
static unsigned int my_ip;

/* ARP cache - one entry only */
static unsigned char cached_mac[6];
static unsigned int cached_ip;

static void process_arp(void)
{
	printf ("RECEIVED ARP PACKET\n");

	if(rxlen < 68) return;
	if(rxbuffer->frame.contents.arp.hwtype != ARP_HWTYPE_ETHERNET) return;
	if(rxbuffer->frame.contents.arp.proto != ARP_PROTO_IP) return;
	if(rxbuffer->frame.contents.arp.hwsize != 6) return;
	if(rxbuffer->frame.contents.arp.protosize != 4) return;
	if(rxbuffer->frame.contents.arp.opcode == ARP_OPCODE_REPLY) {
		if(rxbuffer->frame.contents.arp.sender_ip == cached_ip) {
			int i;
			for(i=0;i<6;i++)
				cached_mac[i] = rxbuffer->frame.contents.arp.sender_mac[i];
		}
		return;
	}
	if(rxbuffer->frame.contents.arp.opcode == ARP_OPCODE_REQUEST) {
		if(rxbuffer->frame.contents.arp.target_ip == my_ip) {
			int i;
			
			fill_eth_header(&txbuffer->frame.eth_header,
				rxbuffer->frame.contents.arp.sender_mac,
				my_mac,
				ETHERTYPE_ARP);
			txlen = 68;
			txbuffer->frame.contents.arp.hwtype = ARP_HWTYPE_ETHERNET;
			txbuffer->frame.contents.arp.proto = ARP_PROTO_IP;
			txbuffer->frame.contents.arp.hwsize = 6;
			txbuffer->frame.contents.arp.protosize = 4;
			txbuffer->frame.contents.arp.opcode = ARP_OPCODE_REPLY;
			txbuffer->frame.contents.arp.sender_ip = my_ip;
			for(i=0;i<6;i++)
				txbuffer->frame.contents.arp.sender_mac[i] = my_mac[i];
			txbuffer->frame.contents.arp.target_ip = rxbuffer->frame.contents.arp.sender_ip;
			for(i=0;i<6;i++)
				txbuffer->frame.contents.arp.target_mac[i] = rxbuffer->frame.contents.arp.sender_mac[i];
			send_packet();
		}
		return;
	}
}

static const unsigned char broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

int microudp_arp_resolve(unsigned int ip)
{
	int i;
	int tries;
	volatile int timeout;

	// Broadcast
	if (ip == 0xFFFFFFFF) {
		cached_ip = ip;
		for (i = 0; i < 6; ++i)
			cached_mac[i] = 0xFF;
		return 1;
	}

	if(cached_ip == ip) {
		for(i=0;i<6;i++)
			if(cached_mac[i]) return 1;
	}
	cached_ip = ip;
	for(i=0;i<6;i++)
		cached_mac[i] = 0;

	for(tries=0;tries<5;tries++) {
		/* Send an ARP request */
		fill_eth_header(&txbuffer->frame.eth_header,
				broadcast,
				my_mac,
				ETHERTYPE_ARP);
		txlen = 68;
		txbuffer->frame.contents.arp.hwtype = ARP_HWTYPE_ETHERNET;
		txbuffer->frame.contents.arp.proto = ARP_PROTO_IP;
		txbuffer->frame.contents.arp.hwsize = 6;
		txbuffer->frame.contents.arp.protosize = 4;
		txbuffer->frame.contents.arp.opcode = ARP_OPCODE_REQUEST;
		txbuffer->frame.contents.arp.sender_ip = my_ip;
		for(i=0;i<6;i++)
			txbuffer->frame.contents.arp.sender_mac[i] = my_mac[i];
		txbuffer->frame.contents.arp.target_ip = ip;
		for(i=0;i<6;i++)
			txbuffer->frame.contents.arp.target_mac[i] = 0;
		send_packet();

		/* Do we get a reply ? */
		for(timeout=0;timeout<2000000;timeout++) {
			microudp_service();
			for(i=0;i<6;i++)
				if(cached_mac[i]) return 1;
		}
	}

	return 0;
}

static unsigned short ip_checksum(unsigned int r, void *buffer, unsigned int length, int complete)
{
	unsigned char *ptr;
	int i;

	ptr = (unsigned char *)buffer;
	length >>= 1;

	for(i=0;i<length;i++)
		r += ((unsigned int)(ptr[2*i]) << 8)|(unsigned int)(ptr[2*i+1]) ;

	/* Add overflows */
	while(r >> 16)
		r = (r & 0xffff) + (r >> 16);

	if(complete) {
		r = ~r;
		r &= 0xffff;
		if(r == 0) r = 0xffff;
	}
	return r;
}

void *microudp_get_tx_buffer(void)
{
	return txbuffer->frame.contents.udp.payload;
}

struct pseudo_header {
	unsigned int src_ip;
	unsigned int dst_ip;
	unsigned char zero;
	unsigned char proto;
	unsigned short length;
} __attribute__((packed));

int microudp_send(unsigned short src_port, unsigned short dst_port, unsigned int length)
{
	struct pseudo_header h;
	unsigned int r;
	
	if((cached_mac[0] == 0) && (cached_mac[1] == 0) && (cached_mac[2] == 0)
		&& (cached_mac[3] == 0) && (cached_mac[4] == 0) && (cached_mac[5] == 0))
		return 0;

	txlen = length + sizeof(struct ethernet_header) + sizeof(struct udp_frame) + 8;
	if(txlen < 72) txlen = 72;
	
	fill_eth_header(&txbuffer->frame.eth_header,
		cached_mac,
		my_mac,
		ETHERTYPE_IP);
	
	txbuffer->frame.contents.udp.ip.version = IP_IPV4;
	txbuffer->frame.contents.udp.ip.diff_services = 0;
	txbuffer->frame.contents.udp.ip.total_length = length + sizeof(struct udp_frame);
	txbuffer->frame.contents.udp.ip.identification = 0;
	txbuffer->frame.contents.udp.ip.fragment_offset = IP_DONT_FRAGMENT;
	txbuffer->frame.contents.udp.ip.ttl = IP_TTL;
	h.proto = txbuffer->frame.contents.udp.ip.proto = IP_PROTO_UDP;
	txbuffer->frame.contents.udp.ip.checksum = 0;
	h.src_ip = txbuffer->frame.contents.udp.ip.src_ip = my_ip;
	h.dst_ip = txbuffer->frame.contents.udp.ip.dst_ip = cached_ip;
	txbuffer->frame.contents.udp.ip.checksum = ip_checksum(0, &txbuffer->frame.contents.udp.ip,
		sizeof(struct ip_header), 1);

	txbuffer->frame.contents.udp.udp.src_port = src_port;
	txbuffer->frame.contents.udp.udp.dst_port = dst_port;
	h.length = txbuffer->frame.contents.udp.udp.length = length + sizeof(struct udp_header);
	txbuffer->frame.contents.udp.udp.checksum = 0;

	h.zero = 0;
	r = ip_checksum(0, &h, sizeof(struct pseudo_header), 0);
	if(length & 1) {
		txbuffer->frame.contents.udp.payload[length] = 0;
		length++;
	}
	r = ip_checksum(r, &txbuffer->frame.contents.udp.udp,
		sizeof(struct udp_header)+length, 1);
	txbuffer->frame.contents.udp.udp.checksum = r;
	
	send_packet();

	return 1;
}

static udp_callback rx_callback;
static tcp_callback rx_tcp_callback;

static void process_udp (unsigned int src_ip, struct udp_frame *udp, int len)
{
	if (len < sizeof (struct udp_header)) return;
	if (udp->header.length > (len - sizeof (struct udp_header))) return;

	if(rx_callback)
		rx_callback (src_ip, udp->header.src_port, udp->header.dst_port, udp->payload, udp->header.length - sizeof (struct udp_header));
}

static void process_tcp (unsigned int src_ip, struct tcp_frame *tcp, int len)
{
	if (len < sizeof (struct tcp_header)) return;

	unsigned int header_len = (tcp->header.tcpoffset & 0xF) << 2;

	if (header_len > len) return;

	unsigned char *payload = (unsigned char *)tcp;
	payload += header_len;
	
	if (rx_tcp_callback)
		rx_tcp_callback (src_ip, tcp->header.src_port, tcp->header.dst_port, payload, len - header_len);
}

static void process_ip(struct ip_frame *ip, int len)
{
	printf ("RECEIVED IP PACKET\n");

	if (len < sizeof (struct ip_header)) return;
	if (ip->header.total_length > len) return;
	/* We don't verify UDP and IP checksums and rely on the Ethernet checksum solely */
	if(ip->header.version != IP_IPV4) return;
	// check disabled for QEMU compatibility
	//if(rxbuffer->frame.contents.udp.ip.diff_services != 0) return;
	if(ip->header.dst_ip != 0xFFFFFFFF && ip->header.dst_ip != my_ip) return;

	if (ip->header.proto == IP_PROTO_UDP)
		process_udp (ip->header.src_ip, &(ip->udp), ip->header.total_length - sizeof (struct ip_header));
	else if (ip->header.proto == IP_PROTO_TCP)
		process_tcp (&(ip->tcp), ip->header.total_length - sizeof (struct ip_header));
}

void microudp_set_callback(udp_callback callback)
{
	rx_callback = callback;
}

void microudp_set_tcp_callback (tcp_callback callback)
{
	rx_tcp_callback = callback;
}

unsigned char *microudp_get_mac (void)
{
	return my_mac;
}

void microudp_set_ip (unsigned int ip)
{
	my_ip = ip;
}

static void process_frame(void)
{
	int i;
	unsigned int received_crc;
	unsigned int computed_crc;

	printf ("Packet received. Checking...\n");

	if (rxlen < 13) return;

	//flush_cpu_dcache();
	for(i=0;i<7;i++)
		if(rxbuffer->frame.eth_header.preamble[i] != 0x55) {
			printf ("Bad preamble\n");
			return;
		}
	if(rxbuffer->frame.eth_header.preamble[7] != 0xd5) { printf("Bad preamble d5\n"); return;}
	received_crc = ((unsigned int)rxbuffer->raw[rxlen-1] << 24)
		|((unsigned int)rxbuffer->raw[rxlen-2] << 16)
		|((unsigned int)rxbuffer->raw[rxlen-3] <<  8)
		|((unsigned int)rxbuffer->raw[rxlen-4]);
	computed_crc = crc32(&rxbuffer->raw[8], rxlen-12);
	if(received_crc != computed_crc) {printf ("Bad CRC\n"); return; }

	rxlen -= 4; /* strip CRC here to be consistent with TX */
	if(rxbuffer->frame.eth_header.ethertype == ETHERTYPE_ARP) process_arp();
	else if(rxbuffer->frame.eth_header.ethertype == ETHERTYPE_IP) process_ip();

	printf ("Packet processed.\n");
}

void microudp_start(const unsigned char *macaddr, unsigned int ip)
{
	int i;

	//irq_ack(IRQ_ETHRX|IRQ_ETHTX);

	rxbuffer0 = (ethernet_buffer *)MINIMAC_RX0_BASE;
	rxbuffer1 = (ethernet_buffer *)MINIMAC_RX1_BASE;
	txbuffer = (ethernet_buffer *)MINIMAC_TX_BASE;

	for(i=0;i<6;i++)
		my_mac[i] = macaddr[i];
	my_ip = ip;

	cached_ip = 0;
	for(i=0;i<6;i++)
		cached_mac[i] = 0;

	rx_callback = (udp_callback)0;

	CSR_MINIMAC_STATE0 = MINIMAC_STATE_LOADED;
	CSR_MINIMAC_STATE1 = MINIMAC_STATE_LOADED;
	CSR_MINIMAC_SETUP = 0;
}

void microudp_service(void)
{
	//if(irq_pending() & IRQ_ETHRX) {
		if(CSR_MINIMAC_STATE0 == MINIMAC_STATE_PENDING) {
			rxlen = CSR_MINIMAC_COUNT0;
			rxbuffer = rxbuffer0;
			process_frame();
			CSR_MINIMAC_STATE0 = MINIMAC_STATE_LOADED;
		}
		if(CSR_MINIMAC_STATE1 == MINIMAC_STATE_PENDING) {
			rxlen = CSR_MINIMAC_COUNT1;
			rxbuffer = rxbuffer1;
			process_frame();
			CSR_MINIMAC_STATE1 = MINIMAC_STATE_LOADED;
		}
		//irq_ack(IRQ_ETHRX);
	//}
}

