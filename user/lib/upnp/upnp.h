/* upnp.h: UPnP Internet Gateway Device client (IGD:1/2), IPv4 only.
 *
 * Finds the router over SSDP, reads its device description and drives the WANIPConnection / WANPPPConnection
 * service by SOAP to learn the external address and manage port mappings. upnp.c is the message codec (pure
 * computation, host-tested); igd.c does the I/O. Links with $(UPNP) $(XML) $(HTTP) $(HEADERS) $(URL) $(NETCONN)
 * $(RESOLVE). Interruptible: UPNP_EINTR on Ctrl+C. */
#ifndef OWOS_UPNP_H
#define OWOS_UPNP_H

#define UPNP_URL_MAX 320
#define UPNP_SVC_MAX 80
#define UPNP_STR_MAX 128

/* A discovered gateway: the absolute SOAP control URL and the service type it speaks. */
struct upnp_igd {
  char ctl[UPNP_URL_MAX];
  char svc[UPNP_SVC_MAX];
};

/* One port-mapping entry. */
struct upnp_map {
  char proto[4]; /* "TCP" or "UDP" */
  int ext_port;
  int int_port;
  char int_ip[16];
  int enabled;
  char desc[UPNP_STR_MAX];
  unsigned lease; /* seconds, 0 = permanent */
};

#define UPNP_EFAIL (-1)  /* no gateway, transport error or malformed reply */
#define UPNP_EFAULT (-2) /* the gateway answered with a UPnP fault; code in upnp_last_fault() */
#define UPNP_EINTR (-4)  /* Ctrl+C */

/* ---- message codec (upnp.c) -------------------------------------------- */

/* M-SEARCH datagram for search target `st`. Returns its length, 0 if cap is too small. */
int upnp_msearch(char *buf, int cap, const char *st, int mx);

/* Pick the WANIPConnection / WANPPPConnection service out of the device description `xml` fetched from `loc`.
 * 1 and *g filled, 0 if there is none or its control URL points at another host. */
int upnp_parse_desc(const char *xml, const char *loc, struct upnp_igd *g);

/* SOAP envelope invoking `action` with the pre-built <arg> elements in `args`. Returns its length, 0 if it does
 * not fit. */
int upnp_soap_body(char *buf, int cap, const struct upnp_igd *g, const char *action, const char *args);

/* The request headers for that envelope (`len` is its length). Returns their length, 0 if cap is too small. */
int upnp_soap_headers(char *buf, int cap, const struct upnp_igd *g, const char *action, int len);

/* errorCode of a SOAP fault body (e.g. 718 ConflictInMappingEntry), or 0 if the body is not a fault. */
int upnp_fault_code(const char *body);

/* ---- network (igd.c) --------------------------------------------------- */

/* SSDP-discover the gateway on the local link. 1 (g filled), 0 none found, UPNP_EINTR. */
int upnp_discover(struct upnp_igd *g);

/* Last fault code seen by a call that returned UPNP_EFAULT. */
int upnp_last_fault(void);

/* This host's IPv4 address as text, from sysfs. 1 / 0 when unconfigured. */
int upnp_local_ip(char out[16]);

/* The calls below return 0 on success or a negative UPNP_E* code. */
int upnp_external_ip(const struct upnp_igd *g, char *out, int cap);
int upnp_add_mapping(const struct upnp_igd *g, const char *proto, int ext_port, int int_port, const char *int_ip,
                     const char *desc, unsigned lease);
int upnp_del_mapping(const struct upnp_igd *g, const char *proto, int ext_port);
/* Entry `index` of the mapping table; UPNP_EFAULT (713) past its end. */
int upnp_get_mapping(const struct upnp_igd *g, int index, struct upnp_map *m);

#endif /* OWOS_UPNP_H */
