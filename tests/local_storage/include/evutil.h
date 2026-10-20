/* Isolated C object-backend fixture only: avoid requiring libevent v1 headers
 * on the minimal CI runner. Never install this directory in native binaries.
 * The tested filesystem backends do not call the event/socket helpers. */
#ifndef CF_LOCAL_STORAGE_TEST_EVUTIL_H
#define CF_LOCAL_STORAGE_TEST_EVUTIL_H

#include <arpa/inet.h>
typedef int evutil_socket_t;

#endif
