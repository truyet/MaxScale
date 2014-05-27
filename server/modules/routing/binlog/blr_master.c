/*
 * This file is distributed as part of MaxScale.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright SkySQL Ab 2014
 */

/**
 * @file blr_master.c - contains code for the router to master communication
 *
 * The binlog router is designed to be used in replication environments to
 * increase the replication fanout of a master server. It provides a transparant
 * mechanism to read the binlog entries for multiple slaves while requiring
 * only a single connection to the actual master to support the slaves.
 *
 * The current prototype implement is designed to support MySQL 5.6 and has
 * a number of limitations. This prototype is merely a proof of concept and
 * should not be considered production ready.
 *
 * @verbatim
 * Revision History
 *
 * Date		Who		Description
 * 02/04/2014	Mark Riddoch		Initial implementation
 *
 * @endverbatim
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <service.h>
#include <server.h>
#include <router.h>
#include <atomic.h>
#include <session.h>
#include <blr.h>
#include <dcb.h>
#include <spinlock.h>

#include <skygw_types.h>
#include <skygw_utils.h>
#include <log_manager.h>

/* Temporary requirement for auth data */
#include <mysql_client_server_protocol.h>

extern int lm_enabled_logfiles_bitmask;

static GWBUF *blr_make_query(char *statement);
static GWBUF *blr_make_registration(ROUTER_INSTANCE *router);
static GWBUF *blr_make_binlog_dump(ROUTER_INSTANCE *router);
static void encode_value(unsigned char *data, unsigned int value, int len);
static void blr_handle_binlog_record(ROUTER_INSTANCE *router, GWBUF *pkt);
static void blr_rotate_event(ROUTER_INSTANCE *router, uint8_t *pkt, REP_HEADER *hdr);
static void blr_distribute_binlog_record(ROUTER_INSTANCE *router, REP_HEADER *hdr, uint8_t *ptr);
static void *CreateMySQLAuthData(char *username, char *password, char *database);
static void blr_extract_header(uint8_t *pkt, REP_HEADER *hdr);
static uint32_t extract_field(uint8_t *src, int bits);

/**
 * blr_start_master - controls the connection of the binlog router to the
 * master MySQL server and triggers the slave registration process for
 * the router.
 *
 * @param	router		The router instance
 */
void
blr_start_master(ROUTER_INSTANCE *router)
{
DCB	*client;
GWBUF	*buf;

	client = dcb_alloc(DCB_ROLE_INTERNAL);
	client->data = CreateMySQLAuthData(router->user, router->password, "");
	router->session = session_alloc(router->service, client);
	client->session = router->session;
	router->master = dcb_connect(router->service->databases, router->session, BLR_PROTOCOL);
	router->master_state = BLRM_AUTHENTICATED;
	buf = blr_make_query("SELECT UNIX_TIMESTAMP()");
	router->master->func.write(router->master, buf);
	router->master_state = BLRM_TIMESTAMP;
}

/**
 * Binlog router master side state machine event handler.
 *
 * Handles an incoming response from the master server to the binlog
 * router.
 *
 * @param router	The router instance
 * @param buf		The incoming packet
 */
void
blr_master_response(ROUTER_INSTANCE *router, GWBUF *buf)
{
char	query[128];

	/*
	 * We need to make sure that incoming packets (gwbufs) are
	 * strictly processed in order and that we do not have packets
	 * from the same master being processed on multiple threads.
	 * To do this we create a queue of the GWBUF structures and have
	 * a flag that indicates if this routine is processing a packet
	 * on another thread. Items will be added to the queue if the
	 * routine is running in another thread. That thread will read
	 * the queue before returning.
	 *
	 * The action of adding items to the queue is protected by a
	 * spinlock and a flag that inidicates if the routine running
	 * in the other thread has reached the point at which it will
	 * no longer check the queue before returning. In order to
	 * manipulate the queue or the flag the router spinlock must
	 * be held.
	 */
	spinlock_acquire(&router->lock);
	if (router->active_logs)
	{
		/*
		 * Thread already processing a packet and has not got
		 * to the point that it will not look at new packets
		 * added to the queue.
		 */
		router->queue = gwbuf_append(router->queue, buf);
		spinlock_release(&router->lock);
		return;
	}
	else
	{
		router->active_logs = 1;
	}
	spinlock_release(&router->lock);

	if (router->master_state < 0 || router->master_state > BLRM_MAXSTATE)
	{
        	LOGIF(LM, (skygw_log_write(
                           LOGFILE_ERROR, "Invalid master state machine state (%d) for binlog router.\n",
					router->master_state)));
		gwbuf_consume(buf, gwbuf_length(buf));
		spinlock_acquire(&router->lock);
		router->active_logs = 0;
		spinlock_release(&router->lock);
		return;
	}

	if (MYSQL_RESPONSE_ERR(buf))
	{
        	LOGIF(LM, (skygw_log_write(
                           LOGFILE_ERROR,
			"Received error: %d, %s from master during %s phase of the master state machine.\n",
			MYSQL_ERROR_CODE(buf), MYSQL_ERROR_MSG(buf), blrm_states[router->master_state]
			)));
		gwbuf_consume(buf, gwbuf_length(buf));
		spinlock_acquire(&router->lock);
		router->active_logs = 0;
		spinlock_release(&router->lock);
		return;
	}
	do {
		switch (router->master_state)
		{
		case BLRM_TIMESTAMP:
			// Response to a timestamp message, no need to save this.
			gwbuf_consume(buf, GWBUF_LENGTH(buf));
			buf = blr_make_query("SHOW VARIABLES LIKE 'SERVER_ID'");
			router->master_state = BLRM_SERVERID;
			router->master->func.write(router->master, buf);
			break;
		case BLRM_SERVERID:
			// Response to fetch of master's server-id
			router->saved_master.server_id = buf;
			// TODO: Extract the value of server-id and place in router->master_id
			buf = blr_make_query("SET @master_heartbeat_period = 1799999979520");
			router->master_state = BLRM_HBPERIOD;
			router->master->func.write(router->master, buf);
			break;
		case BLRM_HBPERIOD:
			// Response to set the heartbeat period
			router->saved_master.heartbeat = buf;
			buf = blr_make_query("SET @master_binlog_checksum = @@global.binlog_checksum");
			router->master_state = BLRM_CHKSUM1;
			router->master->func.write(router->master, buf);
			break;
		case BLRM_CHKSUM1:
			// Response to set the master binlog checksum
			router->saved_master.chksum1 = buf;
			buf = blr_make_query("SELECT @master_binlog_checksum");
			router->master_state = BLRM_CHKSUM2;
			router->master->func.write(router->master, buf);
			break;
		case BLRM_CHKSUM2:
			// Response to the master_binlog_checksum, should be stored
			router->saved_master.chksum2 = buf;
			buf = blr_make_query("SELECT @@GLOBAL.GTID_MODE");
			router->master_state = BLRM_GTIDMODE;
			router->master->func.write(router->master, buf);
			break;
		case BLRM_GTIDMODE:
			// Response to the GTID_MODE, should be stored
			router->saved_master.gtid_mode = buf;
			buf = blr_make_query("SHOW VARIABLES LIKE 'SERVER_UUID'");
			router->master_state = BLRM_MUUID;
			router->master->func.write(router->master, buf);
			break;
		case BLRM_MUUID:
			// Response to the SERVER_UUID, should be stored
			router->saved_master.uuid = buf;
			sprintf(query, "SET @slave_uuid='%s'", router->uuid);
			buf = blr_make_query(query);
			router->master_state = BLRM_SUUID;
			router->master->func.write(router->master, buf);
			break;
		case BLRM_SUUID:
			// Response to the SET @server_uuid, should be stored
			router->saved_master.setslaveuuid = buf;
			buf = blr_make_query("SET NAMES latin1");
			router->master_state = BLRM_LATIN1;
			router->master->func.write(router->master, buf);
			break;
		case BLRM_LATIN1:
			// Response to the SET NAMES latin1, should be stored
			router->saved_master.setnames = buf;
			buf = blr_make_registration(router);
			router->master_state = BLRM_REGISTER;
			router->master->func.write(router->master, buf);
			break;
		case BLRM_REGISTER:
			// Request a dump of the binlog file
			buf = blr_make_binlog_dump(router);
			router->master_state = BLRM_BINLOGDUMP;
			router->master->func.write(router->master, buf);
			break;
		case BLRM_BINLOGDUMP:
			// Main body, we have received a binlog record from the master
			blr_handle_binlog_record(router, buf);
			break;
		}

		/*
		 * Check for messages queued by other threads.
		 */
		spinlock_acquire(&router->lock);
		if ((buf = router->queue) != NULL)
		{
			router->queue = buf->next;
			buf->next = NULL;
		}
		else
		{
			router->active_logs = 0;
		}
		spinlock_release(&router->lock);
	} while (buf != NULL);
}

/**
 * Build a MySQL query into a GWBUF that we can send to the master database
 *
 * @param	query		The text of the query to send
 */
static GWBUF *
blr_make_query(char *query)
{
GWBUF		*buf;
unsigned char	*data;
int		len;

	if ((buf = gwbuf_alloc(strlen(query) + 5)) == NULL)
		return NULL;
	data = GWBUF_DATA(buf);
	len = strlen(query) + 1;
	encode_value(&data[0], len, 24);	// Payload length
	data[3] = 0;				// Sequence id
						// Payload
	data[4] = COM_QUERY;			// Command
	memcpy(&data[5], query, strlen(query));

	return buf;
}

/**
 * Build a MySQL slave registration into a GWBUF that we can send to the
 * master database
 *
 * @param	router 	The router instance
 * @return	A MySQL Replication registration message in a GWBUF structure
 */
static GWBUF *
blr_make_registration(ROUTER_INSTANCE *router)
{
GWBUF		*buf;
unsigned char	*data;
int		len = 18;

	if ((buf = gwbuf_alloc(len + 4)) == NULL)
		return NULL;
	data = GWBUF_DATA(buf);
	encode_value(&data[0], len, 24);				// Payload length
	data[3] = 0;							// Sequence ID
	data[4] = COM_REGISTER_SLAVE;					// Command
	encode_value(&data[5], router->serverid, 32);			// Slave Server ID
	data[9] = 0;							// Slave hostname length
	data[10] = 0;							// Slave username length
	data[11] = 0;							// Slave password length
	encode_value(&data[12], router->service->ports->port, 16);	// Slave master port
	encode_value(&data[14], 0, 32);					// Replication rank
	encode_value(&data[18], router->masterid, 32);			// Master server-id

	return buf;
}


/**
 * Build a Binlog dump command into a GWBUF that we can send to the
 * master database
 *
 * @param	router 	The router instance
 * @return	A MySQL Replication COM_BINLOG_DUMP message in a GWBUF structure
 */
static GWBUF *
blr_make_binlog_dump(ROUTER_INSTANCE *router)
{
GWBUF		*buf;
unsigned char	*data;
int		len = 0x1b;

	if ((buf = gwbuf_alloc(len + 4)) == NULL)
		return NULL;
	data = GWBUF_DATA(buf);

	encode_value(&data[0], len,24);					// Payload length
	data[3] = 0;							// Sequence ID
	data[4] = COM_BINLOG_DUMP;					// Command
	encode_value(&data[5], router->binlog_position, 32);		// binlog position
	encode_value(&data[9], 0, 16);					// Flags
	encode_value(&data[11], router->serverid, 32);			// Server-id of MaxScale
	strncpy((char *)&data[15], router->binlog_name,
						BINLOG_FNAMELEN);	// binlog filename
	return buf;
}


/**
 * Encode a value into a number of bits in a MySQL packet
 *
 * @param	data	Point to location in target packet
 * @param	value	The value to pack
 * @param	len	Number of bits to encode value into
 */
static void
encode_value(unsigned char *data, unsigned int value, int len)
{
	while (len > 0)
	{
		*data++ = value & 0xff;
		value >>= 8;
		len -= 8;
	}
}

/**
 * blr_handle_binlog_record - we have received binlog records from
 * the master and we must now work out what to do with them.
 *
 * @param router	The router instance
 * @param pkt		The binlog records
 */
static void
blr_handle_binlog_record(ROUTER_INSTANCE *router, GWBUF *pkt)
{
uint8_t		*msg = NULL, *ptr, *pdata;
REP_HEADER	hdr;
int		len, reslen;
int		no_residual = 1;

	/* Prepend any residual buffer to the buffer chain we have been called with. */
	if (router->residual)
	{
		pkt = gwbuf_append(router->residual, pkt);
		router->residual = NULL;
		no_residual = 0;
	}

	while (pkt && gwbuf_length(pkt) > 24)
	{
		reslen = GWBUF_LENGTH(pkt);
		pdata = GWBUF_DATA(pkt);
		if (reslen < 3)	// Payload length straddles buffers
		{
			/* Get the length of the packet from the residual and new packet */
			if (reslen >= 3)
			{
				len = extract_field(pdata, 24);
			}
			else if (reslen == 2)
			{
				len = extract_field(pdata, 16);
				len |= (extract_field(GWBUF_DATA(pkt->next), 8) << 16);
			}
			else if (reslen == 1)
			{
				len = extract_field(pdata, 8);
				len |= (extract_field(GWBUF_DATA(pkt->next), 16) << 8);
			}
			len += 4; 	// Allow space for the header
		}
		else
		{
			len = extract_field(pdata, 24) + 4;
		}

		if (reslen < len && gwbuf_length(pkt) >= len)
		{
			/*
			 * The message is contianed in more than the current
			 * buffer, however we have the complete messasge in
			 * this buffer and the chain of remaining buffers.
			 *
			 * Allocate a contiguous buffer for the binlog message
			 * and copy the complete message into this buffer.
			 */
			msg = malloc(len);

			if (GWBUF_LENGTH(pkt->next) < len - reslen)
				printf("Packet (length %d) spans more than 2 buffers\n", len);
			memcpy(msg, pdata, reslen);
			memcpy(&msg[reslen], GWBUF_DATA(pkt->next), len - reslen);
			ptr = msg;
		}
		else if (reslen < len)
		{
			/*
			 * The message is not fully contained in the current
			 * and we do not have the complete message in the
			 * buffer chain. Therefore we must stop processing until
			 * we receive the next buffer.
			 */
			break;
		}
		else
		{
			/*
			 * The message is fully contained in the current buffer
			 */
			ptr = pdata;
		}

		blr_extract_header(ptr, &hdr);
		if (hdr.ok == 0)
		{
			router->stats.n_binlogs++;

// #define SHOW_EVENTS
#ifdef SHOW_EVENTS
			printf("blr: event type 0x%02x, flags 0x%04x, event size %d\n", hdr.event_type, hdr.flags, hdr.event_size);
#endif
			if (hdr.event_type >= 0 && hdr.event_type < 0x24)
				router->stats.events[hdr.event_type]++;
			if (hdr.event_type == FORMAT_DESCRIPTION_EVENT && hdr.next_pos == 0)
			{
				// Fake format description message
				router->stats.n_fakeevents++;
				if (hdr.event_type == FORMAT_DESCRIPTION_EVENT)
				{
					/*
					 * We need to save this to replay to new
					 * slaves that attach later.
					 */
					if (router->saved_master.fde_event)
						free(router->saved_master.fde_event);
					router->saved_master.fde_len = hdr.event_size;
					router->saved_master.fde_event = malloc(hdr.event_size);
					if (router->saved_master.fde_event)
						memcpy(router->saved_master.fde_event,
								ptr + 5, hdr.event_size);
				}
			}
			else
			{
				if (hdr.event_type == HEARTBEAT_EVENT)
				{
#ifdef SHOW_EVENTS
					printf("Replication heartbeat\n");
#endif
					;
				}
				else if (hdr.flags != LOG_EVENT_ARTIFICIAL_F)
				{
					ptr = ptr + 5;	// We don't put the first byte of the payload
							// into the binlog file
					blr_write_binlog_record(router, &hdr, ptr);
					if (hdr.event_type == ROTATE_EVENT)
					{
						blr_rotate_event(router, ptr, &hdr);
					}
					blr_distribute_binlog_record(router, &hdr, ptr);
				}
				else
				{
					ptr += 5;
					if (hdr.event_type == ROTATE_EVENT)
					{
						blr_rotate_event(router, ptr, &hdr);
					}
				}
			}
		}
		else
		{
			printf("Binlog router error: %s\n", &ptr[7]);
			router->stats.n_binlog_errors++;
		}

		if (msg)
		{
			free(msg);
			msg = NULL;
			pkt = gwbuf_consume(pkt, reslen);
			pkt = gwbuf_consume(pkt, len - reslen);
		}
		else
		{
			pkt = gwbuf_consume(pkt, len);
		}
	}

	/*
	 * Check if we have a residual, part binlog message to deal with.
	 * Just simply store the GWBUF for next time
	 */
	if (pkt)
	{
		router->residual = pkt;
	}
	blr_file_flush(router);
}

/**
 * Populate a header structure for a replication message from a GWBUF structure.
 *
 * @param pkt	The incoming packet in a GWBUF chain
 * @param hdr	The packet header to populate
 */
static void
blr_extract_header(uint8_t *ptr, REP_HEADER *hdr)
{

	hdr->payload_len = extract_field(ptr, 24);
	hdr->seqno = ptr[3];
	hdr->ok = ptr[4];
	hdr->timestamp = extract_field(&ptr[5], 32);
	hdr->event_type = ptr[9];
	hdr->serverid = extract_field(&ptr[10], 32);
	hdr->event_size = extract_field(&ptr[14], 32);
	hdr->next_pos = extract_field(&ptr[18], 32);
	hdr->flags = extract_field(&ptr[22], 16);
}

/** 
 * Extract a numeric field from a packet of the specified number of bits
 *
 * @param src	The raw packet source
 * @param birs	The number of bits to extract (multiple of 8)
 */
static uint32_t
extract_field(uint8_t *src, int bits)
{
uint32_t	rval = 0, shift = 0;

	while (bits > 0)
	{
		rval |= (*src++) << shift;
		shift += 8;
		bits -= 8;
	}
	return rval;
}

/**
 * Process a binlog rotate event.
 *
 * @param router	The instance of the router
 * @param ptr		The packet containing the rotate event
 * @param hdr		The replication message header
 */
static void
blr_rotate_event(ROUTER_INSTANCE *router, uint8_t *ptr, REP_HEADER *hdr)
{
int		len;
uint64_t	pos;
char		file[BINLOG_FNAMELEN+1];

	ptr += 19;		// Skip event header
	len = hdr->event_size - 19;	// Event size minus header
	pos = extract_field(ptr, 32) + (extract_field(ptr+4, 32) << 32);
	memcpy(file, ptr + 8, BINLOG_FNAMELEN);
	file[BINLOG_FNAMELEN] = 0;

#ifdef VEBOSE_ROTATE
	printf("binlog rotate: ");
	while (len--)
		printf("0x%02x ", *ptr++);
	printf("\n");
	printf("New file: %s @ %ld\n", file, pos);
#endif

	if (strncmp(router->binlog_name, file, BINLOG_FNAMELEN) != 0)
	{
		router->stats.n_rotates++;
		blr_file_rotate(router, file, pos);
	}
}

/**
 * Create the auth data needed to be able to call dcb_connect.
 * 
 * This doesn't really belong here and should be moved at some stage.
 */
static void *
CreateMySQLAuthData(char *username, char *password, char *database)
{
MYSQL_session	*auth_info;

	if ((auth_info = calloc(1, sizeof(MYSQL_session))) == NULL)
		return NULL;
	strcpy(auth_info->user, username);
	strcpy(auth_info->db, database);
	gw_sha1_str((const uint8_t *)password, strlen(password), auth_info->client_sha1);

	return auth_info;
}

/**
 * Distribute the binlog record we have just received to all the registered slaves.
 *
 * @param	router		The router instance
 * @param	hdr		The replication event header
 * @param	ptr		The raw replication eent data
 */
static void
blr_distribute_binlog_record(ROUTER_INSTANCE *router, REP_HEADER *hdr, uint8_t *ptr)
{
GWBUF		*pkt;
uint8_t		*buf;
ROUTER_SLAVE	*slave;

	spinlock_acquire(&router->lock);
	slave = router->slaves;
	while (slave)
	{
		if (slave->binlog_pos == hdr->next_pos - hdr->event_size)
		{
			pkt = gwbuf_alloc(hdr->event_size + 5);
			buf = GWBUF_DATA(pkt);
			encode_value(buf, hdr->event_size + 1, 24);
			buf += 3;
			*buf++ = slave->seqno++;
			*buf++ = 0;	// OK
			memcpy(buf, ptr, hdr->event_size);
			slave->dcb->func.write(slave->dcb, pkt);
			slave->binlog_pos = hdr->next_pos;
			if (hdr->event_type == ROTATE_EVENT)
			{
				blr_slave_rotate(slave, ptr);
			}
		}

		slave = slave->next;
	}
	spinlock_release(&router->lock);
}