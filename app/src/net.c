#include <pspkernel.h>
#include <psputility.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "net.h"
#include "ui.h"

static int modules_loaded, stack_up;

int net_ready(void)
{
	int state = 0;
	return stack_up && sceNetApctlGetState(&state) >= 0 && state == PSP_NET_APCTL_STATE_GOT_IP;
}

int net_start(char *err, int errlen)
{
	if (!modules_loaded) {
		if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) < 0 || sceUtilityLoadNetModule(PSP_NET_MODULE_INET) < 0) {
			snprintf(err, errlen, "Couldn't load the PSP's network modules.");
			return -1;
		}
		modules_loaded = 1;
	}
	if (!stack_up) {
		if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) < 0 || sceNetInetInit() < 0 ||
		    sceNetApctlInit(0x8000, 48) < 0 || sceNetResolverInit() < 0) {
			snprintf(err, errlen, "Couldn't start the network stack.");
			return -1;
		}
		stack_up = 1;
	}
	if (net_ready())
		return 0;
	if (!ui_netconf()) {
		snprintf(err, errlen, "Not connected to Wi-Fi. Check the WLAN switch and your access point "
		         "(Settings > Network Settings), then try again.");
		return -1;
	}
	return 0;
}

void url_encode(const char *in, char *out, int outlen)
{
	static const char hex[] = "0123456789ABCDEF";
	int o = 0;
	for (; *in && o < outlen - 4; in++) {
		unsigned char c = (unsigned char)*in;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-._~", c)) {
			out[o++] = c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
	}
	out[o] = 0;
}

static int parse_ipv4(const char *s, struct in_addr *addr)
{
	unsigned a, b, c, d;
	char tail;
	if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
		return -1;
	addr->s_addr = htonl(a << 24 | b << 16 | c << 8 | d);
	return 0;
}

static int resolve(const char *host, struct in_addr *addr)
{
	char buf[1024];
	int rid, rc;
	if (parse_ipv4(host, addr) == 0)
		return 0;
	if (sceNetResolverCreate(&rid, buf, sizeof(buf)) < 0)
		return -1;
	rc = sceNetResolverStartNtoA(rid, host, addr, 5, 3);
	sceNetResolverDelete(rid);
	return rc < 0 ? -1 : 0;
}

#define RECV_TIMEOUT_US 500000   /* each recv waits at most this long, so Cancel stays responsive */
#define RECV_TIMEOUT -3

/* > 0 bytes, 0 closed, RECV_TIMEOUT (no data yet), -1 error (errno in *e) */
static int recv_some(int s, void *buf, int len, int *e)
{
	int n = (int)sceNetInetRecv(s, buf, len, 0);
	if (n >= 0)
		return n;
	*e = sceNetInetGetErrno();
	/* EAGAIN / EWOULDBLOCK / ETIMEDOUT in the PSP's and the common errno numberings */
	if (*e == 11 || *e == 35 || *e == 60 || *e == 110 || *e == 0x0B)
		return RECV_TIMEOUT;
	return -1;
}

int http_get(const char *url, const char *out_path, SceKernelUtilsSha1Context *sha1,
             unsigned char **mem, int *memlen, int maxmem, hb_progress cb, void *ctx, char *err, int errlen)
{
	char host[128], req[1536], head[4096];
	const char *p, *path;
	int port = 80, s = -1, hlen = 0, status = 0, rc = -1, idle = 0, e = 0;
	unsigned int tmo = RECV_TIMEOUT_US;
	long long clen = -1, got = 0;
	SceUID out = -1;
	struct sockaddr_in sa;
	unsigned char buf[8192];
	int memcap = 0;

	if (strncmp(url, "http://", 7)) {
		snprintf(err, errlen, "Only http:// relay addresses work on a PSP.");
		return -1;
	}
	p = url + 7;
	path = strchr(p, '/');
	if (!path)
		path = "/";
	{
		int n = (int)(strchr(p, '/') ? strchr(p, '/') - p : (int)strlen(p));
		const char *colon = memchr(p, ':', n);
		if (colon) {
			port = atoi(colon + 1);
			n = (int)(colon - p);
		}
		if (n <= 0 || n >= (int)sizeof(host)) {
			snprintf(err, errlen, "Bad relay address.");
			return -1;
		}
		memcpy(host, p, n);
		host[n] = 0;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (resolve(host, &sa.sin_addr) < 0) {
		snprintf(err, errlen, "Couldn't look up %s.", host);
		return -1;
	}
	s = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
	if (s < 0 || sceNetInetConnect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		snprintf(err, errlen, "Couldn't connect to the relay.");
		goto done;
	}
	snprintf(req, sizeof(req),
	         "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PSP-Homebrew-Store\r\nConnection: close\r\n\r\n",
	         path, host);
	sceNetInetSetsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
	if ((int)sceNetInetSend(s, req, strlen(req), 0) != (int)strlen(req)) {
		snprintf(err, errlen, "Couldn't send the request.");
		goto done;
	}

	/* headers */
	for (;;) {
		int n;
		char *end;
		n = recv_some(s, head + hlen, sizeof(head) - 1 - hlen, &e);
		if (n == RECV_TIMEOUT) {
			if (++idle > 30) {   /* ~15 s */
				snprintf(err, errlen, "The relay didn't answer.");
				goto done;
			}
			if (cb && cb(ctx, 0, -1)) {
				snprintf(err, errlen, "Cancelled.");
				rc = -2;
				goto done;
			}
			continue;
		}
		if (n < 0) {
			snprintf(err, errlen, "Network error (%d) while waiting for the relay.", e);
			goto done;
		}
		if (n == 0) {
			snprintf(err, errlen, "The connection closed early.");
			goto done;
		}
		idle = 0;
		hlen += n;
		head[hlen] = 0;
		end = strstr(head, "\r\n\r\n");
		if (end) {
			char *cl;
			int body = hlen - (int)(end + 4 - head);
			sscanf(head, "HTTP/%*s %d", &status);
			for (cl = head; (cl = strchr(cl, '\n')) != NULL; cl++)
				if (!strncasecmp(cl + 1, "content-length:", 15))
					clen = atoll(cl + 16);
			if (status != 200) {
				snprintf(err, errlen, status == 429 ? "Too many requests; wait a minute and try again." :
				         status == 404 ? "The relay doesn't have that file." :
				         "The relay answered HTTP %d.", status);
				goto done;
			}
			if (out_path) {
				out = sceIoOpen(out_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
				if (out < 0) {
					snprintf(err, errlen, "Couldn't write to the memory stick.");
					goto done;
				}
			} else {
				memcap = clen > 0 ? (int)clen : 65536;
				if (clen > maxmem) {
					snprintf(err, errlen, "The file is too big.");
					goto done;
				}
				*mem = malloc(memcap + 1);
				if (!*mem) {
					snprintf(err, errlen, "Out of memory.");
					goto done;
				}
			}
			memmove(buf, end + 4, body);
			hlen = body;   /* now: bytes of body in buf */
			break;
		}
		if (hlen >= (int)sizeof(head) - 1) {
			snprintf(err, errlen, "Bad answer from the relay.");
			goto done;
		}
	}

	/* body */
	for (;;) {
		int n = hlen;
		hlen = 0;
		if (n > 0) {
			if (sha1)
				sceKernelUtilsSha1BlockUpdate(sha1, buf, n);
			if (out >= 0) {
				if (sceIoWrite(out, buf, n) != n) {
					snprintf(err, errlen, "Couldn't write to the memory stick (is it full?).");
					goto done;
				}
			} else {
				if (got + n > maxmem) {
					snprintf(err, errlen, "The file is too big.");
					goto done;
				}
				if (got + n > memcap) {
					unsigned char *bigger;
					memcap = (int)(got + n) * 2 < maxmem ? (int)(got + n) * 2 : maxmem;
					bigger = realloc(*mem, memcap + 1);
					if (!bigger) {
						snprintf(err, errlen, "Out of memory.");
						goto done;
					}
					*mem = bigger;
				}
				memcpy(*mem + got, buf, n);
			}
			got += n;
		}
		if (cb && cb(ctx, got, clen)) {
			snprintf(err, errlen, "Cancelled.");
			rc = -2;
			goto done;
		}
		if (clen >= 0 && got >= clen)
			break;
		n = recv_some(s, buf, sizeof(buf), &e);
		if (n == RECV_TIMEOUT) {
			if (++idle > 40) {   /* ~20 s with no data */
				snprintf(err, errlen, "The download stalled.");
				goto done;
			}
			continue;
		}
		if (n < 0) {
			snprintf(err, errlen, "Network error (%d).", e);
			goto done;
		}
		idle = 0;
		if (n == 0)
			break;
		hlen = n;
	}
	if (clen >= 0 && got != clen) {
		snprintf(err, errlen, "The download was cut off.");
		goto done;
	}
	if (mem && *mem) {
		(*mem)[got] = 0;
		*memlen = (int)got;
	}
	rc = 0;

done:
	if (out >= 0)
		sceIoClose(out);
	if (s >= 0)
		sceNetInetClose(s);
	if (rc != 0 && mem && *mem) {
		free(*mem);
		*mem = NULL;
	}
	if (rc != 0 && out_path)
		sceIoRemove(out_path);
	return rc;
}
