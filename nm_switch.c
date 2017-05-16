#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>

#define NETMAP_WITH_LIBS
#include <net/netmap.h>
#include <net/netmap_user.h>

#define NM_SW_MAX_PORT 253 // capacity of uint8_t - (drop port + broadcast port)
#define NM_SW_MAX_WORKERS NM_SW_MAX_PORT
#define NM_SW_DROP 255
#define NM_SW_BCAST 254

struct nm_sw_port {
  uint8_t ref;
  char iface[IFNAMSIZ];
  uint8_t nrings;
  struct nm_desc **nds;
};

struct nm_sw_worker_state {
  struct pollfd fds[NM_SW_MAX_PORT];
  struct nm_desc *nds[NM_SW_MAX_PORT];
  struct netmap_ring *rxrings[NM_SW_MAX_PORT];
  struct netmap_ring *txrings[NM_SW_MAX_PORT];
};

struct nm_sw_worker {
  uint8_t id;
  pthread_t thread;
  struct nm_sw_worker_state state;
};

struct nm_sw {
  uint8_t nports;
  uint8_t nworkers;
  struct nm_sw_port *ports[NM_SW_MAX_PORT];
  struct nm_sw_worker *workers[NM_SW_MAX_WORKERS];
};

struct nm_sw *nm_sw_create(void) {
  struct nm_sw *new = malloc(sizeof(struct nm_sw));
  if (!new) {
    return NULL;
  }

  new->nports = 0;
  new->nworkers = 0;
  memset(new->ports, 0, sizeof(struct nm_sw_port *));
  memset(new->workers, 0, sizeof(struct nm_sw_workers *));

  return new;
}

int nm_sw_attach_port(struct nm_sw *sw, struct nm_sw_port *p, uint8_t portid) {
  if (sw->nports == NM_SW_MAX_PORT) {
    D("No more than %d ports can't be attached to this switch", NM_SW_MAX_PORT);
    return -1;
  }

  if (sw->ports[portid] == NULL) {
    sw->ports[portid] = p;
    sw->nports++;
  } else {
    D("Port %u is not available now", portid);
    return -1;
  }

  return 0;
}

struct nm_sw_port *nm_sw_detach_port(struct nm_sw *sw, const char *iface) {
  struct nm_sw_port *ret;

  for (int i = 0; i < NM_SW_MAX_PORT; i++) {
    if (sw->ports[i] == NULL) {
      continue;
    }

    if (strncmp(sw->ports[i]->iface, iface, IFNAMSIZ) == 0) {
      if (sw->ports[i]->ref != 0) {
        D("Port %s is busy", iface);
        return NULL;
      } else {
        ret = sw->ports[i];
        sw->ports[i] = NULL;
        sw->nports--;
        return ret;
      }
    }
  }

  D("%s: No such port", iface);
  return NULL;
}

void nm_sw_destroy(struct nm_sw *sw) {
  if (!sw) {
    return;
  }
  free(sw);
}

struct nm_sw_port *nm_sw_port_create(const char *iface, uint16_t nrings, uint32_t nslots, struct nm_desc *master) {
  struct nm_sw_port *new;
  struct nmreq req;
  struct nm_desc *tmp_desc, saved_desc;

  if (!iface || !nrings || !nslots) {
    return NULL;
  }

  memset(&req, 0, sizeof(req));

  req.nr_rx_rings = nrings;
  req.nr_tx_rings = nrings;
  req.nr_rx_slots = nslots;
  req.nr_tx_slots = nslots;

  tmp_desc = nm_open(iface, &req, 0, master ? master : NULL);
  if (tmp_desc == NULL) {
    D("Unable to open %s: %s", iface, strerror(errno));
    return NULL;
  }

  new = malloc(sizeof(struct nm_sw_port));
  if (new == NULL) {
    D("Memory allocation for new switch port failed");
    nm_close(tmp_desc);
    return NULL;
  }

  strncpy(new->iface, iface, IFNAMSIZ);
  new->nrings = nrings;

  new->nds = calloc(nrings, sizeof(struct nm_desc *));
  if (new->nds == NULL) {
    D("memory allocation for new netmap desc array failed");
    nm_close(tmp_desc);
    free(new);
    return NULL;
  }

  /* save first descriptor imformation and close it */
  memcpy(&saved_desc, tmp_desc, sizeof(struct nm_desc));
  saved_desc.self = &saved_desc;
  saved_desc.mem = NULL;
  nm_close(tmp_desc);
  saved_desc.req.nr_flags &= ~NR_REG_MASK;
  saved_desc.req.nr_flags |= NR_REG_ONE_NIC;
  saved_desc.req.nr_ringid = 0;

  /* bind first ring */
  new->nds[0] = nm_open(iface, &req, NM_OPEN_IFNAME, &saved_desc);
  if (new->nds[0] == NULL) {
    D("Unable to open %s: %s", iface, strerror(errno));
    free(new);
    return NULL;
  }

  for (int i = 1; i < nrings; i++) {
    struct nm_desc nmd = saved_desc;
    uint64_t nmd_flags = 0;
    nmd.self = &nmd;

    nmd.req.nr_flags = saved_desc.req.nr_flags & ~NR_REG_MASK;
    nmd.req.nr_flags |= NR_REG_ONE_NIC;
    nmd.req.nr_ringid = i;

    new->nds[i] = nm_open(iface, NULL, nmd_flags |
        NM_OPEN_IFNAME | NM_OPEN_NO_MMAP, &nmd);
    if (new->nds[i] == NULL) {
      D("Unable to open %s: %s", iface, strerror(errno));
      for (int j = 0; j > 0; j++) {
        nm_close(new->nds[j]);
      }
      free(new);
    }
  }

  new->ref = 0;

  return new;
}

void nm_sw_port_destroy(struct nm_sw_port *p) {
  if (p == NULL) {
    return;
  }

  for (int i = 0; i < p->nrings; i++) {
    nm_close(p->nds[i]);
  }

  D("%s", p->iface);

  free(p->nds);
}

int main(int argc, char **argv) {
  int err;

  struct nm_sw *sw = nm_sw_create();
  if (sw == NULL) {
    D("nm_sw_create failed");
    exit(EXIT_FAILURE);
  }

  struct nm_sw_port *p1 = nm_sw_port_create("netmap:ens4f0", 8, 1024, NULL);
  if (p1 == NULL) {
    D("nm_sw_port_create failed");
  }

  struct nm_sw_port *p2 = nm_sw_port_create("netmap:ens4f1", 8, 1024, p1->nds[0]);
  if (p2 == NULL) {
    D("nm_sw_port_create failed");
  }

  err = nm_sw_attach_port(sw, p1);
  if (err < 0) {
    D("nm_sw_attach_port failed");
  }

  err = nm_sw_attach_port(sw, p2);
  if (err < 0) {
    D("nm_sw_attach_port failed");
  }

  struct nm_sw_port *dp1 = nm_sw_detach_port(sw, "netmap:ens4f0");
  if (dp1 == NULL) {
    D("nm_sw_detach_port failed");
  }
  nm_sw_port_destroy(dp1);

  struct nm_sw_port *dp2 = nm_sw_detach_port(sw, "netmap:ens4f1");
  if (dp2 == NULL) {
    D("nm_sw_detach_port failed");
  }
  nm_sw_port_destroy(dp2);

  nm_sw_destroy(sw);
}
