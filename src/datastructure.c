/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Query processing routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "datastructure.h"
#include "shmem.h"
#include "log.h"
// enum REGEX
#include "regex_r.h"
// reload_per_client_regex()
#include "database/gravity-db.h"
// bool startup
#include "main.h"
// reset_aliasclient()
#include "database/aliasclients.h"
// config struct
#include "config/config.h"
// set_event(RESOLVE_NEW_HOSTNAMES)
#include "events.h"
// overTime array
#include "overTime.h"
// short_path()
#include "files.h"

// converts upper to lower case, and leaves other characters unchanged
void strtolower(char *str)
{
	int i = 0;
	while(str[i]){ str[i] = tolower(str[i]); i++; }
}

// creates a simple hash of a string that fits into a uint32_t
uint32_t __attribute__ ((pure)) hashStr(const char *s)
{
        uint32_t hash = 0;
        // Jenkins' One-at-a-Time hash (http://www.burtleburtle.net/bob/hash/doobs.html)
        for(; *s; ++s)
        {
                hash += *s;
                hash += hash << 10;
                hash ^= hash >> 6;
        }

        hash += hash << 3;
        hash ^= hash >> 11;
        hash += hash << 15;
        return hash;
}

int findQueryID(const int id)
{
	// Loop over all queries - we loop in reverse order (start from the most recent query and
	// continuously walk older queries while trying to find a match. Ideally, we should always
	// find the correct query with zero iterations, but it may happen that queries are processed
	// asynchronously, e.g. for slow upstream relies to a huge amount of requests.
	// We iterate from the most recent query down to at most MAXITER queries in the past to avoid
	// iterating through the entire array of queries
	// MAX(0, a) is used to return 0 in case a is negative (negative array indices are harmful)
	const int until = MAX(0, counters->queries-MAXITER);
	const int start = MAX(0, counters->queries-1);

	// Check UUIDs of queries
	for(int i = start; i >= until; i--)
	{
		const queriesData* query = getQuery(i, true);

		// Check if the returned pointer is valid before trying to access it
		if(query == NULL)
			continue;

		if(query->id == id)
			return i;
	}

	// If not found
	return -1;
}

int _findUpstreamID(const char *upstreamString, const in_port_t port, int line, const char *func, const char *file)
{
	// Go through already knows upstream servers and see if we used one of those
	for(int upstreamID = 0; upstreamID < counters->upstreams; upstreamID++)
	{
		// Get upstream pointer
		upstreamsData* upstream = _getUpstream(upstreamID, false, line, func, file);

		// Check if the returned pointer is valid before trying to access it
		if(upstream == NULL)
			continue;

		if(strcmp(getstr(upstream->ippos), upstreamString) == 0 && upstream->port == port)
			return upstreamID;
	}
	// This upstream server is not known
	// Store ID
	const int upstreamID = counters->upstreams;
	log_debug(DEBUG_GC, "New upstream server: %s:%u (ID %i)", upstreamString, port, upstreamID);

	// Get upstream pointer
	upstreamsData* upstream = _getUpstream(upstreamID, false, line, func, file);
	if(upstream == NULL)
	{
		log_err("Encountered serious memory error in findupstreamID()");
		return -1;
	}

	// Set magic byte
	upstream->magic = MAGICBYTE;
	// Save upstream destination IP address
	upstream->ippos = addstr(upstreamString);
	upstream->failed = 0;
	// Initialize upstream hostname
	// Due to the nature of us being the resolver,
	// the actual resolving of the host name has
	// to be done separately to be non-blocking
	upstream->flags.new = true;
	upstream->namepos = 0; // 0 -> string with length zero
	// Initialize response time values
	upstream->rtime = 0.0;
	upstream->rtuncertainty = 0.0;
	upstream->responses = 0u;
	// This is a new upstream server
	set_event(RESOLVE_NEW_HOSTNAMES);
	upstream->lastQuery = 0.0;
	// Store port
	upstream->port = port;
	// Increase counter by one
	counters->upstreams++;

	return upstreamID;
}

static int get_next_free_domainID(void)
{
	// Compare content of domain against known domain IP addresses
	for(int domainID = 0; domainID < counters->domains; domainID++)
	{
		// Get domain pointer
		domainsData* domain = getDomain(domainID, false);

		// Check if the returned pointer is valid before trying to access it
		if(domain == NULL)
			continue;

		// Check if the magic byte is set
		if(domain->magic == 0x00)
			return domainID;
	}

	// If we did not return until here, then we need to allocate a new domain ID
	return counters->domains;
}

int _findDomainID(const char *domainString, const bool count, int line, const char *func, const char *file)
{
	uint32_t domainHash = hashStr(domainString);
	for(int domainID = 0; domainID < counters->domains; domainID++)
	{
		// Get domain pointer
		domainsData* domain = _getDomain(domainID, false, line, func, file);

		// Check if the returned pointer is valid before trying to access it
		if(domain == NULL)
			continue;

		// Quicker test: Does the domain match the pre-computed hash?
		if(domain->domainhash != domainHash)
			continue;

		// If so, compare the full domain using strcmp
		if(strcmp(getstr(domain->domainpos), domainString) == 0)
		{
			if(count)
			{
				domain->count++;
				domain->lastQuery = double_time();
			}
			return domainID;
		}
	}

	// If we did not return until here, then this domain is not known
	// Store ID
	const int domainID = get_next_free_domainID();

	// Get domain pointer
	domainsData* domain = _getDomain(domainID, false, line, func, file);
	if(domain == NULL)
	{
		log_err("Encountered serious memory error in findDomainID()");
		return -1;
	}

	log_debug(DEBUG_GC, "New domain: %s (ID %d)", domainString, domainID);

	// Set magic byte
	domain->magic = MAGICBYTE;
	// Set its counter to 1 only if this domain is to be counted
	// Domains only encountered during CNAME inspection are NOT counted here
	domain->count = count ? 1 : 0;
	// Set blocked counter to zero
	domain->blockedcount = 0;
	// Store domain name - no need to check for NULL here as it doesn't harm
	domain->domainpos = addstr(domainString);
	// Store pre-computed hash of domain for faster lookups later on
	domain->domainhash = hashStr(domainString);
	domain->lastQuery = 0.0;
	// Increase counter by one
	counters->domains++;

	return domainID;
}

static int get_next_free_clientID(void)
{
	// Compare content of client against known client IP addresses
	for(int clientID = 0; clientID < counters->clients; clientID++)
	{
		// Get client pointer
		clientsData* client = getClient(clientID, false);

		// Check if the returned pointer is valid before trying to access it
		if(client == NULL)
			continue;

		// Check if the magic byte is unset
		if(client->magic == 0x00)
			return clientID;
	}

	// If we did not return until here, then we need to allocate a new client ID
	return counters->clients;
}

int _findClientID(const char *clientIP, const bool count, const bool aliasclient, int line, const char *func, const char *file)
{
	// Compare content of client against known client IP addresses
	for(int clientID=0; clientID < counters->clients; clientID++)
	{
		// Get client pointer
		clientsData* client = _getClient(clientID, true, line, func, file);

		// Check if the returned pointer is valid before trying to access it
		if(client == NULL)
			continue;

		// Quick test: Does the clients IP start with the same character?
		if(getstr(client->ippos)[0] != clientIP[0])
			continue;

		// If so, compare the full IP using strcmp
		if(strcmp(getstr(client->ippos), clientIP) == 0)
		{
			// Add one if count == true (do not add one, e.g., during ARP table processing)
			if(count && !aliasclient) change_clientcount(client, 1, 0, -1, 0);
			return clientID;
		}
	}

	// Return -1 (= not found) if count is false because we do not want to create a new client here
	// Proceed if we are looking for a alias-client because we want to create a new record
	if(!count && !aliasclient)
		return -1;

	// If we did not return until here, then this client is definitely new
	// Store ID
	const int clientID = get_next_free_clientID();

	// Get client pointer
	clientsData* client = _getClient(clientID, false, line, func, file);
	if(client == NULL)
	{
		log_err("Encountered serious memory error in findClientID()");
		return -1;
	}

	log_debug(DEBUG_GC, "New client: %s (ID %d)", clientIP, clientID);

	// Set magic byte
	client->magic = MAGICBYTE;
	// Set its counter to 1
	client->count = (count && !aliasclient)? 1 : 0;
	// Initialize blocked count to zero
	client->blockedcount = 0;
	// Store client IP - no need to check for NULL here as it doesn't harm
	client->ippos = addstr(clientIP);
	// Initialize client hostname
	// Due to the nature of us being the resolver,
	// the actual resolving of the host name has
	// to be done separately to be non-blocking
	client->flags.new = true;
	client->namepos = 0;
	set_event(RESOLVE_NEW_HOSTNAMES);
	// No query seen so far
	client->lastQuery = 0.0;
	client->numQueriesARP = client->count;
	// Configured groups are yet unknown
	client->flags.found_group = false;
	client->groupspos = 0u;
	// Store time this client was added, we re-read group settings
	// some time after adding a client to ensure we pick up possible
	// group configuration though hostname, MAC address or interface
	client->reread_groups = 0u;
	client->firstSeen = time(NULL);
	// Interface is not yet known
	client->ifacepos = 0;
	// Set all MAC address bytes to zero
	client->hwlen = -1;
	memset(client->hwaddr, 0, sizeof(client->hwaddr));
	// This may be an alias-client, the ID is set elsewhere
	client->flags.aliasclient = aliasclient;
	client->aliasclient_id = -1;

	// Initialize client-specific overTime data
	memset(client->overTime, 0, sizeof(client->overTime));

	// Store client ID
	client->id = clientID;

	// Increase counter by one
	counters->clients++;

	// Get groups for this client and set enabled regex filters
	// Note 1: We do this only after increasing the clients counter to
	//         ensure sufficient shared memory is available in the
	//         pre_client_regex object.
	// Note 2: We don't do this before starting up is done as the gravity
	//         database may not be available. All clients initialized
	//         during history reading get their enabled regexs reloaded
	//         in the initial call to FTL_reload_all_domainlists()
	if(!startup && !aliasclient)
		reload_per_client_regex(client);

	// Check if this client is managed by a alias-client
	if(!aliasclient)
		reset_aliasclient(NULL, client);

	return clientID;
}

void change_clientcount(clientsData *client, int total, int blocked, int overTimeIdx, int overTimeMod)
{
		client->count += total;
		client->blockedcount += blocked;
		if(overTimeIdx > -1 && overTimeIdx < OVERTIME_SLOTS)
		{
			overTime[overTimeIdx].total += overTimeMod;
			client->overTime[overTimeIdx] += overTimeMod;
		}

		// Also add counts to the connected alias-client (if any)
		if(client->flags.aliasclient)
		{
			log_warn("Should not add to alias-client directly (client \"%s\" (%s))!",
			         getstr(client->namepos), getstr(client->ippos));
			return;
		}
		if(client->aliasclient_id > -1)
		{
			clientsData *aliasclient = getClient(client->aliasclient_id, true);
			aliasclient->count += total;
			aliasclient->blockedcount += blocked;
			if(overTimeIdx > -1 && overTimeIdx < OVERTIME_SLOTS)
				aliasclient->overTime[overTimeIdx] += overTimeMod;
		}
}

static int get_next_free_cacheID(void)
{
	// Compare content of cache against known cache IP addresses
	for(int cacheID = 0; cacheID < counters->dns_cache_size; cacheID++)
	{
		// Get cache pointer
		DNSCacheData* cache = getDNSCache(cacheID, false);

		// Check if the returned pointer is valid before trying to access it
		if(cache == NULL)
			continue;

		// Check if the magic byte is set
		if(cache->magic == 0x00)
			return cacheID;
	}

	// If we did not return until here, then we need to allocate a new cache ID
	return counters->dns_cache_size;
}

int _findCacheID(const int domainID, const int clientID, const enum query_type query_type,
                 const bool create_new, const char *func, int line, const char *file)
{
	// Compare content of client against known client IP addresses
	for(int cacheID = 0; cacheID < counters->dns_cache_size; cacheID++)
	{
		// Get cache pointer
		DNSCacheData* dns_cache = _getDNSCache(cacheID, true, line, func, file);

		// Check if the returned pointer is valid before trying to access it
		if(dns_cache == NULL)
			continue;

		if(dns_cache->domainID == domainID &&
		   dns_cache->clientID == clientID &&
		   dns_cache->query_type == query_type)
		{
			return cacheID;
		}
	}

	if(!create_new)
		return -1;

	// Get ID of new cache entry
	const int cacheID = get_next_free_cacheID();

	// Get client pointer
	DNSCacheData* dns_cache = _getDNSCache(cacheID, false, line, func, file);

	if(dns_cache == NULL)
	{
		log_err("Encountered serious memory error in findCacheID()");
		return -1;
	}

	log_debug(DEBUG_GC, "New cache entry: domainID %d, clientID %d, query_type %d (ID %d)",
	          domainID, clientID, query_type, cacheID);

	// Initialize cache entry
	dns_cache->magic = MAGICBYTE;
	dns_cache->blocking_status = UNKNOWN_BLOCKED;
	dns_cache->domainID = domainID;
	dns_cache->clientID = clientID;
	dns_cache->query_type = query_type;
	dns_cache->force_reply = 0u;
	dns_cache->list_id = -1; // -1 = not set

	// Increase counter by one
	counters->dns_cache_size++;

	return cacheID;
}

bool isValidIPv4(const char *addr)
{
	struct sockaddr_in sa;
	return inet_pton(AF_INET, addr, &(sa.sin_addr)) != 0;
}

bool isValidIPv6(const char *addr)
{
	struct sockaddr_in6 sa;
	return inet_pton(AF_INET6, addr, &(sa.sin6_addr)) != 0;
}

// Privacy-level sensitive subroutine that returns the domain name
// only when appropriate for the requested query
const char *getDomainString(const queriesData* query)
{
	// Check if the returned pointer is valid before trying to access it
	if(query == NULL || query->domainID < 0)
		return "";

	if(query->privacylevel < PRIVACY_HIDE_DOMAINS)
	{
		// Get domain pointer
		const domainsData* domain = getDomain(query->domainID, true);

		// Check if the returned pointer is valid before trying to access it
		if(domain == NULL)
			return "";

		// Return string
		return getstr(domain->domainpos);
	}
	else
		return HIDDEN_DOMAIN;
}

// Privacy-level sensitive subroutine that returns the domain name
// only when appropriate for the requested query
const char *getCNAMEDomainString(const queriesData* query)
{
	// Check if the returned pointer is valid before trying to access it
	if(query == NULL || query->CNAME_domainID < 0)
		return "";

	if(query->privacylevel < PRIVACY_HIDE_DOMAINS)
	{
		// Get domain pointer
		const domainsData* domain = getDomain(query->CNAME_domainID, true);

		// Check if the returned pointer is valid before trying to access it
		if(domain == NULL)
			return "";

		// Return string
		return getstr(domain->domainpos);
	}
	else
		return HIDDEN_DOMAIN;
}

// Privacy-level sensitive subroutine that returns the client IP
// only when appropriate for the requested query
const char *getClientIPString(const queriesData* query)
{
	// Check if the returned pointer is valid before trying to access it
	if(query == NULL || query->clientID < 0)
		return "";

	if(query->privacylevel < PRIVACY_HIDE_DOMAINS_CLIENTS)
	{
		// Get client pointer
		const clientsData* client = getClient(query->clientID, true);

		// Check if the returned pointer is valid before trying to access it
		if(client == NULL)
			return "";

		// Return string
		return getstr(client->ippos);
	}
	else
		return HIDDEN_CLIENT;
}

// Privacy-level sensitive subroutine that returns the client host name
// only when appropriate for the requested query
const char *getClientNameString(const queriesData* query)
{
	// Check if the returned pointer is valid before trying to access it
	if(query == NULL || query->clientID < 0)
		return "";

	if(query->privacylevel < PRIVACY_HIDE_DOMAINS_CLIENTS)
	{
		// Get client pointer
		const clientsData* client = getClient(query->clientID, true);

		// Check if the returned pointer is valid before trying to access it
		if(client == NULL)
			return "";

		// Return string
		return getstr(client->namepos);
	}
	else
		return HIDDEN_CLIENT;
}

void FTL_reset_per_client_domain_data(void)
{
	log_debug(DEBUG_DATABASE, "Resetting per-client DNS cache, size is %i", counters->dns_cache_size);

	for(int cacheID = 0; cacheID < counters->dns_cache_size; cacheID++)
	{
		// Get cache pointer
		DNSCacheData* dns_cache = getDNSCache(cacheID, true);

		// Check if the returned pointer is valid before trying to access it
		if(dns_cache == NULL)
			continue;

		// Reset blocking status
		dns_cache->blocking_status = UNKNOWN_BLOCKED;
		// Reset domainlist ID
		dns_cache->list_id = -1;
	}
}

// Reloads all domainlists and performs a few extra tasks such as cleaning the
// message table
// May only be called from the database thread
void FTL_reload_all_domainlists(void)
{
	lock_shm();

	// (Re-)open gravity database connection
	gravityDB_reopen();

	// Get size of gravity, number of domains, groups, clients, and lists
	counters->database.gravity = gravityDB_count(GRAVITY_TABLE);
	counters->database.groups = gravityDB_count(GROUPS_TABLE);
	counters->database.clients = gravityDB_count(CLIENTS_TABLE);
	counters->database.lists = gravityDB_count(ADLISTS_TABLE);
	counters->database.bulklists = gravityDB_count(BULKLISTS_TABLE);
	counters->database.domains.allowed = gravityDB_count(DENIED_DOMAINS_TABLE);
	counters->database.domains.denied = gravityDB_count(ALLOWED_DOMAINS_TABLE);

	// Read and compile possible regex filters
	// only after having called gravityDB_open()
	read_regex_from_database();

	// Check for inaccessible adlist URLs
	check_inaccessible_adlists();

	// Reset FTL's internal DNS cache storing whether a specific domain
	// has already been validated for a specific user
	FTL_reset_per_client_domain_data();

	unlock_shm();
}

const char *get_query_type_str(const enum query_type type, const queriesData *query, char buffer[20])
{
	switch (type)
	{
		case TYPE_A:
			return "A";
		case TYPE_AAAA:
			return "AAAA";
		case TYPE_ANY:
			return "ANY";
		case TYPE_SRV:
			return "SRV";
		case TYPE_SOA:
			return "SOA";
		case TYPE_PTR:
			return "PTR";
		case TYPE_TXT:
			return "TXT";
		case TYPE_NAPTR:
			return "NAPTR";
		case TYPE_MX:
			return "MX";
		case TYPE_DS:
			return "DS";
		case TYPE_RRSIG:
			return "RRSIG";
		case TYPE_DNSKEY:
			return "DNSKEY";
		case TYPE_NS:
			return "NS";
		case TYPE_OTHER:
			if(query != NULL && buffer != NULL)
			{
				// Build custom query type string in buffer
				sprintf(buffer, "TYPE%d", query->qtype);
				return buffer;
			}
			else
			{
				// Used, e.g., for regex type matching
				return "OTHER";
			}
		case TYPE_SVCB:
			return "SVCB";
		case TYPE_HTTPS:
			return "HTTPS";
		case TYPE_MAX:
		default:
			return "N/A";
	}
}

const char * __attribute__ ((const)) get_query_status_str(const enum query_status status)
{
	switch (status)
	{
		case QUERY_UNKNOWN:
			return "UNKNOWN";
		case QUERY_GRAVITY:
			return "GRAVITY";
		case QUERY_FORWARDED:
			return "FORWARDED";
		case QUERY_CACHE:
			return "CACHE";
		case QUERY_REGEX:
			return "REGEX";
		case QUERY_DENYLIST:
			return "DENYLIST";
		case QUERY_EXTERNAL_BLOCKED_IP:
			return "EXTERNAL_BLOCKED_IP";
		case QUERY_EXTERNAL_BLOCKED_NULL:
			return "EXTERNAL_BLOCKED_NULL";
		case QUERY_EXTERNAL_BLOCKED_NXRA:
			return "EXTERNAL_BLOCKED_NXRA";
		case QUERY_GRAVITY_CNAME:
			return "GRAVITY_CNAME";
		case QUERY_REGEX_CNAME:
			return "REGEX_CNAME";
		case QUERY_DENYLIST_CNAME:
			return "DENYLIST_CNAME";
		case QUERY_RETRIED:
			return "RETRIED";
		case QUERY_RETRIED_DNSSEC:
			return "RETRIED_DNSSEC";
		case QUERY_IN_PROGRESS:
			return "IN_PROGRESS";
		case QUERY_DBBUSY:
			return "DBBUSY";
		case QUERY_SPECIAL_DOMAIN:
			return "SPECIAL_DOMAIN";
		case QUERY_CACHE_STALE:
			return "CACHE_STALE";
		case QUERY_STATUS_MAX:
		default:
			return "INVALID";
	}
}

const char * __attribute__ ((const)) get_query_reply_str(const enum reply_type reply)
{
	switch (reply)
	{
		case REPLY_UNKNOWN:
			return "UNKNOWN";
		case REPLY_NODATA:
			return "NODATA";
		case REPLY_NXDOMAIN:
			return "NXDOMAIN";
		case REPLY_CNAME:
			return "CNAME";
		case REPLY_IP:
			return "IP";
		case REPLY_DOMAIN:
			return "DOMAIN";
		case REPLY_RRNAME:
			return "RRNAME";
		case REPLY_SERVFAIL:
			return "SERVFAIL";
		case REPLY_REFUSED:
			return "REFUSED";
		case REPLY_NOTIMP:
			return "NOTIMP";
		case REPLY_OTHER:
			return "OTHER";
		case REPLY_DNSSEC:
			return "DNSSEC";
		case REPLY_NONE:
			return "NONE";
		case REPLY_BLOB:
			return "BLOB";
		case QUERY_REPLY_MAX:
		default:
			return "N/A";
	}
}

const char * __attribute__ ((const)) get_query_dnssec_str(const enum dnssec_status dnssec)
{
	switch (dnssec)
	{
		case DNSSEC_UNKNOWN:
			return "UNKNOWN";
		case DNSSEC_SECURE:
			return "SECURE";
		case DNSSEC_INSECURE:
			return "INSECURE";
		case DNSSEC_BOGUS:
			return "BOGUS";
		case DNSSEC_ABANDONED:
			return "ABANDONED";
		case DNSSEC_TRUNCATED:
			return "TRUNCATED";
		case DNSSEC_MAX:
		default:
			return "N/A";
	}
}

const char * __attribute__ ((const)) get_refresh_hostnames_str(const enum refresh_hostnames refresh)
{
	switch (refresh)
	{
		case REFRESH_ALL:
			return "ALL";
		case REFRESH_IPV4_ONLY:
			return "IPV4_ONLY";
		case REFRESH_UNKNOWN:
			return "UNKNOWN";
		case REFRESH_NONE:
			return "NONE";
		default:
			return "N/A";
	}
}

int __attribute__ ((pure)) get_refresh_hostnames_val(const char *refresh_hostnames)
{
	if(strcasecmp(refresh_hostnames, "ALL") == 0)
		return REFRESH_ALL;
	else if(strcasecmp(refresh_hostnames, "IPV4_ONLY") == 0)
		return REFRESH_IPV4_ONLY;
	else if(strcasecmp(refresh_hostnames, "UNKNOWN") == 0)
		return REFRESH_UNKNOWN;
	else if(strcasecmp(refresh_hostnames, "NONE") == 0)
		return REFRESH_NONE;

	// Invalid value
	return -1;
}

const char * __attribute__ ((const)) get_blocking_mode_str(const enum blocking_mode mode)
{
	switch (mode)
	{
		case MODE_IP:
			return "IP";
		case MODE_NX:
			return "NX";
		case MODE_NULL:
			return "NULL";
		case MODE_IP_NODATA_AAAA:
			return "IP_NODATA_AAAA";
		case MODE_NODATA:
			return "NODATA";
		case MODE_MAX:
		default:
			return "N/A";
	}
}

int __attribute__ ((pure)) get_blocking_mode_val(const char *blocking_mode)
{
	if(strcasecmp(blocking_mode, "IP") == 0)
		return MODE_IP;
	else if(strcasecmp(blocking_mode, "NX") == 0)
		return MODE_NX;
	else if(strcasecmp(blocking_mode, "NULL") == 0)
		return MODE_NULL;
	else if(strcasecmp(blocking_mode, "IP_NODATA_AAAA") == 0)
		return MODE_IP_NODATA_AAAA;
	else if(strcasecmp(blocking_mode, "NODATA") == 0)
		return MODE_NODATA;

	// Invalid value
	return -1;
}

bool __attribute__ ((const)) is_blocked(const enum query_status status)
{
	switch (status)
	{
		case QUERY_UNKNOWN:
		case QUERY_FORWARDED:
		case QUERY_CACHE:
		case QUERY_RETRIED:
		case QUERY_RETRIED_DNSSEC:
		case QUERY_IN_PROGRESS:
		case QUERY_CACHE_STALE:
		case QUERY_STATUS_MAX:
		default:
			return false;

		case QUERY_GRAVITY:
		case QUERY_REGEX:
		case QUERY_DENYLIST:
		case QUERY_EXTERNAL_BLOCKED_IP:
		case QUERY_EXTERNAL_BLOCKED_NULL:
		case QUERY_EXTERNAL_BLOCKED_NXRA:
		case QUERY_GRAVITY_CNAME:
		case QUERY_REGEX_CNAME:
		case QUERY_DENYLIST_CNAME:
		case QUERY_DBBUSY:
		case QUERY_SPECIAL_DOMAIN:
			return true;
	}
}

static char blocked_list[32] = { 0 };
const char * __attribute__ ((pure)) get_blocked_statuslist(void)
{
	if(blocked_list[0] != '\0')
		return blocked_list;

	// Build a list of blocked query statuses
	unsigned int first = 0;
	// Open parenthesis
	blocked_list[0] = '(';
	for(enum query_status status = 0; status < QUERY_STATUS_MAX; status++)
		if(is_blocked(status))
			snprintf(blocked_list + strlen(blocked_list),
			         sizeof(blocked_list) - strlen(blocked_list),
			         "%s%d", first++ < 1 ? "" : ",", status);

	// Close parenthesis
	const size_t len = strlen(blocked_list);
	blocked_list[len] = ')';
	blocked_list[len + 1] = '\0';
	return blocked_list;
}

static char cached_list[32] = { 0 };
const char * __attribute__ ((pure)) get_cached_statuslist(void)
{
	if(cached_list[0] != '\0')
		return cached_list;

	// Build a list of cached query statuses
	unsigned int first = 0;
	// Open parenthesis
	cached_list[0] = '(';
	for(enum query_status status = 0; status < QUERY_STATUS_MAX; status++)
		if(is_cached(status))
			snprintf(cached_list + strlen(cached_list),
			         sizeof(cached_list) - strlen(cached_list),
			         "%s%d", first++ < 1 ? "" : ",", status);

	// Close parenthesis
	const size_t len = strlen(cached_list);
	cached_list[len] = ')';
	cached_list[len + 1] = '\0';
	return cached_list;
}

int __attribute__ ((pure)) get_blocked_count(void)
{
	int blocked = 0;
	for(enum query_status status = 0; status < QUERY_STATUS_MAX; status++)
		if(is_blocked(status))
			blocked += counters->status[status];

	return blocked;
}

int __attribute__ ((pure)) get_forwarded_count(void)
{
	return counters->status[QUERY_FORWARDED] +
	       counters->status[QUERY_RETRIED] +
	       counters->status[QUERY_RETRIED_DNSSEC];
}

int __attribute__ ((pure)) get_cached_count(void)
{
	return counters->status[QUERY_CACHE] + counters->status[QUERY_CACHE_STALE];
}

bool __attribute__ ((const)) is_cached(const enum query_status status)
{
	switch (status)
	{
		case QUERY_CACHE:
		case QUERY_CACHE_STALE:
			return true;

		case QUERY_UNKNOWN:
		case QUERY_FORWARDED:
		case QUERY_RETRIED:
		case QUERY_RETRIED_DNSSEC:
		case QUERY_IN_PROGRESS:
		case QUERY_STATUS_MAX:
		case QUERY_GRAVITY:
		case QUERY_REGEX:
		case QUERY_DENYLIST:
		case QUERY_EXTERNAL_BLOCKED_IP:
		case QUERY_EXTERNAL_BLOCKED_NULL:
		case QUERY_EXTERNAL_BLOCKED_NXRA:
		case QUERY_GRAVITY_CNAME:
		case QUERY_REGEX_CNAME:
		case QUERY_DENYLIST_CNAME:
		case QUERY_DBBUSY:
		case QUERY_SPECIAL_DOMAIN:
		default:
			return false;
	}
}

static const char* __attribute__ ((const)) query_status_str(const enum query_status status)
{
	switch (status)
	{
		case QUERY_UNKNOWN:
			return "UNKNOWN";
		case QUERY_GRAVITY:
			return "GRAVITY";
		case QUERY_FORWARDED:
			return "FORWARDED";
		case QUERY_CACHE:
			return "CACHE";
		case QUERY_REGEX:
			return "REGEX";
		case QUERY_DENYLIST:
			return "DENYLIST";
		case QUERY_EXTERNAL_BLOCKED_IP:
			return "EXTERNAL_BLOCKED_IP";
		case QUERY_EXTERNAL_BLOCKED_NULL:
			return "EXTERNAL_BLOCKED_NULL";
		case QUERY_EXTERNAL_BLOCKED_NXRA:
			return "EXTERNAL_BLOCKED_NXRA";
		case QUERY_GRAVITY_CNAME:
			return "GRAVITY_CNAME";
		case QUERY_REGEX_CNAME:
			return "REGEX_CNAME";
		case QUERY_DENYLIST_CNAME:
			return "DENYLIST_CNAME";
		case QUERY_RETRIED:
			return "RETRIED";
		case QUERY_RETRIED_DNSSEC:
			return "RETRIED_DNSSEC";
		case QUERY_IN_PROGRESS:
			return "IN_PROGRESS";
		case QUERY_DBBUSY:
			return "DBBUSY";
		case QUERY_SPECIAL_DOMAIN:
			return "SPECIAL_DOMAIN";
		case QUERY_CACHE_STALE:
			return "CACHE_STALE";
		case QUERY_STATUS_MAX:
			return NULL;
	}
	return NULL;
}

void _query_set_status(queriesData *query, const enum query_status new_status, const bool init,
                       const char *func, const int line, const char *file)
{
	// Debug logging
	if(config.debug.status.v.b)
	{
		if(init)
		{
			const char *newstr = new_status < QUERY_STATUS_MAX ? query_status_str(new_status) : "INVALID";
			log_debug(DEBUG_STATUS, "Query %i: status initialized: %s (%d) in %s() (%s:%i)",
			          query->id, newstr, new_status, func, short_path(file), line);
		}
		else if(query->status == new_status)
		{
			const char *oldstr = query->status < QUERY_STATUS_MAX ? query_status_str(query->status) : "INVALID";
			log_debug(DEBUG_STATUS, "Query %i: status unchanged: %s (%d) in %s() (%s:%i)",
			          query->id, oldstr, query->status, func, short_path(file), line);
		}
		else
		{
			const char *oldstr = query->status < QUERY_STATUS_MAX ? query_status_str(query->status) : "INVALID";
			const char *newstr = new_status < QUERY_STATUS_MAX ? query_status_str(new_status) : "INVALID";
			log_debug(DEBUG_STATUS, "Query %i: status changed: %s (%d) -> %s (%d) in %s() (%s:%i)",
			          query->id, oldstr, query->status, newstr, new_status, func, short_path(file), line);
		}
	}

	// Sanity check
	if(new_status >= QUERY_STATUS_MAX)
		return;

	const enum query_status old_status = query->status;
	if(old_status == new_status && !init)
	{
		// Nothing to do
		return;
	}

	// else: update global counters, ...
	if(!init)
	{
		counters->status[old_status]--;
		log_debug(DEBUG_STATUS, "status %d removed (!init), ID = %d, new count = %d", QUERY_UNKNOWN, query->id, counters->status[QUERY_UNKNOWN]);
	}
	counters->status[new_status]++;
	log_debug(DEBUG_STATUS, "status %d set, ID = %d, new count = %d", new_status, query->id, counters->status[new_status]);

	// ... update overTime counters, ...
	const int timeidx = getOverTimeID(query->timestamp);
	if(is_blocked(old_status) && !init)
		overTime[timeidx].blocked--;
	if(is_blocked(new_status))
		overTime[timeidx].blocked++;

	if((old_status == QUERY_CACHE || old_status == QUERY_CACHE_STALE) && !init)
		overTime[timeidx].cached--;
	if(new_status == QUERY_CACHE || new_status == QUERY_CACHE_STALE)
		overTime[timeidx].cached++;

	if(old_status == QUERY_FORWARDED && !init)
		overTime[timeidx].forwarded--;
	if(new_status == QUERY_FORWARDED)
		overTime[timeidx].forwarded++;

	// ... and set new status
	query->status = new_status;
}

const char * __attribute__ ((const)) get_ptr_type_str(const enum ptr_type piholePTR)
{
	switch(piholePTR)
	{
		case PTR_PIHOLE:
			return "PI.HOLE";
		case PTR_HOSTNAME:
			return "HOSTNAME";
		case PTR_HOSTNAMEFQDN:
			return "HOSTNAMEFQDN";
		case PTR_NONE:
			return "NONE";
	}
	return NULL;
}

int __attribute__ ((pure)) get_ptr_type_val(const char *piholePTR)
{
	if(strcasecmp(piholePTR, "pi.hole") == 0)
		return PTR_PIHOLE;
	else if(strcasecmp(piholePTR, "hostname") == 0)
		return PTR_HOSTNAME;
	else if(strcasecmp(piholePTR, "hostnamefqdn") == 0)
		return PTR_HOSTNAMEFQDN;
	else if(strcasecmp(piholePTR, "none") == 0 ||
		strcasecmp(piholePTR, "false") == 0)
		return PTR_NONE;

	// Invalid value
	return -1;
}

const char * __attribute__ ((const)) get_busy_reply_str(const enum busy_reply replyWhenBusy)
{
	switch(replyWhenBusy)
	{
		case BUSY_BLOCK:
			return "BLOCK";
		case BUSY_ALLOW:
			return "ALLOW";
		case BUSY_REFUSE:
			return "REFUSE";
		case BUSY_DROP:
			return "DROP";
	}
	return NULL;
}

int __attribute__ ((pure)) get_busy_reply_val(const char *replyWhenBusy)
{
	if(strcasecmp(replyWhenBusy, "BLOCK") == 0)
		return BUSY_BLOCK;
	else if(strcasecmp(replyWhenBusy, "ALLOW") == 0)
		return BUSY_ALLOW;
	else if(strcasecmp(replyWhenBusy, "REFUSE") == 0)
		return BUSY_REFUSE;
	else if(strcasecmp(replyWhenBusy, "DROP") == 0)
		return BUSY_DROP;

	// Invalid value
	return -1;
}

const char * __attribute__ ((const)) get_listeningMode_str(const enum listening_mode listeningMode)
{
	switch(listeningMode)
	{
		case LISTEN_LOCAL:
			return "LOCAL";
		case LISTEN_ALL:
			return "ALL";
		case LISTEN_SINGLE:
			return "SINGLE";
		case LISTEN_BIND:
			return "BIND";
		case LISTEN_NONE:
			return "NONE";
	}
	return NULL;
}

int __attribute__ ((pure)) get_listeningMode_val(const char *listeningMode)
{
	if(strcasecmp(listeningMode, "LOCAL") == 0)
		return LISTEN_LOCAL;
	else if(strcasecmp(listeningMode, "ALL") == 0)
		return LISTEN_ALL;
	else if(strcasecmp(listeningMode, "SINGLE") == 0)
		return LISTEN_SINGLE;
	else if(strcasecmp(listeningMode, "BIND") == 0)
		return LISTEN_BIND;
	else if(strcasecmp(listeningMode, "NONE") == 0)
		return LISTEN_NONE;

	// Invalid value
	return -1;
}

const char * __attribute__ ((const)) get_temp_unit_str(const enum temp_unit temp_unit)
{
	switch(temp_unit)
	{
		case TEMP_UNIT_C:
			return "C";
		case TEMP_UNIT_F:
			return "F";
		case TEMP_UNIT_K:
			return "K";
	}
	return NULL;
}

int __attribute__ ((pure)) get_temp_unit_val(const char *temp_unit)
{
	if(strcasecmp(temp_unit, "C") == 0)
		return TEMP_UNIT_C;
	else if(strcasecmp(temp_unit, "F") == 0)
		return TEMP_UNIT_F;
	else if(strcasecmp(temp_unit, "K") == 0)
		return TEMP_UNIT_K;

	// Invalid value
	return -1;
}
