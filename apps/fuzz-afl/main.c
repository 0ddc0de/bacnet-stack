/**
 * @file
 * @brief command line fuzz (data scrambling) interface for security testing
 * @author Anthony Delorenzo <anthony@crystalpeaksecurity.com>
 * @date 2023
 * @copyright SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <assert.h>
/* BACnet Stack defines - first */
#include "bacnet/bacdef.h"
/* BACnet Stack API */
#include "bacnet/bactext.h"
#include "bacnet/bacerror.h"
#include "bacnet/iam.h"
#include "bacnet/list_element.h"
#include "bacnet/delete_object.h"
#include "bacnet/create_object.h"
#include "bacnet/arf.h"
#include "bacnet/npdu.h"
#include "bacnet/apdu.h"
#include "bacnet/version.h"
/* some demo modules we use */
#include "bacnet/basic/sys/debug.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/basic/services.h"
/* port agnostic file */
#include "bacport.h"
/* our datalink layers */
#include "bacnet/datalink/dlmstp.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/datalink/bvlc.h"
#include "bacnet/basic/bbmd/h_bbmd.h"

/* current version of the BACnet stack */
static const char *BACnet_Version = BACNET_VERSION_TEXT;

/**
 * 6.6.1 Routing Tables
 *
 * By definition, a router is a device that is connected to at least
 * two BACnet networks. Each attachment is through a "port." A
 * "routing table" consists of the following information for each port:
 * (a) the MAC address of the port's connection to its network;
 * (b) the 2-octet network number of the directly connected network;
 * (c) a list of network numbers reachable through the port along
 *     with the MAC address of the next router on the path to each
 *     network number and the reachability status of each such network.
 *
 * The "reachability status" is an implementation-dependent value
 * that indicates whether the associated network is able to
 * receive traffic. The reachability status shall be able to
 * distinguish, at a minimum, between "permanent" failures of a route,
 * such as might result from the failure of a router, and "temporary"
 * unreachability due to the imposition of a congestion control
 * restriction.
 */
typedef struct _dnet {
    uint8_t mac[MAX_MAC_LEN];
    uint8_t mac_len;
    uint16_t net;
    bool enabled;
    struct _dnet *dnets;
    struct _dnet *next;
} DNET;
/* The list of DNETs that our router can reach. */
static DNET *Router_Table_Head;
/* track our directly connected ports network number */
static uint16_t BIP_Net;
static uint16_t MSTP_Net;
/* buffer for receiving packets from the directly connected ports */
static uint8_t BIP_Rx_Buffer[BIP_MPDU_MAX];
static uint8_t MSTP_Rx_Buffer[DLMSTP_MPDU_MAX];
/* buffer for transmitting from any port */
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
static uint8_t Tx_Buffer[MAX(DLMSTP_MPDU_MAX, BIP_MPDU_MAX)];
/* main loop exit control */
static bool Exit_Requested;
/* debugging info */
static bool Debug_Enabled;

/**
 * @brief print debug info if debug is enabled
 * @param format - printf format string
 * @param ... variable arguments
 * @return number of bytes printed
 */
static int log_printf(const char *format, ...)
{
    int length = 0;
    va_list ap;

    if (Debug_Enabled) {
        va_start(ap, format);
        length = vfprintf(stdout, format, ap);
        va_end(ap);
        fflush(stdout);
    }

    return length;
}

/**
 * Search the router table to find a matching DNET entry
 *
 * @param net - network number to find a match
 * @param addr - address to be filled with remote router address
 *
 * @return NULL if not found, or a pointer to the directly connected port.
 * If addr is not NULL, the DNET entry address is copied to addr
 * The caller will need to compare the sought after net with the
 * returned port->net to determine if the addr is filled.
 */
static DNET *dnet_find(uint16_t net, BACNET_ADDRESS *addr)
{
    DNET *port = Router_Table_Head;
    DNET *dnet = NULL;
    unsigned int i = 0;

    while (port != NULL) {
        if (net == port->net) {
            /* DNET is directly connected to the router */
            return port;
        } else if (port->dnets) {
            /* search router ports DNET list */
            dnet = port->dnets;
            while (dnet != NULL) {
                if (net == dnet->net) {
                    if (addr) {
                        addr->mac_len = dnet->mac_len;
                        for (i = 0; i < MAX_MAC_LEN; i++) {
                            addr->mac[i] = dnet->mac[i];
                        }
                    }
                    return port;
                }
                dnet = dnet->next;
            }
        }
        port = port->next;
    }

    return NULL;
}

static bool port_find(uint16_t snet, BACNET_ADDRESS *addr)
{
    DNET *port = NULL;
    bool found = false;
    unsigned int i = 0;

    port = Router_Table_Head;
    while (port) {
        if (port->net == snet) {
            if (addr) {
                addr->mac_len = port->mac_len;
                for (i = 0; i < MAX_MAC_LEN; i++) {
                    addr->mac[i] = port->mac[i];
                }
            }
            found = true;
            break;
        }
        port = port->next;
    }

    return found;
}

/**
 * Add a directly connected port to the router table
 *
 * @param snet - router port SNET
 * @param addr - address of port at the net to be added
 */
static void port_add(uint16_t snet, const BACNET_ADDRESS *addr)
{
    DNET *port = NULL;
    DNET *dnet = NULL;
    unsigned int i = 0;

    port = dnet_find(snet, NULL);
    if (!port) {
        port = Router_Table_Head;
        if (!port) {
            /* create first port */
            port = (DNET *)calloc(1, sizeof(DNET));
            assert(port);
            Router_Table_Head = port;
        } else {
            while (port) {
                if (port->next) {
                    port = port->next;
                } else {
                    /* create next port */
                    dnet = (DNET *)calloc(1, sizeof(DNET));
                    assert(dnet);
                    port->next = dnet;
                    port = port->next;
                    break;
                }
            }
        }
        port->net = snet;
        if (addr) {
            port->mac_len = addr->mac_len;
            for (i = 0; i < MAX_MAC_LEN; i++) {
                port->mac[i] = addr->mac[i];
            }
        } else {
            port->mac_len = 0;
        }
        port->enabled = true;
    }
}

/**
 * Add a route to the router table
 *
 * @param snet - router port SNET
 * @param net - net to be added
 * @param addr - address of router at the net to be added
 */
static void dnet_add(uint16_t snet, uint16_t net, const BACNET_ADDRESS *addr)
{
    DNET *dnet = NULL;
    DNET *port = NULL;
    DNET *prior_dnet = NULL;
    unsigned int i = 0;

    /* make sure NETs are not repeated */
    dnet = dnet_find(net, NULL);
    if (dnet) {
        return;
    }
    /* start with the source network number table */
    port = dnet_find(snet, NULL);
    if (!port) {
        return;
    }
    dnet = port->dnets;
    if (dnet == NULL) {
        /* first DNET to add */
        dnet = (DNET *)calloc(1, sizeof(DNET));
        assert(dnet);
        port->dnets = dnet;
        if (addr) {
            dnet->mac_len = addr->mac_len;
            for (i = 0; i < MAX_MAC_LEN; i++) {
                dnet->mac[i] = addr->mac[i];
            }
        }
        dnet->net = net;
        dnet->enabled = true;
        dnet->next = NULL;
    } else {
        while (dnet != NULL) {
            if (dnet->net == net) {
                /* make sure NETs are not repeated */
                return;
            }
            prior_dnet = dnet;
            dnet = dnet->next;
        }
        /* next DNET to add */
        dnet = (DNET *)calloc(1, sizeof(DNET));
        if (addr) {
            dnet->mac_len = addr->mac_len;
            for (i = 0; i < MAX_MAC_LEN; i++) {
                dnet->mac[i] = addr->mac[i];
            }
        }
        dnet->net = net;
        dnet->enabled = true;
        dnet->next = NULL;
        prior_dnet->next = dnet;
    }
}

/**
 * Free the DNET data of a route
 *
 * @param dnets - router info to be freed
 */
static void dnet_cleanup(DNET *dnets)
{
    DNET *dnet = dnets;
    while (dnet != NULL) {
        log_printf("DNET %u removed\n", (unsigned)dnet->net);
        dnet = dnet->next;
        free(dnets);
        dnets = dnet;
    }
}

/**
 * Initialize the a data link broadcast address
 *
 * @param dest - address to be filled with broadcast designator
 */
static void datalink_get_broadcast_address(BACNET_ADDRESS *dest)
{
    if (dest) {
        dest->mac_len = 0;
        dest->net = BACNET_BROADCAST_NETWORK;
        dest->len = 0;
    }

    return;
}

/**
 * function to send a packet out the BACnet/IP and BACnet MSTP ports
 *
 * @param snet - network number of the directly connected port to send
 * @param dest - address to where packet is sent
 * @param npdu_data - NPCI data to control network destination
 * @param pdu - protocol data unit to be sent
 * @param pdu_len - number of bytes to send
 *
 * @return number of bytes sent
 */
static int datalink_send_pdu(
    uint16_t snet,
    BACNET_ADDRESS *dest,
    BACNET_NPDU_DATA *npdu_data,
    uint8_t *pdu,
    unsigned int pdu_len)
{
    int bytes_sent = 0;

    if (snet == 0) {
        log_printf("BVLC & MS/TP Send to DNET %u\n", (unsigned)dest->net);
        bytes_sent = bip_send_pdu(dest, npdu_data, pdu, pdu_len);
        bytes_sent = dlmstp_send_pdu(dest, npdu_data, pdu, pdu_len);
    } else if (snet == BIP_Net) {
        log_printf("BVLC Send to DNET %u\n", (unsigned)dest->net);
        bytes_sent = bip_send_pdu(dest, npdu_data, pdu, pdu_len);
    } else if (snet == MSTP_Net) {
        log_printf("MS/TP Send to DNET %u\n", (unsigned)dest->net);
        bytes_sent = dlmstp_send_pdu(dest, npdu_data, pdu, pdu_len);
    }

    return bytes_sent;
}

/**
 * Broadcast an I-am-router-to-network message
 *
 * @param snet - the directly connected port network number
 * @param dnet - the network number we are saying we are a router to.
 * If the dnet is 0, send a broadcast out each port with an
 * I-Am-Router-To-Network message containing the network
 * numbers of each accessible network except the networks
 * reachable via the network on which the broadcast is being made.
 */
static void send_i_am_router_to_network(uint16_t snet, uint16_t net)
{
    BACNET_ADDRESS dest;
    bool data_expecting_reply = false;
    BACNET_NPDU_DATA npdu_data;
    int pdu_len = 0;
    int len = 0;
    DNET *port = NULL;
    DNET *dnet = NULL;

    datalink_get_broadcast_address(&dest);
    npdu_encode_npdu_network(
        &npdu_data, NETWORK_MESSAGE_I_AM_ROUTER_TO_NETWORK,
        data_expecting_reply, MESSAGE_PRIORITY_NORMAL);
    /* We don't need src information, since a message can't originate from
       our downstream BACnet network. */
    pdu_len = npdu_encode_pdu(&Tx_Buffer[0], &dest, NULL, &npdu_data);
    if (net) {
        len = encode_unsigned16(&Tx_Buffer[pdu_len], net);
        pdu_len += len;
    } else {
        log_printf("I-Am-Router-To-Network ");
        /*  Each router shall broadcast out each port
            an I-Am-Router-To-Network message containing the network
            numbers of each accessible network except the networks
            reachable via the network on which the broadcast is being made.
            This enables routers to build or update their routing table
            entries for each of the network numbers contained in the message.
        */
        port = Router_Table_Head;
        while (port != NULL) {
            if (port->net != snet) {
                log_printf("%u,", port->net);
                len = encode_unsigned16(&Tx_Buffer[pdu_len], port->net);
                pdu_len += len;
                dnet = port->dnets;
                while (dnet != NULL) {
                    log_printf("%u,", dnet->net);
                    len = encode_unsigned16(&Tx_Buffer[pdu_len], dnet->net);
                    pdu_len += len;
                    dnet = dnet->next;
                }
            }
            port = port->next;
        }
        log_printf("from %u\n", snet);
    }
    datalink_send_pdu(snet, &dest, &npdu_data, &Tx_Buffer[0], pdu_len);
}

/** Sends our Routing Table, built from our DNET[] array, as an ACK.
 * There are two cases here:
 * 1) We are responding to a NETWORK_MESSAGE_INIT_RT_TABLE requesting our table.
 *    We will normally broadcast that response.
 * 2) We are ACKing the receipt of a NETWORK_MESSAGE_INIT_RT_TABLE containing a
 *    routing table, and then we will want to respond to that dst router.
 *    In that case, DNET[] should just have one entry of -1 (no routing table
 *    is sent).
 *
 * @param dest [in] If NULL, Ack will be broadcast to the local BACnet network.
 *  Optionally may designate a particular router destination,
 *  especially when ACKing receipt of this message type.
 */
static void
send_initialize_routing_table_ack(uint8_t snet, const BACNET_ADDRESS *dst)
{
    BACNET_ADDRESS dest;
    bool data_expecting_reply = false;
    BACNET_NPDU_DATA npdu_data;
    int pdu_len = 0;
    int len = 0;
    uint8_t count = 0;
    uint8_t port_id = 1;
    DNET *port = NULL;

    if (dst) {
        bacnet_address_copy(&dest, dst);
    } else {
        datalink_get_broadcast_address(&dest);
    }
    npdu_encode_npdu_network(
        &npdu_data, NETWORK_MESSAGE_INIT_RT_TABLE_ACK, data_expecting_reply,
        MESSAGE_PRIORITY_NORMAL);
    /* We don't need src information, since a message can't originate from
       our downstream BACnet network. */
    pdu_len = npdu_encode_pdu(&Tx_Buffer[0], &dest, NULL, &npdu_data);
    /* First, count the number of Ports we will encode */
    port = Router_Table_Head;
    while (port != NULL) {
        count++;
        port = port->next;
    }
    Tx_Buffer[pdu_len] = count;
    pdu_len++;
    if (count > 0) {
        /* Now encode each BACNET_ROUTER_PORT.
         * We will simply use a positive index for PortID,
         * and have no PortInfo.
         */
        port = Router_Table_Head;
        while (port != NULL) {
            len = encode_unsigned16(&Tx_Buffer[pdu_len], port->net);
            pdu_len += len;
            Tx_Buffer[pdu_len] = port_id;
            pdu_len++;
            port_id++;
            Tx_Buffer[pdu_len] = 0;
            pdu_len++;
            port = port->next;
        }
    }
    /* Now send the message */
    datalink_send_pdu(snet, &dest, &npdu_data, &Tx_Buffer[0], pdu_len);
}

/**
 * Sends a reject network message
 *
 * @param snet [in] Which BACnet network orginated the message.
 * @param dst [in] If NULL, request will be broadcast to the local BACnet
 *                 network.  Otherwise, designates a particular router
 *                 destination.
 * @param reject_reason [in] One of the BACNET_NETWORK_REJECT_REASONS codes.
 */
static void send_reject_message_to_network(
    uint16_t snet,
    const BACNET_ADDRESS *dst,
    uint8_t reject_reason,
    uint16_t dnet)
{
    BACNET_ADDRESS dest;
    bool data_expecting_reply = false;
    BACNET_NPDU_DATA npdu_data;
    int pdu_len = 0;
    int len = 0;

    if (dst) {
        bacnet_address_copy(&dest, dst);
    } else {
        datalink_get_broadcast_address(&dest);
    }
    npdu_encode_npdu_network(
        &npdu_data, NETWORK_MESSAGE_REJECT_MESSAGE_TO_NETWORK,
        data_expecting_reply, MESSAGE_PRIORITY_NORMAL);
    /* We don't need src information, since a message can't originate from
       our downstream BACnet network. */
    pdu_len = npdu_encode_pdu(&Tx_Buffer[0], &dest, NULL, &npdu_data);
    /* encode the reject reason */
    Tx_Buffer[pdu_len] = reject_reason;
    pdu_len++;
    if (dnet) {
        len = encode_unsigned16(&Tx_Buffer[pdu_len], dnet);
        pdu_len += len;
    }
    /* Now send the message */
    datalink_send_pdu(snet, &dest, &npdu_data, &Tx_Buffer[0], pdu_len);
}

/**
 * Sends a who-is-router-to-network message
 *
 * @param dnet [in] Which BACnet network we are seeking
 */
static void send_who_is_router_to_network(uint16_t snet, uint16_t dnet)
{
    BACNET_ADDRESS dest;
    bool data_expecting_reply = false;
    BACNET_NPDU_DATA npdu_data;
    int pdu_len = 0;
    int len = 0;

    datalink_get_broadcast_address(&dest);
    npdu_encode_npdu_network(
        &npdu_data, NETWORK_MESSAGE_WHO_IS_ROUTER_TO_NETWORK,
        data_expecting_reply, MESSAGE_PRIORITY_NORMAL);
    pdu_len = npdu_encode_pdu(&Tx_Buffer[0], &dest, NULL, &npdu_data);
    if (dnet) {
        len = encode_unsigned16(&Tx_Buffer[pdu_len], dnet);
        pdu_len += len;
    }
    /* Now send the message to port */
    datalink_send_pdu(snet, &dest, &npdu_data, &Tx_Buffer[0], pdu_len);
}

/**
 * Handler to manage the Who-Is-Router-To-Network Message
 *
 * 6.6.3.2 Who-Is-Router-To-Network
 *
 * When a router receives a Who-Is-Router-To-Network
 * message specifying a particular network number,
 * it shall search its routing table for the network number
 * contained in the message. If the specified network number
 * is found in its table and the port through which it is
 * reachable is not the port from which the
 * Who-Is-Router-To-Network message was received, the
 * router shall construct an I-Am-Router-To-Network message
 * containing the specified network number and send it to the node
 * that generated the request using a broadcast MAC address,
 * thus allowing other nodes on this network to take
 * advantage of the routing information.
 *
 * If the network number is not found in the routing table, the router
 * shall attempt to discover the next router on the path to the
 * indicated destination network by generating a Who-Is-Router-To-Network
 * message containing the specified destination
 * network number and broadcasting it out all its ports other
 * than the one from which the Who-Is-Router-To-Network message
 * arrived. Two cases are possible. In case one the received
 * Who-Is-Router-To-Network message was from the originating
 * device. For this case, the router shall add SNET and SADR
 * fields before broadcasting the subsequent Who-Is-Router-To-
 * Network. This permits an I-Could-Be-Router-To-Network message
 * to be directed to the originating device. The second case
 * is that the received Who-Is-Router-To-Network message came
 * from another router and it already contains SNET and SADR
 * fields. For this case, the SNET and SADR shall be retained
 * in the newly generated Who-Is-Router-To-Network message.
 *
 * If the Who-Is-Router-To-Network message does not specify a
 * particular destination network number, the router shall
 * construct an I-Am-Router-To-Network message containing a
 * list of all the networks it is able to reach through other than the
 * port from which the Who-Is-Router-To-Network message was
 * received and transmit it in the same manner as described
 * above. The message shall list all networks not flagged as
 * permanently unreachable, including those that are temporarily
 * unreachable due to the imposition of congestion control restrictions.
 * Networks that may be reachable through a PTP
 * connection shall be listed only if the connection is currently established.
 *
 * @param snet [in] source network port number
 * @param src  [in] The routing source information, if any.
 *  If src->net and src->len are 0, there is no routing source information.
 * @param npdu_data [in] Contains a filled-out structure with information
 *  decoded from the NCPI and other NPDU bytes.
 * @param npdu [in]  Buffer containing the rest of the NPDU, following the
 *  bytes that have already been decoded.
 * @param npdu_len [in] The length of the remaining NPDU message in npdu[].
 */
static void who_is_router_to_network_handler(
    uint16_t snet,
    const BACNET_ADDRESS *src,
    const BACNET_NPDU_DATA *npdu_data,
    const uint8_t *npdu,
    uint16_t npdu_len)
{
    DNET *port = NULL;
    uint16_t network = 0;
    uint16_t len = 0;

    (void)src;
    (void)npdu_data;
    if (npdu) {
        if (npdu_len >= 2) {
            len += decode_unsigned16(&npdu[len], &network);
            port = dnet_find(network, NULL);
            if (port) {
                /* found in my list! */
                if (port->net != snet) {
                    /* reachable not through the port this message received */
                    send_i_am_router_to_network(snet, network);
                }
            } else {
                /* discover the next router on the path to the network */
                port = Router_Table_Head;
                while (port) {
                    if (port->net != snet) {
                        send_who_is_router_to_network(port->net, network);
                    }
                    port = port->next;
                }
            }
        } else {
            send_i_am_router_to_network(snet, 0);
        }
    }
}

/**
 * Handler to manage the Network Layer Control Messages received in a packet.
 * This handler is called if the NCPI bit 7 indicates that this packet is a
 * network layer message and there is no further DNET to pass it to.
 * The NCPI has already been decoded into the npdu_data structure.
 *
 * @param snet [in] source network port number
 * @param src  [in] The routing source information, if any.
 *  If src->net and src->len are 0, there is no routing source information.
 * @param npdu_data [in] Contains a filled-out structure with information
 *  decoded from the NCPI and other NPDU bytes.
 * @param npdu [in]  Buffer containing the rest of the NPDU, following the
 *  bytes that have already been decoded.
 * @param npdu_len [in] The length of the remaining NPDU message in npdu[].
 */
static void network_control_handler(
    uint16_t snet,
    BACNET_ADDRESS *src,
    BACNET_NPDU_DATA *npdu_data,
    uint8_t *npdu,
    uint16_t npdu_len)
{
    uint16_t npdu_offset = 0;
    uint16_t dnet = 0;
    uint16_t len = 0;
    const char *msg_name = NULL;

    (void)src;
    (void)npdu_data;
    msg_name = bactext_network_layer_msg_name(npdu_data->network_message_type);
    fprintf(stderr, "Received %s\n", msg_name);
    switch (npdu_data->network_message_type) {
        case NETWORK_MESSAGE_WHO_IS_ROUTER_TO_NETWORK:
            who_is_router_to_network_handler(
                snet, src, npdu_data, npdu, npdu_len);
            break;
        case NETWORK_MESSAGE_I_AM_ROUTER_TO_NETWORK:
            /* add its DNETs to our routing table */
            fprintf(stderr, "for Networks: ");
            len = 2;
            while (npdu_len >= len) {
                len = decode_unsigned16(&npdu[npdu_offset], &dnet);
                fprintf(stderr, "%hu", dnet);
                dnet_add(snet, dnet, src);
                npdu_len -= len;
                npdu_offset += len;
                if (npdu_len) {
                    fprintf(stderr, ", ");
                }
            }
            fprintf(stderr, ".\n");
            break;
        case NETWORK_MESSAGE_I_COULD_BE_ROUTER_TO_NETWORK:
            /* Do nothing, same as previous case. */
            break;
        case NETWORK_MESSAGE_REJECT_MESSAGE_TO_NETWORK:
            if (npdu_len >= 3) {
                decode_unsigned16(&npdu[1], &dnet);
                fprintf(stderr, "for Network:%hu\n", dnet);
                switch (npdu[0]) {
                    case 0:
                        fprintf(stderr, "Reason: Other Error.\n");
                        break;
                    case 1:
                        fprintf(stderr, "Reason: Network unreachable.\n");
                        break;
                    case 2:
                        fprintf(stderr, "Reason: Network is busy.\n");
                        break;
                    case 3:
                        fprintf(
                            stderr, "Reason: Unknown network message type.\n");
                        break;
                    case 4:
                        fprintf(stderr, "Reason: Message too long.\n");
                        break;
                    case 5:
                        fprintf(stderr, "Reason: Security Error.\n");
                        break;
                    case 6:
                        fprintf(stderr, "Reason: Invalid address length.\n");
                        break;
                    default:
                        fprintf(stderr, "Reason: %u\n", (unsigned int)npdu[0]);
                        break;
                }
            }
            break;
        case NETWORK_MESSAGE_ROUTER_BUSY_TO_NETWORK:
        case NETWORK_MESSAGE_ROUTER_AVAILABLE_TO_NETWORK:
            /* Do nothing - don't support upstream traffic congestion control */
            break;
        case NETWORK_MESSAGE_INIT_RT_TABLE:
            /* If sent with Number of Ports == 0, we respond with
             * NETWORK_MESSAGE_INIT_RT_TABLE_ACK and a list of all our
             * reachable networks.
             */
            if (npdu_len > 0) {
                /* If Number of Ports is 0, broadcast our "full" table */
                if (npdu[0] == 0) {
                    send_initialize_routing_table_ack(snet, NULL);
                } else {
                    /* they sent us a list */
                    int net_count = npdu[0];
                    while (net_count--) {
                        int i = 1;
                        /* DNET */
                        decode_unsigned16(&npdu[i], &dnet);
                        /* update routing table */
                        dnet_add(snet, dnet, src);
                        if (npdu[i + 3] > 0) {
                            /* find next NET value */
                            i = npdu[i + 3] + 4;
                        } else {
                            i += 4;
                        }
                    }
                    send_initialize_routing_table_ack(snet, NULL);
                }
                break;
            }
            break;
        case NETWORK_MESSAGE_INIT_RT_TABLE_ACK:
            /* Do nothing with the routing table info, since don't support
             * upstream traffic congestion control */
            break;
        case NETWORK_MESSAGE_ESTABLISH_CONNECTION_TO_NETWORK:
        case NETWORK_MESSAGE_DISCONNECT_CONNECTION_TO_NETWORK:
            /* Do nothing - don't support PTP half-router control */
            break;
        default:
            /* An unrecognized message is bad; send an error response. */
            send_reject_message_to_network(
                snet, src, NETWORK_REJECT_UNKNOWN_MESSAGE_TYPE, 0);
            break;
    }
}

/**
 * Fill the router src address with this port router, router network number,
 * and the original src address.
 *
 * @param router_src [in] The src BACNET_ADDRESS for this routed message.
 * @param snet [in] The source network port where the message came from
 * @param src [in] The BACNET_ADDRESS of the message's original src.
 */
static void routed_src_address(
    BACNET_ADDRESS *router_src, uint16_t snet, const BACNET_ADDRESS *src)
{
    unsigned int i = 0;

    if (router_src && src) {
        /* copy our directly connected port address */
        if (port_find(snet, router_src)) {
            if (src->net) {
                /* from a router - add router our table */
                dnet_add(snet, src->net, src);
                /* the routed address stays the same */
                router_src->net = src->net;
                router_src->len = src->len;
                for (i = 0; i < MAX_MAC_LEN; i++) {
                    router_src->adr[i] = src->adr[i];
                }
            } else {
                /* from our directly connected port */
                router_src->net = snet;
                router_src->len = src->mac_len;
                for (i = 0; i < MAX_MAC_LEN; i++) {
                    router_src->adr[i] = src->mac[i];
                }
            }
        }
    }
}

/**
 * If a BACnet NPDU is received with NPCI indicating that the message
 * should be relayed by virtue of the presence of a non-broadcast
 * DNET, the router shall search its routing table for the indicated
 * network number. Normal routing procedures are described in 6.5.
 * If, however, the network number cannot be found in the routing
 * table or through the use of the Who-Is-Router-To-Network message,
 * the router shall generate a Reject-Message-To-Network message and
 * send it to the node that originated the BACnet NPDU.
 * If the NPCI indicates either a remote or global broadcast,
 * the message shall be processed as described in 6.3.2.
 *
 * @param src [in] The BACNET_ADDRESS of the message's source.
 * @param dest [in] The BACNET_ADDRESS of the message's destination.
 * @param DNET_list [in] List of our reachable downstream BACnet Network
 * numbers. Normally just one valid entry; terminated with a -1 value.
 * @param apdu [in] The apdu portion of the request, to be processed.
 * @param apdu_len [in] The total (remaining) length of the apdu.
 */
static void routed_apdu_handler(
    uint16_t snet,
    BACNET_NPDU_DATA *npdu,
    BACNET_ADDRESS *src,
    BACNET_ADDRESS *dest,
    uint8_t *apdu,
    uint16_t apdu_len)
{
    DNET *port = NULL;
    BACNET_ADDRESS local_dest;
    BACNET_ADDRESS remote_dest;
    BACNET_ADDRESS router_src;
    int npdu_len = 0;

    /* for broadcast messages no search is needed */
    if (dest->net == BACNET_BROADCAST_NETWORK) {
        /*  A global broadcast, indicated by a DNET of X'FFFF', is sent
            to all networks through all routers. Upon receipt of a message
            with the global broadcast DNET network number, a router shall
            decrement the Hop Count. If the Hop Count is still greater
            than zero, then the router shall broadcast the message on all
            directly connected networks except the network of origin, using
            the broadcast MAC address appropriate for each destination network.
            If the Hop Count is zero, then the router shall discard
            the message. In order for the message to be disseminated globally,
            the originating device shall use a broadcast MAC address
            on the originating network so that all attached routers may
            receive the message and propagate it further. */
        datalink_get_broadcast_address(&local_dest);
        npdu->hop_count--;
        routed_src_address(&router_src, snet, src);
        /* encode both source and destination for broadcast */
        npdu_len =
            npdu_encode_pdu(&Tx_Buffer[0], &local_dest, &router_src, npdu);
        memmove(&Tx_Buffer[npdu_len], apdu, apdu_len);
        /* send to my other ports */
        log_printf("Routing a BROADCAST from %u\n", (unsigned)snet);
        port = Router_Table_Head;
        while (port != NULL) {
            if (port->net != snet) {
                datalink_send_pdu(
                    port->net, &local_dest, npdu, &Tx_Buffer[0],
                    npdu_len + apdu_len);
            }
            port = port->next;
        }
        return;
    }
    remote_dest = *dest;
    port = dnet_find(dest->net, &remote_dest);
    if (port) {
        if (port->net == dest->net) {
            log_printf("Routing to Port %u\n", (unsigned)dest->net);
            /*  Case 1: the router is directly
                connected to the network referred to by DNET. */
            /*  In the first case, DNET, DADR, and Hop
                Count shall be removed from the NPCI and the message shall be
                sent directly to the destination device with DA set equal to
                DADR. The control octet shall be adjusted accordingly to
                indicate only the presence of SNET and SADR. */
            memmove(&local_dest.mac, dest->adr, MAX_MAC_LEN);
            local_dest.mac_len = dest->len;
            local_dest.net = 0;
            npdu->hop_count--;
            routed_src_address(&router_src, snet, src);
            npdu_len =
                npdu_encode_pdu(&Tx_Buffer[0], &local_dest, &router_src, npdu);
            memmove(&Tx_Buffer[npdu_len], apdu, apdu_len);
            datalink_send_pdu(
                port->net, &local_dest, npdu, &Tx_Buffer[0],
                npdu_len + apdu_len);
        } else {
            log_printf(
                "Routing to another Router %u\n", (unsigned)remote_dest.net);
            /*  Case 2: the message must be
                relayed to another router for further transmission */
            /*  In the second case, if the Hop Count is greater than zero,
                the message shall be sent to the next router on the
                path to the destination network.
                If the Hop Count is zero, then the message shall be
                discarded. */
            npdu->hop_count--;
            routed_src_address(&router_src, snet, src);
            npdu_len =
                npdu_encode_pdu(&Tx_Buffer[0], &remote_dest, &router_src, npdu);
            memmove(&Tx_Buffer[npdu_len], apdu, apdu_len);
            datalink_send_pdu(
                port->net, &remote_dest, npdu, &Tx_Buffer[0],
                npdu_len + apdu_len);
        }
    } else if (dest->net) {
        log_printf("Routing to Unknown Route %u\n", (unsigned)dest->net);
        /* Case 3: a global broadcast is required. */
        dest->mac_len = 0;
        npdu->hop_count--;
        /* encode both source and destination */
        routed_src_address(&router_src, snet, src);
        npdu_len = npdu_encode_pdu(&Tx_Buffer[0], dest, &router_src, npdu);
        memmove(&Tx_Buffer[npdu_len], apdu, apdu_len);
        /* send to all other ports */
        port = Router_Table_Head;
        while (port != NULL) {
            if (port->net != snet) {
                datalink_send_pdu(
                    port->net, dest, npdu, &Tx_Buffer[0], npdu_len + apdu_len);
            }
            port = port->next;
        }
        /*  If the next router is unknown, an attempt shall be made to
            identify it using a Who-Is-Router-To-Network message. */
        send_who_is_router_to_network(0, dest->net);
    }
}

/**
 * Handler for the routing packets only
 *
 * @param src  [out] Returned with routing source information if the NPDU
 *  has any and if this points to non-null storage for it.
 *  If src->net and src->len are 0 on return, there is no
 *  routing source information.
 *  This src describes the original source of the message when
 *  it had to be routed to reach this BACnet Device, and this
 *  is passed down into the apdu_handler.
 * @param DNET_list [in] List of our reachable downstream BACnet Network
 *  numbers terminated with a -1 value.
 * @param pdu [in]  Buffer containing the NPDU and APDU of the received packet.
 * @param pdu_len [in] The size of the received message in the pdu[] buffer.
 */
static void my_routing_npdu_handler(
    uint16_t snet, BACNET_ADDRESS *src, uint8_t *pdu, uint16_t pdu_len)
{
    int apdu_offset = 0;
    BACNET_ADDRESS dest = { 0 };
    BACNET_NPDU_DATA npdu_data = { 0 };

    if (!pdu) {
        /* no packet */
    } else if (pdu[0] == BACNET_PROTOCOL_VERSION) {
        apdu_offset = bacnet_npdu_decode(pdu, pdu_len, &dest, src, &npdu_data);
        if (apdu_offset <= 0) {
            fprintf(stderr, "NPDU: Decoding failed; Discarded!\n");
        } else if (npdu_data.network_layer_message) {
            if ((dest.net == 0) || (dest.net == BACNET_BROADCAST_NETWORK)) {
                network_control_handler(
                    snet, src, &npdu_data, &pdu[apdu_offset],
                    (uint16_t)(pdu_len - apdu_offset));
            } else {
                /* The DNET is set, but we don't support downstream routers,
                 * so we just silently drop this network layer message,
                 * since only routers can handle it (even if for our DNET) */
            }
        } else if ((apdu_offset > 0) && (apdu_offset <= pdu_len)) {
            if ((dest.net == 0) || (dest.net == BACNET_BROADCAST_NETWORK) ||
                (npdu_data.hop_count > 1)) {
                /* only handle the version that we know how to handle */
                /* and we are not a router, so ignore messages with
                   routing information cause they are not for us */
                if ((dest.net == BACNET_BROADCAST_NETWORK) &&
                    ((pdu[apdu_offset] & 0xF0) ==
                     PDU_TYPE_CONFIRMED_SERVICE_REQUEST)) {
                    /* hack for 5.4.5.1 - IDLE */
                    /* ConfirmedBroadcastReceived */
                    /* then enter IDLE - ignore the PDU */
                } else {
                    routed_apdu_handler(
                        snet, &npdu_data, src, &dest, &pdu[apdu_offset],
                        (uint16_t)(pdu_len - apdu_offset));
                    /* add a Device object and application layer */
                    if ((dest.net == 0) ||
                        (dest.net == BACNET_BROADCAST_NETWORK)) {
                        apdu_handler(
                            src, &pdu[apdu_offset],
                            (uint16_t)(pdu_len - apdu_offset));
                    }
                }
            } else {
                fprintf(
                    stderr, "NPDU: DNET=%u.  Discarded!\n", (unsigned)dest.net);
            }
        }
    } else {
        /* unsupported protocol version */
    }

    return;
}

/**
 * Initialize the BACnet MSTP and BACnet/IP data links
 */
static void datalink_init(void)
{
    char *pEnv = NULL;
    BACNET_ADDRESS my_address = { 0 };

    pEnv = getenv("BACNET_ROUTER_DEBUG");
    if (pEnv) {
        bip_debug_enable();
        Debug_Enabled = true;
        log_printf("Debug=enabled\n");
    } else {
        fprintf(stderr, "Debug=disabled\n");
    }
    /* BACnet/IP Initialization */
    pEnv = getenv("BACNET_IP_PORT");
    if (pEnv) {
        bip_set_port((uint16_t)strtol(pEnv, NULL, 0));
    } else {
        /* BIP_Port is statically initialized to 0xBAC0,
         * so if it is different, then it was programmatically altered,
         * and we shouldn't just stomp on it here.
         * Unless it is set below 1024, since:
         * "The range for well-known ports managed by the IANA is 0-1023."
         */
        if (bip_get_port() < 1024) {
            bip_set_port(0xBAC0U);
        }
    }
    if (!bip_init(getenv("BACNET_IFACE"))) {
        exit(1);
    }
    atexit(bip_cleanup);
    /* MS/TP Initialization */
    pEnv = getenv("BACNET_MAX_INFO_FRAMES");
    if (pEnv) {
        dlmstp_set_max_info_frames(strtol(pEnv, NULL, 0));
    } else {
        dlmstp_set_max_info_frames(128);
    }
    pEnv = getenv("BACNET_MAX_MASTER");
    if (pEnv) {
        dlmstp_set_max_master(strtol(pEnv, NULL, 0));
    } else {
        dlmstp_set_max_master(127);
    }
    pEnv = getenv("BACNET_MSTP_BAUD");
    if (pEnv) {
        dlmstp_set_baud_rate(strtol(pEnv, NULL, 0));
    } else {
        dlmstp_set_baud_rate(38400);
    }
    pEnv = getenv("BACNET_MSTP_MAC");
    if (pEnv) {
        dlmstp_set_mac_address(strtol(pEnv, NULL, 0));
    } else {
        dlmstp_set_mac_address(127);
    }
    if (!dlmstp_init(getenv("BACNET_MSTP_IFACE"))) {
        exit(1);
    }
    atexit(dlmstp_cleanup);
    /* router network numbers */
    pEnv = getenv("BACNET_IP_NET");
    if (pEnv) {
        BIP_Net = strtol(pEnv, NULL, 0);
    } else {
        BIP_Net = 1;
    }
    /* configure the first entry in the table - home port */
    bip_get_my_address(&my_address);
    port_add(BIP_Net, &my_address);
    /* MS/TP network */
    pEnv = getenv("BACNET_MSTP_NET");
    if (pEnv) {
        MSTP_Net = strtol(pEnv, NULL, 0);
    } else {
        MSTP_Net = 2;
    }
    /* configure the next entry in the table */
    dlmstp_get_my_address(&my_address);
    port_add(MSTP_Net, &my_address);
}

/**
 * Cleanup memory
 *
 */
static void cleanup(void)
{
    DNET *port = NULL;

    fprintf(stderr, "Cleaning up...\n");
    /* clean up the remote networks */
    port = Router_Table_Head;
    while (port != NULL) {
        dnet_cleanup(port->dnets);
        port = port->next;
    }
    /* clean up the directly connected networks */
    dnet_cleanup(Router_Table_Head);
}

#if defined(_WIN32)
static BOOL WINAPI CtrlCHandler(DWORD dwCtrlType)
{
    dwCtrlType = dwCtrlType;

    /* signal to main loop to exit */
    Exit_Requested = true;
    while (Exit_Requested) {
        Sleep(100);
    }
    exit(0);
}

static void control_c_hooks(void)
{
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), ENABLE_PROCESSED_INPUT);
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlCHandler, TRUE);
}
#else
static void sig_int(int signo)
{
    (void)signo;
    Exit_Requested = true;
    exit(0);
}

static void signal_init(void)
{
    signal(SIGINT, sig_int);
    signal(SIGHUP, sig_int);
    signal(SIGTERM, sig_int);
}

static void control_c_hooks(void)
{
    signal_init();
}
#endif

static void Init_Service_Handlers()
{
    apdu_set_unconfirmed_handler(
        SERVICE_UNCONFIRMED_WHO_IS, handler_who_is_unicast);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_READ_PROP_MULTIPLE, handler_read_property_multiple);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_WRITE_PROPERTY, handler_write_property);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_READ_RANGE, handler_read_range);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_REINITIALIZE_DEVICE, handler_reinitialize_device);
    apdu_set_unconfirmed_handler(
        SERVICE_UNCONFIRMED_UTC_TIME_SYNCHRONIZATION, handler_timesync_utc);
    apdu_set_unconfirmed_handler(
        SERVICE_UNCONFIRMED_TIME_SYNCHRONIZATION, handler_timesync);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL,
        handler_device_communication_control);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_i_am_add);
}

/*
 * FIXME: This is a hack to get things linking correctly
 */
extern int cov_subscribe(void)
{
    return 0;
}

extern int Device_Value_List_Supported(void)
{
    return 0;
}

extern int Encode_RR_payload(void)
{
    return 0;
}

extern bool Device_COV(BACNET_OBJECT_TYPE object_type, uint32_t object_instance)
{
    return false;
}

extern void
Device_COV_Clear(BACNET_OBJECT_TYPE object_type, uint32_t object_instance)
{
    return;
}

extern bool Device_Encode_Value_List(
    BACNET_OBJECT_TYPE object_type,
    uint32_t object_instance,
    BACNET_PROPERTY_VALUE *value_list)
{
    return false;
}

extern int Device_Add_List_Element(BACNET_LIST_ELEMENT_DATA *list_element)
{
    return BACNET_STATUS_ERROR;
}

extern int Device_Remove_List_Element(BACNET_LIST_ELEMENT_DATA *list_element)
{
    return BACNET_STATUS_ERROR;
}

extern bool Device_Write_Property_Local(BACNET_WRITE_PROPERTY_DATA *wp_data)
{
    return false;
}

bool Device_Delete_Object(BACNET_DELETE_OBJECT_DATA *data)
{
    return false;
}

bool Device_Create_Object(BACNET_CREATE_OBJECT_DATA *data){
    return false;
}

int main(int argc, char *argv[])
{
    BACNET_ADDRESS src = { 0 };
    uint16_t pdu_len = 0;

    Init_Service_Handlers();

#ifdef __AFL_HAVE_MANUAL_CONTROL
  __AFL_INIT();
#endif

    pdu_len = read(0, &BIP_Rx_Buffer[0], sizeof(BIP_Rx_Buffer));

    if(pdu_len == -1) {
        perror("read");
        exit(1);
    }

    /* process fuzz input*/
    if (pdu_len) {
        my_routing_npdu_handler(BIP_Net, &src, &BIP_Rx_Buffer[0], pdu_len);
    }

    return 0;
}
