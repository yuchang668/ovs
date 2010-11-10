/*
 * Copyright (c) 2008, 2009, 2010 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "ofp-print.h"
#include <inttypes.h>
#include <stdlib.h>
#include "byte-order.h"
#include "classifier.h"
#include "nx-match.h"
#include "ofp-util.h"
#include "ofpbuf.h"
#include "packets.h"
#include "random.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(ofp_util);

/* Rate limit for OpenFlow message parse errors.  These always indicate a bug
 * in the peer and so there's not much point in showing a lot of them. */
static struct vlog_rate_limit bad_ofmsg_rl = VLOG_RATE_LIMIT_INIT(1, 5);

/* Given the wildcard bit count in the least-significant 6 of 'wcbits', returns
 * an IP netmask with a 1 in each bit that must match and a 0 in each bit that
 * is wildcarded.
 *
 * The bits in 'wcbits' are in the format used in enum ofp_flow_wildcards: 0
 * is exact match, 1 ignores the LSB, 2 ignores the 2 least-significant bits,
 * ..., 32 and higher wildcard the entire field.  This is the *opposite* of the
 * usual convention where e.g. /24 indicates that 8 bits (not 24 bits) are
 * wildcarded. */
ovs_be32
ofputil_wcbits_to_netmask(int wcbits)
{
    wcbits &= 0x3f;
    return wcbits < 32 ? htonl(~((1u << wcbits) - 1)) : 0;
}

/* Given the IP netmask 'netmask', returns the number of bits of the IP address
 * that it wildcards.  'netmask' must be a CIDR netmask (see ip_is_cidr()). */
int
ofputil_netmask_to_wcbits(ovs_be32 netmask)
{
    assert(ip_is_cidr(netmask));
#if __GNUC__ >= 4
    return netmask == htonl(0) ? 32 : __builtin_ctz(ntohl(netmask));
#else
    int wcbits;

    for (wcbits = 32; netmask; wcbits--) {
        netmask &= netmask - 1;
    }

    return wcbits;
#endif
}

/* A list of the FWW_* and OFPFW_ bits that have the same value, meaning, and
 * name. */
#define WC_INVARIANT_LIST \
    WC_INVARIANT_BIT(IN_PORT) \
    WC_INVARIANT_BIT(DL_VLAN) \
    WC_INVARIANT_BIT(DL_SRC) \
    WC_INVARIANT_BIT(DL_DST) \
    WC_INVARIANT_BIT(DL_TYPE) \
    WC_INVARIANT_BIT(NW_PROTO) \
    WC_INVARIANT_BIT(TP_SRC) \
    WC_INVARIANT_BIT(TP_DST)

/* Verify that all of the invariant bits (as defined on WC_INVARIANT_LIST)
 * actually have the same names and values. */
#define WC_INVARIANT_BIT(NAME) BUILD_ASSERT_DECL(FWW_##NAME == OFPFW_##NAME);
    WC_INVARIANT_LIST
#undef WC_INVARIANT_BIT

/* WC_INVARIANTS is the invariant bits (as defined on WC_INVARIANT_LIST) all
 * OR'd together. */
enum {
    WC_INVARIANTS = 0
#define WC_INVARIANT_BIT(NAME) | FWW_##NAME
    WC_INVARIANT_LIST
#undef WC_INVARIANT_BIT
};

/* Converts the ofp_match in 'match' into a cls_rule in 'rule', with the given
 * 'priority'.
 *
 * 'flow_format' must either NXFF_OPENFLOW10 or NXFF_TUN_ID_FROM_COOKIE.  In
 * the latter case only, 'flow''s tun_id field will be taken from the high bits
 * of 'cookie', if 'match''s wildcards do not indicate that tun_id is
 * wildcarded. */
void
ofputil_cls_rule_from_match(const struct ofp_match *match,
                            unsigned int priority, int flow_format,
                            uint64_t cookie, struct cls_rule *rule)
{
    struct flow_wildcards *wc = &rule->wc;
    unsigned int ofpfw;

    /* Initialize rule->priority. */
    ofpfw = ntohl(match->wildcards);
    ofpfw &= flow_format == NXFF_TUN_ID_FROM_COOKIE ? OVSFW_ALL : OFPFW_ALL;
    rule->priority = !ofpfw ? UINT16_MAX : priority;

    /* Initialize most of rule->wc. */
    wc->wildcards = ofpfw & WC_INVARIANTS;
    if (ofpfw & OFPFW_DL_VLAN_PCP) {
        wc->wildcards |= FWW_DL_VLAN_PCP;
    }
    if (ofpfw & OFPFW_NW_TOS) {
        wc->wildcards |= FWW_NW_TOS;
    }
    memset(wc->reg_masks, 0, sizeof wc->reg_masks);
    wc->nw_src_mask = ofputil_wcbits_to_netmask(ofpfw >> OFPFW_NW_SRC_SHIFT);
    wc->nw_dst_mask = ofputil_wcbits_to_netmask(ofpfw >> OFPFW_NW_DST_SHIFT);

    if (!(ofpfw & NXFW_TUN_ID)) {
        rule->flow.tun_id = htonl(ntohll(cookie) >> 32);
    } else {
        wc->wildcards |= FWW_TUN_ID;
        rule->flow.tun_id = 0;
    }

    if (ofpfw & OFPFW_DL_DST) {
        /* OpenFlow 1.0 OFPFW_DL_DST covers the whole Ethernet destination, but
         * Open vSwitch breaks the Ethernet destination into bits as FWW_DL_DST
         * and FWW_ETH_MCAST. */
        wc->wildcards |= FWW_ETH_MCAST;
    }

    /* Initialize rule->flow. */
    rule->flow.nw_src = match->nw_src;
    rule->flow.nw_dst = match->nw_dst;
    rule->flow.in_port = (match->in_port == htons(OFPP_LOCAL) ? ODPP_LOCAL
                     : ntohs(match->in_port));
    rule->flow.dl_vlan = match->dl_vlan;
    rule->flow.dl_vlan_pcp = match->dl_vlan_pcp;
    rule->flow.dl_type = match->dl_type;
    rule->flow.tp_src = match->tp_src;
    rule->flow.tp_dst = match->tp_dst;
    memcpy(rule->flow.dl_src, match->dl_src, ETH_ADDR_LEN);
    memcpy(rule->flow.dl_dst, match->dl_dst, ETH_ADDR_LEN);
    rule->flow.nw_tos = match->nw_tos;
    rule->flow.nw_proto = match->nw_proto;

    /* Clean up. */
    cls_rule_zero_wildcarded_fields(rule);
}

/* Extract 'flow' with 'wildcards' into the OpenFlow match structure
 * 'match'.
 *
 * 'flow_format' must either NXFF_OPENFLOW10 or NXFF_TUN_ID_FROM_COOKIE.  In
 * the latter case only, 'match''s NXFW_TUN_ID bit will be filled in; otherwise
 * it is always set to 0. */
void
ofputil_cls_rule_to_match(const struct cls_rule *rule, int flow_format,
                          struct ofp_match *match)
{
    const struct flow_wildcards *wc = &rule->wc;
    unsigned int ofpfw;

    /* Figure out OpenFlow wildcards. */
    ofpfw = wc->wildcards & WC_INVARIANTS;
    ofpfw |= ofputil_netmask_to_wcbits(wc->nw_src_mask) << OFPFW_NW_SRC_SHIFT;
    ofpfw |= ofputil_netmask_to_wcbits(wc->nw_dst_mask) << OFPFW_NW_DST_SHIFT;
    if (wc->wildcards & FWW_DL_VLAN_PCP) {
        ofpfw |= OFPFW_DL_VLAN_PCP;
    }
    if (wc->wildcards & FWW_NW_TOS) {
        ofpfw |= OFPFW_NW_TOS;
    }
    if (flow_format == NXFF_TUN_ID_FROM_COOKIE && wc->wildcards & FWW_TUN_ID) {
        ofpfw |= NXFW_TUN_ID;
    }

    /* Compose match structure. */
    match->wildcards = htonl(ofpfw);
    match->in_port = htons(rule->flow.in_port == ODPP_LOCAL ? OFPP_LOCAL
                           : rule->flow.in_port);
    match->dl_vlan = rule->flow.dl_vlan;
    match->dl_vlan_pcp = rule->flow.dl_vlan_pcp;
    memcpy(match->dl_src, rule->flow.dl_src, ETH_ADDR_LEN);
    memcpy(match->dl_dst, rule->flow.dl_dst, ETH_ADDR_LEN);
    match->dl_type = rule->flow.dl_type;
    match->nw_src = rule->flow.nw_src;
    match->nw_dst = rule->flow.nw_dst;
    match->nw_tos = rule->flow.nw_tos;
    match->nw_proto = rule->flow.nw_proto;
    match->tp_src = rule->flow.tp_src;
    match->tp_dst = rule->flow.tp_dst;
    memset(match->pad1, '\0', sizeof match->pad1);
    memset(match->pad2, '\0', sizeof match->pad2);
}

/* Returns a transaction ID to use for an outgoing OpenFlow message. */
static ovs_be32
alloc_xid(void)
{
    static uint32_t next_xid = 1;
    return htonl(next_xid++);
}

/* Allocates and stores in '*bufferp' a new ofpbuf with a size of
 * 'openflow_len', starting with an OpenFlow header with the given 'type' and
 * an arbitrary transaction id.  Allocated bytes beyond the header, if any, are
 * zeroed.
 *
 * The caller is responsible for freeing '*bufferp' when it is no longer
 * needed.
 *
 * The OpenFlow header length is initially set to 'openflow_len'; if the
 * message is later extended, the length should be updated with
 * update_openflow_length() before sending.
 *
 * Returns the header. */
void *
make_openflow(size_t openflow_len, uint8_t type, struct ofpbuf **bufferp)
{
    *bufferp = ofpbuf_new(openflow_len);
    return put_openflow_xid(openflow_len, type, alloc_xid(), *bufferp);
}

/* Similar to make_openflow() but creates a Nicira vendor extension message
 * with the specific 'subtype'.  'subtype' should be in host byte order. */
void *
make_nxmsg(size_t openflow_len, uint32_t subtype, struct ofpbuf **bufferp)
{
    return make_nxmsg_xid(openflow_len, subtype, alloc_xid(), bufferp);
}

/* Allocates and stores in '*bufferp' a new ofpbuf with a size of
 * 'openflow_len', starting with an OpenFlow header with the given 'type' and
 * transaction id 'xid'.  Allocated bytes beyond the header, if any, are
 * zeroed.
 *
 * The caller is responsible for freeing '*bufferp' when it is no longer
 * needed.
 *
 * The OpenFlow header length is initially set to 'openflow_len'; if the
 * message is later extended, the length should be updated with
 * update_openflow_length() before sending.
 *
 * Returns the header. */
void *
make_openflow_xid(size_t openflow_len, uint8_t type, ovs_be32 xid,
                  struct ofpbuf **bufferp)
{
    *bufferp = ofpbuf_new(openflow_len);
    return put_openflow_xid(openflow_len, type, xid, *bufferp);
}

/* Similar to make_openflow_xid() but creates a Nicira vendor extension message
 * with the specific 'subtype'.  'subtype' should be in host byte order. */
void *
make_nxmsg_xid(size_t openflow_len, uint32_t subtype, ovs_be32 xid,
               struct ofpbuf **bufferp)
{
    struct nicira_header *nxh = make_openflow_xid(openflow_len, OFPT_VENDOR,
                                                  xid, bufferp);
    nxh->vendor = htonl(NX_VENDOR_ID);
    nxh->subtype = htonl(subtype);
    return nxh;
}

/* Appends 'openflow_len' bytes to 'buffer', starting with an OpenFlow header
 * with the given 'type' and an arbitrary transaction id.  Allocated bytes
 * beyond the header, if any, are zeroed.
 *
 * The OpenFlow header length is initially set to 'openflow_len'; if the
 * message is later extended, the length should be updated with
 * update_openflow_length() before sending.
 *
 * Returns the header. */
void *
put_openflow(size_t openflow_len, uint8_t type, struct ofpbuf *buffer)
{
    return put_openflow_xid(openflow_len, type, alloc_xid(), buffer);
}

/* Appends 'openflow_len' bytes to 'buffer', starting with an OpenFlow header
 * with the given 'type' and an transaction id 'xid'.  Allocated bytes beyond
 * the header, if any, are zeroed.
 *
 * The OpenFlow header length is initially set to 'openflow_len'; if the
 * message is later extended, the length should be updated with
 * update_openflow_length() before sending.
 *
 * Returns the header. */
void *
put_openflow_xid(size_t openflow_len, uint8_t type, ovs_be32 xid,
                 struct ofpbuf *buffer)
{
    struct ofp_header *oh;

    assert(openflow_len >= sizeof *oh);
    assert(openflow_len <= UINT16_MAX);

    oh = ofpbuf_put_uninit(buffer, openflow_len);
    oh->version = OFP_VERSION;
    oh->type = type;
    oh->length = htons(openflow_len);
    oh->xid = xid;
    memset(oh + 1, 0, openflow_len - sizeof *oh);
    return oh;
}

/* Updates the 'length' field of the OpenFlow message in 'buffer' to
 * 'buffer->size'. */
void
update_openflow_length(struct ofpbuf *buffer)
{
    struct ofp_header *oh = ofpbuf_at_assert(buffer, 0, sizeof *oh);
    oh->length = htons(buffer->size);
}

struct ofpbuf *
make_flow_mod(uint16_t command, const struct cls_rule *rule,
              size_t actions_len)
{
    struct ofp_flow_mod *ofm;
    size_t size = sizeof *ofm + actions_len;
    struct ofpbuf *out = ofpbuf_new(size);
    ofm = ofpbuf_put_zeros(out, sizeof *ofm);
    ofm->header.version = OFP_VERSION;
    ofm->header.type = OFPT_FLOW_MOD;
    ofm->header.length = htons(size);
    ofm->cookie = 0;
    ofm->priority = htons(MIN(rule->priority, UINT16_MAX));
    ofputil_cls_rule_to_match(rule, NXFF_OPENFLOW10, &ofm->match);
    ofm->command = htons(command);
    return out;
}

struct ofpbuf *
make_add_flow(const struct cls_rule *rule, uint32_t buffer_id,
              uint16_t idle_timeout, size_t actions_len)
{
    struct ofpbuf *out = make_flow_mod(OFPFC_ADD, rule, actions_len);
    struct ofp_flow_mod *ofm = out->data;
    ofm->idle_timeout = htons(idle_timeout);
    ofm->hard_timeout = htons(OFP_FLOW_PERMANENT);
    ofm->buffer_id = htonl(buffer_id);
    return out;
}

struct ofpbuf *
make_del_flow(const struct cls_rule *rule)
{
    struct ofpbuf *out = make_flow_mod(OFPFC_DELETE_STRICT, rule, 0);
    struct ofp_flow_mod *ofm = out->data;
    ofm->out_port = htons(OFPP_NONE);
    return out;
}

struct ofpbuf *
make_add_simple_flow(const struct cls_rule *rule,
                     uint32_t buffer_id, uint16_t out_port,
                     uint16_t idle_timeout)
{
    if (out_port != OFPP_NONE) {
        struct ofp_action_output *oao;
        struct ofpbuf *buffer;

        buffer = make_add_flow(rule, buffer_id, idle_timeout, sizeof *oao);
        oao = ofpbuf_put_zeros(buffer, sizeof *oao);
        oao->type = htons(OFPAT_OUTPUT);
        oao->len = htons(sizeof *oao);
        oao->port = htons(out_port);
        return buffer;
    } else {
        return make_add_flow(rule, buffer_id, idle_timeout, 0);
    }
}

struct ofpbuf *
make_packet_in(uint32_t buffer_id, uint16_t in_port, uint8_t reason,
               const struct ofpbuf *payload, int max_send_len)
{
    struct ofp_packet_in *opi;
    struct ofpbuf *buf;
    int send_len;

    send_len = MIN(max_send_len, payload->size);
    buf = ofpbuf_new(sizeof *opi + send_len);
    opi = put_openflow_xid(offsetof(struct ofp_packet_in, data),
                           OFPT_PACKET_IN, 0, buf);
    opi->buffer_id = htonl(buffer_id);
    opi->total_len = htons(payload->size);
    opi->in_port = htons(in_port);
    opi->reason = reason;
    ofpbuf_put(buf, payload->data, send_len);
    update_openflow_length(buf);

    return buf;
}

struct ofpbuf *
make_packet_out(const struct ofpbuf *packet, uint32_t buffer_id,
                uint16_t in_port,
                const struct ofp_action_header *actions, size_t n_actions)
{
    size_t actions_len = n_actions * sizeof *actions;
    struct ofp_packet_out *opo;
    size_t size = sizeof *opo + actions_len + (packet ? packet->size : 0);
    struct ofpbuf *out = ofpbuf_new(size);

    opo = ofpbuf_put_uninit(out, sizeof *opo);
    opo->header.version = OFP_VERSION;
    opo->header.type = OFPT_PACKET_OUT;
    opo->header.length = htons(size);
    opo->header.xid = htonl(0);
    opo->buffer_id = htonl(buffer_id);
    opo->in_port = htons(in_port == ODPP_LOCAL ? OFPP_LOCAL : in_port);
    opo->actions_len = htons(actions_len);
    ofpbuf_put(out, actions, actions_len);
    if (packet) {
        ofpbuf_put(out, packet->data, packet->size);
    }
    return out;
}

struct ofpbuf *
make_unbuffered_packet_out(const struct ofpbuf *packet,
                           uint16_t in_port, uint16_t out_port)
{
    struct ofp_action_output action;
    action.type = htons(OFPAT_OUTPUT);
    action.len = htons(sizeof action);
    action.port = htons(out_port);
    return make_packet_out(packet, UINT32_MAX, in_port,
                           (struct ofp_action_header *) &action, 1);
}

struct ofpbuf *
make_buffered_packet_out(uint32_t buffer_id,
                         uint16_t in_port, uint16_t out_port)
{
    if (out_port != OFPP_NONE) {
        struct ofp_action_output action;
        action.type = htons(OFPAT_OUTPUT);
        action.len = htons(sizeof action);
        action.port = htons(out_port);
        return make_packet_out(NULL, buffer_id, in_port,
                               (struct ofp_action_header *) &action, 1);
    } else {
        return make_packet_out(NULL, buffer_id, in_port, NULL, 0);
    }
}

/* Creates and returns an OFPT_ECHO_REQUEST message with an empty payload. */
struct ofpbuf *
make_echo_request(void)
{
    struct ofp_header *rq;
    struct ofpbuf *out = ofpbuf_new(sizeof *rq);
    rq = ofpbuf_put_uninit(out, sizeof *rq);
    rq->version = OFP_VERSION;
    rq->type = OFPT_ECHO_REQUEST;
    rq->length = htons(sizeof *rq);
    rq->xid = htonl(0);
    return out;
}

/* Creates and returns an OFPT_ECHO_REPLY message matching the
 * OFPT_ECHO_REQUEST message in 'rq'. */
struct ofpbuf *
make_echo_reply(const struct ofp_header *rq)
{
    size_t size = ntohs(rq->length);
    struct ofpbuf *out = ofpbuf_new(size);
    struct ofp_header *reply = ofpbuf_put(out, rq, size);
    reply->type = OFPT_ECHO_REPLY;
    return out;
}

static int
check_message_type(uint8_t got_type, uint8_t want_type)
{
    if (got_type != want_type) {
        char *want_type_name = ofp_message_type_to_string(want_type);
        char *got_type_name = ofp_message_type_to_string(got_type);
        VLOG_WARN_RL(&bad_ofmsg_rl,
                     "received bad message type %s (expected %s)",
                     got_type_name, want_type_name);
        free(want_type_name);
        free(got_type_name);
        return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_BAD_TYPE);
    }
    return 0;
}

/* Checks that 'msg' has type 'type' and that it is exactly 'size' bytes long.
 * Returns 0 if the checks pass, otherwise an OpenFlow error code (produced
 * with ofp_mkerr()). */
int
check_ofp_message(const struct ofp_header *msg, uint8_t type, size_t size)
{
    size_t got_size;
    int error;

    error = check_message_type(msg->type, type);
    if (error) {
        return error;
    }

    got_size = ntohs(msg->length);
    if (got_size != size) {
        char *type_name = ofp_message_type_to_string(type);
        VLOG_WARN_RL(&bad_ofmsg_rl,
                     "received %s message of length %zu (expected %zu)",
                     type_name, got_size, size);
        free(type_name);
        return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_BAD_LEN);
    }

    return 0;
}

/* Checks that 'msg' has type 'type' and that 'msg' is 'size' plus a
 * nonnegative integer multiple of 'array_elt_size' bytes long.  Returns 0 if
 * the checks pass, otherwise an OpenFlow error code (produced with
 * ofp_mkerr()).
 *
 * If 'n_array_elts' is nonnull, then '*n_array_elts' is set to the number of
 * 'array_elt_size' blocks in 'msg' past the first 'min_size' bytes, when
 * successful. */
int
check_ofp_message_array(const struct ofp_header *msg, uint8_t type,
                        size_t min_size, size_t array_elt_size,
                        size_t *n_array_elts)
{
    size_t got_size;
    int error;

    assert(array_elt_size);

    error = check_message_type(msg->type, type);
    if (error) {
        return error;
    }

    got_size = ntohs(msg->length);
    if (got_size < min_size) {
        char *type_name = ofp_message_type_to_string(type);
        VLOG_WARN_RL(&bad_ofmsg_rl, "received %s message of length %zu "
                     "(expected at least %zu)",
                     type_name, got_size, min_size);
        free(type_name);
        return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_BAD_LEN);
    }
    if ((got_size - min_size) % array_elt_size) {
        char *type_name = ofp_message_type_to_string(type);
        VLOG_WARN_RL(&bad_ofmsg_rl,
                     "received %s message of bad length %zu: the "
                     "excess over %zu (%zu) is not evenly divisible by %zu "
                     "(remainder is %zu)",
                     type_name, got_size, min_size, got_size - min_size,
                     array_elt_size, (got_size - min_size) % array_elt_size);
        free(type_name);
        return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_BAD_LEN);
    }
    if (n_array_elts) {
        *n_array_elts = (got_size - min_size) / array_elt_size;
    }
    return 0;
}

const struct ofp_flow_stats *
flow_stats_first(struct flow_stats_iterator *iter,
                 const struct ofp_stats_reply *osr)
{
    iter->pos = osr->body;
    iter->end = osr->body + (ntohs(osr->header.length)
                             - offsetof(struct ofp_stats_reply, body));
    return flow_stats_next(iter);
}

const struct ofp_flow_stats *
flow_stats_next(struct flow_stats_iterator *iter)
{
    ptrdiff_t bytes_left = iter->end - iter->pos;
    const struct ofp_flow_stats *fs;
    size_t length;

    if (bytes_left < sizeof *fs) {
        if (bytes_left != 0) {
            VLOG_WARN_RL(&bad_ofmsg_rl,
                         "%td leftover bytes in flow stats reply", bytes_left);
        }
        return NULL;
    }

    fs = (const void *) iter->pos;
    length = ntohs(fs->length);
    if (length < sizeof *fs) {
        VLOG_WARN_RL(&bad_ofmsg_rl, "flow stats length %zu is shorter than "
                     "min %zu", length, sizeof *fs);
        return NULL;
    } else if (length > bytes_left) {
        VLOG_WARN_RL(&bad_ofmsg_rl, "flow stats length %zu but only %td "
                     "bytes left", length, bytes_left);
        return NULL;
    } else if ((length - sizeof *fs) % sizeof fs->actions[0]) {
        VLOG_WARN_RL(&bad_ofmsg_rl, "flow stats length %zu has %zu bytes "
                     "left over in final action", length,
                     (length - sizeof *fs) % sizeof fs->actions[0]);
        return NULL;
    }
    iter->pos += length;
    return fs;
}

static int
check_action_exact_len(const union ofp_action *a, unsigned int len,
                       unsigned int required_len)
{
    if (len != required_len) {
        VLOG_DBG_RL(&bad_ofmsg_rl,
                    "action %u has invalid length %"PRIu16" (must be %u)\n",
                    a->type, ntohs(a->header.len), required_len);
        return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_LEN);
    }
    return 0;
}

/* Checks that 'port' is a valid output port for the OFPAT_OUTPUT action, given
 * that the switch will never have more than 'max_ports' ports.  Returns 0 if
 * 'port' is valid, otherwise an ofp_mkerr() return code. */
static int
check_output_port(uint16_t port, int max_ports)
{
    switch (port) {
    case OFPP_IN_PORT:
    case OFPP_TABLE:
    case OFPP_NORMAL:
    case OFPP_FLOOD:
    case OFPP_ALL:
    case OFPP_CONTROLLER:
    case OFPP_LOCAL:
        return 0;

    default:
        if (port < max_ports) {
            return 0;
        }
        VLOG_WARN_RL(&bad_ofmsg_rl, "unknown output port %x", port);
        return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_OUT_PORT);
    }
}

/* Checks that 'action' is a valid OFPAT_ENQUEUE action, given that the switch
 * will never have more than 'max_ports' ports.  Returns 0 if 'port' is valid,
 * otherwise an ofp_mkerr() return code. */
static int
check_enqueue_action(const union ofp_action *a, unsigned int len,
                     int max_ports)
{
    const struct ofp_action_enqueue *oae;
    uint16_t port;
    int error;

    error = check_action_exact_len(a, len, 16);
    if (error) {
        return error;
    }

    oae = (const struct ofp_action_enqueue *) a;
    port = ntohs(oae->port);
    if (port < max_ports || port == OFPP_IN_PORT) {
        return 0;
    }
    VLOG_WARN_RL(&bad_ofmsg_rl, "unknown enqueue port %x", port);
    return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_OUT_PORT);
}

static int
check_nicira_action(const union ofp_action *a, unsigned int len,
                    const struct flow *flow)
{
    const struct nx_action_header *nah;
    int error;

    if (len < 16) {
        VLOG_DBG_RL(&bad_ofmsg_rl,
                    "Nicira vendor action only %u bytes", len);
        return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_LEN);
    }
    nah = (const struct nx_action_header *) a;

    switch (ntohs(nah->subtype)) {
    case NXAST_RESUBMIT:
    case NXAST_SET_TUNNEL:
    case NXAST_DROP_SPOOFED_ARP:
    case NXAST_SET_QUEUE:
    case NXAST_POP_QUEUE:
        return check_action_exact_len(a, len, 16);

    case NXAST_REG_MOVE:
        error = check_action_exact_len(a, len,
                                       sizeof(struct nx_action_reg_move));
        if (error) {
            return error;
        }
        return nxm_check_reg_move((const struct nx_action_reg_move *) a, flow);

    case NXAST_REG_LOAD:
        error = check_action_exact_len(a, len,
                                       sizeof(struct nx_action_reg_load));
        if (error) {
            return error;
        }
        return nxm_check_reg_load((const struct nx_action_reg_load *) a, flow);

    case NXAST_NOTE:
        return 0;

    default:
        return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_VENDOR_TYPE);
    }
}

static int
check_action(const union ofp_action *a, unsigned int len,
             const struct flow *flow, int max_ports)
{
    int error;

    switch (ntohs(a->type)) {
    case OFPAT_OUTPUT:
        error = check_action_exact_len(a, len, 8);
        if (error) {
            return error;
        }
        return check_output_port(ntohs(a->output.port), max_ports);

    case OFPAT_SET_VLAN_VID:
        error = check_action_exact_len(a, len, 8);
        if (error) {
            return error;
        }
        if (a->vlan_vid.vlan_vid & ~htons(0xfff)) {
            return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_ARGUMENT);
        }
        return 0;

    case OFPAT_SET_VLAN_PCP:
        error = check_action_exact_len(a, len, 8);
        if (error) {
            return error;
        }
        if (a->vlan_vid.vlan_vid & ~7) {
            return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_ARGUMENT);
        }
        return 0;

    case OFPAT_STRIP_VLAN:
    case OFPAT_SET_NW_SRC:
    case OFPAT_SET_NW_DST:
    case OFPAT_SET_NW_TOS:
    case OFPAT_SET_TP_SRC:
    case OFPAT_SET_TP_DST:
        return check_action_exact_len(a, len, 8);

    case OFPAT_SET_DL_SRC:
    case OFPAT_SET_DL_DST:
        return check_action_exact_len(a, len, 16);

    case OFPAT_VENDOR:
        return (a->vendor.vendor == htonl(NX_VENDOR_ID)
                ? check_nicira_action(a, len, flow)
                : ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_VENDOR));

    case OFPAT_ENQUEUE:
        return check_enqueue_action(a, len, max_ports);

    default:
        VLOG_WARN_RL(&bad_ofmsg_rl, "unknown action type %"PRIu16,
                ntohs(a->type));
        return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_TYPE);
    }
}

int
validate_actions(const union ofp_action *actions, size_t n_actions,
                 const struct flow *flow, int max_ports)
{
    size_t i;

    for (i = 0; i < n_actions; ) {
        const union ofp_action *a = &actions[i];
        unsigned int len = ntohs(a->header.len);
        unsigned int n_slots = len / OFP_ACTION_ALIGN;
        unsigned int slots_left = &actions[n_actions] - a;
        int error;

        if (n_slots > slots_left) {
            VLOG_DBG_RL(&bad_ofmsg_rl,
                        "action requires %u slots but only %u remain",
                        n_slots, slots_left);
            return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_LEN);
        } else if (!len) {
            VLOG_DBG_RL(&bad_ofmsg_rl, "action has invalid length 0");
            return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_LEN);
        } else if (len % OFP_ACTION_ALIGN) {
            VLOG_DBG_RL(&bad_ofmsg_rl, "action length %u is not a multiple "
                        "of %d", len, OFP_ACTION_ALIGN);
            return ofp_mkerr(OFPET_BAD_ACTION, OFPBAC_BAD_LEN);
        }

        error = check_action(a, len, flow, max_ports);
        if (error) {
            return error;
        }
        i += n_slots;
    }
    return 0;
}

/* Returns true if 'action' outputs to 'port' (which must be in network byte
 * order), false otherwise. */
bool
action_outputs_to_port(const union ofp_action *action, uint16_t port)
{
    switch (ntohs(action->type)) {
    case OFPAT_OUTPUT:
        return action->output.port == port;
    case OFPAT_ENQUEUE:
        return ((const struct ofp_action_enqueue *) action)->port == port;
    default:
        return false;
    }
}

/* The set of actions must either come from a trusted source or have been
 * previously validated with validate_actions(). */
const union ofp_action *
actions_first(struct actions_iterator *iter,
              const union ofp_action *oa, size_t n_actions)
{
    iter->pos = oa;
    iter->end = oa + n_actions;
    return actions_next(iter);
}

const union ofp_action *
actions_next(struct actions_iterator *iter)
{
    if (iter->pos != iter->end) {
        const union ofp_action *a = iter->pos;
        unsigned int len = ntohs(a->header.len);
        iter->pos += len / OFP_ACTION_ALIGN;
        return a;
    } else {
        return NULL;
    }
}

void
normalize_match(struct ofp_match *m)
{
    enum { OFPFW_NW = (OFPFW_NW_SRC_MASK | OFPFW_NW_DST_MASK | OFPFW_NW_PROTO
                       | OFPFW_NW_TOS) };
    enum { OFPFW_TP = OFPFW_TP_SRC | OFPFW_TP_DST };
    uint32_t wc;

    wc = ntohl(m->wildcards) & OVSFW_ALL;
    if (wc & OFPFW_DL_TYPE) {
        m->dl_type = 0;

        /* Can't sensibly match on network or transport headers if the
         * data link type is unknown. */
        wc |= OFPFW_NW | OFPFW_TP;
        m->nw_src = m->nw_dst = m->nw_proto = m->nw_tos = 0;
        m->tp_src = m->tp_dst = 0;
    } else if (m->dl_type == htons(ETH_TYPE_IP)) {
        if (wc & OFPFW_NW_PROTO) {
            m->nw_proto = 0;

            /* Can't sensibly match on transport headers if the network
             * protocol is unknown. */
            wc |= OFPFW_TP;
            m->tp_src = m->tp_dst = 0;
        } else if (m->nw_proto == IPPROTO_TCP ||
                   m->nw_proto == IPPROTO_UDP ||
                   m->nw_proto == IPPROTO_ICMP) {
            if (wc & OFPFW_TP_SRC) {
                m->tp_src = 0;
            }
            if (wc & OFPFW_TP_DST) {
                m->tp_dst = 0;
            }
        } else {
            /* Transport layer fields will always be extracted as zeros, so we
             * can do an exact-match on those values.  */
            wc &= ~OFPFW_TP;
            m->tp_src = m->tp_dst = 0;
        }
        if (wc & OFPFW_NW_SRC_MASK) {
            m->nw_src &= ofputil_wcbits_to_netmask(wc >> OFPFW_NW_SRC_SHIFT);
        }
        if (wc & OFPFW_NW_DST_MASK) {
            m->nw_dst &= ofputil_wcbits_to_netmask(wc >> OFPFW_NW_DST_SHIFT);
        }
        if (wc & OFPFW_NW_TOS) {
            m->nw_tos = 0;
        } else {
            m->nw_tos &= IP_DSCP_MASK;
        }
    } else if (m->dl_type == htons(ETH_TYPE_ARP)) {
        if (wc & OFPFW_NW_PROTO) {
            m->nw_proto = 0;
        }
        if (wc & OFPFW_NW_SRC_MASK) {
            m->nw_src &= ofputil_wcbits_to_netmask(wc >> OFPFW_NW_SRC_SHIFT);
        }
        if (wc & OFPFW_NW_DST_MASK) {
            m->nw_dst &= ofputil_wcbits_to_netmask(wc >> OFPFW_NW_DST_SHIFT);
        }
        m->tp_src = m->tp_dst = m->nw_tos = 0;
    } else {
        /* Network and transport layer fields will always be extracted as
         * zeros, so we can do an exact-match on those values. */
        wc &= ~(OFPFW_NW | OFPFW_TP);
        m->nw_proto = m->nw_src = m->nw_dst = m->nw_tos = 0;
        m->tp_src = m->tp_dst = 0;
    }
    if (wc & OFPFW_DL_SRC) {
        memset(m->dl_src, 0, sizeof m->dl_src);
    }
    if (wc & OFPFW_DL_DST) {
        memset(m->dl_dst, 0, sizeof m->dl_dst);
    }
    m->wildcards = htonl(wc);
}

/* Returns a string that describes 'match' in a very literal way, without
 * interpreting its contents except in a very basic fashion.  The returned
 * string is intended to be fixed-length, so that it is easy to see differences
 * between two such strings if one is put above another.  This is useful for
 * describing changes made by normalize_match().
 *
 * The caller must free the returned string (with free()). */
char *
ofp_match_to_literal_string(const struct ofp_match *match)
{
    return xasprintf("wildcards=%#10"PRIx32" "
                     " in_port=%5"PRId16" "
                     " dl_src="ETH_ADDR_FMT" "
                     " dl_dst="ETH_ADDR_FMT" "
                     " dl_vlan=%5"PRId16" "
                     " dl_vlan_pcp=%3"PRId8" "
                     " dl_type=%#6"PRIx16" "
                     " nw_tos=%#4"PRIx8" "
                     " nw_proto=%#4"PRIx16" "
                     " nw_src=%#10"PRIx32" "
                     " nw_dst=%#10"PRIx32" "
                     " tp_src=%5"PRId16" "
                     " tp_dst=%5"PRId16,
                     ntohl(match->wildcards),
                     ntohs(match->in_port),
                     ETH_ADDR_ARGS(match->dl_src),
                     ETH_ADDR_ARGS(match->dl_dst),
                     ntohs(match->dl_vlan),
                     match->dl_vlan_pcp,
                     ntohs(match->dl_type),
                     match->nw_tos,
                     match->nw_proto,
                     ntohl(match->nw_src),
                     ntohl(match->nw_dst),
                     ntohs(match->tp_src),
                     ntohs(match->tp_dst));
}

static uint32_t
vendor_code_to_id(uint8_t code)
{
    switch (code) {
#define OFPUTIL_VENDOR(NAME, VENDOR_ID) case NAME: return VENDOR_ID;
        OFPUTIL_VENDORS
#undef OFPUTIL_VENDOR
    default:
        return UINT32_MAX;
    }
}

/* Creates and returns an OpenFlow message of type OFPT_ERROR with the error
 * information taken from 'error', whose encoding must be as described in the
 * large comment in ofp-util.h.  If 'oh' is nonnull, then the error will use
 * oh->xid as its transaction ID, and it will include up to the first 64 bytes
 * of 'oh'.
 *
 * Returns NULL if 'error' is not an OpenFlow error code. */
struct ofpbuf *
make_ofp_error_msg(int error, const struct ofp_header *oh)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

    struct ofpbuf *buf;
    const void *data;
    size_t len;
    uint8_t vendor;
    uint16_t type;
    uint16_t code;
    ovs_be32 xid;

    if (!is_ofp_error(error)) {
        /* We format 'error' with strerror() here since it seems likely to be
         * a system errno value. */
        VLOG_WARN_RL(&rl, "invalid OpenFlow error code %d (%s)",
                     error, strerror(error));
        return NULL;
    }

    if (oh) {
        xid = oh->xid;
        data = oh;
        len = ntohs(oh->length);
        if (len > 64) {
            len = 64;
        }
    } else {
        xid = 0;
        data = NULL;
        len = 0;
    }

    vendor = get_ofp_err_vendor(error);
    type = get_ofp_err_type(error);
    code = get_ofp_err_code(error);
    if (vendor == OFPUTIL_VENDOR_OPENFLOW) {
        struct ofp_error_msg *oem;

        oem = make_openflow_xid(len + sizeof *oem, OFPT_ERROR, xid, &buf);
        oem->type = htons(type);
        oem->code = htons(code);
    } else {
        struct ofp_error_msg *oem;
        struct nx_vendor_error *nve;
        uint32_t vendor_id;

        vendor_id = vendor_code_to_id(vendor);
        if (vendor_id == UINT32_MAX) {
            VLOG_WARN_RL(&rl, "error %x contains invalid vendor code %d",
                         error, vendor);
            return NULL;
        }

        oem = make_openflow_xid(len + sizeof *oem + sizeof *nve,
                                OFPT_ERROR, xid, &buf);
        oem->type = htons(NXET_VENDOR);
        oem->code = htons(NXVC_VENDOR_ERROR);

        nve = ofpbuf_put_uninit(buf, sizeof *nve);
        nve->vendor = htonl(vendor_id);
        nve->type = htons(type);
        nve->code = htons(code);
    }

    if (len) {
        ofpbuf_put(buf, data, len);
    }

    return buf;
}

/* Attempts to pull 'actions_len' bytes from the front of 'b'.  Returns 0 if
 * successful, otherwise an OpenFlow error.
 *
 * If successful, the first action is stored in '*actionsp' and the number of
 * "union ofp_action" size elements into '*n_actionsp'.  Otherwise NULL and 0
 * are stored, respectively.
 *
 * This function does not check that the actions are valid (the caller should
 * do so, with validate_actions()).  The caller is also responsible for making
 * sure that 'b->data' is initially aligned appropriately for "union
 * ofp_action". */
int
ofputil_pull_actions(struct ofpbuf *b, unsigned int actions_len,
                     union ofp_action **actionsp, size_t *n_actionsp)
{
    if (actions_len % OFP_ACTION_ALIGN != 0) {
        VLOG_DBG_RL(&bad_ofmsg_rl, "OpenFlow message actions length %u "
                    "is not a multiple of %d", actions_len, OFP_ACTION_ALIGN);
        goto error;
    }

    *actionsp = ofpbuf_try_pull(b, actions_len);
    if (*actionsp == NULL) {
        VLOG_DBG_RL(&bad_ofmsg_rl, "OpenFlow message actions length %u "
                    "exceeds remaining message length (%zu)",
                    actions_len, b->size);
        goto error;
    }

    *n_actionsp = actions_len / OFP_ACTION_ALIGN;
    return 0;

error:
    *actionsp = NULL;
    *n_actionsp = 0;
    return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_BAD_LEN);
}
