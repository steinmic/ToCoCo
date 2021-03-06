#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "contiki.h"
#include "contiki-lib.h"

#include "powercontrol-binarysearch.h"
#include "../../app-conf.h"
#include "../../lib/boot.h"
#include "../../lib/buffer.h"
#include "../../lib/components.h"
#include "../../lib/networkaddr.h"
#include "../../lib/uniqueid.h"
#include "../../lib/utilities.h"

#define DEBUG 0
#if DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

#if COMPONENT_POWERCONTROL == COMPONENT_POWERCONTROL_BINARYSEARCH

#define MESSAGES_FOR_SUCCESS ((uint8_t) ceil(COMPONENT_POWERCONTROL_BINARYSEARCH_RATIO * COMPONENT_POWERCONTROL_BINARYSEARCH_MESSAGES))
#define POWERLEVELS_INDEX_LAST (COMPONENT_POWERCONTROL_BINARYSEARCH_POWERLEVELS - 1)

typedef struct power_optimization {
	struct power_optimization *next;

	networkaddr_t *node;
	int8_t power;

	struct etimer optimization_sendtimer;
	uint8_t optimization_messages_sent;
	uint8_t optimization_messages_received;
	int8_t optimization_binarysearch_max;
	int8_t optimization_binarysearch_min;
} power_optimization_t;

MEMB(memb_optimizations, power_optimization_t, COMPONENT_NETWORK_NEXTHOPS_MEMORY);
LIST(list_optimizations);

static uint8_t messagetype;
static uint8_t powerlevels[COMPONENT_POWERCONTROL_BINARYSEARCH_POWERLEVELS];

static void _powercontrol_binarysearch_updatelist();
static void _powercontrol_binarysearch_recv(const networkaddr_t *source, buffer_t *data, int8_t rssi);
static void _powercontrol_linklocal(const networkaddr_t *destination, int8_t txpower, int8_t num_tx, bool received);
static inline int8_t _binarysearch_midpoint(int8_t min, int8_t max);

PROCESS(component_powercontrol, "powercontrol: binarysearch");
PROCESS_THREAD(component_powercontrol, ev, data) {
	PROCESS_BEGIN();

	memb_init(&memb_optimizations);
	list_init(list_optimizations);

	BOOT_COMPONENT_WAIT(component_powercontrol);

	messagetype = uniqueid_assign();
	component_network_packet_subscribe(messagetype,  _powercontrol_binarysearch_recv);

	component_network_linklocalsend_subscribe(_powercontrol_linklocal);

	static struct etimer etimer_updatelist;
	etimer_set(&etimer_updatelist, CLOCK_SECOND * COMPONENT_POWERCONTROL_BINARYSEARCH_UPDATEINTERVAL); // wait for routing to stabilize

	int8_t i;
	float step = (COMPONENT_RADIO_TXPOWER_MAX - COMPONENT_RADIO_TXPOWER_MIN) / (1.0 * COMPONENT_POWERCONTROL_BINARYSEARCH_POWERLEVELS);
	for(i = 0; i < COMPONENT_POWERCONTROL_BINARYSEARCH_POWERLEVELS; i++) {
		if(i == POWERLEVELS_INDEX_LAST) {
			powerlevels[i] = COMPONENT_RADIO_TXPOWER_MAX;
		} else {
			powerlevels[i] = (int) (step * (i + 1) + 0.49999); // round(x):int = (int) (x + 0.5 - epsilon)
		}
	}

#if DEBUG
	int8_t j;
	PRINTF("DEBUG: [powercontrol-binarysearch] powerlevels: ");
	for(j = 0; j < COMPONENT_POWERCONTROL_BINARYSEARCH_POWERLEVELS; j++) {
		PRINTF("%d%s", powerlevels[j], (j < COMPONENT_POWERCONTROL_BINARYSEARCH_POWERLEVELS - 1) ? ", " : "\n");
	}
#endif

	while(1) {
		PROCESS_WAIT_EVENT();

		if(etimer_expired(&etimer_updatelist)) {
			etimer_set(&etimer_updatelist, CLOCK_SECOND * COMPONENT_POWERCONTROL_BINARYSEARCH_UPDATEINTERVAL);

			_powercontrol_binarysearch_updatelist();
		} else {
			power_optimization_t *optimization;
			for(optimization = list_head(list_optimizations); optimization != NULL; optimization = list_item_next(optimization)) {
				if(etimer_expired(&optimization->optimization_sendtimer)) {

					if(optimization->optimization_messages_sent < COMPONENT_POWERCONTROL_BINARYSEARCH_MESSAGES) {
						if(optimization->optimization_messages_sent == 0 && optimization->optimization_binarysearch_min == 0 && optimization->optimization_binarysearch_max == POWERLEVELS_INDEX_LAST)
							PRINTF("DEBUG: [powercontrol-binarysearch] testing start: node=%s, txpower=%d\n", networkaddr2string_buffered(optimization->node), powerlevels[_binarysearch_midpoint(optimization->optimization_binarysearch_min, optimization->optimization_binarysearch_max)]);

						if(optimization->optimization_messages_received >= MESSAGES_FOR_SUCCESS) {
							// optimization level has already been reached, no need to send further messages (simply pretend we've done it)
							optimization->optimization_messages_sent++;
						}
						else if((optimization->optimization_messages_sent - optimization->optimization_messages_received) > (COMPONENT_POWERCONTROL_BINARYSEARCH_MESSAGES - MESSAGES_FOR_SUCCESS)) {
							// optimization level can no more been reached, no need to send further messages (simply pretend we've done it)
							optimization->optimization_messages_sent++;
						}
						else {
							int8_t testpower = powerlevels[_binarysearch_midpoint(optimization->optimization_binarysearch_min, optimization->optimization_binarysearch_max)];
							optimization->optimization_messages_sent++;
							component_network_packet_send(COMPONENT_NETWORK_TRANSMISSION_LINKLOCAL_UNICAST, messagetype, optimization->node, component_network_packet_sendbuffer(), testpower, 1);
						}

						etimer_set(&optimization->optimization_sendtimer, CLOCK_SECOND * COMPONENT_POWERCONTROL_BINARYSEARCH_TESTDURATION / COMPONENT_POWERCONTROL_BINARYSEARCH_MESSAGES);
					} else {
						PRINTF("DEBUG: [powercontrol-binarysearch] testing complete: node=%s, txpower=%d, sent=%d, received=%d, ratio=%03d%%\n", networkaddr2string_buffered(optimization->node), powerlevels[_binarysearch_midpoint(optimization->optimization_binarysearch_min, optimization->optimization_binarysearch_max)], optimization->optimization_messages_sent, optimization->optimization_messages_received, (int) (((float) optimization->optimization_messages_received) / ((float) optimization->optimization_messages_sent) * 100));

						if(optimization->optimization_messages_received >= MESSAGES_FOR_SUCCESS) {
							optimization->optimization_binarysearch_max = _binarysearch_midpoint(optimization->optimization_binarysearch_min, optimization->optimization_binarysearch_max);
							optimization->power = (int8_t) (ceil(COMPONENT_POWERCONTROL_BINARYSEARCH_SAFETYMARGIN * COMPONENT_RADIO_TXPOWER_MAX) + powerlevels[optimization->optimization_binarysearch_max]);
							if(optimization->power > COMPONENT_RADIO_TXPOWER_MAX)
								optimization->power = COMPONENT_RADIO_TXPOWER_MAX;
							PRINTF("DEBUG: [powercontrol-binarysearch] new stable power: node=%s, txpower=%d\n", networkaddr2string_buffered(optimization->node), optimization->power);
						} else {
							optimization->optimization_binarysearch_min = _binarysearch_midpoint(optimization->optimization_binarysearch_min, optimization->optimization_binarysearch_max);
						}
						PRINTF("DEBUG: [powercontrol-binarysearch] new binary searchspace: node=%s, bin=[%d, %d]\n", networkaddr2string_buffered(optimization->node), powerlevels[optimization->optimization_binarysearch_min],  powerlevels[optimization->optimization_binarysearch_max]);

						if(optimization->optimization_binarysearch_min == optimization->optimization_binarysearch_max || (optimization->optimization_binarysearch_max - optimization->optimization_binarysearch_min) < 2) {
							// no further optimizations possible because new testpower would be any of the binarysearch space borders
							etimer_stop(&optimization->optimization_sendtimer);
							optimization->optimization_sendtimer.p = PROCESS_ZOMBIE; // etimer_expired is true if no process is assigned to etimer which happens when expiring (event if stopped), so assign the zombie process
							PRINTF("DEBUG: [powercontrol-binarysearch] finished: node=%s, txpower=%d\n", networkaddr2string_buffered(optimization->node), optimization->power);
						} else {
							PRINTF("DEBUG: [powercontrol-binarysearch] testing start: node=%s, txpower=%d\n", networkaddr2string_buffered(optimization->node),  powerlevels[_binarysearch_midpoint(optimization->optimization_binarysearch_min, optimization->optimization_binarysearch_max)]);
							optimization->optimization_messages_sent = 0;
							optimization->optimization_messages_received = 0;
							etimer_reset(&optimization->optimization_sendtimer);
						}
					}
				}
			}
		}
	}

	PROCESS_END();
}

static void _powercontrol_binarysearch_updatelist() {
	nexthop_t *nexthop;
	power_optimization_t *optimization;
	list_t list_nexthops = component_network_nexthops_all();

	// remove optimization entries for nodes no longer in nexthop list
	for(optimization = list_head(list_optimizations); optimization != NULL; optimization = list_item_next(optimization)) {
		bool found = false;
		for(nexthop = list_head(list_nexthops); nexthop != NULL; nexthop = list_item_next(nexthop))
			found |= networkaddr_equal(optimization->node, nexthop->address);

		if(!found) {
			PRINTF("DEBUG: [powercontrol-binarysearch] removed %s from optimization-list\n", networkaddr2string_buffered(optimization->node));
			networkaddr_reference_free(optimization->node);
			etimer_stop(&optimization->optimization_sendtimer);
			list_remove(list_optimizations, optimization);
			memb_free(&memb_optimizations, optimization);
		}
	}

	// add new optimization entries
	for(nexthop = list_head(list_nexthops); nexthop != NULL; nexthop = list_item_next(nexthop)) {
		bool found = false;
		for(optimization = list_head(list_optimizations); optimization != NULL; optimization = list_item_next(optimization))
			found |= networkaddr_equal(optimization->node, nexthop->address);

		if(found)
			continue;

		if((optimization = memb_alloc(&memb_optimizations)) == NULL) {
			printf("ERROR[powercontrol-binarysearch]: optimization-list is full\n");
			continue;
		}

		PRINTF("DEBUG: [powercontrol-binarysearch] adding %s to optimization-list\n", networkaddr2string_buffered(nexthop->address));
		optimization->node = networkaddr_reference_alloc(nexthop->address);
		optimization->power = COMPONENT_RADIO_TXPOWER_MAX;
		etimer_set(&optimization->optimization_sendtimer, CLOCK_SECOND * COMPONENT_POWERCONTROL_BINARYSEARCH_WAITDURATION + random(0, CLOCK_SECOND * COMPONENT_POWERCONTROL_BINARYSEARCH_RANDOMOFFSET));
		optimization->optimization_messages_sent = 0;
		optimization->optimization_messages_received = 0;
		optimization->optimization_binarysearch_max = POWERLEVELS_INDEX_LAST;
		optimization->optimization_binarysearch_min = 0;
		list_add(list_optimizations, optimization);
	}
}

static void _powercontrol_binarysearch_recv(const networkaddr_t *source, buffer_t *receivedata, int8_t rssi) {
	// dummy function for the network layer not throwing a warning that a message has been received
	// with an unregistered message type (which could be an implementation bug)
}

static void _powercontrol_linklocal(const networkaddr_t *destination, int8_t txpower, int8_t num_tx, bool received) {
	power_optimization_t *optimization;
	for(optimization = list_head(list_optimizations); optimization != NULL; optimization = list_item_next(optimization)) {
		int8_t testpower = powerlevels[_binarysearch_midpoint(optimization->optimization_binarysearch_min, optimization->optimization_binarysearch_max)];
		if(networkaddr_equal(optimization->node, destination) && txpower == testpower && received) {
			optimization->optimization_messages_received++;
		}
	}
}

static inline int8_t _binarysearch_midpoint(int8_t min, int8_t max) {
	return min + (max - min) / 2;
}

int8_t component_powercontrol_destinationtxpower(const networkaddr_t *destination) {
	power_optimization_t *optimization;

	for(optimization = list_head(list_optimizations); optimization != NULL; optimization = list_item_next(optimization)) {
		if(networkaddr_equal(destination, optimization->node))
			return optimization->power;
	}

	return -1;
}
#endif
