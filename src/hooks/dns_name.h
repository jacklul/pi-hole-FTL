/* Pi-hole: A black hole for Internet advertisements
*  (c) 2021 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  dnsmasq server interfacing routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef FTL_DNS_NAME_H
#define FTL_DNS_NAME_H

#ifdef FTL_PRIVATE
  #if !defined(FTLDNS)
  #define FTLDNS
  #include "../dnsmasq/dnsmasq.h"
  #undef __USE_XOPEN
  #include "../FTL.h"
  #endif
#endif // FTL_PRIVATE

const char *dns_name(char *name) __attribute__ ((pure));

#endif // FTL_DNS_NAME_H