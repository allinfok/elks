/*
vjhc.c

Van Jacobson header compression based on RFC-1144

Created:	Nov 11, 1993 by Philip Homburg <philip@cs.vu.nl>
*/

/*
  Sep 1997 - Modifications by Claudio Tantignone to work with slip driver
             on Minix 2.0.0

  Mar 1998 - code clean - Claudio Tantignone.
  
  Jul 2002 - ported to ELKS - Harry Kalogirou <harkal@gmx.net>
*/

#include "config.h"
#ifdef CONFIG_CSLIP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "ip.h"
#include "tcp.h"

#include "slip.h"
#include "vjhc.h"

#include "mylib.h"

#define IP_MIN_HDR_SIZE 20
#define IP_MAX_HDR_SIZE 60

#define TCP_MIN_HDR_SIZE 20
#define TCP_MAX_HDR_SIZE 60

#define IH_IHL_MASK     0xf
#define IH_VERSION_MASK 0xf
#define IH_MORE_FRAGS   0x2000
#define IH_DONT_FRAG    0x4000
#define IH_FRAGOFF_MASK 0x1fff

typedef struct snd_state
{
	int s_indx;
	ipaddr_t s_src_ip;
	ipaddr_t s_dst_ip;
	__u32 s_srcdst_port;
	char s_data[IP_MAX_HDR_SIZE + TCP_MAX_HDR_SIZE];
	struct snd_state *s_next;
} snd_state_ut;

typedef struct rcv_state
{
	int s_ip_hdr_len;
	int s_tot_len;
	char s_data[IP_MAX_HDR_SIZE + TCP_MAX_HDR_SIZE];
} rcv_state_ut;

int ip_snd_vjhc= 0;
int ip_snd_vjhc_compress_cid= 0;
int ip_snd_vjhc_state_nr= 16;
int ip_rcv_vjhc= 0;
int ip_rcv_vjhc_compress_cid= 0;
int ip_rcv_vjhc_state_nr= 16;

static int xmit_last;
static snd_state_ut *xmit_state= NULL;
static snd_state_ut *xmit_head;

static int rcv_toss;
static rcv_state_ut *rcv_state;
static rcv_state_ut *rcv_last;

DEFUN_VOID( void ip_vjhc_init )
{
	int i;

	ip_snd_vjhc++;  /* enable send compression */
	ip_rcv_vjhc++;  /* enable recv compression */
	ip_snd_vjhc_compress_cid++;
	ip_rcv_vjhc_compress_cid++;

	if (ip_snd_vjhc)
	{
		xmit_last= -1;
		if (xmit_state != NULL)
		{
			free(xmit_state);
			xmit_state= NULL;
		}
		xmit_state= calloc(ip_snd_vjhc_state_nr, sizeof(snd_state_ut));

		xmit_head= 0;
		for (i= 0; i<ip_snd_vjhc_state_nr; i++)
		{
			xmit_state[i].s_indx= i;
			xmit_state[i].s_next= xmit_head;
			xmit_head= &xmit_state[i];
		}
	}

	if (ip_rcv_vjhc)
	{
		if (rcv_state != NULL)
		{
			free(rcv_state);
			rcv_state= NULL;
		}
		rcv_state= calloc(ip_rcv_vjhc_state_nr, sizeof(snd_state_ut));
	}
}

DEFUN
(int ip_vjhc_compress, (pkt),
	pkt_ut *pkt
)
{
	iphdr_t *ip_hdr, *oip_hdr;
	tcphdr_t *tcp_hdr, *otcp_hdr;
	int ip_hdr_len, tcp_hdr_len, tot_len;
	snd_state_ut *prev, *state, *next;
	int changes;
	__u8 new_hdr[16], *cp;
	__u32 delta, deltaA, deltaS;
	__u16 cksum;

	if (pkt->p_size < IP_MIN_HDR_SIZE + TCP_MIN_HDR_SIZE)
	{
#ifdef DEBUG
		if (opt_d) fprintf(stderr,"ip_vjhc_compress: packet too small %d\n",
			pkt->p_size);
#endif
		return PPP_TYPE_IP;
	}
	ip_hdr= (iphdr_t *)(pkt->p_data + pkt->p_offset);
	if ((ntohs(ip_hdr->frag_off) &
		(IH_MORE_FRAGS | IH_FRAGOFF_MASK)) != 0)
	{
#ifdef DEBUG
		if (opt_d) fprintf(stderr,"ip_vjhc_compress: fragmented packet\n");
#endif
		return PPP_TYPE_IP;
	}
	ip_hdr_len= (ip_hdr->version_ihl & IH_IHL_MASK) * 4;
	tcp_hdr= (tcphdr_t *)(pkt->p_data + pkt->p_offset + ip_hdr_len);
	if ((tcp_hdr->flags & (TF_SYN | TF_FIN | TF_RST | TF_ACK)) !=
		TF_ACK)
	{
#ifdef DEBUG
		if (opt_d) fprintf(stderr,"ip_vjhc_compress: wrong flags in TCP header\n");
#endif
		return PPP_TYPE_IP;
	}
	tcp_hdr_len= TCP_DATAOFF(tcp_hdr);	/* !!! */
	tot_len= ip_hdr_len + tcp_hdr_len;
#ifdef DEBUG
	if (opt_d) {
	fprintf(stderr,"ip_vjhc_compress: packet with size %d\n\t",
		pkt->p_size);
	for (cp= (__u8 *)pkt->p_data+pkt->p_offset; cp < (__u8 *)pkt->p_data +
		pkt->p_offset + ip_hdr_len; cp++)
	{
		fprintf(stderr,"%x ", *cp);
	}
	fprintf(stderr,"\n\t");
	for (; cp < (__u8 *)pkt->p_data + pkt->p_offset + tot_len; cp++)
	{
		fprintf(stderr, "%x ", *cp);
	}
	fprintf(stderr, "\n\t");
	for (; cp < (__u8 *)pkt->p_data + pkt->p_offset + pkt->p_size; cp++)
	{
		fprintf(stderr, "%x ", *cp);
	}
	fprintf(stderr, "\n");
	}
#endif
	for (prev= NULL, state= xmit_head;; )
	{
		if (ip_hdr->saddr == state->s_src_ip &&
			ip_hdr->daddr == state->s_dst_ip &&
			*(__u32 *)&tcp_hdr->sport ==
			state->s_srcdst_port)
		{
#ifdef DEBUG
			if (opt_d) fprintf(stderr, "ip_vjhc_compress: found entry\n");
#endif
			if (prev != NULL)
			{
#ifdef DEBUG
				if (opt_d) fprintf(stderr, "ip_vjhc_compress: moving entry to front\n");
#endif
				prev->s_next= state->s_next;
				state->s_next= xmit_head;
				xmit_head= state;
			}
			break;
		}
		next= state->s_next;
		if (next != NULL)
		{
			prev= state;
			state= next;
			continue;
		}

		/* Not found */
		if (prev != NULL)
		{
			prev->s_next= state->s_next;
			state->s_next= xmit_head;
			xmit_head= state;
		}
		state->s_src_ip= ip_hdr->saddr;
		state->s_dst_ip= ip_hdr->daddr;
		state->s_srcdst_port= *(__u32 *)&tcp_hdr->sport;
#ifdef DEBUG
		if (opt_d) fprintf(stderr,"ip_vjhc_compress: new entry: %x, %x, %x\n",
			state->s_src_ip, state->s_dst_ip,
			state->s_srcdst_port );
#endif
		memcpy(state->s_data, ip_hdr, tot_len);
		xmit_last= ip_hdr->protocol= state->s_indx;
		return PPP_TYPE_VJHC_UNCOMPR;
	}	
	oip_hdr= (iphdr_t *)(state->s_data);
	otcp_hdr= (tcphdr_t *)(state->s_data + ip_hdr_len);

	if (*(__u16 *)&ip_hdr->version_ihl !=
			*(__u16 *)&oip_hdr->version_ihl ||
		*(__u16 *)&ip_hdr->frag_off !=
			*(__u16 *)&oip_hdr->frag_off ||
		*(__u16 *)&ip_hdr->ttl !=
			*(__u16 *)&oip_hdr->ttl ||
		tcp_hdr->data_off != otcp_hdr->data_off ||
		(ip_hdr_len > 20 && memcmp(&ip_hdr[1], &oip_hdr[1],
			ip_hdr_len-20) != 0) ||
		(tcp_hdr_len > 20 && memcmp(&tcp_hdr[1], &otcp_hdr[1],
			tcp_hdr_len-20) != 0))
	{
#ifdef DEBUG
		if (opt_d) fprintf(stderr,"ip_vjhc_compress: ip or tcp header changed\n");
#endif
		memcpy(state->s_data, ip_hdr, tot_len);
		xmit_last= ip_hdr->protocol= state->s_indx;
		return PPP_TYPE_VJHC_UNCOMPR;
	}

	changes= 0;
	cp= new_hdr;

	if (tcp_hdr->flags & TF_URG)
	{
		delta= ntohs(tcp_hdr->urgpnt);
		VJHC_ENCODEZ(cp, delta);
		changes |= VJHC_FLAG_U;
	}
	else if (tcp_hdr->urgpnt != otcp_hdr->urgpnt)
	{
#ifdef DEBUG
		if (opt_d) fprintf(stderr, "ip_vjhc_compress: unexpected urgent pointer change\n");
#endif
		memcpy(state->s_data, ip_hdr, tot_len);
		xmit_last= ip_hdr->protocol= state->s_indx;
		return PPP_TYPE_VJHC_UNCOMPR;
	}

	if ((delta= (__u16)(ntohs(tcp_hdr->window) -
		ntohs(otcp_hdr->window))) != 0)
	{
		VJHC_ENCODE(cp, delta);
		changes |= VJHC_FLAG_W;
	}
	if ((deltaA= (__u32)(ntohl(tcp_hdr->acknum) -
		ntohl(otcp_hdr->acknum))) != 0)
	{
		if (deltaA > 0xffff)
		{
#ifdef DEBUG
			if (opt_d) fprintf(stderr, "ip_vjhc_compress: bad ack change\n");
#endif
			memcpy(state->s_data, ip_hdr, tot_len);
			xmit_last= ip_hdr->protocol= state->s_indx;
			return PPP_TYPE_VJHC_UNCOMPR;
		}
#ifdef DEBUG
		if (opt_d) fprintf(stderr, "ip_vjhc_compress: ack= 0x%08x, oack= 0x%08x, deltaA= 0x%x\n",
			tcp_hdr->th_ack_nr, otcp_hdr->th_ack_nr, deltaA);
#endif
		VJHC_ENCODE(cp, deltaA);
		changes |= VJHC_FLAG_A;
	}
	if ((deltaS= (__u32)(ntohl(tcp_hdr->seqnum) -
		ntohl(otcp_hdr->seqnum))) != 0)
	{
		if (deltaS > 0xffff)
		{
#ifdef DEBUG
			if (opt_d) fprintf(stderr, "ip_vjhc_compress: bad seq change\n");
#endif
			memcpy(state->s_data, ip_hdr, tot_len);
			xmit_last= ip_hdr->protocol= state->s_indx;
			return PPP_TYPE_VJHC_UNCOMPR;
		}
		VJHC_ENCODE(cp, deltaS);
		changes |= VJHC_FLAG_S;
	}

	switch(changes)
	{
	case 0:
	case VJHC_FLAG_U:
		if (ip_hdr->tot_len != oip_hdr->tot_len &&
			ntohs(oip_hdr->tot_len) == tot_len)
		{
			break;
		}
#ifdef DEBUG
		if (opt_d) fprintf(stderr, "ip_vjhc_compress: retransmission\n");
#endif
		/* Fall through */

	case VJHC_SPEC_I:
	case VJHC_SPEC_D:
#ifdef DEBUG
		if ((changes != 0) & opt_d)
			fprintf(stderr, "ip_vjhc_compress: bad special case\n");
#endif
		memcpy(state->s_data, ip_hdr, tot_len);
		xmit_last= ip_hdr->protocol= state->s_indx;
		return PPP_TYPE_VJHC_UNCOMPR;

	case VJHC_FLAG_S | VJHC_FLAG_A:
		if (deltaS == deltaA &&
			deltaS == ntohs(oip_hdr->tot_len) - tot_len)
		{
#ifdef DEBUG
			if (opt_d) fprintf(stderr, "ip_vjhc_compress: special case I\n");
#endif
			changes= VJHC_SPEC_I;
			cp= new_hdr;
		}
		break;

	case VJHC_FLAG_S:
		if (deltaS == ntohs(oip_hdr->tot_len) - tot_len)
		{
#ifdef DEBUG
			if (opt_d) fprintf(stderr, "ip_vjhc_compress: special case D\n");
#endif
			changes= VJHC_SPEC_D;
			cp= new_hdr;
		}
		break;
	}

	if ((delta= ntohs(ip_hdr->id) - ntohs(oip_hdr->id)) != 1)
	{
		VJHC_ENCODEZ(cp, delta);
		changes |= VJHC_FLAG_I;
	}
	if (tcp_hdr->flags & TF_PSH)
		changes |= VJHC_FLAG_P;

	memcpy(state->s_data, ip_hdr, tot_len);
	delta= cp - new_hdr;

	cp= (__u8 *)pkt->p_data + pkt->p_offset + tot_len -delta;
	if (delta != 0)
		memcpy(cp, new_hdr, delta);
	cksum= ntohs(otcp_hdr->chksum);
	*--cp= cksum;
	*--cp= (cksum >> 8);
	if (!ip_snd_vjhc_compress_cid || xmit_last != state->s_indx)
	{
		xmit_last= state->s_indx;
		delta++;
		changes |= VJHC_FLAG_C;
		*--cp= xmit_last;
	}
	*--cp= changes;
	delta += 3;
	pkt->p_offset += tot_len - delta;
	pkt->p_size -= tot_len - delta;
	return PPP_TYPE_VJHC_COMPR;
}

DEFUN
(void ip_vjhc_arr_uncompr, (pkt),
	pkt_ut *pkt
)
{
	rcv_state_ut *state;
	iphdr_t *ip_hdr;
	tcphdr_t *tcp_hdr;
	int ip_hdr_len, tcp_hdr_len, tot_len;
	int slot;

	ip_hdr= (iphdr_t *)(pkt->p_data + pkt->p_offset);
	slot= ip_hdr->protocol;
	ip_hdr->protocol= PROTO_TCP;
#ifdef DEBUG
	if (opt_d) fprintf(stderr, "ip_vjhc_arr_uncompr: packet at slot %d\n", slot); 
#endif
	if (slot >= ip_rcv_vjhc_state_nr)
	{
#ifdef DEBUG
		if (opt_d) fprintf(stderr, "ip_vjhc_arr_uncompr: bad slot\n"); 
#endif
		rcv_toss= 1;
		pkt->p_size = 0;
		return;
	}
	state= &rcv_state[slot];

	ip_hdr_len= (ip_hdr->version_ihl & IH_IHL_MASK) * 4;
	tcp_hdr= (tcphdr_t *)(pkt->p_data + pkt->p_offset + ip_hdr_len);
	tcp_hdr_len= TCP_DATAOFF(tcp_hdr);	/* !!! */
	tot_len= ip_hdr_len + tcp_hdr_len;
	memcpy(state->s_data, ip_hdr, tot_len);
	ip_hdr= (iphdr_t *)state->s_data;
	ip_hdr->check= 0;
	state->s_ip_hdr_len= ip_hdr_len;
	state->s_tot_len= tot_len;
	rcv_toss= 0;
	rcv_last= state;

}

DEFUN
(void ip_vjhc_arr_compr, (pkt),
	pkt_ut *pkt
)
{
	iphdr_t *ip_hdr;
	tcphdr_t *tcp_hdr;
	rcv_state_ut *state;
	int slot;
	int changes;
	__u8 *cp;
	__u32 delta;
	int tot_len;

	cp= (__u8 *)pkt->p_data + pkt->p_offset;
	changes= *cp++;
#ifdef DEBUG
	if (opt_d) fprintf(stderr,"ip_vjhc_arr_compr: changes= 0x%x\n", changes); 
#endif
	if (changes & VJHC_FLAG_C)
	{
		slot= *cp++;
#ifdef DEBUG
		if (opt_d) fprintf(stderr,"ip_vjhc_arr_compr: packet at slot %d\n", slot); 
#endif
		if (slot >= ip_rcv_vjhc_state_nr)
		{
#ifdef DEBUG
			if (opt_d) fprintf(stderr,"ip_vjhc_arr_compr: bad slot\n"); 
#endif
			rcv_toss= 1;
			pkt->p_size=0;
			return;
		}
		state= rcv_last= &rcv_state[slot];
		rcv_toss= 0;
	}
	else
	{
		if (!ip_rcv_vjhc_compress_cid)
		{
#ifdef DEBUG
			if (opt_d) fprintf(stderr, "ip_vjhc_arr_compr: got compressed id but id compression is not enabled\n");
#endif
			pkt->p_size=0;
			return;
		}
		if (rcv_toss)
		{
#ifdef DEBUG
			if (opt_d) fprintf(stderr,"ip_vjhc_arr_compr: tossing packets\n"); 
#endif
			pkt->p_size=0;
			return;
		}
		state= rcv_last;
	}

	ip_hdr= (iphdr_t *)state->s_data;
	tcp_hdr= (tcphdr_t *)(state->s_data+state->s_ip_hdr_len);
	tot_len= state->s_tot_len;
	tcp_hdr->chksum= htons((cp[0] << 8) | cp[1]);
	cp += 2;
	if (changes & VJHC_FLAG_P)
		tcp_hdr->flags |= TF_PSH;
	else
		tcp_hdr->flags &= ~TF_PSH;

	switch (changes & VJHC_SPEC_MASK)
	{
	case VJHC_SPEC_I:
		delta= ntohs(ip_hdr->tot_len) - tot_len;
		tcp_hdr->acknum= htonl(ntohl(tcp_hdr->acknum) + delta);
		tcp_hdr->seqnum= htonl(ntohl(tcp_hdr->seqnum) + delta);
		break;
	case VJHC_SPEC_D:
		delta= ntohs(ip_hdr->tot_len) - tot_len;
		tcp_hdr->seqnum= htonl(ntohl(tcp_hdr->seqnum) + delta);
		break;
	default:
		if (changes & VJHC_FLAG_U)
		{
			tcp_hdr->flags |= TF_URG;
			VJHC_DECODEU(cp, tcp_hdr->urgpnt);
		}
		else
			tcp_hdr->flags &= ~TF_URG;
		if (changes & VJHC_FLAG_W)
		{
			VJHC_DECODES(cp, tcp_hdr->window);
		}
		if (changes & VJHC_FLAG_A)
		{
#ifdef DEBUG
			if (opt_d) fprintf(stderr,"ip_vjhc_arr_compr: oack= 0x%08x\n",
				tcp_hdr->acknum); 
#endif
			VJHC_DECODEL(cp, tcp_hdr->acknum);
#ifdef DEBUG
			if (opt_d) fprintf(stderr,"ip_vjhc_arr_compr: ack= 0x%08x\n",
				tcp_hdr->acknum); 
#endif
		}
		if (changes & VJHC_FLAG_S)
		{
			VJHC_DECODEL(cp, tcp_hdr->seqnum);
		}
		break;
	}

	if (changes & VJHC_FLAG_I)
	{
		VJHC_DECODES(cp, ip_hdr->id);
	}
	else
		ip_hdr->id= htons(ntohs(ip_hdr->id) + 1);

	delta= cp - (__u8 *)(pkt->p_data+pkt->p_offset);
	if (delta > pkt->p_size)
	{
#ifdef DEBUG
		if (opt_d) fprintf(stderr,"ip_vjhc_arr_compr: truncated packet\n"); 
#endif
		rcv_toss= 1;
		pkt->p_size=0;
		return;
	}
	pkt->p_offset += delta;
	pkt->p_size -= delta;
	if (pkt->p_offset < tot_len)
	{
#ifdef DEBUG
		if (opt_d) fprintf(stderr, "ip_vjhc_arr_compr: not enough space to insert header\n"); 
#endif
		rcv_toss= 1;
		pkt->p_size=0;
		return;
	}
	pkt->p_offset -= tot_len;
	pkt->p_size += tot_len;
	cp= (__u8 *)pkt->p_data+pkt->p_offset;
	ip_hdr->tot_len= htons(pkt->p_size);
	memcpy(cp, ip_hdr, tot_len);
	ip_hdr= (iphdr_t *)cp;
	ip_hdr->check= 0;
	ip_hdr->check= ip_calc_chksum((char *)ip_hdr, state->s_ip_hdr_len);
#ifdef DEBUG
	if (opt_d) {
	fprintf(stderr, "ip_vjhc_arr_compr: packet with size %d\n\t",
		pkt->p_size);
	for (cp= (__u8 *)pkt->p_data+pkt->p_offset; cp < (__u8 *)pkt->p_data +
		pkt->p_offset + state->s_ip_hdr_len; cp++)
	{
		fprintf(stderr, "%x ", *cp);
	}
	fprintf(stderr, "\n\t");
	for (; cp < (__u8 *)pkt->p_data + pkt->p_offset + tot_len; cp++)
	{
		fprintf(stderr, "%x ", *cp);
	}
	fprintf(stderr, "\n\t");
	for (; cp < (__u8 *)pkt->p_data + pkt->p_offset + pkt->p_size; cp++)
	{
		fprintf(stderr, "%x ", *cp);
	}
	fprintf(stderr, "\n");
	}
#endif
}

#endif  /* CSLIP */


