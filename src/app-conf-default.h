#ifndef __APP_CONF_H_
#define __APP_CONF_H_

#include "app-conf-constants.h"

#define COMPONENT_NETWORK_RIME_BASESTATION "1.0"
#define COMPONENT_NETWORK_IPV6_BASESTATION "aaaa::0212:7401:0001:0101"

#define COMPONENT_APPLICATION_MESHMESSAGES_NODESLIST_RIME "1.0,2.0,3.0"

#define COMPONENT_NEIGHBORDISCOVERY_EDGEWEIGHT_DISTANCE_POSITIONS "1.0=22|25,2.0=18|105,3.0=90|239";

#define COMPONENT_APPLICATION                  ...
#define COMPONENT_NEIGHBORDISCOVERY            ...
#define COMPONENT_NEIGHBORDISCOVERY_EDGEWEIGHT ...
#define COMPONENT_NETWORK                      COMPONENT_NETWORK_MAKEFILE
#define COMPONENT_POWERCONTROL                 ...
#define COMPONENT_RADIO                        COMPONENT_RADIO_AUTODETECT
#define COMPONENT_TOPOLOGYCONTROL              ...

#define UTILITIES_CONTIKIRANDOM_STATICSEED 123456
#define UTILITIES_CONTIKIRANDOM_SEED UTILITIES_CONTIKIRANDOM_SEED_STATIC

#endif /* __APP_CONF_H_ */
