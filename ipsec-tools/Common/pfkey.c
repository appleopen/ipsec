/*	$KAME: pfkey.c,v 1.47 2003/10/02 19:52:12 itojun Exp $	*/

/*
 * Copyright (C) 1995, 1996, 1997, 1998, and 1999 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <net/pfkeyv2.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET6_IPSEC
#  include <netinet6/ipsec.h>
#else
#  include <netinet/ipsec.h>
#endif

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>

#include "var.h"
#include "ipsec_strerror.h"
#include "libpfkey.h"

#define CALLOC(size, cast) (cast)calloc(1, (size))

static int findsupportedmap (int);
static int setsupportedmap (struct sadb_supported *);
static struct sadb_alg *findsupportedalg (u_int, u_int);
static int pfkey_send_x1 (int, u_int, u_int, u_int, struct sockaddr_storage *,
	struct sockaddr_storage *, u_int32_t, u_int32_t, u_int, caddr_t,
	u_int, u_int, u_int, u_int, u_int, u_int32_t, u_int32_t,
	u_int32_t, u_int32_t, u_int32_t, u_int16_t, u_int);
static int pfkey_send_x2 (int, u_int, u_int, u_int,
	struct sockaddr_storage *, struct sockaddr_storage *, u_int32_t);
static int pfkey_send_x3 (int, u_int, u_int);
static int pfkey_send_x4 (int, u_int, struct sockaddr_storage *, struct sockaddr_storage *, u_int,
	struct sockaddr_storage *, struct sockaddr_storage *, u_int, u_int, u_int64_t, u_int64_t,
	char *, int, u_int32_t, char *, char *, char *, u_int);
static int pfkey_send_x5 (int, u_int, u_int32_t);

static caddr_t pfkey_setsadbmsg (caddr_t, caddr_t, u_int, u_int,
	u_int, u_int32_t, pid_t);
static caddr_t pfkey_setsadbsa (caddr_t, caddr_t, u_int32_t, u_int,
	u_int, u_int, u_int32_t, u_int16_t);
static caddr_t pfkey_setsadbaddr (caddr_t, caddr_t, u_int,
	struct sockaddr_storage *, u_int, u_int);
static caddr_t pfkey_setsadbkey (caddr_t, caddr_t, u_int, caddr_t, u_int);
static caddr_t pfkey_setsadblifetime (caddr_t, caddr_t, u_int, u_int32_t,
	u_int32_t, u_int32_t, u_int32_t);
static caddr_t pfkey_setsadbipsecif(caddr_t, caddr_t, char *,
                                    char *, char *, int);
static caddr_t pfkey_setsadbxsa2 (caddr_t, caddr_t, u_int32_t, u_int32_t, u_int);

#ifdef SADB_X_EXT_NAT_T_TYPE
static caddr_t pfkey_set_natt_type (caddr_t, caddr_t, u_int, u_int8_t);
static caddr_t pfkey_set_natt_port (caddr_t, caddr_t, u_int, u_int16_t);
#endif
#ifdef SADB_X_EXT_NAT_T_FRAG
static caddr_t pfkey_set_natt_frag (caddr_t, caddr_t, u_int, u_int16_t);
#endif

/*
 * make and search supported algorithm structure.
 */
static struct sadb_supported *ipsec_supported[] = { NULL, NULL, NULL, 
#ifdef SADB_X_SATYPE_TCPSIGNATURE
    NULL,
#endif
};

static int supported_map[] = {
	SADB_SATYPE_AH,
	SADB_SATYPE_ESP,
	SADB_X_SATYPE_IPCOMP,
#ifdef SADB_X_SATYPE_TCPSIGNATURE
	SADB_X_SATYPE_TCPSIGNATURE,
#endif
};

static int
findsupportedmap(int satype)
{
	int i;

	for (i = 0; i < sizeof(supported_map)/sizeof(supported_map[0]); i++)
		if (supported_map[i] == satype)
			return i;
	return -1;
}

static struct sadb_alg *
findsupportedalg(u_int satype, u_int alg_id)
{
	int algno;
	int tlen;
	caddr_t p;

	/* validity check */
	algno = findsupportedmap((int)satype);
	if (algno == -1) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return NULL;
	}
	if (ipsec_supported[algno] == NULL) {
		__ipsec_errcode = EIPSEC_DO_GET_SUPP_LIST;
		return NULL;
	}

	tlen = ipsec_supported[algno]->sadb_supported_len
		- sizeof(struct sadb_supported);
	p = (void *)(ipsec_supported[algno] + 1);
	while (tlen > 0) {
		if (tlen < sizeof(struct sadb_alg)) {
			/* invalid format */
			break;
		}
		if (((struct sadb_alg *)(void *)p)->sadb_alg_id == alg_id)
			return (void *)p;

		tlen -= sizeof(struct sadb_alg);
		p += sizeof(struct sadb_alg);
	}

	__ipsec_errcode = EIPSEC_NOT_SUPPORTED;
	return NULL;
}

static int
setsupportedmap(struct sadb_supported *sup)
{
	struct sadb_supported **ipsup;

	switch (sup->sadb_supported_exttype) {
	case SADB_EXT_SUPPORTED_AUTH:
		ipsup = &ipsec_supported[findsupportedmap(SADB_SATYPE_AH)];
		break;
	case SADB_EXT_SUPPORTED_ENCRYPT:
		ipsup = &ipsec_supported[findsupportedmap(SADB_SATYPE_ESP)];
		break;
	default:
		__ipsec_errcode = EIPSEC_INVAL_SATYPE;
		return -1;
	}

	if (*ipsup)
		free(*ipsup);

	*ipsup = malloc((size_t)sup->sadb_supported_len);
	if (!*ipsup) {
		__ipsec_set_strerror(strerror(errno));
		return -1;
	}
	memcpy(*ipsup, sup, (size_t)sup->sadb_supported_len);

	return 0;
}

/*
 * check key length against algorithm specified.
 * This function is called with SADB_EXT_SUPPORTED_{AUTH,ENCRYPT} as the
 * augument, and only calls to ipsec_check_keylen2();
 * keylen is the unit of bit.
 * OUT:
 *	-1: invalid.
 *	 0: valid.
 */
int
ipsec_check_keylen(u_int supported, u_int alg_id, u_int keylen)
{
	u_int satype;

	/* validity check */
	switch (supported) {
	case SADB_EXT_SUPPORTED_AUTH:
		satype = SADB_SATYPE_AH;
		break;
	case SADB_EXT_SUPPORTED_ENCRYPT:
		satype = SADB_SATYPE_ESP;
		break;
	default:
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}

	return ipsec_check_keylen2(satype, alg_id, keylen);
}

/*
 * check key length against algorithm specified.
 * satype is one of satype defined at pfkeyv2.h.
 * keylen is the unit of bit.
 * OUT:
 *	-1: invalid.
 *	 0: valid.
 */
int
ipsec_check_keylen2(u_int satype, u_int alg_id, u_int keylen)
{
	struct sadb_alg *alg;

	alg = findsupportedalg(satype, alg_id);
	if (!alg)
		return -1;

	if (keylen < alg->sadb_alg_minbits || keylen > alg->sadb_alg_maxbits) {
		fprintf(stderr, "%d %d %d\n", keylen, alg->sadb_alg_minbits,
			alg->sadb_alg_maxbits);
		__ipsec_errcode = EIPSEC_INVAL_KEYLEN;
		return -1;
	}

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return 0;
}

/*
 * get max/min key length against algorithm specified.
 * satype is one of satype defined at pfkeyv2.h.
 * keylen is the unit of bit.
 * OUT:
 *	-1: invalid.
 *	 0: valid.
 */
int
ipsec_get_keylen(u_int supported, u_int alg_id, struct sadb_alg *alg0)
{
	struct sadb_alg *alg;
	u_int satype;

	/* validity check */
	if (!alg0) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}

	switch (supported) {
	case SADB_EXT_SUPPORTED_AUTH:
		satype = SADB_SATYPE_AH;
		break;
	case SADB_EXT_SUPPORTED_ENCRYPT:
		satype = SADB_SATYPE_ESP;
		break;
	default:
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}

	alg = findsupportedalg(satype, alg_id);
	if (!alg)
		return -1;

	memcpy(alg0, alg, sizeof(*alg0));

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return 0;
}

/*
 * set the rate for SOFT lifetime against HARD one.
 * If rate is more than 100 or equal to zero, then set to 100.
 */
static u_int soft_lifetime_allocations_rate = PFKEY_SOFT_LIFETIME_RATE;
static u_int soft_lifetime_bytes_rate = PFKEY_SOFT_LIFETIME_RATE;
static u_int soft_lifetime_addtime_rate = PFKEY_SOFT_LIFETIME_RATE;
static u_int soft_lifetime_usetime_rate = PFKEY_SOFT_LIFETIME_RATE;

u_int
pfkey_set_softrate(u_int type, u_int rate)
{
	__ipsec_errcode = EIPSEC_NO_ERROR;

	if (rate > 100 || rate == 0)
		rate = 100;

	switch (type) {
	case SADB_X_LIFETIME_ALLOCATIONS:
		soft_lifetime_allocations_rate = rate;
		return 0;
	case SADB_X_LIFETIME_BYTES:
		soft_lifetime_bytes_rate = rate;
		return 0;
	case SADB_X_LIFETIME_ADDTIME:
		soft_lifetime_addtime_rate = rate;
		return 0;
	case SADB_X_LIFETIME_USETIME:
		soft_lifetime_usetime_rate = rate;
		return 0;
	}

	__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
	return 1;
}

/*
 * get current rate for SOFT lifetime against HARD one.
 * ATTENTION: ~0 is returned if invalid type was passed.
 */
u_int
pfkey_get_softrate(u_int type)
{
	switch (type) {
	case SADB_X_LIFETIME_ALLOCATIONS:
		return soft_lifetime_allocations_rate;
	case SADB_X_LIFETIME_BYTES:
		return soft_lifetime_bytes_rate;
	case SADB_X_LIFETIME_ADDTIME:
		return soft_lifetime_addtime_rate;
	case SADB_X_LIFETIME_USETIME:
		return soft_lifetime_usetime_rate;
	}

	return (u_int)~0;
}

/*
 * sending SADB_GETSPI message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_getspi(int so, u_int satype, u_int mode, struct sockaddr_storage *src, struct sockaddr_storage *dst, 
                  u_int32_t min, u_int32_t max, u_int32_t reqid, u_int use_addtime, u_int64_t l_addtime, u_int32_t seq, u_int always_expire)
{
	struct sadb_msg *newmsg;
	caddr_t ep;
	int len;
	int need_spirange = 0;
	caddr_t p;
	int plen;

	/* validity check */
	if (src == NULL || dst == NULL) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}
	if (src->ss_family != dst->ss_family) {
		__ipsec_errcode = EIPSEC_FAMILY_MISMATCH;
		return -1;
	}
	if (min > max || (min > 0 && min <= 255)) {
		__ipsec_errcode = EIPSEC_INVAL_SPI;
		return -1;
	}
	switch (src->ss_family) {
	case AF_INET:
		plen = sizeof(struct in_addr) << 3;
		break;
	case AF_INET6:
		plen = sizeof(struct in6_addr) << 3;
		break;
	default:
		__ipsec_errcode = EIPSEC_INVAL_FAMILY;
		return -1;
	}

	/* create new sadb_msg to send. */
	len = sizeof(struct sadb_msg)
		+ sizeof(struct sadb_x_sa2)
		+ sizeof(struct sadb_address)
		+ PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)src))
		+ sizeof(struct sadb_address)
		+ PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)dst))
		+ ((use_addtime) ? sizeof(struct sadb_lifetime) : 0);

	if (min > 255 && max < (u_int)~0) {
		need_spirange++;
		len += sizeof(struct sadb_spirange);
	}

	if ((newmsg = CALLOC((size_t)len, struct sadb_msg *)) == NULL) {
		__ipsec_set_strerror(strerror(errno));
		return -1;
	}
	ep = ((caddr_t)(void *)newmsg) + len;

	p = pfkey_setsadbmsg((void *)newmsg, ep, SADB_GETSPI,
	    (u_int)len, satype, seq, getpid());
	if (!p) {
		free(newmsg);
		return -1;
	}

	p = pfkey_setsadbxsa2(p, ep, mode, reqid, always_expire);
	if (!p) {
		free(newmsg);
		return -1;
	}

	/* set sadb_address for source */
	p = pfkey_setsadbaddr(p, ep, SADB_EXT_ADDRESS_SRC, src, (u_int)plen,
	    IPSEC_ULPROTO_ANY);
	if (!p) {
		free(newmsg);
		return -1;
	}

	/* set sadb_address for destination */
	p = pfkey_setsadbaddr(p, ep, SADB_EXT_ADDRESS_DST, dst, (u_int)plen,
	    IPSEC_ULPROTO_ANY);
	if (!p) {
		free(newmsg);
		return -1;
	}
	
	if (use_addtime) {
		/* set sadb_lifetime, only hard lifetime applicable for larval SAs */
		p = pfkey_setsadblifetime(p, ep, SADB_EXT_LIFETIME_HARD,
								  0, 0, l_addtime, 0);
		if (!p) {
			free(newmsg);
			return -1;
		}
	}

	/* proccessing spi range */
	if (need_spirange) {
		struct sadb_spirange spirange;

		if (p + sizeof(spirange) > ep) {
			free(newmsg);
			return -1;
		}

		memset(&spirange, 0, sizeof(spirange));
		spirange.sadb_spirange_len = PFKEY_UNIT64(sizeof(spirange));
		spirange.sadb_spirange_exttype = SADB_EXT_SPIRANGE;
		spirange.sadb_spirange_min = min;
		spirange.sadb_spirange_max = max;

		memcpy(p, &spirange, sizeof(spirange));

		p += sizeof(spirange);
	}
	if (p != ep) {
		free(newmsg);
		return -1;
	}

	/* send message */
	len = pfkey_send(so, newmsg, len);
	free(newmsg);

	if (len < 0)
		return -1;

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return len;
}


/*
 * sending SADB_UPDATE message to the kernel.
 * The length of key material is a_keylen + e_keylen.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_update(int so, u_int satype, u_int mode, struct sockaddr_storage *src, struct sockaddr_storage *dst, 
                  u_int32_t spi, u_int32_t reqid, u_int wsize, caddr_t keymat, u_int e_type, u_int e_keylen, 
                  u_int a_type, u_int a_keylen, u_int flags, u_int32_t l_alloc, u_int64_t l_bytes, 
                  u_int64_t l_addtime, u_int64_t l_usetime, u_int32_t seq, u_int16_t port, u_int always_expire)
{
	int len;
	if ((len = pfkey_send_x1(so, SADB_UPDATE, satype, mode, src, dst, spi,
			reqid, wsize,
			keymat, e_type, e_keylen, a_type, a_keylen, flags,
			l_alloc, (u_int)l_bytes, (u_int)l_addtime, 
			(u_int)l_usetime, seq, port, always_expire)) < 0)
		return -1;

	return len;
}


/*
 * sending SADB_ADD message to the kernel.
 * The length of key material is a_keylen + e_keylen.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_add(int so, u_int satype, u_int mode, struct sockaddr_storage *src, struct sockaddr_storage *dst, 
               u_int32_t spi, u_int32_t reqid, u_int wsize, caddr_t keymat, u_int e_type, u_int e_keylen, 
               u_int a_type, u_int a_keylen, u_int flags, u_int32_t l_alloc, u_int64_t l_bytes, 
               u_int64_t l_addtime, u_int64_t l_usetime, u_int32_t seq, u_int16_t port, u_int always_expire)
{
	int len;
	if ((len = pfkey_send_x1(so, SADB_ADD, satype, mode, src, dst, spi,
			reqid, wsize,
			keymat, e_type, e_keylen, a_type, a_keylen, flags,
			l_alloc, (u_int)l_bytes, (u_int)l_addtime, 
			(u_int)l_usetime, seq, port, always_expire)) < 0)
		return -1;

	return len;
}


/*
 * sending SADB_DELETE message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_delete(so, satype, mode, src, dst, spi)
	int so;
	u_int satype, mode;
	struct sockaddr_storage *src, *dst;
	u_int32_t spi;
{
	int len;
	if ((len = pfkey_send_x2(so, SADB_DELETE, satype, mode, src, dst, spi)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_DELETE without spi to the kernel.  This is
 * the "delete all" request (an extension also present in
 * Solaris).
 *
 * OUT:
 *	positive: success and return length sent
 *	-1	: error occured, and set errno
 */
/*ARGSUSED*/
int
pfkey_send_delete_all(int so, u_int satype, u_int mode, struct sockaddr_storage *src, struct sockaddr_storage *dst)
{
	struct sadb_msg *newmsg;
	int len;
	caddr_t p;
	int plen;
	caddr_t ep;

	/* validity check */
	if (src == NULL || dst == NULL) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}
	if (src->ss_family != dst->ss_family) {
		__ipsec_errcode = EIPSEC_FAMILY_MISMATCH;
		return -1;
	}
	switch (src->ss_family) {
	case AF_INET:
		plen = sizeof(struct in_addr) << 3;
		break;
	case AF_INET6:
		plen = sizeof(struct in6_addr) << 3;
		break;
	default:
		__ipsec_errcode = EIPSEC_INVAL_FAMILY;
		return -1;
	}

	/* create new sadb_msg to reply. */
	len = sizeof(struct sadb_msg)
		+ sizeof(struct sadb_address)
		+ PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)src))
		+ sizeof(struct sadb_address)
		+ PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)dst));

	if ((newmsg = CALLOC((size_t)len, struct sadb_msg *)) == NULL) {
		__ipsec_set_strerror(strerror(errno));
		return -1;
	}
	ep = ((caddr_t)(void *)newmsg) + len;

	p = pfkey_setsadbmsg((void *)newmsg, ep, SADB_DELETE, (u_int)len, 
	    satype, 0, getpid());
	if (!p) {
		free(newmsg);
		return -1;
	}
	p = pfkey_setsadbaddr(p, ep, SADB_EXT_ADDRESS_SRC, src, (u_int)plen,
	    IPSEC_ULPROTO_ANY);
	if (!p) {
		free(newmsg);
		return -1;
	}
	p = pfkey_setsadbaddr(p, ep, SADB_EXT_ADDRESS_DST, dst, (u_int)plen,
	    IPSEC_ULPROTO_ANY);
	if (!p || p != ep) {
		free(newmsg);
		return -1;
	}

	/* send message */
	len = pfkey_send(so, newmsg, len);
	free(newmsg);

	if (len < 0)
		return -1;

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return len;
}

/*
 * sending SADB_GET message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_get(int so, u_int satype, u_int mode, struct sockaddr_storage *src, struct sockaddr_storage *dst, u_int32_t spi)
{
	int len;
	if ((len = pfkey_send_x2(so, SADB_GET, satype, mode, src, dst, spi)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_REGISTER message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_register(int so, u_int satype)
{
	int len, algno;

	if (satype == PF_UNSPEC) {
		for (algno = 0;
		     algno < sizeof(supported_map)/sizeof(supported_map[0]);
		     algno++) {
			if (ipsec_supported[algno]) {
				free(ipsec_supported[algno]);
				ipsec_supported[algno] = NULL;
			}
		}
	} else {
		algno = findsupportedmap((int)satype);
		if (algno == -1) {
			__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
			return -1;
		}

		if (ipsec_supported[algno]) {
			free(ipsec_supported[algno]);
			ipsec_supported[algno] = NULL;
		}
	}

	if ((len = pfkey_send_x3(so, SADB_REGISTER, satype)) < 0)
		return -1;

	return len;
}

/*
 * receiving SADB_REGISTER message from the kernel, and copy buffer for
 * sadb_supported returned into ipsec_supported.
 * OUT:
 *	 0: success and return length sent.
 *	-1: error occured, and set errno.
 */
int
pfkey_recv_register(int so)
{
	pid_t pid = getpid();
	struct sadb_msg *newmsg;
	int error = -1;

	/* receive message */
	for (;;) {
		if ((newmsg = pfkey_recv(so)) == NULL)
			return -1;
		if (newmsg->sadb_msg_type == SADB_REGISTER &&
		    newmsg->sadb_msg_pid == pid)
			break;
		free(newmsg);
	}

	/* check and fix */
	newmsg->sadb_msg_len = PFKEY_UNUNIT64(newmsg->sadb_msg_len);

	error = pfkey_set_supported(newmsg, newmsg->sadb_msg_len);
	free(newmsg);

	if (error == 0)
		__ipsec_errcode = EIPSEC_NO_ERROR;

	return error;
}

/*
 * receiving SADB_REGISTER message from the kernel, and copy buffer for
 * sadb_supported returned into ipsec_supported.
 * NOTE: sadb_msg_len must be host order.
 * IN:
 *	tlen: msg length, it's to makeing sure.
 * OUT:
 *	 0: success and return length sent.
 *	-1: error occured, and set errno.
 */
int
pfkey_set_supported(struct sadb_msg *msg, int tlen)
{
	struct sadb_supported *sup;
	caddr_t p;
	caddr_t ep;

	/* validity */
	if (msg->sadb_msg_len != tlen) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}

	p = (void *)msg;
	ep = p + tlen;

	p += sizeof(struct sadb_msg);

	while (p < ep) {
		sup = (void *)p;
		if (ep < p + sizeof(*sup) ||
		    PFKEY_EXTLEN(sup) < sizeof(*sup) ||
		    ep < p + sup->sadb_supported_len) {
			/* invalid format */
			break;
		}

		switch (sup->sadb_supported_exttype) {
		case SADB_EXT_SUPPORTED_AUTH:
		case SADB_EXT_SUPPORTED_ENCRYPT:
			break;
		default:
			__ipsec_errcode = EIPSEC_INVAL_SATYPE;
			return -1;
		}

		/* fixed length */
		sup->sadb_supported_len = PFKEY_EXTLEN(sup);

		/* set supported map */
		if (setsupportedmap(sup) != 0)
			return -1;

		p += sup->sadb_supported_len;
	}

	if (p != ep) {
		__ipsec_errcode = EIPSEC_INVAL_SATYPE;
		return -1;
	}

	__ipsec_errcode = EIPSEC_NO_ERROR;

	return 0;
}

/*
 * sending SADB_FLUSH message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_flush(int so, u_int satype)
{
	int len;

	if ((len = pfkey_send_x3(so, SADB_FLUSH, satype)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_DUMP message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_dump(int so, u_int satype)
{
	int len;

	if ((len = pfkey_send_x3(so, SADB_DUMP, satype)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_X_PROMISC message to the kernel.
 * NOTE that this function handles promisc mode toggle only.
 * IN:
 *	flag:	set promisc off if zero, set promisc on if non-zero.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 *	0     : error occured, and set errno.
 *	others: a pointer to new allocated buffer in which supported
 *	        algorithms is.
 */
int
pfkey_send_promisc_toggle(int so, int flag)
{
	int len;

	if ((len = pfkey_send_x3(so, SADB_X_PROMISC, 
	    (u_int)(flag ? 1 : 0))) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_X_SPDADD message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spdadd(int so, struct sockaddr_storage *src, u_int prefs, struct sockaddr_storage *dst, 
                  u_int prefd, u_int proto, caddr_t policy, int policylen, u_int32_t seq)
{
	int len;

	if ((len = pfkey_send_x4(so, SADB_X_SPDADD,
				src, NULL, prefs, dst, NULL, prefd, proto,
				(u_int64_t)0, (u_int64_t)0,
				policy, policylen, seq, NULL, NULL, NULL, 0)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_X_SPDADD message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spdadd_with_interface(int so, struct sockaddr_storage *src, struct sockaddr_storage *src_end, u_int prefs, struct sockaddr_storage *dst,
                                 struct sockaddr_storage *dst_end, u_int prefd, u_int proto, caddr_t policy, int policylen, u_int32_t seq, char *ipsec_if,
                                 char *internal_if, char *outgoing_if, u_int disabled)
{
	int len;
    
	if ((len = pfkey_send_x4(so, SADB_X_SPDADD,
                             src, src_end, prefs, dst, dst_end, prefd, proto,
                             (u_int64_t)0, (u_int64_t)0,
                             policy, policylen, seq, ipsec_if, internal_if, outgoing_if, disabled)) < 0)
		return -1;
    
	return len;
}

/*
 * sending SADB_X_SPDADD message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spdadd2(int so, struct sockaddr_storage *src, u_int prefs, struct sockaddr_storage *dst, u_int prefd, u_int proto, u_int64_t ltime, u_int64_t vtime,
		caddr_t policy, int policylen, u_int32_t seq)
{
	int len;

	if ((len = pfkey_send_x4(so, SADB_X_SPDADD,
				src, NULL, prefs, dst, NULL, prefd, proto,
				ltime, vtime,
				policy, policylen, seq, NULL, NULL, NULL, 0)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_X_SPDUPDATE message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spdupdate(int so, struct sockaddr_storage *src, u_int prefs, struct sockaddr_storage *dst, 
                     u_int prefd, u_int proto, caddr_t policy, int policylen, u_int32_t seq)
{
	int len;

	if ((len = pfkey_send_x4(so, SADB_X_SPDUPDATE,
				src, NULL, prefs, dst, NULL, prefd, proto,
				(u_int64_t)0, (u_int64_t)0,
				policy, policylen, seq, NULL, NULL, NULL, 0)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_X_SPDUPDATE message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spdupdate2(int so, struct sockaddr_storage *src, u_int prefs, struct sockaddr_storage *dst, 
                      u_int prefd, u_int proto, u_int64_t ltime, u_int64_t vtime,
                      caddr_t policy, int policylen, u_int32_t seq)
{
	int len;

	if ((len = pfkey_send_x4(so, SADB_X_SPDUPDATE,
				src, NULL, prefs, dst, NULL, prefd, proto,
				ltime, vtime,
				policy, policylen, seq, NULL, NULL, NULL, 0)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_X_SPDDELETE message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spddelete(int so, struct sockaddr_storage *src, u_int prefs, struct sockaddr_storage *dst, 
                     u_int prefd, u_int proto, caddr_t policy, int policylen, u_int32_t seq)
{
	int len;

	if (policylen != sizeof(struct sadb_x_policy)) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}

	if ((len = pfkey_send_x4(so, SADB_X_SPDDELETE,
				src, NULL, prefs, dst, NULL, prefd, proto,
				(u_int64_t)0, (u_int64_t)0,
				policy, policylen, seq, NULL, NULL, NULL, 0)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_X_SPDDELETE message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spddelete2(int so, u_int32_t spid)
{
	int len;

	if ((len = pfkey_send_x5(so, SADB_X_SPDDELETE2, spid)) < 0)
		return -1;

	return len;
}

int
pfkey_send_spdenable(int so, u_int32_t spid)
{
	int len;
	
	if ((len = pfkey_send_x5(so, SADB_X_SPDENABLE, spid)) < 0)
		return -1;
	
	return len;
}

int
pfkey_send_spddisable(int so, u_int32_t spid)
{
	int len;
	
	if ((len = pfkey_send_x5(so, SADB_X_SPDDISABLE, spid)) < 0)
		return -1;
	
	return len;
}

/*
 * sending SADB_X_SPDGET message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spdget(int so, u_int32_t spid)
{
	int len;

	if ((len = pfkey_send_x5(so, SADB_X_SPDGET, spid)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_X_SPDSETIDX message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spdsetidx(int so, struct sockaddr_storage *src, u_int prefs, struct sockaddr_storage *dst, 
                     u_int prefd, u_int proto, caddr_t policy, int policylen, u_int32_t seq)
{
	int len;

	if (policylen != sizeof(struct sadb_x_policy)) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}

	if ((len = pfkey_send_x4(so, SADB_X_SPDSETIDX,
				src, NULL, prefs, dst, NULL, prefd, proto,
				(u_int64_t)0, (u_int64_t)0,
				policy, policylen, seq, NULL, NULL, NULL, 0)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_SPDFLUSH message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spdflush(int so)
{
	int len;

	if ((len = pfkey_send_x3(so, SADB_X_SPDFLUSH, SADB_SATYPE_UNSPEC)) < 0)
		return -1;

	return len;
}

/*
 * sending SADB_SPDDUMP message to the kernel.
 * OUT:
 *	positive: success and return length sent.
 *	-1	: error occured, and set errno.
 */
int
pfkey_send_spddump(int so)
{
	int len;

	if ((len = pfkey_send_x3(so, SADB_X_SPDDUMP, SADB_SATYPE_UNSPEC)) < 0)
		return -1;

	return len;
}


/* sending SADB_ADD or SADB_UPDATE message to the kernel */
static int
pfkey_send_x1(int so, u_int type, u_int satype, u_int mode, struct sockaddr_storage *src, 
              struct sockaddr_storage *dst, u_int32_t spi, u_int32_t reqid, u_int wsize,
              caddr_t keymat, u_int e_type, u_int e_keylen, u_int a_type, u_int a_keylen, u_int flags,
              u_int32_t l_alloc, u_int32_t l_bytes, u_int32_t l_addtime, u_int32_t l_usetime, u_int32_t seq, u_int16_t port,
              u_int always_expire)
{
	struct sadb_msg *newmsg;
	int len;
	caddr_t p;
	int plen;
	caddr_t ep;

	/* validity check */
	if (src == NULL || dst == NULL) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}
	if (src->ss_family != dst->ss_family) {
		__ipsec_errcode = EIPSEC_FAMILY_MISMATCH;
		return -1;
	}
	switch (src->ss_family) {
	case AF_INET:
		plen = sizeof(struct in_addr) << 3;
		break;
	case AF_INET6:
		plen = sizeof(struct in6_addr) << 3;
		break;
	default:
		__ipsec_errcode = EIPSEC_INVAL_FAMILY;
		return -1;
	}

	switch (satype) {
	case SADB_SATYPE_ESP:
		if (e_type == SADB_EALG_NONE) {
			__ipsec_errcode = EIPSEC_NO_ALGS;
			return -1;
		}
		break;
	case SADB_SATYPE_AH:
		if (e_type != SADB_EALG_NONE) {
			__ipsec_errcode = EIPSEC_INVAL_ALGS;
			return -1;
		}
		if (a_type == SADB_AALG_NONE) {
			__ipsec_errcode = EIPSEC_NO_ALGS;
			return -1;
		}
		break;
	case SADB_X_SATYPE_IPCOMP:
		if (e_type == SADB_X_CALG_NONE) {
			__ipsec_errcode = EIPSEC_INVAL_ALGS;
			return -1;
		}
		if (a_type != SADB_AALG_NONE) {
			__ipsec_errcode = EIPSEC_NO_ALGS;
			return -1;
		}
		break;
#ifdef SADB_X_AALG_TCP_MD5
	case SADB_X_SATYPE_TCPSIGNATURE:
		if (e_type != SADB_EALG_NONE) {
			__ipsec_errcode = EIPSEC_INVAL_ALGS;
			return -1;
		}
		if (a_type != SADB_X_AALG_TCP_MD5) {
			__ipsec_errcode = EIPSEC_INVAL_ALGS;
			return -1;
		}
		break;
#endif
	default:
		__ipsec_errcode = EIPSEC_INVAL_SATYPE;
		return -1;
	}

	/* create new sadb_msg to reply. */
	len = sizeof(struct sadb_msg)
		+ sizeof(struct sadb_sa_2)
		+ sizeof(struct sadb_x_sa2)
		+ sizeof(struct sadb_address)
		+ PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)src))
		+ sizeof(struct sadb_address)
		+ PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)dst))
		+ sizeof(struct sadb_lifetime)
		+ sizeof(struct sadb_lifetime);
		
	if (e_type != SADB_EALG_NONE && satype != SADB_X_SATYPE_IPCOMP)
		len += (sizeof(struct sadb_key) + PFKEY_ALIGN8(e_keylen));
	if (a_type != SADB_AALG_NONE)
		len += (sizeof(struct sadb_key) + PFKEY_ALIGN8(a_keylen));

	if ((newmsg = CALLOC((size_t)len, struct sadb_msg *)) == NULL) {
		__ipsec_set_strerror(strerror(errno));
		return -1;
	}
	ep = ((caddr_t)(void *)newmsg) + len;

	p = pfkey_setsadbmsg((void *)newmsg, ep, type, (u_int)len,
	                     satype, seq, getpid());
	if (!p) {
		free(newmsg);
		return -1;
	}
	p = pfkey_setsadbsa(p, ep, spi, wsize, a_type, e_type, flags, port);
	if (!p) {
		free(newmsg);
		return -1;
	}
	p = pfkey_setsadbxsa2(p, ep, mode, reqid, always_expire);
	if (!p) {
		free(newmsg);
		return -1;
	}
	p = pfkey_setsadbaddr(p, ep, SADB_EXT_ADDRESS_SRC, src, (u_int)plen,
	    IPSEC_ULPROTO_ANY);
	if (!p) {
		free(newmsg);
		return -1;
	}
	p = pfkey_setsadbaddr(p, ep, SADB_EXT_ADDRESS_DST, dst, (u_int)plen,
	    IPSEC_ULPROTO_ANY);
	if (!p) {
		free(newmsg);
		return -1;
	}

	if (e_type != SADB_EALG_NONE && satype != SADB_X_SATYPE_IPCOMP) {
		p = pfkey_setsadbkey(p, ep, SADB_EXT_KEY_ENCRYPT,
		                   keymat, e_keylen);
		if (!p) {
			free(newmsg);
			return -1;
		}
	}
	if (a_type != SADB_AALG_NONE) {
		p = pfkey_setsadbkey(p, ep, SADB_EXT_KEY_AUTH,
		                   keymat + e_keylen, a_keylen);
		if (!p) {
			free(newmsg);
			return -1;
		}
	}

	/* set sadb_lifetime for destination */
	p = pfkey_setsadblifetime(p, ep, SADB_EXT_LIFETIME_HARD,
			l_alloc, l_bytes, l_addtime, l_usetime);
	if (!p) {
		free(newmsg);
		return -1;
	}
	p = pfkey_setsadblifetime(p, ep, SADB_EXT_LIFETIME_SOFT,
			l_alloc, l_bytes, l_addtime, l_usetime);
	if (!p) {
		free(newmsg);
		return -1;
	}
	
	if (p != ep) {
		free(newmsg);
		return -1;
	}

	/* send message */
	len = pfkey_send(so, newmsg, len);
	free(newmsg);

	if (len < 0)
		return -1;

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return len;
}


/* sending SADB_DELETE or SADB_GET message to the kernel */
/*ARGSUSED*/
static int
pfkey_send_x2(int so, u_int type, u_int satype, u_int mode, struct sockaddr_storage *src, struct sockaddr_storage *dst, u_int32_t spi)
{
	struct sadb_msg *newmsg;
	int len;
	caddr_t p;
	int plen;
	caddr_t ep;

	/* validity check */
	if (src == NULL || dst == NULL) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}
	if (src->ss_family != dst->ss_family) {
		__ipsec_errcode = EIPSEC_FAMILY_MISMATCH;
		return -1;
	}
	switch (src->ss_family) {
	case AF_INET:
		plen = sizeof(struct in_addr) << 3;
		break;
	case AF_INET6:
		plen = sizeof(struct in6_addr) << 3;
		break;
	default:
		__ipsec_errcode = EIPSEC_INVAL_FAMILY;
		return -1;
	}

	/* create new sadb_msg to reply. */
	len = sizeof(struct sadb_msg)
		+ sizeof(struct sadb_sa_2)
		+ sizeof(struct sadb_address)
		+ PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)src))
		+ sizeof(struct sadb_address)
		+ PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)dst));

	if ((newmsg = CALLOC((size_t)len, struct sadb_msg *)) == NULL) {
		__ipsec_set_strerror(strerror(errno));
		return -1;
	}
	ep = ((caddr_t)(void *)newmsg) + len;

	p = pfkey_setsadbmsg((void *)newmsg, ep, type, (u_int)len, satype, 0,
	    getpid());
	if (!p) {
		free(newmsg);
		return -1;
	}
	p = pfkey_setsadbsa(p, ep, spi, 0, 0, 0, 0, 0);
	if (!p) {
		free(newmsg);
		return -1;
	}
	p = pfkey_setsadbaddr(p, ep, SADB_EXT_ADDRESS_SRC, src, (u_int)plen,
	    IPSEC_ULPROTO_ANY);
	if (!p) {
		free(newmsg);
		return -1;
	}
	p = pfkey_setsadbaddr(p, ep, SADB_EXT_ADDRESS_DST, dst, (u_int)plen,
	    IPSEC_ULPROTO_ANY);
	if (!p || p != ep) {
		free(newmsg);
		return -1;
	}

	/* send message */
	len = pfkey_send(so, newmsg, len);
	free(newmsg);

	if (len < 0)
		return -1;

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return len;
}

/*
 * sending SADB_REGISTER, SADB_FLUSH, SADB_DUMP or SADB_X_PROMISC message
 * to the kernel
 */
static int
pfkey_send_x3(int so, u_int type, u_int satype)
{
	struct sadb_msg *newmsg;
	int len;
	caddr_t p;
	caddr_t ep;

	/* validity check */
	switch (type) {
	case SADB_X_PROMISC:
		if (satype != 0 && satype != 1) {
			__ipsec_errcode = EIPSEC_INVAL_SATYPE;
			return -1;
		}
		break;
	default:
		switch (satype) {
		case SADB_SATYPE_UNSPEC:
		case SADB_SATYPE_AH:
		case SADB_SATYPE_ESP:
		case SADB_X_SATYPE_IPCOMP:
#ifdef SADB_X_SATYPE_TCPSIGNATURE
		case SADB_X_SATYPE_TCPSIGNATURE:
#endif
			break;
		default:
			__ipsec_errcode = EIPSEC_INVAL_SATYPE;
			return -1;
		}
	}

	/* create new sadb_msg to send. */
	len = sizeof(struct sadb_msg);

	if ((newmsg = CALLOC((size_t)len, struct sadb_msg *)) == NULL) {
		__ipsec_set_strerror(strerror(errno));
		return -1;
	}
	ep = ((caddr_t)(void *)newmsg) + len;

	p = pfkey_setsadbmsg((void *)newmsg, ep, type, (u_int)len, satype, 0,
	    getpid());
	if (!p || p != ep) {
		free(newmsg);
		return -1;
	}

	/* send message */
	len = pfkey_send(so, newmsg, len);
	free(newmsg);

	if (len < 0)
		return -1;

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return len;
}

/* sending SADB_X_SPDADD message to the kernel */
static int
pfkey_send_x4(int so, u_int type, struct sockaddr_storage *src, struct sockaddr_storage *src_end, u_int prefs, struct sockaddr_storage *dst, struct sockaddr_storage *dst_end,
              u_int prefd, u_int proto, u_int64_t ltime, u_int64_t vtime, char *policy, int policylen, u_int32_t seq, char *ipsec_if, char *internal_if, char *outgoing_if,
              u_int disabled)
{
	struct sadb_msg *newmsg;
	int len;
	caddr_t p;
	int plen;
	caddr_t ep;
    int include_ipsec_if_msg = 0;

	/* validity check */
	if (src == NULL || dst == NULL) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}
	if (src->ss_family != dst->ss_family) {
		__ipsec_errcode = EIPSEC_FAMILY_MISMATCH;
		return -1;
	}

	switch (src->ss_family) {
	case AF_INET:
		plen = sizeof(struct in_addr) << 3;
		break;
	case AF_INET6:
		plen = sizeof(struct in6_addr) << 3;
		break;
	default:
		__ipsec_errcode = EIPSEC_INVAL_FAMILY;
		return -1;
	}
	if (prefs > plen || prefd > plen) {
		__ipsec_errcode = EIPSEC_INVAL_PREFIXLEN;
		return -1;
	}
    
    if (ipsec_if || internal_if || outgoing_if || disabled) {
        include_ipsec_if_msg = 1;
    }

	/* create new sadb_msg to reply. */
	len = sizeof(struct sadb_msg)
        + sizeof(struct sadb_address)
        + PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)src))
        + ((src_end) ? sizeof(struct sadb_address) + PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)src_end)) : 0)
        + sizeof(struct sadb_address)
        + PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)src))
        + ((dst_end) ? sizeof(struct sadb_address) + PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)dst_end)) : 0)
        + ((include_ipsec_if_msg) ? sizeof(struct sadb_x_ipsecif) : 0)
        + sizeof(struct sadb_lifetime)
        + policylen;

	if ((newmsg = CALLOC((size_t)len, struct sadb_msg *)) == NULL) {
		__ipsec_set_strerror(strerror(errno));
		return -1;
	}
	ep = ((caddr_t)(void *)newmsg) + len;

	p = pfkey_setsadbmsg((void *)newmsg, ep, type, (u_int)len,
	    SADB_SATYPE_UNSPEC, seq, getpid());
	if (!p) {
		free(newmsg);
		return -1;
	}
	if (src_end) {
        p = pfkey_setsadbaddr(p, ep, SADB_X_EXT_ADDR_RANGE_SRC_START, src, prefs, proto);
        if (!p) {
            free(newmsg);
            return -1;
        }
        p = pfkey_setsadbaddr(p, ep, SADB_X_EXT_ADDR_RANGE_SRC_END, src_end, prefs, proto);
        if (!p) {
            free(newmsg);
            return -1;
        }
    } else {
        p = pfkey_setsadbaddr(p, ep, SADB_EXT_ADDRESS_SRC, src, prefs, proto);
        if (!p) {
            free(newmsg);
            return -1;
        }
    }
    if (dst_end) {
        p = pfkey_setsadbaddr(p, ep, SADB_X_EXT_ADDR_RANGE_DST_START, dst, prefd, proto);
        if (!p) {
            free(newmsg);
            return -1;
        }
        p = pfkey_setsadbaddr(p, ep, SADB_X_EXT_ADDR_RANGE_DST_END, dst_end, prefd, proto);
        if (!p) {
            free(newmsg);
            return -1;
        }
    } else {
        p = pfkey_setsadbaddr(p, ep, SADB_EXT_ADDRESS_DST, dst, prefd, proto);
        if (!p) {
            free(newmsg);
            return -1;
        }
    }
    if (include_ipsec_if_msg) {
        p = pfkey_setsadbipsecif(p, ep, internal_if, outgoing_if, ipsec_if, disabled);
        if (!p) {
            free(newmsg);
            return -1;
        }
    }
	p = pfkey_setsadblifetime(p, ep, SADB_EXT_LIFETIME_HARD,
			0, 0, (u_int)ltime, (u_int)vtime);
	if (!p || p + policylen != ep) {
		free(newmsg);
		return -1;
	}
	memcpy(p, policy, (size_t)policylen);

	/* send message */
	len = pfkey_send(so, newmsg, len);
	free(newmsg);

	if (len < 0)
		return -1;

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return len;
}

/* sending SADB_X_SPDGET or SADB_X_SPDDELETE message to the kernel */
static int
pfkey_send_x5(int so, u_int type, u_int32_t spid)
{
	struct sadb_msg *newmsg;
	struct sadb_x_policy xpl;
	int len;
	caddr_t p;
	caddr_t ep;

	/* create new sadb_msg to reply. */
	len = sizeof(struct sadb_msg)
		+ sizeof(xpl);

	if ((newmsg = CALLOC((size_t)len, struct sadb_msg *)) == NULL) {
		__ipsec_set_strerror(strerror(errno));
		return -1;
	}
	ep = ((caddr_t)(void *)newmsg) + len;

	p = pfkey_setsadbmsg((void *)newmsg, ep, type, (u_int)len,
	    SADB_SATYPE_UNSPEC, 0, getpid());
	if (!p) {
		free(newmsg);
		return -1;
	}

	if (p + sizeof(xpl) != ep) {
		free(newmsg);
		return -1;
	}
	memset(&xpl, 0, sizeof(xpl));
	xpl.sadb_x_policy_len = PFKEY_UNIT64(sizeof(xpl));
	xpl.sadb_x_policy_exttype = SADB_X_EXT_POLICY;
	xpl.sadb_x_policy_id = spid;
	memcpy(p, &xpl, sizeof(xpl));

	/* send message */
	len = pfkey_send(so, newmsg, len);
	free(newmsg);

	if (len < 0)
		return -1;

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return len;
}

/*
 * open a socket.
 * OUT:
 *	-1: fail.
 *	others : success and return value of socket.
 */
int
pfkey_open()
{
	int so;
	int bufsiz = 0;	/* Max allowed by default */
	const unsigned long newbufk = 2176;
	unsigned long oldmax;
	size_t	oldmaxsize = sizeof(oldmax);
	unsigned long newmax = newbufk * (1024 + 128);

	if ((so = socket(PF_KEY, SOCK_RAW, PF_KEY_V2)) < 0) {
		__ipsec_set_strerror(strerror(errno));
		return -1;
	}
    
	/*
	 * This is a temporary workaround for KAME PR 154.
	 * Don't really care even if it fails.
	 */
	if (sysctlbyname("kern.ipc.maxsockbuf", &oldmax, &oldmaxsize, &newmax, sizeof(newmax)) != 0)
		bufsiz = 233016;	/* Max allowed by default */
	else
		bufsiz = newbufk * 1024;
	
	setsockopt(so, SOL_SOCKET, SO_SNDBUF, &bufsiz, sizeof(bufsiz));
	setsockopt(so, SOL_SOCKET, SO_RCVBUF, &bufsiz, sizeof(bufsiz));
	
	if (bufsiz == newbufk * 1024)
		sysctlbyname("kern.ipc.maxsockbuf", NULL, NULL, &oldmax, oldmaxsize);

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return so;
}

/*
 * close a socket.
 * OUT:
 *	 0: success.
 *	-1: fail.
 */
void
pfkey_close_sock(int so)
{
	(void)close(so);

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return;
}

/*
 * receive sadb_msg data, and return pointer to new buffer allocated.
 * Must free this buffer later.
 * OUT:
 *	NULL	: error occured.
 *	others	: a pointer to sadb_msg structure.
 *
 * XXX should be rewritten to pass length explicitly
 */
struct sadb_msg *
pfkey_recv(int so)
{
	struct sadb_msg buf, *newmsg;
	ssize_t len;
	int reallen;

	while ((len = recv(so, (void *)&buf, sizeof(buf), MSG_PEEK)) < 0) {
		if (errno == EINTR)
			continue;
		__ipsec_set_strerror(strerror(errno));
		return NULL;
	}

	if (len < sizeof(buf)) {
		recv(so, (void *)&buf, sizeof(buf), 0);
		__ipsec_errcode = EIPSEC_MAX;
		return NULL;
	}

	/* read real message */
	reallen = PFKEY_UNUNIT64(buf.sadb_msg_len);
	if ((newmsg = CALLOC((size_t)reallen, struct sadb_msg *)) == 0) {
		__ipsec_set_strerror(strerror(errno));
		return NULL;
	}

	while ((len = recv(so, (void *)newmsg, (socklen_t)reallen, 0)) < 0) {
		if (errno == EINTR)
			continue;
		__ipsec_set_strerror(strerror(errno));
		free(newmsg);
		return NULL;
	}

	if (len != reallen) {
		__ipsec_errcode = EIPSEC_SYSTEM_ERROR;
		free(newmsg);
		return NULL;
	}

	/* don't trust what the kernel says, validate! */
	if (PFKEY_UNUNIT64(newmsg->sadb_msg_len) != len) {
		__ipsec_errcode = EIPSEC_SYSTEM_ERROR;
		free(newmsg);
		return NULL;
	}

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return newmsg;
}

/*
 * send message to a socket.
 * OUT:
 *	 others: success and return length sent.
 *	-1     : fail.
 */
int
pfkey_send(int so, struct sadb_msg *msg, int len)
{
	if ((len = send(so, (void *)msg, (socklen_t)len, 0)) < 0) {
		__ipsec_set_strerror(strerror(errno));
		return -1;
	}

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return len;
}

/*
 * %%% Utilities
 * NOTE: These functions are derived from netkey/key.c in KAME.
 */
/*
 * set the pointer to each header in this message buffer.
 * IN:	msg: pointer to message buffer.
 *	mhp: pointer to the buffer initialized like below:
 *		caddr_t mhp[SADB_EXT_MAX + 1];
 * OUT:	-1: invalid.
 *	 0: valid.
 *
 * XXX should be rewritten to obtain length explicitly
 */
int
pfkey_align(struct sadb_msg *msg, caddr_t *mhp)
{
	struct sadb_ext *ext;
	int i;
	caddr_t p;
	caddr_t ep;	/* XXX should be passed from upper layer */

	/* validity check */
	if (msg == NULL || mhp == NULL) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}

	/* initialize */
	for (i = 0; i < SADB_EXT_MAX + 1; i++)
		mhp[i] = NULL;

	mhp[0] = (void *)msg;

	/* initialize */
	p = (void *) msg;
	ep = p + PFKEY_UNUNIT64(msg->sadb_msg_len);

	/* skip base header */
	p += sizeof(struct sadb_msg);

	while (p < ep) {
		ext = (void *)p;
		if (ep < p + sizeof(*ext) || PFKEY_EXTLEN(ext) < sizeof(*ext) ||
		    ep < p + PFKEY_EXTLEN(ext)) {
			/* invalid format */
			break;
		}

		/* duplicate check */
		/* XXX Are there duplication either KEY_AUTH or KEY_ENCRYPT ?*/
		if (mhp[ext->sadb_ext_type] != NULL) {
			__ipsec_errcode = EIPSEC_INVAL_EXTTYPE;
			return -1;
		}

		/* set pointer */
		switch (ext->sadb_ext_type) {
		case SADB_EXT_SA:
		case SADB_EXT_LIFETIME_CURRENT:
		case SADB_EXT_LIFETIME_HARD:
		case SADB_EXT_LIFETIME_SOFT:
		case SADB_EXT_ADDRESS_SRC:
		case SADB_EXT_ADDRESS_DST:
		case SADB_EXT_ADDRESS_PROXY:
		case SADB_EXT_KEY_AUTH:
			/* XXX should to be check weak keys. */
		case SADB_EXT_KEY_ENCRYPT:
			/* XXX should to be check weak keys. */
		case SADB_EXT_IDENTITY_SRC:
		case SADB_EXT_IDENTITY_DST:
		case SADB_EXT_SENSITIVITY:
		case SADB_EXT_PROPOSAL:
		case SADB_EXT_SUPPORTED_AUTH:
		case SADB_EXT_SUPPORTED_ENCRYPT:
		case SADB_EXT_SPIRANGE:
		case SADB_X_EXT_POLICY:
		case SADB_X_EXT_SA2:
        case SADB_EXT_SESSION_ID:
        case SADB_EXT_SASTAT:
#ifdef SADB_X_EXT_NAT_T_TYPE
		case SADB_X_EXT_NAT_T_TYPE:
		case SADB_X_EXT_NAT_T_SPORT:
		case SADB_X_EXT_NAT_T_DPORT:
		case SADB_X_EXT_NAT_T_OA:
#endif
#ifdef SADB_X_EXT_TAG
		case SADB_X_EXT_TAG:
#endif
#ifdef SADB_X_EXT_PACKET
		case SADB_X_EXT_PACKET:
#endif
        case SADB_X_EXT_IPSECIF:
        case SADB_X_EXT_ADDR_RANGE_SRC_START:
        case SADB_X_EXT_ADDR_RANGE_SRC_END:
        case SADB_X_EXT_ADDR_RANGE_DST_START:
        case SADB_X_EXT_ADDR_RANGE_DST_END:
			mhp[ext->sadb_ext_type] = (void *)ext;
			break;
		default:
			__ipsec_errcode = EIPSEC_INVAL_EXTTYPE;
			return -1;
		}

		p += PFKEY_EXTLEN(ext);
	}

	if (p != ep) {
		__ipsec_errcode = EIPSEC_INVAL_SADBMSG;
		return -1;
	}

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return 0;
}

/*
 * check basic usage for sadb_msg,
 * NOTE: This routine is derived from netkey/key.c in KAME.
 * IN:	msg: pointer to message buffer.
 *	mhp: pointer to the buffer initialized like below:
 *
 *		caddr_t mhp[SADB_EXT_MAX + 1];
 *
 * OUT:	-1: invalid.
 *	 0: valid.
 */
int
pfkey_check(caddr_t * mhp)
{
	struct sadb_msg *msg;

	/* validity check */
	if (mhp == NULL || mhp[0] == NULL) {
		__ipsec_errcode = EIPSEC_INVAL_ARGUMENT;
		return -1;
	}

	msg = (void *)mhp[0];

	/* check version */
	if (msg->sadb_msg_version != PF_KEY_V2) {
		__ipsec_errcode = EIPSEC_INVAL_VERSION;
		return -1;
	}

	/* check type */
	if (msg->sadb_msg_type > SADB_MAX) {
		__ipsec_errcode = EIPSEC_INVAL_MSGTYPE;
		return -1;
	}

	/* check SA type */
	switch (msg->sadb_msg_satype) {
	case SADB_SATYPE_UNSPEC:
		switch (msg->sadb_msg_type) {
		case SADB_GETSPI:
		case SADB_UPDATE:
		case SADB_ADD:
		case SADB_DELETE:
		case SADB_GET:
		case SADB_ACQUIRE:
		case SADB_EXPIRE:
#ifdef SADB_X_NAT_T_NEW_MAPPING
		case SADB_X_NAT_T_NEW_MAPPING:
#endif
			__ipsec_errcode = EIPSEC_INVAL_SATYPE;
			return -1;
		}
		break;
	case SADB_SATYPE_ESP:
	case SADB_SATYPE_AH:
	case SADB_X_SATYPE_IPCOMP:
#ifdef SADB_X_SATYPE_TCPSIGNATURE
	case SADB_X_SATYPE_TCPSIGNATURE:
#endif
		switch (msg->sadb_msg_type) {
		case SADB_X_SPDADD:
		case SADB_X_SPDDELETE:
		case SADB_X_SPDGET:
		case SADB_X_SPDDUMP:
		case SADB_X_SPDFLUSH:
			__ipsec_errcode = EIPSEC_INVAL_SATYPE;
			return -1;
		}
#ifdef SADB_X_NAT_T_NEW_MAPPING
		if (msg->sadb_msg_type == SADB_X_NAT_T_NEW_MAPPING &&
		    msg->sadb_msg_satype != SADB_SATYPE_ESP) {
			__ipsec_errcode = EIPSEC_INVAL_SATYPE;
			return -1;
		}
#endif
		break;
	case SADB_SATYPE_RSVP:
	case SADB_SATYPE_OSPFV2:
	case SADB_SATYPE_RIPV2:
	case SADB_SATYPE_MIP:
		__ipsec_errcode = EIPSEC_NOT_SUPPORTED;
		return -1;
	case 1:	/* XXX: What does it do ? */
		if (msg->sadb_msg_type == SADB_X_PROMISC)
			break;
		/*FALLTHROUGH*/
	default:
		__ipsec_errcode = EIPSEC_INVAL_SATYPE;
		return -1;
	}

	/* check field of upper layer protocol and address family */
	if (mhp[SADB_EXT_ADDRESS_SRC] != NULL
	 && mhp[SADB_EXT_ADDRESS_DST] != NULL) {
		struct sadb_address *src0, *dst0;

		src0 = (void *)(mhp[SADB_EXT_ADDRESS_SRC]);
		dst0 = (void *)(mhp[SADB_EXT_ADDRESS_DST]);

		if (src0->sadb_address_proto != dst0->sadb_address_proto) {
			__ipsec_errcode = EIPSEC_PROTO_MISMATCH;
			return -1;
		}

		if (PFKEY_ADDR_SADDR(src0)->sa_family
		 != PFKEY_ADDR_SADDR(dst0)->sa_family) {
			__ipsec_errcode = EIPSEC_FAMILY_MISMATCH;
			return -1;
		}

		switch (PFKEY_ADDR_SADDR(src0)->sa_family) {
		case AF_INET:
		case AF_INET6:
			break;
		default:
			__ipsec_errcode = EIPSEC_INVAL_FAMILY;
			return -1;
		}

		/*
		 * prefixlen == 0 is valid because there must be the case
		 * all addresses are matched.
		 */
	}

	__ipsec_errcode = EIPSEC_NO_ERROR;
	return 0;
}

/*
 * set data into sadb_msg.
 * `buf' must has been allocated sufficiently.
 */
static caddr_t
pfkey_setsadbmsg(caddr_t buf, caddr_t lim, u_int type, u_int tlen, u_int satype, u_int32_t seq, pid_t pid)
{
	struct sadb_msg *p;
	u_int len;

	p = (void *)buf;
	len = sizeof(struct sadb_msg);

	if (buf + len > lim)
		return NULL;

	memset(p, 0, len);
	p->sadb_msg_version = PF_KEY_V2;
	p->sadb_msg_type = type;
	p->sadb_msg_errno = 0;
	p->sadb_msg_satype = satype;
	p->sadb_msg_len = PFKEY_UNIT64(tlen);
	p->sadb_msg_reserved = 0;
	p->sadb_msg_seq = seq;
	p->sadb_msg_pid = (u_int32_t)pid;

	return(buf + len);
}

/*
 * copy secasvar data into sadb_address.
 * `buf' must has been allocated sufficiently.
 */
static caddr_t
pfkey_setsadbsa(caddr_t buf, caddr_t lim, u_int32_t spi, u_int wsize, u_int auth, u_int enc, u_int32_t flags, u_int16_t port)
{
	struct sadb_sa_2 *p;
	u_int len;

	p = (void *)buf;
	len = sizeof(struct sadb_sa_2);

	if (buf + len > lim)
		return NULL;

	memset(p, 0, len);
	p->sa.sadb_sa_len = PFKEY_UNIT64(len);
	p->sa.sadb_sa_exttype = SADB_EXT_SA;
	p->sa.sadb_sa_spi = spi;
	p->sa.sadb_sa_replay = wsize;
	p->sa.sadb_sa_state = SADB_SASTATE_LARVAL;
	p->sa.sadb_sa_auth = auth;
	p->sa.sadb_sa_encrypt = enc;
	p->sa.sadb_sa_flags = flags;
	p->sadb_sa_natt_port = port;

	return(buf + len);
}

/*
 * set data into sadb_address.
 * `buf' must has been allocated sufficiently.
 * prefixlen is in bits.
 */
static caddr_t
pfkey_setsadbaddr(caddr_t buf, caddr_t lim, u_int exttype, struct sockaddr_storage *saddr, u_int prefixlen, u_int ul_proto)
{
	struct sadb_address *p;
	u_int len;

	p = (void *)buf;
	len = sizeof(struct sadb_address) + PFKEY_ALIGN8(sysdep_sa_len((struct sockaddr *)saddr));

	if (buf + len > lim)
		return NULL;

	memset(p, 0, len);
	p->sadb_address_len = PFKEY_UNIT64(len);
	p->sadb_address_exttype = exttype & 0xffff;
	p->sadb_address_proto = ul_proto & 0xff;
	p->sadb_address_prefixlen = prefixlen;
	p->sadb_address_reserved = 0;

	memcpy(p + 1, saddr, (size_t)sysdep_sa_len((struct sockaddr *)saddr));

	return(buf + len);
}

/*
 * set sadb_key structure after clearing buffer with zero.
 * OUT: the pointer of buf + len.
 */
static caddr_t
pfkey_setsadbkey(caddr_t buf, caddr_t lim, u_int type, caddr_t key, u_int keylen)
{
	struct sadb_key *p;
	u_int len;

	p = (void *)buf;
	len = sizeof(struct sadb_key) + PFKEY_ALIGN8(keylen);

	if (buf + len > lim)
		return NULL;

	memset(p, 0, len);
	p->sadb_key_len = PFKEY_UNIT64(len);
	p->sadb_key_exttype = type;
	p->sadb_key_bits = keylen << 3;
	p->sadb_key_reserved = 0;

	memcpy(p + 1, key, keylen);

	return buf + len;
}

/*
 * set sadb_lifetime structure after clearing buffer with zero.
 * OUT: the pointer of buf + len.
 */
static caddr_t
pfkey_setsadblifetime(caddr_t buf, caddr_t lim, u_int type, u_int32_t l_alloc, 
                      u_int32_t l_bytes, u_int32_t l_addtime, u_int32_t l_usetime)
{
	struct sadb_lifetime *p;
	u_int len;

	p = (void *)buf;
	len = sizeof(struct sadb_lifetime);

	if (buf + len > lim)
		return NULL;

	memset(p, 0, len);
	p->sadb_lifetime_len = PFKEY_UNIT64(len);
	p->sadb_lifetime_exttype = type;

	switch (type) {
	case SADB_EXT_LIFETIME_SOFT:
		p->sadb_lifetime_allocations
			= (l_alloc * soft_lifetime_allocations_rate) /100;
		p->sadb_lifetime_bytes
			= (l_bytes * soft_lifetime_bytes_rate) /100;
		p->sadb_lifetime_addtime
			= (l_addtime * soft_lifetime_addtime_rate) /100;
		p->sadb_lifetime_usetime
			= (l_usetime * soft_lifetime_usetime_rate) /100;
		break;
	case SADB_EXT_LIFETIME_HARD:
		p->sadb_lifetime_allocations = l_alloc;
		p->sadb_lifetime_bytes = l_bytes;
		p->sadb_lifetime_addtime = l_addtime;
		p->sadb_lifetime_usetime = l_usetime;
		break;
	}

	return buf + len;
}

static caddr_t
pfkey_setsadbipsecif(caddr_t buf, caddr_t lim, char *internal_if, char *outgoing_if, char *ipsec_if, int init_disabled)
{
	struct sadb_x_ipsecif *p;
	u_int len;
    
	p = (void *)buf;
	len = sizeof(struct sadb_x_ipsecif);
    
	if (buf + len > lim)
		return NULL;
    
	memset(p, 0, len);
    p->sadb_x_ipsecif_len = PFKEY_UNIT64(len);
	p->sadb_x_ipsecif_exttype = SADB_X_EXT_IPSECIF;
    
    if (internal_if != NULL)
        strncpy(p->sadb_x_ipsecif_internal_if, internal_if, sizeof(p->sadb_x_ipsecif_internal_if));
    if (outgoing_if != NULL)
        strncpy(p->sadb_x_ipsecif_outgoing_if, outgoing_if, sizeof(p->sadb_x_ipsecif_outgoing_if));
    if (ipsec_if != NULL)
        strncpy(p->sadb_x_ipsecif_ipsec_if, ipsec_if, sizeof(p->sadb_x_ipsecif_ipsec_if));
    
	p->sadb_x_ipsecif_init_disabled = init_disabled;
    
	return (buf + len);
}

/*
 * copy secasvar data into sadb_address.
 * `buf' must has been allocated sufficiently.
 */
static caddr_t
pfkey_setsadbxsa2(caddr_t buf, caddr_t lim, u_int32_t mode0, u_int32_t reqid, u_int always_expire)
{
	struct sadb_x_sa2 *p;
	u_int8_t mode = mode0 & 0xff;
	u_int len;

	p = (void *)buf;
	len = sizeof(struct sadb_x_sa2);

	if (buf + len > lim)
		return NULL;

	memset(p, 0, len);
	p->sadb_x_sa2_len = PFKEY_UNIT64(len);
	p->sadb_x_sa2_exttype = SADB_X_EXT_SA2;
	p->sadb_x_sa2_mode = mode;
	p->sadb_x_sa2_reqid = reqid;
	p->sadb_x_sa2_alwaysexpire = always_expire;
#ifdef SADB_X_EXT_SA2_DELETE_ON_DETACH
	p->sadb_x_sa2_flags |= SADB_X_EXT_SA2_DELETE_ON_DETACH;
#endif /* SADB_X_EXT_SA2_DELETE_ON_DETACH */

	return(buf + len);
}

#ifdef SADB_X_EXT_NAT_T_TYPE
static caddr_t
pfkey_set_natt_type(caddr_t buf, caddr_t lim, u_int type, u_int8_t l_natt_type)
{
	struct sadb_x_nat_t_type *p;
	u_int len;

	p = (void *)buf;
	len = sizeof(struct sadb_x_nat_t_type);

	if (buf + len > lim)
		return NULL;

	memset(p, 0, len);
	p->sadb_x_nat_t_type_len = PFKEY_UNIT64(len);
	p->sadb_x_nat_t_type_exttype = type;
	p->sadb_x_nat_t_type_type = l_natt_type;

	return(buf + len);
}

static caddr_t
pfkey_set_natt_port(caddr_t buf, caddr_t lim, u_int type, u_int16_t l_natt_port)
{
	struct sadb_x_nat_t_port *p;
	u_int len;

	p = (void *)buf;
	len = sizeof(struct sadb_x_nat_t_port);

	if (buf + len > lim)
		return NULL;

	memset(p, 0, len);
	p->sadb_x_nat_t_port_len = PFKEY_UNIT64(len);
	p->sadb_x_nat_t_port_exttype = type;
	p->sadb_x_nat_t_port_port = htons(l_natt_port);

	return(buf + len);
}
#endif

#ifdef SADB_X_EXT_NAT_T_FRAG
static caddr_t
pfkey_set_natt_frag(caddr_t buf, caddr_t lim, u_int type, u_int16_t l_natt_frag)
{
	struct sadb_x_nat_t_frag *p;
	u_int len;

	p = (void *)buf;
	len = sizeof(struct sadb_x_nat_t_frag);

	if (buf + len > lim)
		return NULL;

	memset(p, 0, len);
	p->sadb_x_nat_t_frag_len = PFKEY_UNIT64(len);
	p->sadb_x_nat_t_frag_exttype = type;
	p->sadb_x_nat_t_frag_fraglen = l_natt_frag;

	return(buf + len);
}
#endif


static caddr_t
pfkey_setsadbsession_id (caddr_t   buf,
                         caddr_t   lim,
                         u_int64_t session_ids[],
                         u_int32_t max_session_ids)
{
	struct sadb_session_id *p;
	u_int len;

    if (!max_session_ids)
        return NULL;

	p = (void *)buf;
	len = sizeof(*p);

	if (buf + len > lim)
		return NULL;

	bzero(p, len);
	p->sadb_session_id_len = PFKEY_UNIT64(len);
	p->sadb_session_id_exttype = SADB_EXT_SESSION_ID;
    p->sadb_session_id_v[0] = session_ids[0];
    if (max_session_ids > 1)
        p->sadb_session_id_v[1] = session_ids[1];

	return(buf + len);
}

static caddr_t
pfkey_setsadbsastats (caddr_t        buf,
                      caddr_t        lim,
                      u_int32_t      dir,
                      struct sastat *stats,
                      u_int32_t      max_stats)
{
    struct sadb_sastat *p;
	u_int len, list_len;

    if (!stats || !max_stats)
        return NULL;

	p = ALIGNED_CAST(__typeof__(p))buf;                     // Wcast-align fix - buffer passed to here is malloc'd message buffer
    list_len = sizeof(*stats) * max_stats;
	len = sizeof(*p) + PFKEY_ALIGN8(list_len);

	if (buf + len > lim)
		return NULL;

	bzero(p, len);
	p->sadb_sastat_len      = PFKEY_UNIT64(len);
	p->sadb_sastat_exttype  = SADB_EXT_SASTAT;
    p->sadb_sastat_dir      = dir;
    p->sadb_sastat_list_len = max_stats;
    bcopy(stats, p + 1, list_len);

	return(buf + len);
}

int
pfkey_send_getsastats (int            so,
                       u_int32_t      seq,
                       u_int64_t     *session_ids,
                       u_int32_t      max_session_ids,
                       u_int8_t       dir,
                       struct sastat *stats,
                       u_int32_t      max_stats)
{
	struct sadb_msg *newmsg;
	caddr_t ep;
	int list_len, out_len, len;
	caddr_t p;

    if (!session_ids || !stats || !max_stats) {
        return -1;
    }

    list_len = sizeof(*stats) * max_stats;
    /* create new sadb_msg to send. */
    out_len = sizeof(struct sadb_msg) + sizeof(struct sadb_session_id) + sizeof(struct sadb_sastat) + PFKEY_ALIGN8(list_len);

    if ((newmsg = CALLOC((size_t)out_len, struct sadb_msg *)) == NULL) {
        __ipsec_set_strerror(strerror(errno));
        return -1;
    }
    ep = ((caddr_t)(void *)newmsg) + out_len;

    p = pfkey_setsadbmsg((void *)newmsg, ep, SADB_GETSASTAT, (u_int)out_len, SADB_SATYPE_UNSPEC, seq, getpid());
    if (!p) {
        free(newmsg);
        return -1;
    }

    p = pfkey_setsadbsession_id(p, ep, session_ids, max_session_ids);
    if (!p) {
        free(newmsg);
        return -1;
    }

    p = pfkey_setsadbsastats(p, ep, dir, stats, max_stats);
    if (!p) {
        free(newmsg);
        return -1;
    }

    /* send message */
    len = pfkey_send(so, newmsg, out_len);
    free(newmsg);

    if (len < 0)
        return -1;

    __ipsec_errcode = EIPSEC_NO_ERROR;
    return len;
}
