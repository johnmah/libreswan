/*
 * low-level ip_address ugliness
 *
 * Copyright (C) 2000  Henry Spencer.
 * Copyright (C) 2018  Andrew Cagney.
 * Copyright (C) 2019 D. Hugh Redelmeier <hugh@mimosa.com>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/lgpl-2.1.txt>.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
 * License for more details.
 */

#include "jambuf.h"
#include "ip_address.h"
#include "lswlog.h"		/* for libreswan_log() */

/*
 * portof - get the port field of an ip_address in network order.
 *
 * Return -1 if ip_address isn't valid.
 */

int nportof(const ip_address * src)
{
	switch (src->u.v4.sin_family) {
	case AF_INET:
		return src->u.v4.sin_port;

	case AF_INET6:
		return src->u.v6.sin6_port;

	default:
		return -1;
	}
}

int hportof(const ip_address *src)
{
	int nport = nportof(src);
	if (nport >= 0) {
		return ntohs(nport);
	} else {
		return -1;
	}
}

/*
 * setportof - set the network ordered port field of an ip_address
 */

ip_address nsetportof(int port /* network order */, ip_address dst)
{
	switch (dst.u.v4.sin_family) {
	case AF_INET:
		dst.u.v4.sin_port = port;
		break;
	case AF_INET6:
		dst.u.v6.sin6_port = port;
		break;
	default:
		/* not asserting, who knows what nonsense a user can generate */
		libreswan_log("Will not set port on bogus address 0.0.0.0");
	}
	return dst;
}

ip_address hsetportof(int port /* host byte order */, ip_address dst)
{
	return nsetportof(htons(port), dst);
}

/*
 * sockaddrof - get a pointer to the sockaddr hiding inside an ip_address
 */
struct sockaddr *sockaddrof(const ip_address *src)
{
	switch (src->u.v4.sin_family) {
	case AF_INET:
		return (struct sockaddr *)&src->u.v4;

	case AF_INET6:
		return (struct sockaddr *)&src->u.v6;

	default:
		return NULL;	/* "can't happen" */
	}
}

/*
 * sockaddrlenof - get length of the sockaddr hiding inside an ip_address
 *
 * Return 0 on error.
 */
size_t sockaddrlenof(const ip_address * src)
{
	switch (src->u.v4.sin_family) {
	case AF_INET:
		return sizeof(src->u.v4);

	case AF_INET6:
		return sizeof(src->u.v6);

	default:
		return 0;
	}
}

/*
 * simplified interface to addrtot()
 *
 * Caller should allocate a buffer to hold the result as long
 * as the resulting string is needed.  Usually just long enough
 * to output.
 */

const char *ipstr(const ip_address *src, ipstr_buf *b)
{
	return str_address_cooked(src, b);
}

const char *sensitive_ipstr(const ip_address *src, ipstr_buf *b)
{
	return str_address_sensitive(src, b);
}

shunk_t address_as_shunk(const ip_address *address)
{
	if (address == NULL) {
		return null_shunk;
	}
	switch (address->u.v4.sin_family) {
	case AF_INET:
		return THING_AS_SHUNK(address->u.v4.sin_addr.s_addr); /* strip const */
	case AF_INET6:
		return THING_AS_SHUNK(address->u.v6.sin6_addr); /* strip const */
	default:
		return null_shunk;
	}
}

/*
 * Implement https://tools.ietf.org/html/rfc5952
 */

static void jam_raw_ipv4_address(jambuf_t *buf, shunk_t a, char sepc)
{
	const char seps[2] = { sepc == 0 ? '.' : sepc, 0, };
	const char *sep = "";
	for (size_t i = 0; i < a.len; i++) {
		uint8_t ia = a.ptr[i];
		jam(buf, "%s%"PRIu8, sep, ia);
		sep = seps;
	}
}

static void jam_raw_ipv6_address(jambuf_t *buf, shunk_t a, char sepc,
				 shunk_t skip)
{
	const char seps[2] = { sepc == 0 ? ':' : sepc, 0, };
	const void *ptr = a.ptr;
	const char *sep = "";
	const void *last = a.ptr + a.len - 2;
	while (ptr <= last) {
		if (ptr == skip.ptr) {
			/* skip zero run */
			jam(buf, "%s%s", seps, seps);
			sep = "";
			ptr += skip.len;
		} else {
			/*
			 * suppress leading zeros in two-byte
			 * big-endian hex value, need to cast away
			 * ptr's sign
			 */
			const uint8_t *p = (const uint8_t*)ptr;
			unsigned ia = (p[0] << 8) + p[1];
			jam(buf, "%s%x", sep, ia);
			sep = seps;
			ptr += 2;
		}
	}
}

void jam_address_raw(jambuf_t *buf, const ip_address *address, char sepc)
{
	if (address == NULL) {
		jam(buf, "<none>");
		return;
	}
	shunk_t a = address_as_shunk(address);
	int type = addrtypeof(address);
	switch (type) {
	case AF_INET: /* N.N.N.N */
		jam_raw_ipv4_address(buf, a, sepc);
		break;
	case AF_INET6: /* N:N:...:N */
		jam_raw_ipv6_address(buf, a, sepc, null_shunk);
		break;
	case AF_UNSPEC:
		jam(buf, "<unspecified>");
		break;
	default:
		bad_case(type);
	}
}

/*
 * Find longest run of zero pairs that should be suppressed (need at
 * least two).
 */
static shunk_t zeros_to_skip(shunk_t a)
{
	shunk_t zero = null_shunk;
	const char *ptr = a.ptr;
	const char *last = a.ptr + a.len - 2;
	while (ptr <= last) {
		unsigned l = 0;
		for (l = 0; ptr + l <= last; l += 2) {
			/* ptr is probably signed */
			if (ptr[l+0] != 0 || ptr[l+1] != 0) {
				break;
			}
		}
		if (l > 2 && l > zero.len) {
			zero.ptr = ptr;
			zero.len = l;
			ptr += l;
		} else {
			ptr += 2;
		}
	}
	return zero;
}

static void format_address_cooked(jambuf_t *buf, bool sensitive,
				  const ip_address *address)
{
	/*
	 * A NULL address can't be sensitive.
	 */
	if (address == NULL) {
		jam(buf, "<none>");
		return;
	}
	if (sensitive) {
		jam(buf, "<ip-address>");
		return;
	}
	shunk_t a = address_as_shunk(address);
	int type = addrtypeof(address);
	switch (type) {
	case AF_INET: /* N.N.N.N */
		jam_raw_ipv4_address(buf, a, 0);
		break;
	case AF_INET6: /* N:N:...:N */
		jam_raw_ipv6_address(buf, a, 0, zeros_to_skip(a));
		break;
	case AF_UNSPEC:
		jam(buf, "<unspecified>");
		break;
	default:
		jam(buf, "<invalid>");
		break;
	}
}

void jam_address_cooked(jambuf_t *buf, const ip_address *address)
{
	format_address_cooked(buf, false, address);
}

void jam_address_sensitive(jambuf_t *buf, const ip_address *address)
{
	format_address_cooked(buf, !log_ip, address);
}

void jam_address_reversed(jambuf_t *buf, const ip_address *address)
{
	shunk_t bytes = address_as_shunk(address);
	int type = addrtypeof(address);
	switch (type) {
	case AF_INET:
		for (int i = bytes.len - 1; i >= 0; i--) {
			uint8_t byte = bytes.ptr[i];
			jam(buf, "%d.", byte);
		}
		jam(buf, "IN-ADDR.ARPA.");
		break;
	case AF_INET6:
		for (int i = bytes.len - 1; i >= 0; i--) {
			uint8_t byte = bytes.ptr[i];
			jam(buf, "%x.%x.", byte & 0xf, byte >> 4);
		}
		jam(buf, "IP6.ARPA.");
		break;
	case AF_UNSPEC:
		jam(buf, "<unspecified>");
		break;
	default:
		jam(buf, "<invalid>");
		break;
	}
}

const char *str_address_raw(const ip_address *src, char sep,
			    address_buf *dst)
{
	jambuf_t buf = ARRAY_AS_JAMBUF(dst->buf);
	jam_address_raw(&buf, src, sep);
	return dst->buf;
}

const char *str_address_cooked(const ip_address *src,
			       address_buf *dst)
{
	jambuf_t buf = ARRAY_AS_JAMBUF(dst->buf);
	jam_address_cooked(&buf, src);
	return dst->buf;
}

const char *str_address_sensitive(const ip_address *src,
				  address_buf *dst)
{
	jambuf_t buf = ARRAY_AS_JAMBUF(dst->buf);
	jam_address_sensitive(&buf, src);
	return dst->buf;
}

const char *str_address_reversed(const ip_address *src,
				 address_reversed_buf *dst)
{
	jambuf_t buf = ARRAY_AS_JAMBUF(dst->buf);
	jam_address_reversed(&buf, src);
	return dst->buf;
}
