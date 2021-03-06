#include "include/sgx/sgxFunc.h"


#include <assert.h>
#include <string.h>

#include "mtcp.h"
#include "arp.h"
#include "socket.h"
#include "eth_out.h"
#include "ip_out.h"
#include "mos_api.h"
#include "tcp_util.h"
#include "tcp_in.h"
#include "tcp_out.h"
#include "tcp_ring_buffer.h"
#include "eventpoll.h"
#include "debug.h"
#include "timer.h"
#include "ip_in.h"
#include "config.h"
// #include "tcp_rb.h"
// void frags_insert(tcprb_t *rb, tcpfrag_t *f);
// tcpfrag_t * frags_new(void);
//#include "tcp_stream.h"
//extern tcp_stream *
//AttachServerTCPStream(mtcp_manager_t mtcp, tcp_stream *cs, int type,
//	uint32_t saddr, uint16_t sport, uint32_t daddr, uint16_t dport);

#if LightBox == 1
#include "../../../core/enclave/include/state_mgmt_t.h"
#endif


extern struct pkt_info *
ClonePacketCtx(struct pkt_info *to, unsigned char *frame, struct pkt_info *from);

#define VERIFY_RX_CHECKSUM	TRUE
/*----------------------------------------------------------------------------*/
static inline uint32_t
DetectStreamType(mtcp_manager_t mtcp, struct pkt_ctx *pctx,
				 uint32_t ip, uint16_t port)
{
	/* To Do: We will extend this filter to check listeners for proxy as well */
	struct sockaddr_in *addr;
	int rc, cnt_match, socktype;
	struct mon_listener *walk;
	struct sfbpf_program fcode;

	cnt_match = 0;
	rc = 0;

	if (mtcp->num_msp > 0) {
		/* mtcp_bind_monitor_filter()
		 * - create MonitorTCPStream only when the filter of any of the existing
		 *   passive sockets match the incoming flow */
		TAILQ_FOREACH(walk, &mtcp->monitors, link) {
			/* For every passive monitor sockets, */
			socktype = walk->socket->socktype;
			if (socktype != MOS_SOCK_MONITOR_STREAM)
				continue; // XXX: can this happen??

			/* if pctx hits the filter rule, handle the passive monitor socket */
			fcode = walk->stream_syn_fcode;
			if (!(ISSET_BPFFILTER(fcode) && pctx &&
#if CAIDA == 0
				EVAL_BPFFILTER(fcode, (uint8_t *)pctx->p.iph - sizeof(struct ethhdr),
							   pctx->p.ip_len + sizeof(struct ethhdr)) == 0)) {
#else
				EVAL_BPFFILTER(fcode, (uint8_t *)pctx->p.iph, 
							   pctx->p.ip_len) == 0)) {
#endif
				walk->is_stream_syn_filter_hit = 1;// set the 'filter hit' flag to 1
				cnt_match++; // count the number of matched sockets
			}
		}

		/* if there's any passive monitoring socket whose filter is hit,
		   we should create monitor stream */
		if (cnt_match > 0)
			rc = STREAM_TYPE(MOS_SOCK_MONITOR_STREAM_ACTIVE);
	}
	
	if (mtcp->listener) {
		/* Detect end TCP stack mode */
		addr = &mtcp->listener->socket->saddr;
		if (addr->sin_port == port) {
			if (addr->sin_addr.s_addr != INADDR_ANY) {
				if (ip == addr->sin_addr.s_addr) {
					rc |= STREAM_TYPE(MOS_SOCK_STREAM);
				}
			} else {
				int i;
				
				for (i = 0; i < g_config.mos->netdev_table->num; i++) {
					if (ip == g_config.mos->netdev_table->ent[i]->ip_addr) {
						rc |= STREAM_TYPE(MOS_SOCK_STREAM);
					}
				}
			}
		}
	}
	
	return rc;
}
/*----------------------------------------------------------------------------*/
static inline tcp_stream *
CreateServerStream(mtcp_manager_t mtcp, int type, struct pkt_ctx *pctx)
{
	tcp_stream *cur_stream = NULL;
	
	/* create new stream and add to flow hash table */
	cur_stream = CreateTCPStream(mtcp, NULL, type,
			pctx->p.iph->daddr, pctx->p.tcph->dest, 
			pctx->p.iph->saddr, pctx->p.tcph->source, NULL);
	if (!cur_stream) {
		TRACE_ERROR("INFO: Could not allocate tcp_stream!\n");
		return FALSE;
	}

	cur_stream->rcvvar->irs = pctx->p.seq;
	cur_stream->sndvar->peer_wnd = pctx->p.window;
	cur_stream->rcv_nxt = cur_stream->rcvvar->irs;
	cur_stream->sndvar->cwnd = 1;
	ParseTCPOptions(cur_stream, pctx->p.cur_ts, (uint8_t *)pctx->p.tcph + 
			TCP_HEADER_LEN, (pctx->p.tcph->doff << 2) - TCP_HEADER_LEN);
	
	return cur_stream;
}
/*----------------------------------------------------------------------------*/
static inline tcp_stream *
CreateMonitorStream(mtcp_manager_t mtcp, struct pkt_ctx* pctx, 
		    uint32_t stream_type, unsigned int *hash)
{
	tcp_stream *stream = NULL;
	struct socket_map *walk;
	/* create a client stream context */
	stream = CreateDualTCPStream(mtcp, NULL, stream_type, pctx->p.iph->daddr,
				     pctx->p.tcph->dest, pctx->p.iph->saddr, 
				     pctx->p.tcph->source, NULL);
	if (!stream)
		return FALSE;
	
	stream->side = MOS_SIDE_CLI;
	stream->pair_stream->side = MOS_SIDE_SVR;
	/* update recv context */
	stream->rcvvar->irs = pctx->p.seq;
	stream->sndvar->peer_wnd = pctx->p.window;
	stream->rcv_nxt = stream->rcvvar->irs + 1;
	stream->sndvar->cwnd = 1;
	
	/* 
	 * if buffer management is off, then disable 
	 * monitoring tcp ring of either streams (only if stream
	 * is just monitor stream active)
	 */
	if (IS_STREAM_TYPE(stream, MOS_SOCK_MONITOR_STREAM_ACTIVE)) {
		assert(IS_STREAM_TYPE(stream->pair_stream,
				      MOS_SOCK_MONITOR_STREAM_ACTIVE));
		
		stream->buffer_mgmt = FALSE;
		stream->pair_stream->buffer_mgmt = FALSE;
		
		/*
		 * if there is even a single monitor asking for
		 * buffer management, enable it (that's why the
		 * need for the loop)
		 */
		uint8_t bm;
		stream->status_mgmt = 0;
		SOCKQ_FOREACH_START(walk, &stream->msocks) {
			bm = walk->monitor_stream->monitor_listener->server_buf_mgmt;
			if (bm > stream->buffer_mgmt) {
				stream->buffer_mgmt = bm;
			}
			if (walk->monitor_stream->monitor_listener->server_mon == 1) {
				stream->status_mgmt = 1;
			}
		} SOCKQ_FOREACH_END;

		stream->pair_stream->status_mgmt = 0;
		SOCKQ_FOREACH_START(walk, &stream->pair_stream->msocks) {
			bm = walk->monitor_stream->monitor_listener->client_buf_mgmt;
			if (bm > stream->pair_stream->buffer_mgmt) {
				stream->pair_stream->buffer_mgmt = bm;
			}
			if (walk->monitor_stream->monitor_listener->client_mon == 1) {
				stream->pair_stream->status_mgmt = 1;
			}			
		} SOCKQ_FOREACH_END;
	}
	
	ParseTCPOptions(stream, pctx->p.cur_ts,
			(uint8_t *)pctx->p.tcph + TCP_HEADER_LEN,
			(pctx->p.tcph->doff << 2) - TCP_HEADER_LEN);
	
	return stream;
}
/*----------------------------------------------------------------------------*/
static inline struct tcp_stream *
FindStream(mtcp_manager_t mtcp, struct pkt_ctx *pctx, unsigned int *hash)
{
	struct tcp_stream temp_stream;

	temp_stream.saddr = pctx->p.iph->daddr;
	temp_stream.sport = pctx->p.tcph->dest;
	temp_stream.daddr = pctx->p.iph->saddr;
	temp_stream.dport = pctx->p.tcph->source;
	return HTSearch(mtcp->tcp_flow_table, &temp_stream, hash);
}
/*----------------------------------------------------------------------------*/
/* Create new flow for new packet or return NULL */
/*----------------------------------------------------------------------------*/


static inline struct tcp_stream *
CreateStream(mtcp_manager_t mtcp, struct pkt_ctx *pctx, unsigned int *hash) 
{
	tcp_stream *cur_stream = NULL;
	uint32_t stream_type;

	
	const struct iphdr *iph = pctx->p.iph;
	const struct tcphdr* tcph = pctx->p.tcph;

	if (tcph->syn && !tcph->ack)
	{
		/* handle the SYN */

		stream_type = DetectStreamType(mtcp, pctx, iph->daddr, tcph->dest);
		if (!stream_type)
		{
			TRACE_DBG("Refusing SYN packet.\n");
#ifdef DBGMSG
			DumpIPPacket(mtcp, iph, pctx->p.ip_len);
#endif
			return NULL;
		}

		/* if it is accepting connections only */
		if (stream_type == STREAM_TYPE(MOS_SOCK_STREAM))
		{
			//printf("create MOS_SOCK_STREAM stream.\n");
			cur_stream = CreateServerStream(mtcp, stream_type, pctx);
			if (!cur_stream) {
				TRACE_DBG("No available space in flow pool.\n");
#ifdef DBGMSG
				DumpIPPacket(mtcp, iph, pctx->p.ip_len);
#endif
			}
		}
		else if (stream_type & STREAM_TYPE(MOS_SOCK_MONITOR_STREAM_ACTIVE))
		{
			//printf("create MOS_SOCK_MONITOR_STREAM_ACTIVE stream.\n");
			cur_stream = CreateClientTCPStream(mtcp, NULL, stream_type,
				pctx->p.iph->saddr, pctx->p.tcph->source,
				pctx->p.iph->daddr, pctx->p.tcph->dest,
				hash);

			if (!cur_stream) {
				TRACE_DBG("No available space in flow pool.\n");
#ifdef DBGMSG
				DumpIPPacket(mtcp, iph, pctx->p.ip_len);
#endif
			}
		}
		else
		{
			/* invalid stream type! */
		}

		return cur_stream;

	}
	else {
#ifdef SGX_HANDLE_WEIRD_PKT 
		//if (pctx->p.payloadlen > 0)
		//{
		//	stream_type = STREAM_TYPE(MOS_SOCK_MONITOR_STREAM_ACTIVE);
		//	cur_stream = CreateClientTCPStream(mtcp, NULL, stream_type,
		//		pctx->p.iph->saddr, pctx->p.tcph->source,
		//		pctx->p.iph->daddr, pctx->p.tcph->dest,
		//		hash);

		//	if (!cur_stream) {
		//		TRACE_ERROR("No available space in flow pool.\n");
		//	}
		//	return cur_stream;
		//}
#endif
		//TRACE_DBG("Weird packet comes.\n");
#ifdef DBGMSG
		DumpIPPacket(mtcp, iph, pctx->p.ip_len);
#endif
		return NULL;
	}


}
/*----------------------------------------------------------------------------*/
inline void
FillPacketContextTCPInfo(struct pkt_ctx *pctx, struct tcphdr * tcph)
{
	pctx->p.tcph = tcph;
	pctx->p.payload    = (uint8_t *)tcph + (tcph->doff << 2);
	pctx->p.payloadlen = pctx->p.ip_len - (pctx->p.payload - (u_char *)pctx->p.iph);
	pctx->p.seq = ntohl(tcph->seq);
	pctx->p.ack_seq = ntohl(tcph->ack_seq);
	pctx->p.window = ntohs(tcph->window);
	pctx->p.offset = 0;

	
	return ;
}
/*----------------------------------------------------------------------------*/
/**
 * Called for every incoming packet from the NIC (when monitoring is disabled)
 */
static void 
HandleSockStream(mtcp_manager_t mtcp, struct tcp_stream *cur_stream, 
				struct pkt_ctx *pctx) 
{
	UpdateRecvTCPContext(mtcp, cur_stream, pctx);
	DoActionEndTCPPacket(mtcp, cur_stream, pctx);	
}
/*----------------------------------------------------------------------------*/
void 
UpdateMonitor(mtcp_manager_t mtcp, struct tcp_stream *sendside_stream,
	struct tcp_stream *recvside_stream, struct pkt_ctx *pctx,
	bool is_pkt_reception)
{
	struct socket_map *walk;

	assert(pctx);

#ifdef RECORDPKT_PER_STREAM 
	/* clone sendside_stream even if sender is disabled */
	ClonePacketCtx(&sendside_stream->last_pctx.p,
		sendside_stream->last_pkt_data, &(pctx.p));
#endif

	/* update send stream context first */
	if (sendside_stream->status_mgmt) {
		sendside_stream->cb_events = MOS_ON_PKT_IN;

		if (is_pkt_reception)
			UpdatePassiveSendTCPContext(mtcp, sendside_stream, pctx);

		sendside_stream->allow_pkt_modification = true;
		/* POST hook of sender */
		if (sendside_stream->side == MOS_SIDE_CLI) {
			SOCKQ_FOREACH_START(walk, &sendside_stream->msocks) {
				HandleCallback(mtcp, MOS_HK_SND, walk, sendside_stream->side,
					pctx, sendside_stream->cb_events);
			} SOCKQ_FOREACH_END;
		}
		else { /* sendside_stream->side == MOS_SIDE_SVR */
			SOCKQ_FOREACH_REVERSE(walk, &sendside_stream->msocks) {
				HandleCallback(mtcp, MOS_HK_SND, walk, sendside_stream->side,
					pctx, sendside_stream->cb_events);
			} SOCKQ_FOREACH_END;
		}
		sendside_stream->allow_pkt_modification = false;
	}

	/* Attach Server-side stream (Create peer stream)*/
	if (recvside_stream == NULL) {
		assert(sendside_stream->side == MOS_SIDE_CLI);
		if ((recvside_stream = AttachServerTCPStream(mtcp, sendside_stream, 0,
			pctx->p.iph->saddr, pctx->p.tcph->source,
			pctx->p.iph->daddr, pctx->p.tcph->dest)) == NULL)
		{
			DestroyTCPStream(mtcp, sendside_stream);
			return;
		}
		/* update recv context */
		recvside_stream->rcvvar->irs = pctx->p.seq;
		recvside_stream->sndvar->peer_wnd = pctx->p.window;
		recvside_stream->rcv_nxt = recvside_stream->rcvvar->irs + 1;
		recvside_stream->sndvar->cwnd = 1;

		ParseTCPOptions(recvside_stream, pctx->p.cur_ts,
			(uint8_t *)pctx->p.tcph + TCP_HEADER_LEN,
			(pctx->p.tcph->doff << 2) - TCP_HEADER_LEN);
	}

	/* Perform post-send tcp activities */
	//PostSendTCPAction(mtcp, pctx, recvside_stream, sendside_stream);
#ifdef ALLOW_PKT_DROP
	if (pctx->p.payloadlen > 0&&!recvside_stream->status_mgmt)
	{
		printf("recvside_stream->status_mgmt is 0, set to 1.\n");
		recvside_stream->status_mgmt = 1;
	}

#endif

	if (/*1*/recvside_stream->status_mgmt) {
		recvside_stream->cb_events = MOS_ON_PKT_IN;

#ifdef ALLOW_PKT_DROP
		if (!recvside_stream->rcvvar->rcvbuf)
		{
#if LightBox == 1
			recvside_stream->rcvvar->rcvbuf = &recvside_stream->rcvvar->rcvbuf_buffer;
#else
			recvside_stream->rcvvar->rcvbuf = tcprb_new(mtcp->bufseg_pool, g_config.mos->rmem_size, recvside_stream->buffer_mgmt);
#endif
			recvside_stream->rcvvar->rcvbuf->cur_recv_size = 0;
		}


		if (recvside_stream->rcvvar->rcvbuf)
		{
			char* buffer = recvside_stream->rcvvar->rcvbuf->recv_buffer;
			int * p_size = &recvside_stream->rcvvar->rcvbuf->cur_recv_size;
			if (*p_size + pctx->p.payloadlen > RECV_BUFFER_SIZE)
			{
				tcp_stream * cur_stream = recvside_stream;
				/* MOS_ON_ERROR: recv buffer overflow */
				if (cur_stream->side == MOS_SIDE_CLI)
				{
					SOCKQ_FOREACH_REVERSE(walk, &cur_stream->msocks) {
						HandleCallback(mtcp, MOS_NULL, walk, cur_stream->side,
							pctx, MOS_ON_ERROR);
					} SOCKQ_FOREACH_END;
				}
				else { /* cur_stream->side == MOS_SIDE_SVR */
					SOCKQ_FOREACH_START(walk, &cur_stream->msocks)
					{
						HandleCallback(mtcp, MOS_NULL, walk, cur_stream->side,
							pctx, MOS_ON_ERROR);
					} SOCKQ_FOREACH_END;
				}

				*p_size = 0;
			}
			memcpy(buffer + *p_size, pctx->p.payload, pctx->p.payloadlen);
			*p_size += pctx->p.payloadlen;
			recvside_stream->last_active_ts = pctx->p.cur_ts;
		}

#else
		/* Predict events which may be raised prior to performing TCP processing */
		PreRecvTCPEventPrediction(mtcp, pctx, recvside_stream);

		/* retransmitted packet should avoid event simulation */
		//if ((recvside_stream->cb_events & MOS_ON_REXMIT) == 0)
		/* update receive stream context (recv_side stream) */
		if (is_pkt_reception)
			UpdateRecvTCPContext(mtcp, recvside_stream, pctx);
		else
			UpdatePassiveRecvTCPContext(mtcp, recvside_stream, pctx);
#endif


		/* POST hook of receiver */
		if (recvside_stream->side == MOS_SIDE_CLI) {
			SOCKQ_FOREACH_REVERSE(walk, &recvside_stream->msocks) {
				HandleCallback(mtcp, MOS_HK_RCV, walk, recvside_stream->side,
					       pctx, recvside_stream->cb_events);
			} SOCKQ_FOREACH_END;
		} else { /* recvside_stream->side == MOS_SIDE_SVR */
			SOCKQ_FOREACH_START(walk, &recvside_stream->msocks) {
				HandleCallback(mtcp, MOS_HK_RCV, walk, recvside_stream->side,
					       pctx, recvside_stream->cb_events);
			} SOCKQ_FOREACH_END;			
		}
	}
	
	/* reset callback events counter */
	recvside_stream->cb_events = 0;
	sendside_stream->cb_events = 0;
}
/*----------------------------------------------------------------------------*/
static void 
HandleMonitorStream(mtcp_manager_t mtcp, struct tcp_stream *sendside_stream, 
			struct tcp_stream *recvside_stream, struct pkt_ctx *pctx) 
{
	UpdateMonitor(mtcp, sendside_stream, recvside_stream, pctx, true);
#ifdef ALLOW_PKT_DROP
	return;
#endif
	recvside_stream = sendside_stream->pair_stream;

	if (HAS_STREAM_TYPE(recvside_stream, MOS_SOCK_STREAM)) {
		DoActionEndTCPPacket(mtcp, recvside_stream, pctx);
	} else {
		/* forward packets */
		if (pctx->forward)
			ForwardIPPacket(mtcp, pctx);

		if (recvside_stream->stream_type == sendside_stream->stream_type &&
		    IS_STREAM_TYPE(recvside_stream, MOS_SOCK_MONITOR_STREAM_ACTIVE)) 
		{
			if (((recvside_stream->state == TCP_ST_TIME_WAIT &&
				  g_config.mos->tcp_tw_interval == 0) ||
			     recvside_stream->state == TCP_ST_CLOSED_RSVD ||
			     !recvside_stream->status_mgmt) &&
			    ((sendside_stream->state == TCP_ST_TIME_WAIT &&
				  g_config.mos->tcp_tw_interval == 0) ||
			     sendside_stream->state == TCP_ST_CLOSED_RSVD ||
			     !sendside_stream->status_mgmt))

				DestroyTCPStream(mtcp, recvside_stream);
		}
	}
}
int cacheMissFlow = 0;
int cacheHitFlow = 0;
int client_new_stream = 0;
int server_new_stream = 0;
extern struct timeval trace_clock;
/*----------------------------------------------------------------------------*/
int
ProcessInTCPPacket(mtcp_manager_t mtcp, struct pkt_ctx *pctx)
{
	uint64_t events = 0;
	struct tcp_stream *cur_stream;
	struct iphdr* iph;
	struct tcphdr* tcph;
	struct mon_listener *walk;
	unsigned int hash = 0;
	
	iph = pctx->p.iph;
	tcph = (struct tcphdr *)((u_char *)pctx->p.iph + (pctx->p.iph->ihl << 2));
	
	FillPacketContextTCPInfo(pctx, tcph);

#ifdef SGX_HANDLE_WEIRD_PKT
	if (!pctx->p.payloadlen)
	{
		return TRUE;
	}
#endif

	/* callback for monitor raw socket */
	TAILQ_FOREACH(walk, &mtcp->monitors, link)
		if (walk->socket->socktype == MOS_SOCK_MONITOR_RAW)
#ifdef ALLOW_PKT_DROP
			printf("find raw tcp monitor.\n");
#else
			HandleCallback(mtcp, MOS_NULL, walk->socket, MOS_SIDE_BOTH,
				pctx, MOS_ON_PKT_IN);
#endif


	if (pctx->p.ip_len < ((iph->ihl + tcph->doff) << 2))
		//tcp error 1
		return ERROR;

#ifdef VERIFY_RX_CHECKSUM
#undef VERIFY_RX_CHECKSUM
#endif
#if VERIFY_RX_CHECKSUM
	if (TCPCalcChecksum((uint16_t *)pctx->p.tcph,
						(tcph->doff << 2) + pctx->p.payloadlen,
						iph->saddr, pctx->p.iph->daddr)) {
		TRACE_DBG("Checksum Error: Original: 0x%04x, calculated: 0x%04x\n",
				tcph->check, TCPCalcChecksum((uint16_t *)tcph,
				(tcph->doff << 2) + pctx->p.payloadlen,
				iph->saddr, iph->daddr));
		if (pctx->forward && mtcp->num_msp)
			ForwardIPPacket(mtcp, pctx);
		return ERROR;
		//tcp error 2
	}
#endif
	events |= MOS_ON_PKT_IN;

	/* Check whether a packet is belong to any stream */

#if LightBox == 1 
	fid_t fid;
	memset(&fid, 0, sizeof(fid));
	fid.src_ip = pctx->p.iph->daddr;
	fid.dst_ip = pctx->p.iph->saddr;
	fid.src_port = pctx->p.tcph->dest;
	fid.dst_port = pctx->p.tcph->source;
	fid.proto = PF_INET;
	struct timeval cur_ts = { 0, 0 };
#if TRACE_CLOCK == 0
	gettimeofday(&cur_ts, NULL);
#else
	cur_ts = trace_clock;
#endif
	state_entry_t* state_entry;
	// no creation of new stream here
	flow_tracking_status rlt = flow_tracking_no_creation(&fid, &state_entry, cur_ts.tv_sec, 0);

	if (rlt == ft_miss)
	{
		cur_stream = 0;
	}
	else
	{
		if (rlt == ft_cache_hit)
		{
			cacheHitFlow += 1;
		}
		else if (rlt == ft_store_hit)
		{
			cacheMissFlow += 1;
		}
		cur_stream = (tcp_stream*)&state_entry->state;

		if (true)
		{
			// repair self pointer
			tcp_stream* stream = cur_stream;
			struct sockent *__s = &stream->msocks_buffer[0];
			(__s)->link.tqe_next = (&stream->msocks)->tqh_first = 0;
			(&stream->msocks)->tqh_last = &(__s)->link.tqe_next;
			(&stream->msocks)->tqh_first = (__s);
			(__s)->link.tqe_prev = &(&stream->msocks)->tqh_first;

			if (stream->side == 0)
			{
				__s->sock->monitor_stream->stream = stream;
			}
			else
			{
				__s->sock->monitor_stream->stream = stream->pair_stream;
			}
			
			if (stream->sndvar)
			{
				stream->sndvar = &stream->sndvar_buffer;

				if (stream->sndvar->sndbuf)
				{
					stream->sndvar->sndbuf = &stream->sndvar->sndbuf_buffer;

					if (stream->sndvar->sndbuf->data)
					{
						stream->sndvar->sndbuf->data = stream->sndvar->sndbuf->data_buffer;
					}
				}
			}
			if (stream->rcvvar)
			{
				stream->rcvvar = &stream->rcvvar_buffer;
				
				if (stream->rcvvar->rcvbuf)
				{
					stream->rcvvar->rcvbuf = &stream->rcvvar->rcvbuf_buffer;

					tcprb_t * rb = stream->rcvvar->rcvbuf;
					tcpfrag_t* frag = rb->tcpfrag_t_buffer;

					rb->current_frag_no = 1;
					rb->frag_used = 1;

					(frag)->link.tqe_next = (&rb->frags)->tqh_first = 0;
					(&rb->frags)->tqh_last = &(frag)->link.tqe_next;
					(&rb->frags)->tqh_first = (frag);
					(frag)->link.tqe_prev = &(&rb->frags)->tqh_first;

					tcpbufseg_t* seg;
					int i, veri = rb->seg_used;
					rb->bufsegs.tqh_first = 0;
					rb->bufsegs.tqh_last = &rb->bufsegs.tqh_first;
					for (i = 0; i < rb->current_seg_no; i++)
					{
						seg = &rb->tcpbufseg_buffer[i];
						assert(seg->id == i);
						TAILQ_INSERT_TAIL(&rb->bufsegs, seg, link);
						veri >>= 1;
					}
					assert(veri == 0);
				}
			}
		}

		// also bring pair in enclave
		if (cur_stream->pair_stream)
		{
			fid_t pairFid = fid;
			fid.src_ip = pctx->p.iph->saddr;
			fid.dst_ip = pctx->p.iph->daddr;
			fid.src_port = pctx->p.tcph->source;
			fid.dst_port = pctx->p.tcph->dest;

			state_entry_t* pair_entry;
			rlt = flow_tracking(&fid, &pair_entry, cur_ts.tv_sec, 0);

			if (rlt == ft_cache_hit)
			{
				cacheHitFlow += 1;
			}
			else if (rlt == ft_store_hit)
			{
				cacheMissFlow += 1;
			}

			//assert(rlt != ft_miss);
			{
				cur_stream->pair_stream = (tcp_stream*)&pair_entry->state;
				cur_stream->pair_stream->pair_stream = cur_stream;

				if (true)
				{
					// repair pointer
					tcp_stream* stream = cur_stream->pair_stream;
					struct sockent *__s = &stream->msocks_buffer[0];
					(__s)->link.tqe_next = (&stream->msocks)->tqh_first = 0;
					(&stream->msocks)->tqh_last = &(__s)->link.tqe_next;
					(&stream->msocks)->tqh_first = (__s);
					(__s)->link.tqe_prev = &(&stream->msocks)->tqh_first;

					if (stream->side == 0)
					{
						__s->sock->monitor_stream->stream = stream;
					}
					else
					{
						__s->sock->monitor_stream->stream = stream->pair_stream;
					}

					if (stream->sndvar)
					{
						stream->sndvar = &stream->sndvar_buffer;

						if (stream->sndvar->sndbuf)
						{
							stream->sndvar->sndbuf = &stream->sndvar->sndbuf_buffer;

							if (stream->sndvar->sndbuf->data)
							{
								stream->sndvar->sndbuf->data = stream->sndvar->sndbuf->data_buffer;
							}
						}
					}
					if (stream->rcvvar)
					{
						stream->rcvvar = &stream->rcvvar_buffer;

						if (stream->rcvvar->rcvbuf)
						{
							stream->rcvvar->rcvbuf = &stream->rcvvar->rcvbuf_buffer;

							tcprb_t * rb = stream->rcvvar->rcvbuf;
							tcpfrag_t* frag = rb->tcpfrag_t_buffer;

							rb->current_frag_no = 1;
							rb->frag_used = 1;

							(frag)->link.tqe_next = (&rb->frags)->tqh_first = 0;
							(&rb->frags)->tqh_last = &(frag)->link.tqe_next;
							(&rb->frags)->tqh_first = (frag);
							(frag)->link.tqe_prev = &(&rb->frags)->tqh_first;

							tcpbufseg_t* seg;
							int i, veri = rb->seg_used;
							rb->bufsegs.tqh_first = 0;
							rb->bufsegs.tqh_last = &rb->bufsegs.tqh_first;
							for (i = 0; i < rb->current_seg_no; i++)
							{
								seg = &rb->tcpbufseg_buffer[i];
								assert(seg->id == i);
								TAILQ_INSERT_TAIL(&rb->bufsegs, seg, link);
								veri >>= 1;
							}
							assert(veri == 0);
						}
					}
				}
			}
		}

		if(cur_stream->saddr != cur_stream->pair_stream->daddr)
		printf("hell %d %d %d %d\npair %d %d %d %d\n",
			cur_stream->saddr, cur_stream->daddr, cur_stream->sport, cur_stream->dport,
			cur_stream->pair_stream->saddr, cur_stream->pair_stream->daddr, cur_stream->pair_stream->sport, cur_stream->pair_stream->dport);
	}

#else
	cur_stream = FindStream(mtcp, pctx, &hash);
#endif


/*	if (cur_stream)
	{
        printf("curstream found!\n");
		if (cur_stream->side)
		{
			cur_stream->side = 0;
			
			if (cur_stream->pair_stream)
			{
				cur_stream->pair_stream->side = 1;
			}
		}
	}
*/
#ifdef SGX_HANDLE_WEIRD_PKT
//    printf("payloadlen %d %p\n", pctx->p.payloadlen, cur_stream);

	//if (pctx->p.payloadlen > 0 && (!cur_stream || cur_stream->state != TCP_ST_ESTABLISHED))
	if (!cur_stream || cur_stream->state != TCP_ST_ESTABLISHED)
	{
        //printf("new wierd!\n");
		if (!cur_stream)
		{
			events = MOS_ON_PKT_IN;
			if (mtcp->num_msp > 0) {
				struct mon_listener *walk;
				struct sfbpf_program fcode;
				/* mtcp_bind_monitor_filter()
				* - create MonitorTCPStream only when the filter of any of the existing
				*   passive sockets match the incoming flow */
				TAILQ_FOREACH(walk, &mtcp->monitors, link) {
					/* For every passive monitor sockets, */
					int socktype = walk->socket->socktype;
					if (socktype != MOS_SOCK_MONITOR_STREAM)
						continue; // XXX: can this happen??

								  /* if pctx hits the filter rule, handle the passive monitor socket */
					fcode = walk->stream_syn_fcode;
					if (!(ISSET_BPFFILTER(fcode) && pctx &&
#if CAIDA == 0
						EVAL_BPFFILTER(fcode, (uint8_t *)pctx->p.iph - sizeof(struct ethhdr),
							pctx->p.ip_len + sizeof(struct ethhdr)) == 0)) 
#else
						EVAL_BPFFILTER(fcode, (uint8_t *)pctx->p.iph,
							pctx->p.ip_len) == 0)) 
#endif
					{
						walk->is_stream_syn_filter_hit = 1;// set the 'filter hit' flag to 1
					}
					else
					{
						assert(0);
					}
				}
			}
			if ((cur_stream = CreateClientTCPStream(mtcp, NULL, STREAM_TYPE(MOS_SOCK_MONITOR_STREAM_ACTIVE),
				pctx->p.iph->saddr, pctx->p.tcph->source,
				pctx->p.iph->daddr, pctx->p.tcph->dest,
				&hash)) == NULL)
			{
				TRACE_ERROR("Create cur_stream failed.\n");
			}
			++client_new_stream;
		}
	//	assert(cur_stream->side == MOS_SIDE_CLI);
		tcp_stream* stream = cur_stream;
		tcp_stream* pair_stream = stream->pair_stream;

		// repair stream
		if (!stream->on_timeout_list)
			AddtoTimeoutList(mtcp, stream);
		stream->cb_events = 0;
		stream->state = TCP_ST_ESTABLISHED;
		stream->snd_nxt = pctx->p.seq;
		stream->rcv_nxt = pctx->p.ack_seq;
		stream->sndvar->iss = pctx->p.seq - 1;


		// repair pair_stream
		if (!pair_stream)
		{
			if ((pair_stream = AttachServerTCPStream(mtcp, stream, 0,
				pctx->p.iph->saddr, pctx->p.tcph->source,
				pctx->p.iph->daddr, pctx->p.tcph->dest)) == NULL)
			{
				TRACE_ERROR("Create pair_stream failed.\n");
			}
			++server_new_stream;
			/* update recv context */
			pair_stream->sndvar->peer_wnd = pctx->p.window;
			pair_stream->sndvar->cwnd = pair_stream->sndvar->mss * 2;
			pair_stream->rcvvar->irs = stream->sndvar->iss;
		}
		pair_stream->rcv_nxt = pctx->p.seq;
		pair_stream->snd_nxt = pctx->p.ack_seq;
		pair_stream->state = TCP_ST_ESTABLISHED;
		pair_stream->cb_events = 0;
		pair_stream->actions = 16;

        //if(cur_stream)
        //    printf("wired handled!\n");
	}

	/*static long long pktNo = 0;
	if (FALSE && ++pktNo)
	{
		printf("pkt: %d, arrived, pload len is %d.\n", pktNo, pctx->p.payloadlen);
		if (cur_stream->rcv_nxt != pctx->p.ack_seq)
		{
			printf("pkt: %d, ack_seq is %d, rcv is %d\n", pktNo, pctx->p.ack_seq, cur_stream->rcv_nxt);
		}

		if (cur_stream->snd_nxt != pctx->p.seq)
		{
			printf("pkt: %d, seq is %d, snd is %d\n", pktNo, pctx->p.seq, cur_stream->snd_nxt);
		}
		{
			const struct pkt_info pinfo = pctx->p;
			const tcp_stream* stream = cur_stream;
			const tcp_stream* pair_stream = cur_stream->pair_stream;

			if (stream)
			{
				struct tcp_recv_vars* rcv = stream->rcvvar;
				struct tcp_send_vars* snd = stream->sndvar;

				if (rcv)
				{
					tcprb_t* rcvbuf = rcv->rcvbuf;
					assert(1);
				}
				struct sockent* sock = stream->msocks.tqh_first;
				if (sock && sock->sock&& sock->sock->monitor_stream)
				{
					struct mon_stream* moni = sock->sock->monitor_stream;
					printf("sock id is %d.\n", sock->sock->id);
					assert(moni->stream == stream);
				}
				assert(1);
			}

			stream = pair_stream;
			if (stream)
			{

				struct tcp_recv_vars* rcv = stream->rcvvar;
				struct tcp_send_vars* snd = stream->sndvar;

				if (rcv)
				{
					tcprb_t* rcvbuf = rcv->rcvbuf;
					assert(1);
				}
				assert(1);
			}
		}

	}*/

	//assert(cur_stream->side == MOS_SIDE_CLI);
	if (cur_stream && (cur_stream->pair_stream->rcv_nxt != pctx->p.seq 
		|| cur_stream->snd_nxt != pctx->p.seq
		|| cur_stream->pair_stream->snd_nxt != pctx->p.ack_seq
		|| cur_stream->rcv_nxt != pctx->p.ack_seq
		) &&cur_stream->pair_stream->rcvvar->rcvbuf
		//&&pctx->p.payloadlen>0)
        )
	{
		tcp_stream* stream = cur_stream;
		tcp_stream* pair_stream = stream->pair_stream;
		int find_error = 0;

		if (cur_stream->snd_nxt != cur_stream->pair_stream->rcv_nxt)
		{
		    //TRACE_ERROR("Stream already error 111!\n");
			cur_stream->snd_nxt = cur_stream->pair_stream->rcv_nxt;
			find_error = 1;
		}
		if (cur_stream->rcv_nxt != cur_stream->pair_stream->snd_nxt)
		{
		    //TRACE_ERROR("Stream already error 222!\n");
			cur_stream->rcv_nxt = cur_stream->pair_stream->snd_nxt;
			find_error = 1;
		}

		if (pair_stream->rcvvar->irs != stream->sndvar->iss)
		{
            //TRACE_ERROR("Stream already error 333!\n");
			stream->sndvar->iss = pair_stream->rcvvar->irs;
			find_error = 1;
		}

		tcprb_t* rb = pair_stream->rcvvar->rcvbuf;

		//const int rcv_buf_len = 8192;
		const int rcv_buf_len = 4096;
		if (rb&&(rb->len != rcv_buf_len || rb->metalen != rcv_buf_len
			|| rb->pile<rb->head
			|| rb->pile-rb->head>rcv_buf_len))
		{
			//TRACE_ERROR("Stream rb already error!\n");
			rb->len = rcv_buf_len;
			rb->metalen = rcv_buf_len;
			rb->pile = rb->head;
			find_error = 1;
		}

		struct _tcpfrag_t* frag = TAILQ_FIRST(&rb->frags);
        if(frag == NULL)
        {
            printf("frag null!\n");
// #if LightBox == 1
//             if(rb->current_frag_no == MAX_FRAG_COUNT)
//             {
//                 TRACE_ERROR("tcpfrag_t_buffer is full.\n");
//                 rb->frag_used = 0;
//             }
//             rb->current_frag_no++;
//             new = &rb->tcpfrag_t_buffer(findEmptyLoc(&rb->frag_used));
//             memset(frag, 0, sizeof(*frag));
// #else
//             frag = frags_new();
// #endif
//             frags_insert(rb, frag);
        }
		if(frag &&(rb->head !=frag->head || frag->tail<frag->head
			|| rb->pile > frag->tail
			|| frag->tail- frag->head > rb->len))
		{
            //TRACE_ERROR("Stream frag seq already error!\n");
			frag->tail = frag->head;
			frag->head = rb->head;
			rb->pile = frag->tail;
			find_error = 1;
		}

		struct mon_stream * moni = cur_stream->msocks.tqh_first->sock->monitor_stream;
		if (moni && moni->peek_offset[1] > rb->head + rb->len)
		{
			//TRACE_ERROR("Stream moni peek already error!\n");
			moni->peek_offset[1] = rb->head + rb->len;
			find_error = 1;
		}
		
		// flush recv buffer
		struct socket_map *walk;
		SOCKQ_FOREACH_START(walk, &cur_stream->msocks) 
		{
			if (walk->monitor_stream->peek_offset[cur_stream->side] < rb->pile)
				HandleCallback(mtcp, MOS_NULL, walk, cur_stream->side,
					pctx, MOS_ON_ERROR);
			rb->head = rb->pile;
		} SOCKQ_FOREACH_END;

		// repair
		enum
		{
			MODIFY_MOS,
			MODIFY_PKT
		} repair_way;

		repair_way = MODIFY_MOS;
		if (repair_way)
		{
			if (pair_stream->snd_nxt != pctx->p.ack_seq||find_error)
			{
				pctx->p.ack_seq = pair_stream->snd_nxt;
			}

			if (pair_stream->rcv_nxt != pctx->p.seq || find_error)
			{
				//pair_stream->rcv_nxt = pair_stream->rcvvar->irs + frag->tail + 1;
				//cur_stream->snd_nxt = cur_stream->pair_stream->rcv_nxt;
				pctx->p.seq = pair_stream->rcv_nxt;
			}
		}
		else
		{
			if (stream->rcv_nxt < pctx->p.ack_seq || find_error)
			{
				stream->rcv_nxt = pctx->p.ack_seq;
				pair_stream->snd_nxt = pctx->p.ack_seq;
				pair_stream->sndvar->iss = pctx->p.ack_seq - 1;
				stream->rcvvar->irs = pair_stream->sndvar->iss;
			}

			if (pair_stream->rcv_nxt < pctx->p.seq || find_error)
			{
				stream->snd_nxt = pctx->p.seq;
				pair_stream->rcv_nxt = pctx->p.seq;
				stream->sndvar->iss = pctx->p.seq - 1;
				pair_stream->rcvvar->irs = stream->sndvar->iss;

				frag->tail = pair_stream->rcv_nxt - pair_stream->rcvvar->irs -1;
				frag->head = frag->tail;
				rb->head = frag->head;
				rb->pile = frag->head;

#if LightBox == 1 
				rb->current_frag_no = 1;
				rb->frag_used = 1;
				rb->tcpfrag_t_buffer[0] = *frag;
				frag = rb->tcpfrag_t_buffer;
#endif

				(frag)->link.tqe_next = (&rb->frags)->tqh_first = 0;
				(&rb->frags)->tqh_last = &(frag)->link.tqe_next;
				(&rb->frags)->tqh_first = (frag);
				(frag)->link.tqe_prev = &(&rb->frags)->tqh_first;

				moni->peek_offset[1] = 0;

			}
		}

	}
#endif

	if (!cur_stream) {
        printf("about to create stream!\n");
		/*
		* No need to create stream for monitor.
		*  But do create 1 for client case!
		*/
		if (mtcp->listener == NULL && mtcp->num_msp == 0) {
			//if (pctx->forward)
			//	ForwardIPPacket(mtcp, pctx);
			//tcp error 3
			return TRUE;
		}
		/* Create new flow for new packet or return NULL */
		cur_stream = CreateStream(mtcp, pctx, &hash);
		if (!cur_stream)
			events = MOS_ON_ORPHAN;
        printf("stream created!\n");
	}

	if (cur_stream) {
		cur_stream->cb_events = events;

		if (cur_stream->rcvvar && cur_stream->rcvvar->rcvbuf)
			pctx->p.offset = (uint64_t)seq2loff(cur_stream->rcvvar->rcvbuf,
				pctx->p.seq, cur_stream->rcvvar->irs + 1);

		if (IS_STREAM_TYPE(cur_stream, MOS_SOCK_STREAM))
			//tcp sock
			HandleSockStream(mtcp, cur_stream, pctx);

		else if (HAS_STREAM_TYPE(cur_stream, MOS_SOCK_MONITOR_STREAM_ACTIVE))
			// tcp sock moni
			HandleMonitorStream(mtcp, cur_stream, cur_stream->pair_stream, pctx);

		else
			assert(0);

	}
	else {

		// tcp no stream
		struct mon_listener *walk;
		struct sfbpf_program fcode;
		/* 
		 * event callback for pkt_no_conn; MOS_SIDE_BOTH
		 * means that we can't judge sides here 
		 */
		TAILQ_FOREACH(walk, &mtcp->monitors, link) {
			/* mtcp_bind_monitor_filter()
			 * - apply stream orphan filter to every pkt before raising ORPHAN event */
			fcode = walk->stream_orphan_fcode;
			if (!(ISSET_BPFFILTER(fcode) && pctx &&
#if CAIDA == 0
				EVAL_BPFFILTER(fcode, (uint8_t *)pctx->p.iph - sizeof(struct ethhdr),
							   pctx->p.ip_len + sizeof(struct ethhdr)) == 0)) {
#else
				EVAL_BPFFILTER(fcode, (uint8_t *)pctx->p.iph,
							   pctx->p.ip_len) == 0)) {
#endif
				HandleCallback(mtcp, MOS_NULL, walk->socket, MOS_SIDE_BOTH,
					       pctx, events);
			}
		}
		if (mtcp->listener) {
			/* RFC 793 (page 65) says
			   "An incoming segment containing a RST is discarded."
			   if the TCP state is CLOSED (= TCP stream does not exist). */
			if (!tcph->rst)
				/* Send RST if it is run as EndTCP only mode */
				SendTCPPacketStandalone(mtcp,
							iph->daddr, tcph->dest, iph->saddr, tcph->source,
							0, pctx->p.seq + pctx->p.payloadlen + 1, 0,
							TCP_FLAG_RST | TCP_FLAG_ACK,
							NULL, 0, pctx->p.cur_ts, 0, 0, -1);
		} else if (pctx->forward) {
			/* Do forward or drop if it run as Monitor only mode */
			ForwardIPPacket(mtcp, pctx);
		}
	}
	
	return TRUE;
}


