/* vmware.c — VMware hypervisor backdoor transport + detection. See vmware.h. */

#include <stdint.h>
#include "vmware.h"

/* The backdoor call. A 32-bit `in` from VMWARE_PORT (in DX) is intercepted by
 * the hypervisor, which treats EAX/EBX/ECX/EDX/ESI/EDI as the message and writes
 * its reply back into them — so every register is both input and output. On a
 * machine with no backdoor the port is unmapped: the `in` reads 0xFFFFFFFF into
 * EAX and leaves EBX untouched, which is how vmware_present tells "absent". */
void vmware_backdoor(struct vmware_regs *r)
{
	__asm__ volatile ("inl %%dx, %%eax"
	                  : "+a"(r->eax), "+b"(r->ebx), "+c"(r->ecx),
	                    "+d"(r->edx), "+S"(r->esi), "+D"(r->edi)
	                  : : "memory");
}

void vmware_send(uint16_t cmd, struct vmware_regs *r)
{
	r->eax = VMWARE_MAGIC;
	r->ecx = cmd;
	r->edx = VMWARE_PORT;
	vmware_backdoor(r);
}

int vmware_present(void)
{
	static int cached = -1;
	if (cached >= 0)
		return cached;

	struct vmware_regs r = { 0 };
	r.ebx = ~VMWARE_MAGIC;             /* a sentinel the hypervisor overwrites    */
	vmware_send(VMWARE_CMD_GETVERSION, &r);

	/* A real backdoor echoes the magic into EBX and a version (not -1) in EAX. */
	cached = (r.ebx == VMWARE_MAGIC && r.eax != 0xFFFFFFFFu) ? 1 : 0;
	return cached;
}

/* --- GuestRPC message channel ------------------------------------------------
 * The backdoor "message" sub-protocol (BDOOR_CMD_MESSAGE) carries variable-
 * length strings both ways over a channel the guest opens. VMware Tools layers
 * "RPCI" guest->host requests on it ("info-set ...", "tools.set.version ...").
 * Each call sets a sub-type in ECX's high word; status comes back in ECX's high
 * word; the channel id rides EDX's high word; a per-channel cookie (returned by
 * OPEN in ESI/EDI) must be echoed back on every later call, which happens for
 * free because we reuse one register struct across the whole exchange. */

#define BDOOR_CMD_MESSAGE 0x1E

#define MSG_OPEN        0x0000
#define MSG_SENDSIZE    0x0001
#define MSG_SENDPAYLOAD 0x0002
#define MSG_RECVSIZE    0x0003
#define MSG_RECVPAYLOAD 0x0004
#define MSG_RECVSTATUS  0x0005
#define MSG_CLOSE       0x0006

#define MSG_ST_SUCCESS  0x0001
#define MSG_ST_DORECV   0x0002

#define GUESTMSG_FLAG_COOKIE 0x80000000u
#define RPCI_PROTOCOL        0x49435052u   /* "RPCI"                              */

/* One message-channel call. Loads EBX (the call's parameter) and the sub-type +
 * channel, preserves ESI/EDI (the cookie) across calls, and returns the status
 * word from ECX's high half. */
static uint32_t msg_call(uint16_t subtype, uint32_t param, uint32_t channel,
                         struct vmware_regs *r)
{
	r->eax = VMWARE_MAGIC;
	r->ebx = param;
	r->ecx = ((uint32_t)subtype << 16) | BDOOR_CMD_MESSAGE;
	r->edx = (channel << 16) | VMWARE_PORT;
	vmware_backdoor(r);                /* ESI/EDI carry the cookie in and out      */
	return r->ecx >> 16;
}

int vmware_rpc(const char *req, char *resp, int resp_max)
{
	if (!vmware_present())
		return -1;

	int len = 0;
	while (req[len])
		len++;

	struct vmware_regs r = { 0 };      /* cookie starts cleared                    */
	uint32_t st;

	st = msg_call(MSG_OPEN, RPCI_PROTOCOL | GUESTMSG_FLAG_COOKIE, 0, &r);
	if (!(st & MSG_ST_SUCCESS))
		return -1;
	uint32_t channel = r.edx >> 16;

	st = msg_call(MSG_SENDSIZE, (uint32_t)len, channel, &r);
	if (!(st & MSG_ST_SUCCESS))
		goto fail;

	/* Ship the request four bytes per call, little-endian. */
	for (int i = 0; i < len; i += 4) {
		uint32_t word = 0;
		for (int b = 0; b < 4 && i + b < len; b++)
			word |= (uint32_t)(uint8_t)req[i + b] << (b * 8);
		st = msg_call(MSG_SENDPAYLOAD, word, channel, &r);
		if (!(st & MSG_ST_SUCCESS))
			goto fail;
	}

	/* Reply length, then drain the reply four bytes at a time. */
	st = msg_call(MSG_RECVSIZE, 0, channel, &r);
	if (!(st & MSG_ST_SUCCESS))
		goto fail;

	int rlen = (st & MSG_ST_DORECV) ? (int)r.ebx : 0;
	int success = 0, out = 0;
	for (int i = 0; i < rlen; i += 4) {
		st = msg_call(MSG_RECVPAYLOAD, MSG_ST_SUCCESS, channel, &r);
		if (!(st & MSG_ST_SUCCESS))
			goto fail;
		uint32_t word = r.ebx;
		for (int b = 0; b < 4 && i + b < rlen; b++) {
			char c = (char)((word >> (b * 8)) & 0xFF);
			int pos = i + b;
			if (pos == 0)
				success = (c == '1');   /* RPCI marks success as "1 ..." */
			/* Copy the payload after the "1 "/"0 " marker into resp. */
			if (pos >= 2 && resp && out < resp_max - 1)
				resp[out++] = c;
		}
	}
	if (resp && resp_max > 0)
		resp[out] = '\0';

	msg_call(MSG_RECVSTATUS, MSG_ST_SUCCESS, channel, &r);
	msg_call(MSG_CLOSE, 0, channel, &r);
	return success ? out : -1;

fail:
	msg_call(MSG_CLOSE, 0, channel, &r);
	return -1;
}

/* --- Legacy copy/paste backdoor ----------------------------------------------
 * The original (pre-GuestRPC) clipboard channel: four single-instruction
 * backdoor commands carrying plain text, four bytes at a time. GET_SELLENGTH
 * returns the host clipboard's byte length (or a negative sentinel meaning
 * "nothing new / don't exchange"), then GET_NEXTPIECE is read ceil(len/4) times,
 * each handing back four bytes in EAX. The push side mirrors it: SET_SELLENGTH
 * announces the length in EBX, then SET_NEXTPIECE ships four bytes per call. */

#define BDOOR_CMD_GETSELLENGTH 6
#define BDOOR_CMD_GETNEXTPIECE 7
#define BDOOR_CMD_SETSELLENGTH 8
#define BDOOR_CMD_SETNEXTPIECE 9

/* An implausibly large length means either a wedged host or — on a hypervisor
 * whose vmport doesn't implement these commands — a junk register left over from
 * the call (e.g. our own magic echoed back). Reject it rather than draining
 * megabytes of garbage; real clipboards we care about (commands, URLs, snippets)
 * are far under this. */
#define CLIP_SANE_MAX (64 * 1024)

int vmware_clip_recv(char *buf, int max)
{
	if (!vmware_present())
		return -1;

	struct vmware_regs r = { 0 };
	vmware_send(BDOOR_CMD_GETSELLENGTH, &r);

	int32_t total = (int32_t)r.eax;
	if (total < 0 || total > CLIP_SANE_MAX)   /* nothing new, or unsupported     */
		return -1;

	int out = 0;
	for (int32_t got = 0; got < total; ) {
		struct vmware_regs p = { 0 };
		vmware_send(BDOOR_CMD_GETNEXTPIECE, &p);
		uint32_t word = p.eax;
		for (int b = 0; b < 4 && got < total; b++, got++) {
			char c = (char)((word >> (b * 8)) & 0xFF);
			if (out < max)
				buf[out++] = c;       /* still drain past max to stay in sync    */
		}
	}
	return out;
}

int vmware_clip_send(const char *buf, int len)
{
	if (!vmware_present())
		return -1;
	if (len < 0)
		len = 0;

	struct vmware_regs r = { 0 };
	r.ebx = (uint32_t)len;
	vmware_send(BDOOR_CMD_SETSELLENGTH, &r);

	for (int i = 0; i < len; i += 4) {
		uint32_t word = 0;
		for (int b = 0; b < 4 && i + b < len; b++)
			word |= (uint32_t)(uint8_t)buf[i + b] << (b * 8);
		struct vmware_regs p = { 0 };
		p.ebx = word;
		vmware_send(BDOOR_CMD_SETNEXTPIECE, &p);
	}
	return len;
}
