/*
 * CTAPI front-end for libifd
 *
 * Copyright (C) 2003 Olaf Kirch <okir@suse.de>
 */

#include <stdlib.h>
#include <openct/ifd.h>
#include <openct/buffer.h>
#include <openct/logging.h>
#include <openct/conf.h>
#include <openct/error.h>
#include "ctapi.h"

static int	ifd_ctapi_control(ifd_reader_t *,
			const void *, size_t,
			void *, size_t);
static int	ifd_ctapi_reset(ifd_reader_t *, ifd_iso_apdu_t *,
			ct_buf_t *, time_t, const char *);
static int	ifd_ctapi_request_icc(ifd_reader_t *, ifd_iso_apdu_t *,
			ct_buf_t *, ct_buf_t *);
static int	ifd_ctapi_status(ifd_reader_t *, ifd_iso_apdu_t *,
			ct_buf_t *);
static int	ifd_ctapi_error(ct_buf_t *, unsigned int);
static int	ifd_ctapi_put_sw(ct_buf_t *, unsigned int);


/*
 * Initialize card terminal #N.
 * As all the terminals are configured by libifd internally,
 * we ignore the port number
 */
char
CT_init(unsigned short ctn, unsigned short pn)
{
	static int	first_time = 1;
	ifd_reader_t	*reader;

	/* When doing first time initialization, init the
	 * library */
	if (first_time) {
		first_time = 0;
		ifd_init();
	}

	if (!(reader = ifd_reader_by_index(ctn)))
		return ERR_INVALID;

	/* FIXME: just activate, or reset as well? */
	if (ifd_activate(reader) < 0)
		return ERR_INVALID;

	return OK;
}

char
CT_close(unsigned short ctn)
{
	ifd_reader_t	*reader;

	if (!(reader = ifd_reader_by_index(ctn)))
		return ERR_INVALID;
	if (reader)
		ifd_deactivate(reader);
	return OK;
}

char
CT_data(unsigned short ctn,
	unsigned char  *dad,
	unsigned char  *sad,
	unsigned short lc,
	unsigned char  *cmd,
	unsigned short *lr,
	unsigned char  *rsp)
{
	ifd_reader_t	*reader;
	int		rc;

	if (!(reader = ifd_reader_by_index(ctn))
	 || !sad || !dad)
		return ERR_INVALID;

	if (ct_config.debug > 1) {
		ct_debug("CT_data(dad=%d lc=%u lr=%u cmd=%s",
				*dad, lc, *lr, ct_hexdump(cmd, lc));
	}
	switch (*dad) {
	case 0:
		rc = ifd_card_command(reader, 0,
				cmd, lc, rsp, *lr);
		break;
	case 1:
		rc = ifd_ctapi_control(reader,
				cmd, lc, rsp, *lr);
		break;
	case 2:
		ct_error("CT-API: host talking to itself - "
			  "needs professional help?");
		return ERR_INVALID;
	case 3:
		rc = ifd_card_command(reader, 1,
				cmd, lc, rsp, *lr);
		break;
	default:
		ct_error("CT-API: unknown DAD %u", *dad);
		return ERR_INVALID;
	}

	/* Somewhat simplistic error translation */
	if (rc < 0)
		return ERR_INVALID;

	*lr = rc;
	return OK;
}

/*
 * Handle CTBCS messages
 */
int
ifd_ctapi_control(ifd_reader_t *reader,
		const void *cmd, size_t cmd_len,
		void *rsp, size_t rsp_len)
{
	ifd_iso_apdu_t	iso;
	ct_buf_t	sbuf, rbuf;
	int		rc;

	if (rsp_len < 2)
		return ERR_INVALID;

	if (ifd_iso_apdu_parse(cmd, cmd_len, &iso) < 0) {
		ct_error("Unable to parse CTBCS APDU");
		return ERR_INVALID;
	}

	ct_buf_set(&sbuf, (void *) cmd, cmd_len);
	ct_buf_init(&rbuf, rsp, rsp_len);

	if (iso.cla != CTBCS_CLA) {
		ct_error("Bad CTBCS APDU, cla=0x%02x", iso.cla);
		ifd_ctapi_error(&rbuf, CTBCS_SW_BAD_CLASS);
		goto out;
	}

	switch (iso.ins) {
	case CTBCS_INS_RESET:
		rc = ifd_ctapi_reset(reader, &iso, &rbuf, 0, NULL);
		break;
	case CTBCS_INS_REQUEST_ICC:
		rc = ifd_ctapi_request_icc(reader, &iso, &sbuf, &rbuf);
		break;
	case CTBCS_INS_STATUS:
		rc = ifd_ctapi_status(reader, &iso, &rbuf);
		break;
	default:
		ct_error("Bad CTBCS APDU, ins=0x%02x", iso.ins);
		rc = ifd_ctapi_error(&rbuf, CTBCS_SW_BAD_INS);
	}

	if (rc < 0)
		return rc;

	if (ct_buf_avail(&rbuf) > iso.le + 2)
		ifd_ctapi_error(&rbuf, CTBCS_SW_BAD_LENGTH);

out:	return ct_buf_avail(&rbuf);
}

/*
 * Request ICC
 */
static int
ifd_ctapi_request_icc(ifd_reader_t *reader, ifd_iso_apdu_t *iso,
		ct_buf_t *sbuf, ct_buf_t *rbuf)
{
	time_t		timeout = 0;
	char		msgbuf[256], *message;

	switch (iso->p2 >> 4) {
	case 0x00:
		/* use default label, or label specified
		 * in data. An empty message string indicates
		 * the default message */
		message = msgbuf;
		msgbuf[0] = '\0';
		break;
	case 0x0f:
		/* No message */
		message = NULL;
	default:
		return ifd_ctapi_error(rbuf, CTBCS_SW_BAD_PARAMS);
	}

	/* XXX use ct_tlv_* functions */
	while (ct_buf_avail(sbuf)) {
		unsigned char	type, len, val;

		if (ct_buf_get(sbuf, &type, 1) < 0
		 || ct_buf_get(sbuf, &len, 1) < 0
		 || ct_buf_avail(sbuf) < len)
			goto bad_length;

		if (type == 0x50) {
			ct_buf_get(sbuf, msgbuf, len);
			msgbuf[len] = '\0';
		} else if (type == 0x80) {
			if (len != 1)
				goto bad_length;
			ct_buf_get(sbuf, &val, 1);
			timeout = val;
		} else {
			/* Ignore unknown tag */
			ct_buf_get(sbuf, NULL, len);
		}
	}

	/* ifd_ctapi_reset does all the rest of the work */
	return ifd_ctapi_reset(reader, iso, rbuf, timeout, message);

bad_length:
	return ifd_ctapi_error(rbuf, CTBCS_SW_BAD_LENGTH);
}

int
ifd_ctapi_reset(ifd_reader_t *reader, ifd_iso_apdu_t *iso,
		ct_buf_t *rbuf,
		time_t timeout, const char *message)
{
	unsigned char	unit;
	unsigned int	atrlen = 0;
	unsigned char	atr[64];
	int		rc;

	unit = iso->p1;
	switch (unit) {
	case CTBCS_UNIT_INTERFACE1:
	case CTBCS_UNIT_INTERFACE2:
		rc = ifd_card_reset(reader, unit - CTBCS_UNIT_INTERFACE1,
				atr, sizeof(atr));
		break;

	default:
		/* Unknown unit */
		return ifd_ctapi_error(rbuf, CTBCS_SW_BAD_PARAMS);
	}

	if (rc < 0)
		return ERR_TRANS;
	
	switch (iso->p2 & 0xF) {
	case CTBCS_P2_RESET_NO_RESP:
		atrlen = 0;
		break;
	case CTBCS_P2_RESET_GET_ATR:
		atrlen = rc;
		break;
	case CTBCS_P2_RESET_GET_HIST:
		ct_error("CTAPI RESET: P2=GET_HIST not supported yet");
		return ifd_ctapi_error(rbuf, CTBCS_SW_BAD_PARAMS);
	}

	if (ct_buf_put(rbuf, atr, atrlen) < 0
	 || ifd_ctapi_put_sw(rbuf, 0x9000) < 0)
		return ifd_ctapi_error(rbuf, CTBCS_SW_BAD_LENGTH);

	return 0;
}

static int
ifd_ctapi_status(ifd_reader_t *reader, ifd_iso_apdu_t *iso, ct_buf_t *rbuf)
{
	unsigned int	n;

	for (n = 0; n < reader->nslots; n++) {
		unsigned char	c;
		int		status;

		if (ifd_card_status(reader, n, &status) < 0)
			status = 0;

		c = (status & IFD_CARD_PRESENT)
			? CTBCS_DATA_STATUS_CARD_CONNECT
			: CTBCS_DATA_STATUS_NOCARD;
		if (ct_buf_put(rbuf, &c, 1) < 0)
			goto bad_length;
	}

	if (ifd_ctapi_put_sw(rbuf, 0x9000) < 0)
		goto bad_length;

	return 0;

bad_length:
	return ifd_ctapi_error(rbuf, CTBCS_SW_BAD_LENGTH);
}

/*
 * Functions for setting the SW
 */
static int
ifd_ctapi_error(ct_buf_t *bp, unsigned int sw)
{
	ct_buf_clear(bp);
	return ifd_ctapi_put_sw(bp, sw);
}

int
ifd_ctapi_put_sw(ct_buf_t *bp, unsigned int sw)
{
	unsigned char	temp[2];

	temp[0] = sw >> 8;
	temp[1] = sw & 0xff;

	if (ct_buf_put(bp, temp, 2) < 0)
		return ERR_INVALID;
	return 2;
}