/************************************************************************************
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *
 *  This program is the proprietary software of Broadcom Corporation and/or its
 *  licensors, and may only be used, duplicated, modified or distributed
 *  pursuant to the terms and conditions of a separate, written license
 *  agreement executed between you and Broadcom (an "Authorized License").
 *  Except as set forth in an Authorized License, Broadcom grants no license
 *  (express or implied), right to use, or waiver of any kind with respect to
 *  the Software, and Broadcom expressly reserves all rights in and to the
 *  Software and all intellectual property rights therein.
 *  IF YOU HAVE NO AUTHORIZED LICENSE, THEN YOU HAVE NO RIGHT TO USE THIS
 *  SOFTWARE IN ANY WAY, AND SHOULD IMMEDIATELY NOTIFY BROADCOM AND DISCONTINUE
 *  ALL USE OF THE SOFTWARE.
 *
 *  Except as expressly set forth in the Authorized License,
 *
 *  1.     This program, including its structure, sequence and organization,
 *         constitutes the valuable trade secrets of Broadcom, and you shall
 *         use all reasonable efforts to protect the confidentiality thereof,
 *         and to use this information only in connection with your use of
 *         Broadcom integrated circuit products.
 *
 *  2.     TO THE MAXIMUM EXTENT PERMITTED BY LAW, THE SOFTWARE IS PROVIDED
 *         "AS IS" AND WITH ALL FAULTS AND BROADCOM MAKES NO PROMISES,
 *         REPRESENTATIONS OR WARRANTIES, EITHER EXPRESS, IMPLIED, STATUTORY,
 *         OR OTHERWISE, WITH RESPECT TO THE SOFTWARE.  BROADCOM SPECIFICALLY
 *         DISCLAIMS ANY AND ALL IMPLIED WARRANTIES OF TITLE, MERCHANTABILITY,
 *         NONINFRINGEMENT, FITNESS FOR A PARTICULAR PURPOSE, LACK OF VIRUSES,
 *         ACCURACY OR COMPLETENESS, QUIET ENJOYMENT, QUIET POSSESSION OR
 *         CORRESPONDENCE TO DESCRIPTION. YOU ASSUME THE ENTIRE RISK ARISING OUT
 *         OF USE OR PERFORMANCE OF THE SOFTWARE.
 *
 *  3.     TO THE MAXIMUM EXTENT PERMITTED BY LAW, IN NO EVENT SHALL BROADCOM OR
 *         ITS LICENSORS BE LIABLE FOR
 *         (i)   CONSEQUENTIAL, INCIDENTAL, SPECIAL, INDIRECT, OR EXEMPLARY
 *               DAMAGES WHATSOEVER ARISING OUT OF OR IN ANY WAY RELATING TO
 *               YOUR USE OF OR INABILITY TO USE THE SOFTWARE EVEN IF BROADCOM
 *               HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES; OR
 *         (ii)  ANY AMOUNT IN EXCESS OF THE AMOUNT ACTUALLY PAID FOR THE
 *               SOFTWARE ITSELF OR U.S. $1, WHICHEVER IS GREATER. THESE
 *               LIMITATIONS SHALL APPLY NOTWITHSTANDING ANY FAILURE OF
 *               ESSENTIAL PURPOSE OF ANY LIMITED REMEDY.
 *
 ************************************************************************************/

/************************************************************************************
 *
 *  Filename:      btif_hl.c
 *
 *  Description:   Health Device Profile Bluetooth Interface
 *
 *
 ***********************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <cutils/sockets.h>
#include <cutils/log.h>

#include <hardware/bluetooth.h>
#include <hardware/bt_hl.h>

//#define LOG_TAG "BTIF_HL"
#include "btif_common.h"
#include "btif_util.h"

#include "bd.h"
#include "bta_api.h"
#include "bta_hl_api.h"
#include "mca_api.h"
#include "btif_hl.h"
#include "gki.h"



extern int btif_hl_update_maxfd( int max_org_s);
extern void btif_hl_select_monitor_callback( fd_set *p_cur_set, fd_set *p_org_set );
extern void btif_hl_select_wakeup_callback( fd_set *p_org_set , int wakeup_signal );
extern int btif_hl_update_maxfd( int max_org_s);
extern void btif_hl_select_monitor_callback( fd_set *p_cur_set, fd_set *p_org_set );
extern void btif_hl_select_wakeup_callback( fd_set *p_org_set , int wakeup_signal );
extern void btif_hl_soc_thread_init(void);
extern void btif_hl_release_mcl_sockets(UINT8 app_idx, UINT8 mcl_idx);
extern BOOLEAN btif_hl_create_socket(UINT8 app_idx, UINT8 mcl_idx, UINT8 mdl_idx);
extern void btif_hl_release_socket(UINT8 app_idx, UINT8 mcl_idx, UINT8 mdl_idx);

btif_hl_cb_t btif_hl_cb;
btif_hl_cb_t *p_btif_hl_cb = &btif_hl_cb;

/************************************************************************************
**  Static variables
************************************************************************************/
static bthl_callbacks_t  bt_hl_callbacks_cb;
static bthl_callbacks_t *bt_hl_callbacks=NULL;

/* signal socketpair to wake up select loop */

const int btif_hl_signal_select_wakeup = 1;
const int btif_hl_signal_select_exit = 2;
const int btif_hl_signal_select_close_connected = 3;

static int listen_s = -1;
static int connected_s = -1;
static int select_thread_id = -1;
static int signal_fds[2];
static BUFFER_Q soc_queue;

static inline int btif_hl_select_wakeup(void);
static inline int btif_hl_select_exit(void);
static inline int btif_hl_select_close_connected(void);
static UINT8 btif_hl_get_next_app_id(void);
static int btif_hl_get_next_channel_id(UINT8 app_id);
static void btif_hl_init_next_app_id(void);
static void btif_hl_init_next_channel_id(void);
static void btif_hl_ctrl_cback(tBTA_HL_CTRL_EVT event, tBTA_HL_CTRL *p_data);
static void btif_hl_set_state(btif_hl_state_t state);
static btif_hl_state_t btif_hl_get_state(void);

#define CHECK_CALL_CBACK(P_CB, P_CBACK, ...)\
    if (P_CB && P_CB->P_CBACK) {            \
        P_CB->P_CBACK(__VA_ARGS__);         \
    }                                       \
    else {                                  \
        ASSERTC(0, "Callback is NULL", 0);  \
    }


#define BTIF_HL_CALL_CBACK(P_CB, P_CBACK, ...)\
     if((p_btif_hl_cb->state != BTIF_HL_STATE_DISABLING) &&\
         (p_btif_hl_cb->state != BTIF_HL_STATE_DISABLED))  \
     {                                                     \
        if (P_CB && P_CB->P_CBACK) {                       \
            P_CB->P_CBACK(__VA_ARGS__);                    \
        }                                                  \
        else {                                             \
            ASSERTC(0, "Callback is NULL", 0);             \
        }                                                  \
    }


#define CHECK_BTHL_INIT() if (bt_hl_callbacks == NULL)\
    {\
        BTIF_TRACE_WARNING1("BTHL: %s: BTHL not initialized", __FUNCTION__);\
        return BT_STATUS_NOT_READY;\
    }\
    else\
    {\
        BTIF_TRACE_EVENT1("BTHL: %s", __FUNCTION__);\
    }


static const btif_hl_data_type_cfg_t data_type_table[] = {
    /* Data Specilization                   Ntx     Nrx (from Bluetooth SIG's HDP whitepaper)*/
    {BTIF_HL_DATA_TYPE_PULSE_OXIMETER,      9216,   256},
    {BTIF_HL_DATA_TYPE_BLOOD_PRESSURE_MON,  896,    224},
    {BTIF_HL_DATA_TYPE_BODY_THERMOMETER,    896,    224},
    {BTIF_HL_DATA_TYPE_BODY_WEIGHT_SCALE,   896,    224},
    {BTIF_HL_DATA_TYPE_GLUCOSE_METER,       896,    224},
    {BTIF_HL_DATA_TYPE_STEP_COUNTER,        6624,   224}
};

#define BTIF_HL_DATA_TABLE_SIZE  (sizeof(data_type_table) / sizeof(btif_hl_data_type_cfg_t))
#define BTIF_HL_DEFAULT_SRC_TX_APDU_SIZE   10240 /* use this size if the data type is not defined in the table; for futur proof */
#define BTIF_HL_DEFAULT_SRC_RX_APDU_SIZE   512  /* use this size if the data type is not defined in the table; for futur proof */
/************************************************************************************
**  Static utility functions
************************************************************************************/

/*******************************************************************************
**
** Function      btif_hl_find_mdl_idx
**
** Description  Find the MDL index using MDL ID
**
** Returns      BOOLEAN
**
*******************************************************************************/
static BOOLEAN btif_hl_find_mdl_idx(UINT8 app_idx, UINT8 mcl_idx, UINT16 mdl_id,
                                    UINT8 *p_mdl_idx)
{
    btif_hl_mcl_cb_t      *p_mcb  = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
    BOOLEAN found=FALSE;
    UINT8 i;

    for (i=0; i < BTA_HL_NUM_MDLS_PER_MCL ; i ++)
    {
        if (p_mcb->mdl[i].in_use  &&
            (mdl_id !=0) &&
            (p_mcb->mdl[i].mdl_id== mdl_id))
        {
            found = TRUE;
            *p_mdl_idx = i;
            break;
        }
    }

    BTIF_TRACE_DEBUG4("%s found=%d mdl_id=%d mdl_idx=%d ",
                      __FUNCTION__,found, mdl_id, i);

    return found;
}

/*******************************************************************************
**
** Function      btif_hl_get_buf
**
** Description   get buffer
**
** Returns     void
**
*******************************************************************************/
void * btif_hl_get_buf(UINT16 size)
{
    void *p_new;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);
    BTIF_TRACE_DEBUG2("ret size=%d GKI_MAX_BUF_SIZE=%d",size, 6000);

    if (size < 6000)
    {
        p_new = GKI_getbuf(size);
    }
    else
    {
        BTIF_TRACE_DEBUG0("btif_hl_get_buf use HL large data pool");
        p_new = GKI_getpoolbuf(4);
    }

    return p_new;
}

void btif_hl_free_buf(void **p)
{
    if (*p != NULL)
    {
        BTIF_TRACE_DEBUG1("%s OK", __FUNCTION__ );
        GKI_freebuf(*p);
        *p = NULL;
    }
    else
        BTIF_TRACE_ERROR1("%s NULL pointer",__FUNCTION__ );
}
/*******************************************************************************
**
** Function      btif_hl_is_the_first_reliable_existed
**
** Description  This function checks whether the first reliable DCH channel
**              has been setup on the MCL or not
**
** Returns      BOOLEAN - TRUE exist
**                        FALSE does not exist
**
*******************************************************************************/
BOOLEAN btif_hl_is_the_first_reliable_existed(UINT8 app_idx, UINT8 mcl_idx )
{
    btif_hl_mcl_cb_t          *p_mcb  =BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
    BOOLEAN is_existed =FALSE;
    UINT8 i ;

    for (i=0; i< BTA_HL_NUM_MDLS_PER_MCL; i++)
    {
        if (p_mcb->mdl[i].in_use && p_mcb->mdl[i].is_the_first_reliable)
        {
            is_existed = TRUE;
            break;
        }
    }

    BTIF_TRACE_DEBUG1("bta_hl_is_the_first_reliable_existed is_existed=%d  ",is_existed );
    return is_existed;
}



/*******************************************************************************
**
** Function      btif_hl_clean_delete_mdl
**
** Description   Cleanup the delete mdl control block
**
** Returns     Nothing
**
*******************************************************************************/
static void btif_hl_clean_delete_mdl(btif_hl_delete_mdl_t *p_cb)
{
    BTIF_TRACE_DEBUG1("%s", __FUNCTION__ );
    memset(p_cb, 0 , sizeof(btif_hl_delete_mdl_t));
}

/*******************************************************************************
**
** Function      btif_hl_clean_pcb
**
** Description   Cleanup the pending chan control block
**
** Returns     Nothing
**
*******************************************************************************/
static void btif_hl_clean_pcb(btif_hl_pending_chan_cb_t *p_pcb)
{
    BTIF_TRACE_DEBUG1("%s", __FUNCTION__ );
    memset(p_pcb, 0 , sizeof(btif_hl_pending_chan_cb_t));
}


/*******************************************************************************
**
** Function      btif_hl_clean_mdl_cb
**
** Description   Cleanup the MDL control block
**
** Returns     Nothing
**
*******************************************************************************/
static void btif_hl_clean_mdl_cb(btif_hl_mdl_cb_t *p_dcb)
{
    BTIF_TRACE_DEBUG1("%s", __FUNCTION__ );
    btif_hl_free_buf((void **) &p_dcb->p_rx_pkt);
    btif_hl_free_buf((void **) &p_dcb->p_tx_pkt);
    memset(p_dcb, 0 , sizeof(btif_hl_mdl_cb_t));
}


/*******************************************************************************
**
** Function      btif_hl_reset_mcb
**
** Description   Reset MCL control block
**
** Returns      BOOLEAN
**
*******************************************************************************/
static void btif_hl_clean_mcl_cb(UINT8 app_idx, UINT8 mcl_idx)
{
    btif_hl_mcl_cb_t     *p_mcb;
    BTIF_TRACE_DEBUG3("%s app_idx=%d, mcl_idx=%d", __FUNCTION__,app_idx, mcl_idx);
    p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
    memset(p_mcb, 0, sizeof(btif_hl_mcl_cb_t));
}


/*******************************************************************************
**
** Function      btif_hl_find_sdp_idx_using_mdep_filter
**
** Description  This function finds the SDP record index using MDEP filter parameters
**
** Returns      BOOLEAN
**
*******************************************************************************/
static void btif_hl_reset_mdep_filter(UINT8 app_idx)
{
    btif_hl_app_cb_t          *p_acb  =BTIF_HL_GET_APP_CB_PTR(app_idx);
    p_acb->filter.num_elems = 0;
}

/*******************************************************************************
**
** Function      btif_hl_find_sdp_idx_using_mdep_filter
**
** Description  This function finds the SDP record index using MDEP filter parameters
**
** Returns      BOOLEAN
**
*******************************************************************************/
static BOOLEAN btif_hl_find_sdp_idx_using_mdep_filter(UINT8 app_idx, UINT8 mcl_idx, UINT8 *p_sdp_idx)
{
    btif_hl_app_cb_t          *p_acb  =BTIF_HL_GET_APP_CB_PTR(app_idx);
    btif_hl_mcl_cb_t          *p_mcb  =BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
    UINT8                   i, j, num_recs,num_elems, num_mdeps, mdep_cnt, mdep_idx;
    tBTA_HL_MDEP_ROLE       peer_mdep_role;
    UINT16                  data_type;
    tBTA_HL_SDP_MDEP_CFG    *p_mdep;
    BOOLEAN                 found = FALSE;
    BOOLEAN                 elem_found;

    num_recs = p_mcb->sdp.num_recs;
    num_elems = p_acb->filter.num_elems;
    if (!num_elems)
    {
        *p_sdp_idx = 0;
        found = TRUE;
        return found;
    }

    for (i=0; i<num_recs; i++)
    {
        num_mdeps = p_mcb->sdp.sdp_rec[i].num_mdeps;
        for (j=0; j<num_elems; j++ )
        {
            data_type = p_acb->filter.elem[j].data_type;
            peer_mdep_role = p_acb->filter.elem[j].peer_mdep_role;
            elem_found = FALSE;
            mdep_cnt =0;
            mdep_idx=0;
            while (!elem_found && mdep_idx < num_mdeps )
            {
                p_mdep = &(p_mcb->sdp.sdp_rec[i].mdep_cfg[mdep_idx]);
                if ( (p_mdep->data_type == data_type) &&
                     (p_mdep->mdep_role == peer_mdep_role) )
                {
                    elem_found = TRUE;
                }
                else
                {
                    mdep_idx++;
                }
            }

            if (!elem_found)
            {
                found = FALSE;
                break;
            }
            else
            {
                found = TRUE;
            }
        }

        if (found)
        {
            *p_sdp_idx = i;
            break;
        }
    }

    BTIF_TRACE_DEBUG3("%s found=%d sdp_idx=%d",__FUNCTION__ , found, *p_sdp_idx);

    btif_hl_reset_mdep_filter(app_idx);

    return found;
}

/*******************************************************************************
**
** Function      btif_hl_dch_open
**
** Description   Process DCH open request using the DCH Open API parameters
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_hl_dch_open(UINT8 app_id, BD_ADDR bd_addr, tBTA_HL_DCH_OPEN_PARAM *p_dch_open_api,
                         int mdep_cfg_idx,
                         btif_hl_pend_dch_op_t op, int *channel_id)
{
    btif_hl_app_cb_t     *p_acb;
    btif_hl_mcl_cb_t     *p_mcb;
    btif_hl_pending_chan_cb_t     *p_pcb;
    UINT8               app_idx, mcl_idx;
    BOOLEAN status = FALSE;
    if (btif_hl_find_app_idx(app_id, &app_idx))
    {
        if (btif_hl_find_mcl_idx(app_idx, bd_addr , &mcl_idx))
        {
            p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);

            p_pcb = BTIF_HL_GET_PCB_PTR(app_idx, mcl_idx);
            if (!p_pcb->in_use)
            {
                p_mcb->req_ctrl_psm = p_dch_open_api->ctrl_psm;

                p_pcb->in_use = TRUE;
                *channel_id       =
                p_pcb->channel_id =  (int) btif_hl_get_next_channel_id(app_id);
                p_pcb->cb_state = BTIF_HL_CHAN_CB_STATE_CONNECTING_PENDING;
                p_pcb->mdep_cfg_idx = mdep_cfg_idx;
                p_pcb->op = op;

                if (p_mcb->sdp.num_recs)
                {
                    if (p_mcb->valid_sdp_idx)
                    {
                        p_dch_open_api->ctrl_psm  = p_mcb->ctrl_psm;
                    }
                    BTA_HlDchOpen(p_mcb->mcl_handle, p_dch_open_api);
                    status = TRUE;
                }
                else
                {
                    p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
                    p_mcb->cch_oper = BTIF_HL_CCH_OP_DCH_OPEN;
                    BTA_HlSdpQuery(p_acb->app_handle, bd_addr);
                    status = TRUE;
                }
            }
        }
    }

    BTIF_TRACE_DEBUG1("status=%d ", status);
    return status;
}

void btif_hl_copy_bda(bt_bdaddr_t *bd_addr, BD_ADDR  bda)
{
    UINT8 i;
    for (i=0; i<6; i++)
    {
        bd_addr->address[i] = bda[i] ;
    }

}

void btif_hl_display_bt_bda(bt_bdaddr_t *bd_addr)
{
    BTIF_TRACE_DEBUG6("DB [%02x:%02x:%02x:%02x:%02x:%02x]",
                      bd_addr->address[0],   bd_addr->address[1], bd_addr->address[2],
                      bd_addr->address[3],  bd_addr->address[4],   bd_addr->address[5]);
}

/*******************************************************************************
**
** Function         btif_hl_dch_abort
**
** Description      Process DCH abort request
**
** Returns          Nothing
**
*******************************************************************************/
void  btif_hl_dch_abort(UINT8 app_id,
                        BD_ADDR bd_addr)
{
/* todo */
//    UINT8                app_idx, mcl_idx;
//    btif_hl_mcl_cb_t      *p_mcb;
//
//    if (btif_hl_find_app_idx(app_id, &app_idx))
//    {
//        if (btif_hl_find_mcl_idx(app_idx, bd_addr, &mcl_idx))
//        {
//            p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
//            BTA_HlDchAbort(p_mcb->mcl_handle);
//        }
//    }
}
/*******************************************************************************
**
** Function      btif_hl_cch_open
**
** Description   Process CCH open request
**
** Returns     Nothing
**
*******************************************************************************/
BOOLEAN btif_hl_cch_open(UINT8 app_id, BD_ADDR bd_addr, UINT16 ctrl_psm,
                         int mdep_cfg_idx,
                         btif_hl_pend_dch_op_t op, int *channel_id)
{

    btif_hl_app_cb_t            *p_acb;
    btif_hl_mcl_cb_t            *p_mcb;
    btif_hl_pending_chan_cb_t   *p_pcb;
    UINT8                       app_idx, mcl_idx, chan_idx;
    BOOLEAN                     status = TRUE;

    BTIF_TRACE_DEBUG5("%s app_id=%d ctrl_psm=%d mdep_cfg_idx=%d op=%d",
                      __FUNCTION__, app_id, ctrl_psm, mdep_cfg_idx, op);
    BTIF_TRACE_DEBUG6("DB [%02x:%02x:%02x:%02x:%02x:%02x]",
                      bd_addr[0],  bd_addr[1],bd_addr[2],  bd_addr[3], bd_addr[4],  bd_addr[5]);

    if (btif_hl_find_app_idx(app_id, &app_idx))
    {
        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);

        if (!btif_hl_find_mcl_idx(app_idx, bd_addr, &mcl_idx))
        {
            if (btif_hl_find_avail_mcl_idx(app_idx, &mcl_idx))
            {
                p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
                memset(p_mcb,0, sizeof(btif_hl_mcl_cb_t));
                p_mcb->in_use = TRUE;
                bdcpy(p_mcb->bd_addr, bd_addr);

                if (!ctrl_psm)
                {
                    p_mcb->cch_oper = BTIF_HL_CCH_OP_MDEP_FILTERING;
                }
                else
                {
                    p_mcb->cch_oper        = BTIF_HL_CCH_OP_MATCHED_CTRL_PSM;
                    p_mcb->req_ctrl_psm    = ctrl_psm;
                }

                p_pcb = BTIF_HL_GET_PCB_PTR(app_idx, mcl_idx);
                p_pcb->in_use = TRUE;
                p_pcb->mdep_cfg_idx = mdep_cfg_idx;
                memcpy(p_pcb->bd_addr, bd_addr, sizeof(BD_ADDR));
                p_pcb->op = op;

                switch (op)
                {
                    case BTIF_HL_PEND_DCH_OP_OPEN:
                        *channel_id       =
                        p_pcb->channel_id =  (int) btif_hl_get_next_channel_id(app_id);
                        p_pcb->cb_state = BTIF_HL_CHAN_CB_STATE_CONNECTING_PENDING;
                        break;
                    case BTIF_HL_PEND_DCH_OP_DELETE_MDL:
                        p_pcb->channel_id =  p_acb->delete_mdl.channel_id;
                        p_pcb->cb_state = BTIF_HL_CHAN_CB_STATE_DESTROYED_PENDING;
                        break;
                    default:
                        break;
                }
                BTA_HlSdpQuery(p_acb->app_handle, bd_addr);

            }
            else
            {
                status = FALSE;
                BTIF_TRACE_ERROR0("Open CCH request discarded- No mcl cb");
            }
        }
        else
        {
            status = FALSE;
            BTIF_TRACE_ERROR0("Open CCH request discarded- already in USE");
        }
    }
    else
    {
        status = FALSE;
        BTIF_TRACE_ERROR1("Invalid app_id=%d", app_id);
    }

    if (channel_id)
    {
        BTIF_TRACE_DEBUG2("status=%d channel_id=0x%08x", status, *channel_id);
    }
    else
    {
        BTIF_TRACE_DEBUG1("status=%d ", status);
    }
    return status;
}


/*******************************************************************************
**
** Function      btif_hl_find_mdl_idx_using_handle
**
** Description  Find the MDL index using channel id
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_hl_find_mdl_cfg_idx_using_channel_id(int channel_id,
                                                  UINT8 *p_app_idx,
                                                  UINT8 *p_mdl_cfg_idx)
{
    btif_hl_app_cb_t      *p_acb;
    btif_hl_mdl_cfg_t     *p_mdl;
    BOOLEAN found=FALSE;
    UINT8 i,j;

    for (i=0; i < BTA_HL_NUM_APPS ; i ++)
    {
        p_acb =BTIF_HL_GET_APP_CB_PTR(i);
        for (j=0; j< BTA_HL_NUM_MDL_CFGS; j++)
        {
            p_mdl =BTIF_HL_GET_MDL_CFG_PTR(i,j);
            if (p_acb->in_use &&
                p_mdl->base.active &&
                (p_mdl->extra.channel_id == channel_id))
            {
                found = TRUE;
                *p_app_idx = i;
                *p_mdl_cfg_idx =j;
                break;
            }
        }
    }

    BTIF_TRACE_EVENT5("%s found=%d channel_id=0x%08x, app_idx=%d mdl_cfg_idx=%d  ",
                      __FUNCTION__,found,channel_id, i,j );
    return found;
}
/*******************************************************************************
**
** Function      btif_hl_find_mdl_idx_using_handle
**
** Description  Find the MDL index using channel id
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_hl_find_mdl_idx_using_channel_id(int channel_id,
                                              UINT8 *p_app_idx,UINT8 *p_mcl_idx,
                                              UINT8 *p_mdl_idx)
{
    btif_hl_app_cb_t      *p_acb;
    btif_hl_mcl_cb_t      *p_mcb;
    btif_hl_mdl_cb_t      *p_dcb;
    BOOLEAN found=FALSE;
    UINT8 i,j,k;

    for (i=0; i < BTA_HL_NUM_APPS ; i ++)
    {
        p_acb =BTIF_HL_GET_APP_CB_PTR(i);
        for (j=0; j< BTA_HL_NUM_MCLS; j++)
        {
            p_mcb =BTIF_HL_GET_MCL_CB_PTR(i,j);
            for (k=0; k< BTA_HL_NUM_MDLS_PER_MCL; k++)
            {
                p_dcb =BTIF_HL_GET_MDL_CB_PTR(i,j,k);
                if (p_acb->in_use &&
                    p_mcb->in_use &&
                    p_dcb->in_use &&
                    (p_dcb->channel_id == channel_id))
                {
                    found = TRUE;
                    *p_app_idx = i;
                    *p_mcl_idx =j;
                    *p_mdl_idx = k;
                    break;
                }
            }
        }
    }
    BTIF_TRACE_DEBUG5("%s found=%d app_idx=%d mcl_idx=%d mdl_idx=%d  ",
                      __FUNCTION__,found,i,j,k );
    return found;
}

/*******************************************************************************
**
** Function      btif_hl_find_mdl_idx_using_handle
**
** Description  Find the MDL index using handle
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_hl_find_mdl_idx_using_handle(tBTA_HL_MDL_HANDLE mdl_handle,
                                          UINT8 *p_app_idx,UINT8 *p_mcl_idx,
                                          UINT8 *p_mdl_idx)
{
    btif_hl_app_cb_t      *p_acb;
    btif_hl_mcl_cb_t      *p_mcb;
    btif_hl_mdl_cb_t      *p_dcb;
    BOOLEAN found=FALSE;
    UINT8 i,j,k;

    for (i=0; i < BTA_HL_NUM_APPS ; i ++)
    {
        p_acb =BTIF_HL_GET_APP_CB_PTR(i);
        for (j=0; j< BTA_HL_NUM_MCLS; j++)
        {
            p_mcb =BTIF_HL_GET_MCL_CB_PTR(i,j);
            for (k=0; k< BTA_HL_NUM_MDLS_PER_MCL; k++)
            {
                p_dcb =BTIF_HL_GET_MDL_CB_PTR(i,j,k);
                if (p_acb->in_use &&
                    p_mcb->in_use &&
                    p_dcb->in_use &&
                    (p_dcb->mdl_handle == mdl_handle))
                {
                    found = TRUE;
                    *p_app_idx = i;
                    *p_mcl_idx =j;
                    *p_mdl_idx = k;
                    break;
                }
            }
        }
    }


    BTIF_TRACE_EVENT5("%s found=%d app_idx=%d mcl_idx=%d mdl_idx=%d  ",
                      __FUNCTION__,found,i,j,k );
    return found;
}
/*******************************************************************************
**
** Function        btif_hl_find_peer_mdep_id
**
** Description      Find the peer MDEP ID from the received SPD records
**
** Returns          BOOLEAN
**
*******************************************************************************/
static BOOLEAN btif_hl_find_peer_mdep_id(UINT8 app_id, BD_ADDR bd_addr,
                                         tBTA_HL_MDEP_ROLE local_mdep_role,
                                         UINT16 data_type,
                                         tBTA_HL_MDEP_ID *p_peer_mdep_id)
{
    UINT8               app_idx, mcl_idx;
    btif_hl_app_cb_t     *p_acb;
    btif_hl_mcl_cb_t     *p_mcb;
    tBTA_HL_SDP_REC     *p_rec;
    UINT8               i, num_mdeps;
    BOOLEAN             found = FALSE;
    tBTA_HL_MDEP_ROLE   peer_mdep_role;


    BTIF_TRACE_DEBUG4("%s app_id=%d local_mdep_role=%d, data_type=%d",
                      __FUNCTION__, app_id, local_mdep_role, data_type);

    BTIF_TRACE_DEBUG6("DB [%02x:%02x:%02x:%02x:%02x:%02x]",
                      bd_addr[0],  bd_addr[1],
                      bd_addr[2],  bd_addr[3],
                      bd_addr[4],  bd_addr[5]);


    BTIF_TRACE_DEBUG1("local_mdep_role=%d", local_mdep_role);
    BTIF_TRACE_DEBUG1("data_type=%d", data_type);

    if (local_mdep_role == BTA_HL_MDEP_ROLE_SINK)
        peer_mdep_role = BTA_HL_MDEP_ROLE_SOURCE;
    else
        peer_mdep_role = BTA_HL_MDEP_ROLE_SINK;

    if (btif_hl_find_app_idx(app_id, &app_idx) )
    {
        p_acb  = BTIF_HL_GET_APP_CB_PTR(app_idx);
        if (btif_hl_find_mcl_idx(app_idx, bd_addr, &mcl_idx))
        {
            p_mcb  =BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);

            BTIF_TRACE_DEBUG2("app_idx=%d mcl_idx=%d",app_idx, mcl_idx);
            BTIF_TRACE_DEBUG1("sdp_idx=%d",p_mcb->sdp_idx);
            p_rec = &p_mcb->sdp.sdp_rec[p_mcb->sdp_idx];
            num_mdeps = p_rec->num_mdeps;
            BTIF_TRACE_DEBUG1("num_mdeps=%d", num_mdeps);

            for (i=0; i< num_mdeps; i++)
            {
                BTIF_TRACE_DEBUG2("p_rec->mdep_cfg[%d].mdep_role=%d",i, p_rec->mdep_cfg[i].mdep_role);
                BTIF_TRACE_DEBUG2("p_rec->mdep_cfg[%d].data_type =%d",i, p_rec->mdep_cfg[i].data_type );
                if ((p_rec->mdep_cfg[i].mdep_role == peer_mdep_role) &&
                    (p_rec->mdep_cfg[i].data_type == data_type))
                {
                    found = TRUE;
                    *p_peer_mdep_id = p_rec->mdep_cfg[i].mdep_id;
                    break;
                }
            }
        }
    }

    BTIF_TRACE_DEBUG2("found =%d  *p_peer_mdep_id=%d", found,  *p_peer_mdep_id);

    return found;
}
/*******************************************************************************
**
** Function        btif_hl_find_local_mdep_id
**
** Description      Find the local MDEP ID from the MDEP configuration
**
** Returns          BOOLEAN
**
*******************************************************************************/
static BOOLEAN btif_hl_find_local_mdep_id(UINT8 app_id,
                                          tBTA_HL_MDEP_ROLE local_mdep_role,
                                          UINT16 mdep_data_type,
                                          tBTA_HL_MDEP_ID *p_local_mdep_id){
    UINT8 app_idx;
    btif_hl_app_cb_t      *p_acb;
    UINT8  i,j;
    BOOLEAN found = FALSE;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);

    if (btif_hl_find_app_idx(app_id, &app_idx) )
    {
        p_acb  =BTIF_HL_GET_APP_CB_PTR(app_idx);

        for (i=0; i< p_acb->sup_feature.num_of_mdeps; i++)
        {
            if (p_acb->sup_feature.mdep[i].mdep_cfg.mdep_role == local_mdep_role )
            {
                for (j=0; j< p_acb->sup_feature.mdep[i].mdep_cfg.num_of_mdep_data_types; j++)
                {
                    if ( p_acb->sup_feature.mdep[i].mdep_cfg.data_cfg[j].data_type == mdep_data_type)
                    {
                        found = TRUE;
                        *p_local_mdep_id = p_acb->sup_feature.mdep[i].mdep_id;
                        return found;
                    }
                }
            }
        }

    }
    BTIF_TRACE_DEBUG2("found=%d local mdep id=%d", found, *p_local_mdep_id );
    return found;
}





/*******************************************************************************
**
** Function      btif_hl_find_mdep_cfg_idx
**
** Description  Find the MDEP configuration index using local MDEP_ID
**
** Returns      BOOLEAN
**
*******************************************************************************/
static  BOOLEAN btif_hl_find_mdep_cfg_idx(UINT8 app_idx,  tBTA_HL_MDEP_ID local_mdep_id,
                                          UINT8 *p_mdep_cfg_idx)
{
    btif_hl_app_cb_t      *p_acb =BTIF_HL_GET_APP_CB_PTR(app_idx);
    tBTA_HL_SUP_FEATURE     *p_sup_feature= &p_acb->sup_feature;
    BOOLEAN found =FALSE;
    UINT8 i;

    for (i=0; i< p_sup_feature->num_of_mdeps; i++)
    {
        if ( p_sup_feature->mdep[i].mdep_id == local_mdep_id)
        {
            found = TRUE;
            *p_mdep_cfg_idx = i;
            break;
        }
    }

    BTIF_TRACE_DEBUG4("%s found=%d mdep_idx=%d local_mdep_id=%d ",
                      __FUNCTION__, found,i, local_mdep_id );
    return found;
}



/*******************************************************************************
**
** Function      btif_hl_find_mcl_idx
**
** Description  Find the MCL index using BD address
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_hl_find_mcl_idx(UINT8 app_idx, BD_ADDR p_bd_addr, UINT8 *p_mcl_idx)
{
    BOOLEAN found=FALSE;
    UINT8 i;
    btif_hl_app_cb_t  *p_acb =BTIF_HL_GET_APP_CB_PTR(app_idx);
    btif_hl_mcl_cb_t  *p_mcb;

    for (i=0; i < BTA_HL_NUM_MCLS ; i ++)
    {
        p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, i);
        if (p_mcb->in_use &&
            (!memcmp (p_mcb->bd_addr, p_bd_addr, BD_ADDR_LEN)))
        {
            found = TRUE;
            *p_mcl_idx = i;
            break;
        }
    }


    BTIF_TRACE_DEBUG3("%s found=%d idx=%d",__FUNCTION__, found, i);
    return found;
}
/*******************************************************************************
**
** Function         btif_hl_init
**
** Description      HL initialization function.
**
** Returns          void
**
*******************************************************************************/
static void btif_hl_init(void)
{
    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);
    memset(p_btif_hl_cb, 0, sizeof(btif_hl_cb_t));
    btif_hl_init_next_app_id();
    btif_hl_init_next_channel_id();
    /* todo load existing app and mdl configuration */

    btif_hl_set_state(BTIF_HL_STATE_ENABLING);
    BTA_HlEnable(btif_hl_ctrl_cback);
}
/*******************************************************************************
**
** Function         btif_hl_disable
**
** Description      Disable initialization function.
**
** Returns          void
**
*******************************************************************************/
static void btif_hl_disable(void)
{
    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);

    if ((p_btif_hl_cb->state != BTIF_HL_STATE_DISABLING) &&
        (p_btif_hl_cb->state != BTIF_HL_STATE_DISABLED))
    {
        btif_hl_set_state(BTIF_HL_STATE_DISABLING);
        BTA_HlDisable();
    }
}
/*******************************************************************************
**
** Function      btif_hl_is_no_active_app
**
** Description  Find whether or not  any APP is still in use
**
** Returns      BOOLEAN
**
*******************************************************************************/
static BOOLEAN btif_hl_is_no_active_app(void)
{
    BOOLEAN no_active_app = TRUE;
    UINT8 i;

    for (i=0; i < BTA_HL_NUM_APPS ; i ++)
    {
        if (btif_hl_cb.acb[i].in_use)
        {
            no_active_app = FALSE;
            break;
        }
    }

    BTIF_TRACE_DEBUG2("%s no_active_app=%d  ", __FUNCTION__, no_active_app );
    return no_active_app;
}

static void btif_hl_free_pending_reg_idx(UINT8 reg_idx)
{


    if ((reg_idx < BTA_HL_NUM_APPS) && btif_hl_cb.pcb[reg_idx].in_use )
    {
        btif_hl_cb.pcb[reg_idx].in_use = FALSE;
        memset (&btif_hl_cb.pcb[reg_idx], 0, sizeof(btif_hl_pending_reg_cb_t));
    }
}

static void btif_hl_free_app_idx(UINT8 app_idx)
{

    if ((app_idx < BTA_HL_NUM_APPS) && btif_hl_cb.acb[app_idx].in_use )
    {
        btif_hl_cb.acb[app_idx].in_use = FALSE;
        memset (&btif_hl_cb.acb[app_idx], 0, sizeof(btif_hl_app_cb_t));
    }
}

static void btif_hl_set_state(btif_hl_state_t state)
{
    BTIF_TRACE_DEBUG2("btif_hl_set_state:  %d ---> %d ", p_btif_hl_cb->state, state);
    p_btif_hl_cb->state = state;
}

static btif_hl_state_t btif_hl_get_state(void)
{
    BTIF_TRACE_DEBUG1("btif_hl_get_state:  %d   ", p_btif_hl_cb->state);
    return p_btif_hl_cb->state;
}

/*******************************************************************************
**
** Function      btif_hl_find_data_type_idx
**
** Description  Find the index in the data type table
**
** Returns      BOOLEAN
**
*******************************************************************************/
static BOOLEAN  btif_hl_find_data_type_idx(UINT16 data_type, UINT8 *p_idx)
{
    BOOLEAN found = FALSE;
    UINT8 i;

    for (i=0; i< BTIF_HL_DATA_TABLE_SIZE; i++ )
    {
        if (data_type_table[i].data_type == data_type)
        {
            found = TRUE;
            *p_idx= i;
            break;
        }
    }

    BTIF_TRACE_DEBUG4("%s found=%d, data_type=0x%x idx=%d", __FUNCTION__, found, data_type, i);
    return found;
}

/*******************************************************************************
**
** Function      btif_hl_get_max_tx_apdu_size
**
** Description  Find the maximum TX APDU size for the specified data type and
**              MDEP role
**
** Returns      UINT16
**
*******************************************************************************/
UINT16  btif_hl_get_max_tx_apdu_size(tBTA_HL_MDEP_ROLE mdep_role,
                                     UINT16 data_type )
{
    UINT8 idx;
    UINT16 max_tx_apdu_size =0;

    if (btif_hl_find_data_type_idx(data_type, &idx))
    {
        if (mdep_role == BTA_HL_MDEP_ROLE_SOURCE)
        {
            max_tx_apdu_size = data_type_table[idx].max_tx_apdu_size;
        }
        else
        {
            max_tx_apdu_size = data_type_table[idx].max_rx_apdu_size;
        }
    }
    else
    {
        if (mdep_role == BTA_HL_MDEP_ROLE_SOURCE)
        {
            max_tx_apdu_size = BTIF_HL_DEFAULT_SRC_TX_APDU_SIZE;
        }
        else
        {
            max_tx_apdu_size = BTIF_HL_DEFAULT_SRC_RX_APDU_SIZE;
        }

    }

    BTIF_TRACE_DEBUG4("%s mdep_role=%d data_type=0x%4x size=%d",
                      __FUNCTION__, mdep_role, data_type, max_tx_apdu_size);
    return max_tx_apdu_size;
}


/*******************************************************************************
**
** Function      btif_hl_get_max_rx_apdu_size
**
** Description  Find the maximum RX APDU size for the specified data type and
**              MDEP role
**
** Returns      UINT16
**
*******************************************************************************/
UINT16  btif_hl_get_max_rx_apdu_size(tBTA_HL_MDEP_ROLE mdep_role,
                                     UINT16 data_type )
{
    UINT8  idx;
    UINT16 max_rx_apdu_size =0;

    if (btif_hl_find_data_type_idx(data_type, &idx))
    {
        if (mdep_role == BTA_HL_MDEP_ROLE_SOURCE)
        {
            max_rx_apdu_size = data_type_table[idx].max_rx_apdu_size;
        }
        else
        {
            max_rx_apdu_size = data_type_table[idx].max_tx_apdu_size;
        }
    }
    else
    {
        if (mdep_role == BTA_HL_MDEP_ROLE_SOURCE)
        {
            max_rx_apdu_size = BTIF_HL_DEFAULT_SRC_RX_APDU_SIZE;
        }
        else
        {
            max_rx_apdu_size = BTIF_HL_DEFAULT_SRC_TX_APDU_SIZE;
        }
    }


    BTIF_TRACE_DEBUG4("%s mdep_role=%d data_type=0x%4x size=%d",
                      __FUNCTION__, mdep_role, data_type, max_rx_apdu_size);

    return max_rx_apdu_size;

}




static BOOLEAN btif_hl_get_bta_mdep_role(bthl_mdep_role_t mdep, tBTA_HL_MDEP_ROLE *p)
{
    BOOLEAN status = TRUE;
    switch (mdep)
    {
        case BTHL_MDEP_ROLE_SOURCE:
            *p = BTA_HL_MDEP_ROLE_SOURCE;
            break;
        case BTHL_MDEP_ROLE_SINK:
            *p = BTA_HL_MDEP_ROLE_SINK;
            break;
        default:
            status = FALSE;
            break;
    }

    BTIF_TRACE_DEBUG3("%s status = %d BTA mdep_role=%d ",
                      __FUNCTION__, status, *p);
    return status;
}

static BOOLEAN btif_hl_get_bta_channel_type(bthl_channel_type_t channel_type, tBTA_HL_DCH_CFG *p)
{
    BOOLEAN status = TRUE;
    switch (channel_type)
    {
        case BTHL_CHANNEL_TYPE_RELIABLE:
            *p = BTA_HL_DCH_CFG_RELIABLE;
            break;
        case BTHL_CHANNEL_TYPE_STREAMING:
            *p = BTA_HL_DCH_CFG_STREAMING;
            break;
        case BTHL_CHANNEL_TYPE_ANY:
            *p = BTA_HL_DCH_CFG_NO_PREF;
            break;
        default:
            status = FALSE;
            break;
    }
    BTIF_TRACE_DEBUG3("%s status = %d BTA DCH CFG=%d (1-rel 2-strm",
                      __FUNCTION__, status, *p);
    return status;
}




static UINT8 btif_hl_get_next_app_id()
{
    UINT8 next_app_id = btif_hl_cb.next_app_id;

    btif_hl_cb.next_app_id++;
    /* todo check the new channel_is is in use or not if in use go to next channel id and repeat the checking */

    return next_app_id;
}

static int btif_hl_get_next_channel_id(UINT8 app_id)
{
    UINT16 next_channel_id = btif_hl_cb.next_channel_id;
    int channel_id;
    btif_hl_cb.next_channel_id++;

    channel_id = (app_id << 16) + next_channel_id;

    BTIF_TRACE_DEBUG4("%s channel_id=0x%08x, app_id=0x%02x0000 next_channel_id=0x%04x", __FUNCTION__,
                      channel_id, app_id,  next_channel_id);
    /* todo check the new channel_is is in use or not if in use go to next channel id and repeat the checking */
    return channel_id;
}


static UINT8 btif_hl_get_app_id(int channel_id)
{
    UINT8 app_id =(UINT8) (channel_id >> 16);

    BTIF_TRACE_DEBUG3("%s channel_id=0x%08x, app_id=0x%02x ", __FUNCTION__,channel_id, app_id);
    return app_id;
}

static void btif_hl_init_next_app_id(void)
{
    btif_hl_cb.next_app_id = 1; /* todo check existing app_ids and initialize next avail app_id */
}

static void btif_hl_init_next_channel_id(void)
{
    btif_hl_cb.next_channel_id = 1; /* todo check existing app_ids and initialize next avail app_id */
}


/*******************************************************************************
**
** Function      btif_hl_save_mdl_cfg
**
** Description  Save the MDL configuration
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN  btif_hl_save_mdl_cfg(UINT8 app_id, UINT8 item_idx,
                              tBTA_HL_MDL_CFG *p_mdl_cfg)
{
    btif_hl_mdl_cfg_t     *p_mdl=NULL;
    BOOLEAN             success = FALSE;
    btif_hl_app_cb_t      *p_acb;
    btif_hl_mcl_cb_t     *p_mcb;
    UINT8               app_idx, mcl_idx, mdl_idx;

    BTIF_TRACE_DEBUG5("%s app_ids=%d item_idx=%d, local_mdep_id=%d mdl_id=%d",__FUNCTION__, app_id, item_idx, p_mdl_cfg->local_mdep_id, p_mdl_cfg->mdl_id );

    if (btif_hl_find_app_idx(app_id, &app_idx))
    {
        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
        p_mdl = BTIF_HL_GET_MDL_CFG_PTR(app_idx, item_idx);
        if (p_mdl)
        {
            memcpy(&p_mdl->base, p_mdl_cfg, sizeof(tBTA_HL_MDL_CFG));
            if (btif_hl_find_mcl_idx(app_idx, p_mdl->base.peer_bd_addr  , &mcl_idx))
            {
                p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
                p_mdl->extra.app_active = TRUE;
                p_mdl->extra.app_idx = app_idx;
                p_mdl->extra.channel_id = p_mcb->pcb.channel_id ;
                p_mdl->extra.mdep_cfg_idx = p_mcb->pcb.mdep_cfg_idx;
                p_mdl->extra.data_type = p_acb->sup_feature.mdep[p_mcb->pcb.mdep_cfg_idx].mdep_cfg.data_cfg[0].data_type;

                BTIF_TRACE_DEBUG3("channel_id=0x%08x mdep_cfg_idx=%d, data_type=0x%04x",p_mdl->extra.channel_id, p_mdl->extra.mdep_cfg_idx, p_mdl->extra.data_type  );
                /* todo update NV */
                success = TRUE;
            }
        }
    }


    BTIF_TRACE_DEBUG2("%s success=%d  ",__FUNCTION__, success );

    return success;
}



/*******************************************************************************
**
** Function      btif_hl_delete_mdl_cfg
**
** Description  Delete the MDL configuration
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN  btif_hl_delete_mdl_cfg(UINT8 app_id, UINT8 item_idx)
{
    btif_hl_mdl_cfg_t     *p_mdl=NULL;
    BOOLEAN             success = FALSE;
    btif_hl_app_cb_t      *p_acb;
    UINT8               app_idx;

    if (btif_hl_find_app_idx(app_id, &app_idx))
    {

        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);

        p_mdl = BTIF_HL_GET_MDL_CFG_PTR(app_idx, item_idx);
        if (p_mdl)
        {
            memset(p_mdl, 0, sizeof(btif_hl_mdl_cfg_t));
            /* todo update NV */
            success = TRUE;
        }
    }


    BTIF_TRACE_DEBUG2("%s success=%d  ",__FUNCTION__, success );


    return success;
}

/*******************************************************************************
**
** Function      btif_hl_find_app_idx_using_handle
**
** Description  Find the applicaiton index using handle
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_hl_find_app_idx_using_handle(tBTA_HL_APP_HANDLE app_handle,
                                          UINT8 *p_app_idx)
{
    BOOLEAN found=FALSE;
    UINT8 i;

    for (i=0; i < BTA_HL_NUM_APPS ; i ++)
    {
        if (btif_hl_cb.acb[i].in_use &&
            (btif_hl_cb.acb[i].app_handle == app_handle))
        {
            found = TRUE;
            *p_app_idx = i;
            break;
        }
    }

    BTIF_TRACE_EVENT4("%s status=%d handle=%d app_idx=%d ",
                      __FUNCTION__, found, app_handle , i);

    return found;
}

/*******************************************************************************
**
** Function      btif_hl_find_mcl_idx_using_handle
**
** Description  Find the MCL index using handle
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_hl_find_mcl_idx_using_handle( tBTA_HL_MCL_HANDLE mcl_handle,
                                           UINT8 *p_app_idx, UINT8 *p_mcl_idx)
{
    btif_hl_app_cb_t  *p_acb;
    BOOLEAN         found=FALSE;
    UINT8 i,j;

    for (i=0; i<BTA_HL_NUM_APPS; i++)
    {
        p_acb =BTIF_HL_GET_APP_CB_PTR(i);
        for (j=0; j < BTA_HL_NUM_MCLS ; j++)
        {
            if (p_acb->mcb[j].in_use &&
                (p_acb->mcb[j].mcl_handle == mcl_handle))
            {
                found = TRUE;
                *p_app_idx = i;
                *p_mcl_idx = j;
                break;
            }
        }
    }

    BTIF_TRACE_DEBUG4("%s found=%d app_idx=%d mcl_idx=%d",__FUNCTION__,
                      found, i, j);
    return found;
}

/*******************************************************************************
**
** Function      btif_hl_find_app_idx
**
** Description  Find the application index using application ID
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_hl_find_app_idx(UINT8 app_id, UINT8 *p_app_idx)
{
    BOOLEAN found=FALSE;
    UINT8 i;

    for (i=0; i < BTA_HL_NUM_APPS ; i ++)
    {

        if (btif_hl_cb.acb[i].in_use &&
            (btif_hl_cb.acb[i].app_id == app_id))
        {
            found = TRUE;
            *p_app_idx = i;
            break;
        }
    }
    BTIF_TRACE_DEBUG3("%s found=%d app_idx=%d", __FUNCTION__, found, i );

    return found;
}


/*******************************************************************************
**
** Function      btif_hl_find_avail_app_idx
**
** Description  Find a not in-use APP index
**
** Returns      BOOLEAN
**
*******************************************************************************/
static BOOLEAN btif_hl_find_avail_pending_reg_idx(UINT8 *p_idx)
{
    BOOLEAN found = FALSE;
    UINT8 i;

    for (i=0; i < BTA_HL_NUM_APPS ; i ++)
    {
        if (!btif_hl_cb.pcb[i].in_use)
        {
            found = TRUE;
            *p_idx = i;
            break;
        }
    }

    BTIF_TRACE_DEBUG3("%s found=%d app_idx=%d", __FUNCTION__, found, i);
    return found;
}

/*******************************************************************************
**
** Function      btif_hl_find_avail_pending_chan_idx
**
** Description  Find a not in-use pending chan index
**
** Returns      BOOLEAN
**
*******************************************************************************/
/*static BOOLEAN btif_hl_find_avail_pending_chan_idx(UINT8 app_idx, UINT8 mcl_idx, UINT8 *p_idx)
{
    BOOLEAN found = FALSE;
    UINT8 i;

    for (i=0; i < BTA_HL_NUM_MDLS_PER_MCL ; i ++)
    {
        if (!btif_hl_cb.acb[app_idx].mcb[mcl_idx].pcb[i].in_use)
        {
            found = TRUE;
            *p_idx = i;
            break;
        }
    }

    BTIF_TRACE_DEBUG3("%s found=%d app_idx=%d", __FUNCTION__, found, i);
    return found;
}
*/
/*******************************************************************************
**
** Function      btif_hl_find_avail_mdl_idx
**
** Description  Find a not in-use MDL index
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_hl_find_avail_mdl_idx(UINT8 app_idx, UINT8 mcl_idx,
                                   UINT8 *p_mdl_idx)
{
    btif_hl_mcl_cb_t      *p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
    BOOLEAN found=FALSE;
    UINT8 i;

    for (i=0; i < BTA_HL_NUM_MDLS_PER_MCL ; i ++)
    {
        if (!p_mcb->mdl[i].in_use)
        {
            btif_hl_clean_mdl_cb(&p_mcb->mdl[i]);
            found = TRUE;
            *p_mdl_idx = i;
            break;
        }
    }

    BTIF_TRACE_DEBUG3("%s found=%d idx=%d",__FUNCTION__, found, i);
    return found;
}


/*******************************************************************************
**
** Function      btif_hl_find_avail_mcl_idx
**
** Description  Find a not in-use MDL index
**
** Returns      BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_hl_find_avail_mcl_idx(UINT8 app_idx, UINT8 *p_mcl_idx)
{
    BOOLEAN found=FALSE;
    UINT8 i;

    for (i=0; i < BTA_HL_NUM_MCLS ; i ++)
    {
        if (!btif_hl_cb.acb[app_idx].mcb[i].in_use)
        {
            found = TRUE;
            *p_mcl_idx = i;
            break;
        }
    }
    BTIF_TRACE_DEBUG3("%s found=%d app_idx=%d", __FUNCTION__, found, i);
    return found;
}

/*******************************************************************************
**
** Function      btif_hl_find_avail_app_idx
**
** Description  Find a not in-use APP index
**
** Returns      BOOLEAN
**
*******************************************************************************/
static BOOLEAN btif_hl_find_avail_app_idx(UINT8 *p_idx)
{
    BOOLEAN found = FALSE;
    UINT8 i;

    for (i=0; i < BTA_HL_NUM_APPS ; i ++)
    {
        if (!btif_hl_cb.acb[i].in_use)
        {
            found = TRUE;
            *p_idx = i;
            break;
        }
    }

    BTIF_TRACE_DEBUG3("%s found=%d app_idx=%d", __FUNCTION__, found, i);
    return found;
}



/************************************************************************************
**  Functions
************************************************************************************/

/*******************************************************************************
**
** Function         btif_hl_proc_dereg_cfm
**
** Description      Process the de-registration confirmation
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_dereg_cfm(tBTA_HL *p_data)

{
    btif_hl_app_cb_t        *p_acb;
    UINT8                   app_idx;
    int                     app_id;
    bthl_app_reg_state_t    state = BTHL_APP_REG_STATE_DEREG_SUCCESS;
    BTIF_TRACE_DEBUG3("%s de-reg status=%d app_handle=%d", __FUNCTION__, p_data->dereg_cfm.status, p_data->dereg_cfm.app_handle);

    if (btif_hl_find_app_idx_using_handle(p_data->dereg_cfm.app_handle, &app_idx))
    {
        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
        if (p_data->dereg_cfm.status == BTA_HL_STATUS_OK)
        {
            app_id = (int) p_acb->app_id;
            memset(p_acb, 0,sizeof(btif_hl_app_cb_t));
        }
        else
        {
            state = BTHL_APP_REG_STATE_DEREG_FAILED;
        }

        BTIF_TRACE_DEBUG2("app_id=%d state=%d", app_id, state);
        BTIF_HL_CALL_CBACK(bt_hl_callbacks, app_reg_state_cb, app_id, state );

        if (btif_hl_is_no_active_app())
        {
            btif_hl_disable();
        }

    }

}


/*******************************************************************************
**
** Function         btif_hl_proc_reg_cfm
**
** Description      Process the registration confirmation
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_reg_cfm(tBTA_HL *p_data)

{
    btif_hl_app_cb_t      *p_acb;
    UINT8                   app_idx;
    bthl_app_reg_state_t    state = BTHL_APP_REG_STATE_REG_SUCCESS;

    BTIF_TRACE_DEBUG3("%s reg status=%d app_handle=%d", __FUNCTION__, p_data->reg_cfm.status, p_data->reg_cfm.app_handle);

    if (btif_hl_find_app_idx(p_data->reg_cfm.app_id, &app_idx))
    {
        p_acb =BTIF_HL_GET_APP_CB_PTR(app_idx);

        if (p_data->reg_cfm.status == BTA_HL_STATUS_OK)
        {
            p_acb->app_handle = p_data->reg_cfm.app_handle;
        }
        else
        {
            btif_hl_free_app_idx(app_idx);
            state = BTHL_APP_REG_STATE_REG_FAILED;
        }

        BTIF_TRACE_DEBUG3("%s app_id=%d reg state=%d", __FUNCTION__,  p_data->reg_cfm.app_id, state);
        BTIF_HL_CALL_CBACK(bt_hl_callbacks, app_reg_state_cb, ((int) p_data->reg_cfm.app_id), state );
    }
}

/*******************************************************************************
**
** Function         btif_hl_proc_sdp_info_ind
**
** Description      Process the SDP info indication
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_sdp_info_ind(tBTA_HL *p_data)

{
    btif_hl_app_cb_t         *p_acb;
    UINT8                   app_idx;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);
    if (btif_hl_find_app_idx_using_handle(p_data->sdp_info_ind.app_handle, &app_idx))
    {
        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
        memcpy(&p_acb->sdp_info_ind, &p_data->sdp_info_ind, sizeof(tBTA_HL_SDP_INFO_IND));
    }
}

void btif_hl_set_chan_cb_state(UINT8 app_idx, UINT8 mcl_idx, btif_hl_chan_cb_state_t state)
{
    btif_hl_pending_chan_cb_t   *p_pcb = BTIF_HL_GET_PCB_PTR(app_idx, mcl_idx);
    btif_hl_chan_cb_state_t cur_state = p_pcb->cb_state;

    if (cur_state != state)
    {
        p_pcb->cb_state = state;
        BTIF_TRACE_DEBUG3("%s state %d--->%d",__FUNCTION__, cur_state, state);
    }

}

void btif_hl_send_destroyed_cb(btif_hl_app_cb_t        *p_acb )
{
    bt_bdaddr_t     bd_addr;
    int             app_id = (int) btif_hl_get_app_id(p_acb->delete_mdl.channel_id);

    btif_hl_copy_bda(&bd_addr, p_acb->delete_mdl.bd_addr);
    BTIF_TRACE_DEBUG1("%s",__FUNCTION__);
    BTIF_TRACE_DEBUG4("channel_id=0x%08x mdep_cfg_idx=%d, state=%d fd=%d",p_acb->delete_mdl.channel_id,
                      p_acb->delete_mdl.mdep_cfg_idx, BTHL_CONN_STATE_DESTROYED, 0);
    btif_hl_display_bt_bda(&bd_addr);

    BTIF_HL_CALL_CBACK(bt_hl_callbacks, channel_state_cb,  app_id,
                       &bd_addr, p_acb->delete_mdl.mdep_cfg_idx,
                       p_acb->delete_mdl.channel_id, BTHL_CONN_STATE_DESTROYED, 0 );
}


void btif_hl_send_disconnecting_cb(UINT8 app_idx, UINT8 mcl_idx, UINT8 mdl_idx)
{
    btif_hl_mdl_cb_t        *p_dcb = BTIF_HL_GET_MDL_CB_PTR( app_idx,  mcl_idx, mdl_idx);
    btif_hl_soc_cb_t        *p_scb = p_dcb->p_scb;
    bt_bdaddr_t             bd_addr;
    int                     app_id = (int) btif_hl_get_app_id(p_scb->channel_id);

    btif_hl_copy_bda(&bd_addr, p_scb->bd_addr);

    BTIF_TRACE_DEBUG1("%s",__FUNCTION__);
    BTIF_TRACE_DEBUG4("channel_id=0x%08x mdep_cfg_idx=%d, state=%d fd=%d",p_scb->channel_id,
                      p_scb->mdep_cfg_idx, BTHL_CONN_STATE_DISCONNECTING, p_scb->socket_id[0]);
    btif_hl_display_bt_bda(&bd_addr);
    BTIF_HL_CALL_CBACK(bt_hl_callbacks, channel_state_cb,  app_id,
                       &bd_addr, p_scb->mdep_cfg_idx,
                       p_scb->channel_id, BTHL_CONN_STATE_DISCONNECTING, p_scb->socket_id[0] );
}

void btif_hl_send_disconnected_cb( btif_hl_soc_cb_t    *p_scb)
{
    bt_bdaddr_t             bd_addr;
    int                     app_id = (int) btif_hl_get_app_id(p_scb->channel_id);

    btif_hl_copy_bda(&bd_addr, p_scb->bd_addr);

    BTIF_TRACE_DEBUG1("%s",__FUNCTION__);
    BTIF_TRACE_DEBUG4("channel_id=0x%08x mdep_cfg_idx=%d, state=%d fd=%d",p_scb->channel_id,
                      p_scb->mdep_cfg_idx, BTHL_CONN_STATE_DISCONNECTED, p_scb->socket_id[0]);
    btif_hl_display_bt_bda(&bd_addr);

    BTIF_HL_CALL_CBACK(bt_hl_callbacks, channel_state_cb,  app_id,
                       &bd_addr, p_scb->mdep_cfg_idx,
                       p_scb->channel_id, BTHL_CONN_STATE_DISCONNECTED, p_scb->socket_id[0] );
}


void btif_hl_send_connected_cb(UINT8 app_idx, UINT8 mcl_idx, UINT8 mdl_idx)
{
    btif_hl_mcl_cb_t        *p_mcb = BTIF_HL_GET_MCL_CB_PTR( app_idx,  mcl_idx);
    btif_hl_mdl_cb_t        *p_dcb = BTIF_HL_GET_MDL_CB_PTR( app_idx,  mcl_idx, mdl_idx);
    btif_hl_soc_cb_t        *p_scb = p_dcb->p_scb;
    bt_bdaddr_t             bd_addr;
    int                     app_id = (int) btif_hl_get_app_id(p_dcb->channel_id);

    btif_hl_copy_bda(&bd_addr, p_mcb->bd_addr);
    BTIF_TRACE_DEBUG1("%s",__FUNCTION__);
    BTIF_TRACE_DEBUG4("channel_id=0x%08x mdep_cfg_idx=%d, state=%d fd=%d",p_dcb->channel_id,
                      p_dcb->local_mdep_cfg_idx, BTHL_CONN_STATE_DISCONNECTED, p_scb->socket_id[0]);
    btif_hl_display_bt_bda(&bd_addr);
    BTIF_HL_CALL_CBACK(bt_hl_callbacks, channel_state_cb,  app_id,
                       &bd_addr, p_dcb->local_mdep_cfg_idx,
                       p_dcb->channel_id, BTHL_CONN_STATE_CONNECTED, p_scb->socket_id[0] );
}

void btif_hl_send_setup_connecting_cb(UINT8 app_idx, UINT8 mcl_idx){
    btif_hl_pending_chan_cb_t   *p_pcb = BTIF_HL_GET_PCB_PTR(app_idx, mcl_idx);
    bt_bdaddr_t                 bd_addr;
    int                         app_id = (int) btif_hl_get_app_id(p_pcb->channel_id);

    btif_hl_copy_bda(&bd_addr, p_pcb->bd_addr);

    if (p_pcb->in_use && p_pcb->cb_state == BTIF_HL_CHAN_CB_STATE_CONNECTING_PENDING)
    {
        BTIF_TRACE_DEBUG1("%s",__FUNCTION__);
        BTIF_TRACE_DEBUG4("channel_id=0x%08x mdep_cfg_idx=%d state=%d fd=%d",p_pcb->channel_id,
                          p_pcb->mdep_cfg_idx, BTHL_CONN_STATE_CONNECTING, 0);
        btif_hl_display_bt_bda(&bd_addr);

        BTIF_HL_CALL_CBACK(bt_hl_callbacks, channel_state_cb, app_id,
                           &bd_addr, p_pcb->mdep_cfg_idx,
                           p_pcb->channel_id, BTHL_CONN_STATE_CONNECTING, 0 );
        btif_hl_set_chan_cb_state(app_idx, mcl_idx, BTIF_HL_CHAN_CB_STATE_CONNECTED_PENDING);
    }
}

void btif_hl_send_setup_disconnected_cb(UINT8 app_idx, UINT8 mcl_idx){
    btif_hl_pending_chan_cb_t   *p_pcb = BTIF_HL_GET_PCB_PTR(app_idx, mcl_idx);
    bt_bdaddr_t                 bd_addr;
    int                         app_id = (int) btif_hl_get_app_id(p_pcb->channel_id);

    btif_hl_copy_bda(&bd_addr, p_pcb->bd_addr);

    BTIF_TRACE_DEBUG2("%s p_pcb->in_use=%d",__FUNCTION__, p_pcb->in_use);
    if (p_pcb->in_use)
    {
        BTIF_TRACE_DEBUG1("%p_pcb->cb_state=%d",p_pcb->cb_state);
        if (p_pcb->cb_state == BTIF_HL_CHAN_CB_STATE_CONNECTING_PENDING)
        {
            BTIF_TRACE_DEBUG4("channel_id=0x%08x mdep_cfg_idx=%d state=%d fd=%d",p_pcb->channel_id,
                              p_pcb->mdep_cfg_idx, BTHL_CONN_STATE_CONNECTING, 0);
            btif_hl_display_bt_bda(&bd_addr);
            BTIF_HL_CALL_CBACK(bt_hl_callbacks, channel_state_cb, app_id,
                               &bd_addr, p_pcb->mdep_cfg_idx,
                               p_pcb->channel_id, BTHL_CONN_STATE_CONNECTING, 0 );

            BTIF_TRACE_DEBUG4("channel_id=0x%08x mdep_cfg_idx=%d state=%d fd=%d",p_pcb->channel_id,
                              p_pcb->mdep_cfg_idx, BTHL_CONN_STATE_DISCONNECTED, 0);
            btif_hl_display_bt_bda(&bd_addr);
            BTIF_HL_CALL_CBACK(bt_hl_callbacks, channel_state_cb, app_id,
                               &bd_addr, p_pcb->mdep_cfg_idx,
                               p_pcb->channel_id, BTHL_CONN_STATE_DISCONNECTED, 0 );
        }
        else if (p_pcb->cb_state == BTIF_HL_CHAN_CB_STATE_CONNECTED_PENDING)
        {
            BTIF_TRACE_DEBUG4("channel_id=0x%08x mdep_cfg_idx=%d state=%d fd=%d",p_pcb->channel_id,
                              p_pcb->mdep_cfg_idx, BTHL_CONN_STATE_DISCONNECTED, 0);
            btif_hl_display_bt_bda(&bd_addr);
            BTIF_HL_CALL_CBACK(bt_hl_callbacks, channel_state_cb,  app_id,
                               &bd_addr, p_pcb->mdep_cfg_idx,
                               p_pcb->channel_id, BTHL_CONN_STATE_DISCONNECTED, 0 );
        }
        btif_hl_clean_pcb(p_pcb);
    }
}
/*******************************************************************************
**
** Function         btif_hl_proc_sdp_query_cfm
**
** Description      Process the SDP query confirmation
**
** Returns          Nothing
**
*******************************************************************************/
static BOOLEAN btif_hl_proc_sdp_query_cfm(tBTA_HL *p_data){
    btif_hl_app_cb_t                *p_acb;
    btif_hl_mcl_cb_t                *p_mcb;
    tBTA_HL_SDP                     *p_sdp;
    tBTA_HL_CCH_OPEN_PARAM          open_param;
    UINT8                           app_idx, mcl_idx, sdp_idx;
    UINT8                           num_recs, i, num_mdeps, j;
    btif_hl_cch_op_t                old_cch_oper;
    BOOLEAN                         status =FALSE;
    btif_hl_pending_chan_cb_t     *p_pcb;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);

    p_sdp = p_data->sdp_query_cfm.p_sdp;
    num_recs = p_sdp->num_recs;

    BTIF_TRACE_DEBUG1("num of SDP records=%d",num_recs);
    for (i=0; i<num_recs; i++)
    {
        BTIF_TRACE_DEBUG3("rec_idx=%d ctrl_psm=0x%x data_psm=0x%x",
                          (i+1),p_sdp->sdp_rec[i].ctrl_psm, p_sdp->sdp_rec[i].data_psm);
        BTIF_TRACE_DEBUG1("MCAP supported procedures=0x%x",p_sdp->sdp_rec[i].mcap_sup_proc);
        num_mdeps = p_sdp->sdp_rec[i].num_mdeps;
        BTIF_TRACE_DEBUG1("num of mdeps =%d",num_mdeps);
        for (j=0; j< num_mdeps; j++)
        {
            BTIF_TRACE_DEBUG4("mdep_idx=%d mdep_id=0x%x data_type=0x%x mdep_role=0x%x",
                              (j+1),
                              p_sdp->sdp_rec[i].mdep_cfg[j].mdep_id,
                              p_sdp->sdp_rec[i].mdep_cfg[j].data_type,
                              p_sdp->sdp_rec[i].mdep_cfg[j].mdep_role );
        }
    }

    if (btif_hl_find_app_idx_using_handle(p_data->sdp_query_cfm.app_handle, &app_idx))
    {
        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);

        if (btif_hl_find_mcl_idx(app_idx, p_data->sdp_query_cfm.bd_addr, &mcl_idx))
        {
            p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
            if (p_mcb->cch_oper != BTIF_HL_CCH_OP_NONE)
            {
                memcpy(&p_mcb->sdp, p_sdp, sizeof(tBTA_HL_SDP));
                old_cch_oper = p_mcb->cch_oper;
                p_mcb->cch_oper = BTIF_HL_CCH_OP_NONE;

                switch (old_cch_oper)
                {
                    case BTIF_HL_CCH_OP_MDEP_FILTERING:
                        status = btif_hl_find_sdp_idx_using_mdep_filter(app_idx, mcl_idx, &sdp_idx);
                        break;
                    default:
                        break;
                }

                if (status)
                {
                    p_mcb->sdp_idx       = sdp_idx;
                    p_mcb->valid_sdp_idx = TRUE;
                    p_mcb->ctrl_psm      = p_mcb->sdp.sdp_rec[sdp_idx].ctrl_psm;


                    switch (old_cch_oper)
                    {
                        case BTIF_HL_CCH_OP_MDEP_FILTERING:
                            p_pcb = BTIF_HL_GET_PCB_PTR(app_idx, mcl_idx);
                            if (p_pcb->in_use)
                            {
                                switch (p_pcb->op)
                                {
                                    case BTIF_HL_PEND_DCH_OP_OPEN:
                                        btif_hl_send_setup_connecting_cb(app_idx, mcl_idx);
                                        break;
                                    case BTIF_HL_PEND_DCH_OP_DELETE_MDL:
                                    default:
                                        break;
                                }
                                open_param.ctrl_psm = p_mcb->ctrl_psm;
                                bdcpy(open_param.bd_addr, p_mcb->bd_addr);
                                open_param.sec_mask = (BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT);
                                BTA_HlCchOpen(p_acb->app_handle, &open_param);
                            }
                            break;

                        case BTIF_HL_CCH_OP_DCH_OPEN:
                            status = btif_hl_proc_pending_op(app_idx,mcl_idx);
                            break;

                        case BTIF_HL_CCH_OP_DCH_RECONNECT:
                            //reconnect_param.ctrl_psm = p_mcb->ctrl_psm;
                            //reconnect_param.mdl_id =  p_mcb->acquire.acquire_info.mdl_id;
                            //BTA_HlDchReconnect(p_mcb->mcl_handle, &reconnect_param);
                            break;

                        default:
                            BTIF_TRACE_ERROR1("Invalid CCH oper %d", old_cch_oper);
                            break;
                    }

                }
                else
                {
                    BTIF_TRACE_ERROR0("Can not find SDP idx discard CCH Open request");
                }
            }
        }
    }

    return status;
}


/*******************************************************************************
**
** Function         btif_hl_proc_cch_open_ind
**
** Description      Process the CCH open indication
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_cch_open_ind(tBTA_HL *p_data)

{
    btif_hl_mcl_cb_t         *p_mcb;
    UINT8                   app_idx, mcl_idx;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);
    if (btif_hl_find_app_idx_using_handle(p_data->cch_open_ind.app_handle, &app_idx))
    {
        if (!btif_hl_find_mcl_idx(app_idx, p_data->cch_open_ind.bd_addr, &mcl_idx))
        {
            if (btif_hl_find_avail_mcl_idx(app_idx, &mcl_idx))
            {
                p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
                memset(p_mcb, 0, sizeof(btif_hl_mcl_cb_t));
                p_mcb->in_use = TRUE;
                p_mcb->is_connected = TRUE;
                p_mcb->mcl_handle = p_data->cch_open_ind.mcl_handle;
                bdcpy(p_mcb->bd_addr, p_data->cch_open_ind.bd_addr);
            }
        }
        else
        {
            BTIF_TRACE_ERROR0("The MCL already exist for cch_open_ind");
        }
    }
}

/*******************************************************************************
**
** Function         btif_hl_proc_pending_op
**
** Description      Process the pending dch operation.
**
** Returns          Nothing
**
*******************************************************************************/
BOOLEAN btif_hl_proc_pending_op(UINT8 app_idx, UINT8 mcl_idx)

{

    btif_hl_app_cb_t            *p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
    btif_hl_mcl_cb_t            *p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
    btif_hl_pending_chan_cb_t   *p_pcb;
    BOOLEAN                     status = FALSE;
    tBTA_HL_DCH_OPEN_PARAM      dch_open;

    p_pcb = BTIF_HL_GET_PCB_PTR(app_idx, mcl_idx);
    if (p_pcb->in_use)
    {
        switch (p_pcb->op)
        {
            case BTIF_HL_PEND_DCH_OP_OPEN:
                BTIF_TRACE_DEBUG0("op BTIF_HL_PEND_DCH_OP_OPEN");
                dch_open.ctrl_psm = p_mcb->ctrl_psm;
                dch_open.local_mdep_id = p_acb->sup_feature.mdep[p_pcb->mdep_cfg_idx].mdep_id;
                if (btif_hl_find_peer_mdep_id(p_acb->app_id, p_mcb->bd_addr,
                                              p_acb->sup_feature.mdep[p_pcb->mdep_cfg_idx].mdep_cfg.mdep_role,
                                              p_acb->sup_feature.mdep[p_pcb->mdep_cfg_idx].mdep_cfg.data_cfg[0].data_type, &dch_open.peer_mdep_id ))
                {
                    dch_open.local_cfg = p_acb->channel_type[p_pcb->mdep_cfg_idx];
                    if ((p_acb->sup_feature.mdep[p_pcb->mdep_cfg_idx].mdep_cfg.mdep_role == BTA_HL_MDEP_ROLE_SOURCE)
                        && !btif_hl_is_the_first_reliable_existed(app_idx, mcl_idx))
                    {
                        dch_open.local_cfg = BTA_HL_DCH_CFG_RELIABLE;
                    }
                    dch_open.sec_mask = (BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT);

                    BTIF_TRACE_DEBUG1("dch_open.local_cfg=%d  ", dch_open.local_cfg);
                    BTIF_TRACE_DEBUG0("calling BTA_HlDchOpen");
                    btif_hl_send_setup_connecting_cb(app_idx,mcl_idx);
                    BTA_HlDchOpen(p_mcb->mcl_handle, &dch_open);
                    status = TRUE;
                }
                break;
            case BTIF_HL_PEND_DCH_OP_DELETE_MDL:
                BTA_HlDeleteMdl(p_mcb->mcl_handle, p_acb->delete_mdl.mdl_id);
                status = TRUE;
                break;

            default:
                break;
        }


    }
    return status;
}




/*******************************************************************************
**
** Function         btif_hl_proc_cch_open_cfm
**
** Description      Process the CCH open confirmation
**
** Returns          Nothing
**
*******************************************************************************/
static BOOLEAN btif_hl_proc_cch_open_cfm(tBTA_HL *p_data)

{
    btif_hl_app_cb_t         *p_acb;
    btif_hl_mcl_cb_t         *p_mcb;
    UINT8                    app_idx, mcl_idx;
    BOOLEAN                  status = FALSE;
    tBTA_HL_DCH_OPEN_PARAM   dch_open;


    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);

    if (btif_hl_find_app_idx_using_handle(p_data->cch_open_cfm.app_handle, &app_idx))
    {
        BTIF_TRACE_DEBUG1("app_idx=%d", app_idx);
        if (btif_hl_find_mcl_idx(app_idx, p_data->cch_open_cfm.bd_addr, &mcl_idx))
        {
            p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);

            p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
            BTIF_TRACE_DEBUG1("mcl_idx=%d", mcl_idx);
            p_mcb->mcl_handle = p_data->cch_open_cfm.mcl_handle;
            p_mcb->is_connected = TRUE;

            status = btif_hl_proc_pending_op(app_idx, mcl_idx);


        }
    }

    return status;
}


/*******************************************************************************
**
** Function         btif_hl_proc_cch_close_ind
**
** Description      Process the CCH close indication
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_cch_close_ind(tBTA_HL *p_data)

{
    UINT8                   app_idx, mcl_idx;
    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);

    if (btif_hl_find_mcl_idx_using_handle(p_data->cch_close_ind.mcl_handle, &app_idx, &mcl_idx))
    {
        btif_hl_send_setup_disconnected_cb(app_idx, mcl_idx);
        btif_hl_release_mcl_sockets(app_idx, mcl_idx);
        btif_hl_clean_mcl_cb(app_idx, mcl_idx);
    }
}


/*******************************************************************************
**
** Function         btif_hl_proc_cch_close_cfm
**
** Description      Process the CCH close confirmation
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_cch_close_cfm(tBTA_HL *p_data)

{
    UINT8                   app_idx, mcl_idx;
    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);

    if (btif_hl_find_mcl_idx_using_handle(p_data->cch_close_cfm.mcl_handle, &app_idx, &mcl_idx))
    {
        btif_hl_send_setup_disconnected_cb(app_idx, mcl_idx);
        btif_hl_release_mcl_sockets(app_idx, mcl_idx);
        btif_hl_clean_mcl_cb(app_idx, mcl_idx);
    }
}

/*******************************************************************************
**
** Function         btif_hl_proc_create_ind
**
** Description      Process the MDL create indication
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_create_ind(tBTA_HL *p_data){
    btif_hl_app_cb_t         *p_acb;
    btif_hl_mcl_cb_t         *p_mcb;
    tBTA_HL_MDEP            *p_mdep;
    UINT8                   app_idx, mcl_idx, mdep_cfg_idx;
    BOOLEAN                 first_reliable_exist;
    BOOLEAN                 success = TRUE;
    tBTA_HL_DCH_CFG         rsp_cfg = BTA_HL_DCH_CFG_UNKNOWN;
    tBTA_HL_DCH_CREATE_RSP  rsp_code = BTA_HL_DCH_CREATE_RSP_CFG_REJ;
    tBTA_HL_DCH_CREATE_RSP_PARAM create_rsp_param;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);

    if (btif_hl_find_mcl_idx_using_handle(p_data->dch_create_ind.mcl_handle, &app_idx, &mcl_idx))
    {
        p_acb =BTIF_HL_GET_APP_CB_PTR(app_idx);
        p_mcb =BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);

        if (btif_hl_find_mdep_cfg_idx(app_idx, p_data->dch_create_ind.local_mdep_id, &mdep_cfg_idx))
        {
            p_mdep = &(p_acb->sup_feature.mdep[mdep_cfg_idx]);
            first_reliable_exist = btif_hl_is_the_first_reliable_existed(app_idx, mcl_idx);
            switch (p_mdep->mdep_cfg.mdep_role)
            {
                case BTA_HL_MDEP_ROLE_SOURCE:
                    if (p_data->dch_create_ind.cfg == BTA_HL_DCH_CFG_NO_PREF)
                    {
                        if (first_reliable_exist)
                        {
                            rsp_cfg = p_acb->channel_type[mdep_cfg_idx];
                        }
                        else
                        {
                            rsp_cfg = BTA_HL_DCH_CFG_RELIABLE;
                        }
                        rsp_code = BTA_HL_DCH_CREATE_RSP_SUCCESS;
                    }

                    break;
                case BTA_HL_MDEP_ROLE_SINK:

                    if ((p_data->dch_create_ind.cfg  == BTA_HL_DCH_CFG_RELIABLE) ||
                        (first_reliable_exist && (p_data->dch_create_ind.cfg  == BTA_HL_DCH_CFG_STREAMING)))
                    {
                        rsp_code = BTA_HL_DCH_CREATE_RSP_SUCCESS;
                        rsp_cfg = p_data->dch_create_ind.cfg;
                    }
                    break;
                default:
                    break;
            }
        }
    }
    else
    {
        success = FALSE;
    }

    if (success)
    {
        BTIF_TRACE_DEBUG2("create response rsp_code=%d rsp_cfg=%d", rsp_code, rsp_cfg );
        create_rsp_param.local_mdep_id = p_data->dch_create_ind.local_mdep_id;
        create_rsp_param.mdl_id = p_data->dch_create_ind.mdl_id;
        create_rsp_param.rsp_code = rsp_code;
        create_rsp_param.cfg_rsp = rsp_cfg;
        BTA_HlDchCreateRsp(p_mcb->mcl_handle, &create_rsp_param);
    }
}

/*******************************************************************************
**
** Function         btif_hl_proc_dch_open_ind
**
** Description      Process the DCH open indication
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_dch_open_ind(tBTA_HL *p_data)

{
    btif_hl_app_cb_t         *p_acb;
    btif_hl_mcl_cb_t         *p_mcb;
    btif_hl_mdl_cb_t         *p_dcb;
    UINT8                    app_idx, mcl_idx, mdl_idx, mdep_cfg_idx;
    UINT8                    dc_cfg;
    BOOLEAN close_dch = FALSE;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);

    if (btif_hl_find_mcl_idx_using_handle(p_data->dch_open_ind.mcl_handle, &app_idx, &mcl_idx ))
    {
        p_acb =BTIF_HL_GET_APP_CB_PTR(app_idx);
        p_mcb =BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);

        if (btif_hl_find_avail_mdl_idx(app_idx, mcl_idx, &mdl_idx))
        {
            p_dcb = BTIF_HL_GET_MDL_CB_PTR(app_idx, mcl_idx, mdl_idx);

            if (btif_hl_find_mdep_cfg_idx(app_idx, p_data->dch_open_ind.local_mdep_id, &mdep_cfg_idx))
            {
                p_dcb->in_use               = TRUE;
                p_dcb->mdl_handle           =  p_data->dch_open_ind.mdl_handle;
                p_dcb->local_mdep_cfg_idx   = mdep_cfg_idx;
                p_dcb->local_mdep_id        = p_data->dch_open_ind.local_mdep_id;
                p_dcb->mdl_id               = p_data->dch_open_ind.mdl_id;
                p_dcb->dch_mode             = p_data->dch_open_ind.dch_mode;
                p_dcb->dch_mode             = p_data->dch_open_ind.dch_mode;
                p_dcb->is_the_first_reliable = p_data->dch_open_ind.first_reliable;
                p_dcb->mtu                  = p_data->dch_open_ind.mtu;

                p_dcb->channel_id = btif_hl_get_next_channel_id(p_acb->app_id);
                BTIF_TRACE_DEBUG4(" app_idx=%d mcl_idx=%d mdl_idx=%d channel_id=%d",
                                  app_idx, mcl_idx, mdl_idx, p_dcb->channel_id  );
                if (!btif_hl_create_socket(app_idx, mcl_idx, mdl_idx))
                {
                    BTIF_TRACE_ERROR0("Unable to create socket");
                    close_dch = TRUE;
                }
            }
            else
            {
                BTIF_TRACE_ERROR1("INVALID_LOCAL_MDEP_ID mdep_id=%d",p_data->dch_open_cfm.local_mdep_id);
                close_dch = TRUE;
            }

            if (close_dch)
                btif_hl_clean_mdl_cb(p_dcb);
        }
        else
            close_dch = TRUE;
    }
    else
        close_dch = TRUE;

    if (close_dch)
        BTA_HlDchClose(p_data->dch_open_cfm.mdl_handle);
}

/*******************************************************************************
**
** Function         btif_hl_proc_dch_open_cfm
**
** Description      Process the DCH close confirmation
**
** Returns          Nothing
**
*******************************************************************************/
static BOOLEAN btif_hl_proc_dch_open_cfm(tBTA_HL *p_data)

{
    btif_hl_app_cb_t            *p_acb;
    btif_hl_mcl_cb_t            *p_mcb;
    btif_hl_mdl_cb_t            *p_dcb;
    btif_hl_pending_chan_cb_t   *p_pcb;
    UINT8                    app_idx, mcl_idx, mdl_idx, mdep_cfg_idx;
    BOOLEAN                  status = FALSE;
    BOOLEAN                  close_dch = FALSE;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);

    if (btif_hl_find_mcl_idx_using_handle(p_data->dch_open_cfm.mcl_handle, &app_idx, &mcl_idx ))
    {
        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
        p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
        p_pcb = BTIF_HL_GET_PCB_PTR(app_idx, mcl_idx);

        if (btif_hl_find_avail_mdl_idx(app_idx, mcl_idx, &mdl_idx))
        {
            p_dcb = BTIF_HL_GET_MDL_CB_PTR(app_idx, mcl_idx, mdl_idx);

            if (btif_hl_find_mdep_cfg_idx(app_idx, p_data->dch_open_cfm.local_mdep_id, &mdep_cfg_idx))
            {
                p_dcb->in_use               = TRUE;
                p_dcb->mdl_handle           = p_data->dch_open_cfm.mdl_handle;
                p_dcb->local_mdep_cfg_idx   = mdep_cfg_idx;
                p_dcb->local_mdep_id        = p_data->dch_open_cfm.local_mdep_id;
                p_dcb->mdl_id               = p_data->dch_open_cfm.mdl_id;
                p_dcb->dch_mode             = p_data->dch_open_cfm.dch_mode;
                p_dcb->is_the_first_reliable= p_data->dch_open_cfm.first_reliable;
                p_dcb->mtu                  = p_data->dch_open_cfm.mtu;
                p_dcb->channel_id           = p_pcb->channel_id;

                BTIF_TRACE_DEBUG3("app_idx=%d mcl_idx=%d mdl_idx=%d",  app_idx, mcl_idx, mdl_idx  );
                btif_hl_send_setup_connecting_cb(app_idx, mcl_idx);
                if (btif_hl_create_socket(app_idx, mcl_idx, mdl_idx))
                {
                    status = TRUE;
                    BTIF_TRACE_DEBUG4("app_idx=%d mcl_idx=%d mdl_idx=%d p_dcb->channel_id=0x%08x",  app_idx, mcl_idx, mdl_idx, p_dcb->channel_id);
                    btif_hl_clean_pcb(p_pcb);
                }
                else
                {
                    BTIF_TRACE_ERROR0("Unable to create socket");
                    close_dch = TRUE;
                }
            }
            else
            {
                BTIF_TRACE_ERROR1("INVALID_LOCAL_MDEP_ID mdep_id=%d",p_data->dch_open_cfm.local_mdep_id);
                close_dch = TRUE;
            }

            if (close_dch)
            {
                btif_hl_clean_mdl_cb(p_dcb);
                BTA_HlDchClose(p_data->dch_open_cfm.mdl_handle);
            }
        }
    }

    return status;
}

/*******************************************************************************
**
** Function         btif_hl_proc_dch_reconnect_ind
**
** Description      Process the DCH reconnect indication
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_dch_reconnect_ind(tBTA_HL *p_data)

{
    btif_hl_app_cb_t        *p_acb;
    btif_hl_mcl_cb_t        *p_mcb;
    btif_hl_mdl_cb_t        *p_dcb;
    UINT8                   app_idx, mcl_idx, mdl_idx, mdep_cfg_idx, dc_cfg;
    BOOLEAN                 close_dch = FALSE;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);


    if (btif_hl_find_mcl_idx_using_handle(p_data->dch_reconnect_ind.mcl_handle, &app_idx, &mcl_idx))
    {
        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
        p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);

        if (btif_hl_find_avail_mdl_idx(app_idx, mcl_idx, &mdl_idx))
        {
            p_dcb =BTIF_HL_GET_MDL_CB_PTR(app_idx, mcl_idx, mdl_idx);

            if (btif_hl_find_mdep_cfg_idx(app_idx, p_data->dch_reconnect_ind.local_mdep_id, &mdep_cfg_idx))
            {
                p_dcb->in_use               = TRUE;
                p_dcb->mdl_handle           = p_data->dch_reconnect_ind.mdl_handle;
                p_dcb->local_mdep_cfg_idx   = mdep_cfg_idx;
                p_dcb->local_mdep_id        = p_data->dch_reconnect_ind.local_mdep_id;
                p_dcb->mdl_id               = p_data->dch_reconnect_ind.mdl_id;
                p_dcb->dch_mode             = p_data->dch_reconnect_ind.dch_mode;
                p_dcb->dch_mode             = p_data->dch_reconnect_ind.dch_mode;
                p_dcb->is_the_first_reliable= p_data->dch_reconnect_ind.first_reliable;
                p_dcb->mtu                  = p_data->dch_reconnect_ind.mtu;
                p_dcb->channel_id           = btif_hl_get_next_channel_id(p_acb->app_id);

                BTIF_TRACE_DEBUG4(" app_idx=%d mcl_idx=%d mdl_idx=%d channel_id=%d",
                                  app_idx, mcl_idx, mdl_idx, p_dcb->channel_id  );
                if (!btif_hl_create_socket(app_idx, mcl_idx, mdl_idx))
                {
                    BTIF_TRACE_ERROR0("Unable to create socket");
                    close_dch = TRUE;
                }
            }
            else
            {
                BTIF_TRACE_ERROR1("INVALID_LOCAL_MDEP_ID mdep_id=%d",p_data->dch_open_cfm.local_mdep_id);
                close_dch = TRUE;
            }

            if (close_dch)
                btif_hl_clean_mdl_cb(p_dcb);
        }
        else
            close_dch = TRUE;
    }
    else
        close_dch = TRUE;

    if (close_dch)
        BTA_HlDchClose(p_data->dch_reconnect_ind.local_mdep_id);


}



/*******************************************************************************
**
** Function         btif_hl_proc_dch_close_ind
**
** Description      Process the DCH close indication
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_dch_close_ind(tBTA_HL *p_data)

{
    btif_hl_mdl_cb_t         *p_dcb;
    UINT8                   app_idx, mcl_idx, mdl_idx;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);
    if (btif_hl_find_mdl_idx_using_handle(p_data->dch_close_ind.mdl_handle,
                                          &app_idx, &mcl_idx, &mdl_idx ))
    {
        p_dcb = BTIF_HL_GET_MDL_CB_PTR(app_idx, mcl_idx, mdl_idx);
        btif_hl_release_socket(app_idx,mcl_idx, mdl_idx);
        btif_hl_clean_mdl_cb(p_dcb);
        BTIF_TRACE_DEBUG1("remote DCH close success mdl_idx=%d", mdl_idx);
    }
}

/*******************************************************************************
**
** Function         btif_hl_proc_dch_close_cfm
**
** Description      Process the DCH reconnect confirmation
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_dch_close_cfm(tBTA_HL *p_data)

{
    btif_hl_mdl_cb_t         *p_dcb;
    UINT8                   app_idx, mcl_idx, mdl_idx;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);
    if (btif_hl_find_mdl_idx_using_handle(p_data->dch_close_cfm.mdl_handle,
                                          &app_idx, &mcl_idx, &mdl_idx ))
    {
        p_dcb = BTIF_HL_GET_MDL_CB_PTR(app_idx, mcl_idx, mdl_idx);
        btif_hl_release_socket(app_idx,mcl_idx,mdl_idx);
        btif_hl_clean_mdl_cb(p_dcb);
        BTIF_TRACE_DEBUG1("BTAPP local DCH close success mdl_idx=%d", mdl_idx);
    }
}


/*******************************************************************************
**
** Function         btif_hl_proc_abort_ind
**
** Description      Process the abort indicaiton
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_abort_ind(tBTA_HL_MCL_HANDLE mcl_handle){

    UINT8                   app_idx,mcl_idx;
    BTIF_TRACE_DEBUG1("%s", __FUNCTION__ );
    if (btif_hl_find_mcl_idx_using_handle(mcl_handle, &app_idx, &mcl_idx))
    {
        /* tod0 */
    }
}




/*******************************************************************************
**
** Function         btif_hl_proc_abort_cfm
**
** Description      Process the abort confirmation
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_abort_cfm(tBTA_HL_MCL_HANDLE mcl_handle){
    UINT8                   app_idx,mcl_idx;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__ );
    if (btif_hl_find_mcl_idx_using_handle(mcl_handle, &app_idx, &mcl_idx))
    {
        /* tod0 */
    }
}



/*******************************************************************************
**
** Function         btif_hl_proc_send_data_cfm
**
** Description      Process the send data confirmation
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_send_data_cfm(tBTA_HL_MDL_HANDLE mdl_handle,
                                       tBTA_HL_STATUS status){
    UINT8                   app_idx,mcl_idx, mdl_idx;
    btif_hl_mdl_cb_t         *p_dcb;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);
    if (btif_hl_find_mdl_idx_using_handle(mdl_handle,
                                          &app_idx, &mcl_idx, &mdl_idx ))
    {
        p_dcb =BTIF_HL_GET_MDL_CB_PTR(app_idx, mcl_idx, mdl_idx);
        btif_hl_free_buf((void **) &p_dcb->p_tx_pkt);
        BTIF_TRACE_DEBUG1("send success free p_tx_pkt tx_size=%d", p_dcb->tx_size);
        p_dcb->tx_size = 0;
    }
}



/*******************************************************************************
**
** Function         btif_hl_proc_dch_cong_ind
**
** Description      Process the DCH congestion change indication
**
** Returns          Nothing
**
*******************************************************************************/
static void btif_hl_proc_dch_cong_ind(tBTA_HL *p_data)

{
    btif_hl_mdl_cb_t         *p_dcb;
    UINT8                   app_idx, mcl_idx, mdl_idx;

    BTIF_TRACE_DEBUG0("btif_hl_proc_dch_cong_ind");


    if (btif_hl_find_mdl_idx_using_handle(p_data->dch_cong_ind.mdl_handle, &app_idx, &mcl_idx, &mdl_idx))
    {
        p_dcb =BTIF_HL_GET_MDL_CB_PTR(app_idx, mcl_idx, mdl_idx);
        p_dcb->cong = p_data->dch_cong_ind.cong;
    }
}

/*******************************************************************************
**
** Function         btif_hl_send_chan_state_cb_evt
**
** Description      Process send channel state callback event in the btif task context
**
** Returns          void
**
*******************************************************************************/
static void btif_hl_send_chan_state_cb_evt(UINT16 event, char* p_param)
{

    btif_hl_send_chan_state_cb_t    *p_data = (btif_hl_send_chan_state_cb_t *)p_param;
    bt_bdaddr_t                     bd_addr;
    bthl_channel_state_t            state=BTHL_CONN_STATE_DISCONNECTED;
    BOOLEAN                         send_cb=TRUE;


    BTIF_TRACE_DEBUG2("%s event %d", __FUNCTION__, event);
    btif_hl_copy_bda(&bd_addr, p_data->bd_addr);

    switch (event)
    {
        case BTIF_HL_SEND_CONNECTED_CB:

            if (p_data->cb_state == BTIF_HL_CHAN_CB_STATE_CONNECTED_PENDING)
            {
                state = BTHL_CONN_STATE_CONNECTED;
            }
            break;
        case BTIF_HL_SEND_DISCONNECTED_CB:
            if (p_data->cb_state == BTIF_HL_CHAN_CB_STATE_DISCONNECTED_PENDING)
            {
                state = BTHL_CONN_STATE_DISCONNECTED;
            }
            break;

        default:
            send_cb  = FALSE;
            break;
    }

    if (send_cb)
    {
        BTIF_TRACE_DEBUG5("channel_id=0x%08x mdep_cfg_idx=%d,cb_state=%d  state=%d  fd=%d",p_data->channel_id,
                          p_data->mdep_cfg_index, p_data->cb_state, state,  p_data->fd);
        btif_hl_display_bt_bda(&bd_addr);
        BTIF_HL_CALL_CBACK(bt_hl_callbacks, channel_state_cb,  p_data->app_id,
                           &bd_addr, p_data->mdep_cfg_index,
                           p_data->channel_id, state, p_data->fd );
    }
}

/*******************************************************************************
**
** Function         btif_hl_upstreams_evt
**
** Description      Process HL events
**
** Returns          void
**
*******************************************************************************/
static void btif_hl_upstreams_evt(UINT16 event, char* p_param){
    tBTA_HL *p_data = (tBTA_HL *)p_param;
    UINT8                 app_idx, mcl_idx;
    btif_hl_app_cb_t      *p_acb;
    btif_hl_mcl_cb_t      *p_mcb = NULL;
    BD_ADDR               bd_addr;
    btif_hl_pend_dch_op_t  pending_op;
    BOOLEAN status;

    BTIF_TRACE_DEBUG2("%s event %d", __FUNCTION__, event);
    switch (event)
    {

        case BTA_HL_REGISTER_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_REGISTER_CFM_EVT");
            BTIF_TRACE_DEBUG3("app_id=%d app_handle=%d status=%d ",
                              p_data->reg_cfm.app_id,
                              p_data->reg_cfm.app_handle,
                              p_data->reg_cfm.status );

            btif_hl_proc_reg_cfm(p_data);
            break;
        case BTA_HL_SDP_INFO_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_SDP_INFO_IND_EVT");
            BTIF_TRACE_DEBUG5("app_handle=%d ctrl_psm=0x%04x data_psm=0x%04x x_spec=%d mcap_sup_procs=0x%02x",
                              p_data->sdp_info_ind.app_handle,
                              p_data->sdp_info_ind.ctrl_psm,
                              p_data->sdp_info_ind.data_psm,
                              p_data->sdp_info_ind.data_x_spec,
                              p_data->sdp_info_ind.mcap_sup_procs);
            btif_hl_proc_sdp_info_ind(p_data);
            break;

        case BTA_HL_DEREGISTER_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DEREGISTER_CFM_EVT");
            BTIF_TRACE_DEBUG2("app_handle=%d status=%d ",
                              p_data->dereg_cfm.app_handle,
                              p_data->dereg_cfm.status );
            btif_hl_proc_dereg_cfm(p_data);
            break;

        case BTA_HL_SDP_QUERY_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_SDP_QUERY_CFM_EVT");
            BTIF_TRACE_DEBUG2("app_handle=%d status =%d",
                              p_data->sdp_query_cfm.app_handle,
                              p_data->sdp_query_cfm.status);

            BTIF_TRACE_DEBUG6("DB [%02x] [%02x] [%02x] [%02x] [%02x] [%02x]",
                              p_data->sdp_query_cfm.bd_addr[0], p_data->sdp_query_cfm.bd_addr[1],
                              p_data->sdp_query_cfm.bd_addr[2], p_data->sdp_query_cfm.bd_addr[3],
                              p_data->sdp_query_cfm.bd_addr[4], p_data->sdp_query_cfm.bd_addr[5]);

            if (p_data->sdp_query_cfm.status == BTA_HL_STATUS_OK)
                status = btif_hl_proc_sdp_query_cfm(p_data);
            else
                status = FALSE;

            if (!status)
            {
                if (btif_hl_find_app_idx_using_handle(p_data->sdp_query_cfm.app_handle, &app_idx))
                {
                    p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
                    if (btif_hl_find_mcl_idx(app_idx, p_data->sdp_query_cfm.bd_addr, &mcl_idx))
                    {
                        p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
                        if ( (p_mcb->cch_oper =  BTIF_HL_CCH_OP_MDEP_FILTERING) ||
                             (p_mcb->cch_oper == BTIF_HL_CCH_OP_DCH_OPEN) )
                        {
                            pending_op = p_mcb->pcb.op;
                            switch (pending_op)
                            {
                                case BTIF_HL_PEND_DCH_OP_OPEN:
                                    btif_hl_send_setup_disconnected_cb(app_idx, mcl_idx);
                                    break;
                                case BTIF_HL_PEND_DCH_OP_RECONNECT:
                                case BTIF_HL_PEND_DCH_OP_DELETE_MDL:
                                default:
                                    break;
                            }
                            if (!p_mcb->is_connected)
                                btif_hl_clean_mcl_cb(app_idx, mcl_idx);
                        }
                    }
                }
            }

            break;


        case BTA_HL_CCH_OPEN_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_CCH_OPEN_CFM_EVT");
            BTIF_TRACE_DEBUG3("app_handle=%d mcl_handle=%d status =%d",
                              p_data->cch_open_cfm.app_handle,
                              p_data->cch_open_cfm.mcl_handle,
                              p_data->cch_open_cfm.status);
            BTIF_TRACE_DEBUG6("DB [%02x] [%02x] [%02x] [%02x] [%02x] [%02x]",
                              p_data->cch_open_cfm.bd_addr[0], p_data->cch_open_cfm.bd_addr[1],
                              p_data->cch_open_cfm.bd_addr[2], p_data->cch_open_cfm.bd_addr[3],
                              p_data->cch_open_cfm.bd_addr[4], p_data->cch_open_cfm.bd_addr[5]);

            if (p_data->cch_open_cfm.status == BTA_HL_STATUS_OK)
            {
                status = btif_hl_proc_cch_open_cfm(p_data);
            }
            else
            {
                status = FALSE;
            }

            if (!status)
            {
                if (btif_hl_find_app_idx_using_handle(p_data->cch_open_cfm.app_handle, &app_idx))
                {
                    p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
                    if (btif_hl_find_mcl_idx(app_idx, p_data->cch_open_cfm.bd_addr, &mcl_idx))
                    {
                        p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
                        pending_op = p_mcb->pcb.op;
                        switch (pending_op)
                        {
                            case BTIF_HL_PEND_DCH_OP_OPEN:
                                btif_hl_send_setup_disconnected_cb(app_idx, mcl_idx);
                                break;
                            case BTIF_HL_PEND_DCH_OP_RECONNECT:
                            case BTIF_HL_PEND_DCH_OP_DELETE_MDL:
                            default:
                                break;
                        }
                        btif_hl_clean_mcl_cb(app_idx, mcl_idx);
                    }
                }
            }
            break;

        case BTA_HL_DCH_OPEN_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_OPEN_CFM_EVT");
            BTIF_TRACE_DEBUG3("mcl_handle=%d mdl_handle=0x%x status=%d ",
                              p_data->dch_open_cfm.mcl_handle,
                              p_data->dch_open_cfm.mdl_handle,
                              p_data->dch_open_cfm.status);
            BTIF_TRACE_DEBUG5("first_reliable =%d dch_mode=%d local_mdep_id=%d mdl_id=%d mtu=%d",
                              p_data->dch_open_cfm.first_reliable,
                              p_data->dch_open_cfm.dch_mode,
                              p_data->dch_open_cfm.local_mdep_id,
                              p_data->dch_open_cfm.mdl_id,
                              p_data->dch_open_cfm.mtu);
            if (p_data->dch_open_cfm.status == BTA_HL_STATUS_OK)
            {
                status = btif_hl_proc_dch_open_cfm(p_data);
            }
            else
            {
                status = FALSE;
            }

            if (!status)
            {
                if (btif_hl_find_mcl_idx_using_handle(p_data->dch_open_cfm.mcl_handle,&app_idx, &mcl_idx))
                {
                    p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
                    pending_op = p_mcb->pcb.op;
                    switch (pending_op)
                    {
                        case BTIF_HL_PEND_DCH_OP_OPEN:
                            btif_hl_send_setup_disconnected_cb(app_idx, mcl_idx);
                            break;
                        case BTIF_HL_PEND_DCH_OP_RECONNECT:
                        case BTIF_HL_PEND_DCH_OP_DELETE_MDL:
                        default:
                            break;
                    }
                    //todo start a timer to close cch if no dch
                }
            }
            break;


        case BTA_HL_CCH_OPEN_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_CCH_OPEN_IND_EVT");
            BTIF_TRACE_DEBUG2("app_handle=%d mcl_handle=%d",
                              p_data->cch_open_ind.app_handle,
                              p_data->cch_open_ind.mcl_handle);
            BTIF_TRACE_DEBUG6("DB [%02x] [%02x] [%02x] [%02x] [%02x] [%02x]",
                              p_data->cch_open_ind.bd_addr[0], p_data->cch_open_ind.bd_addr[1],
                              p_data->cch_open_ind.bd_addr[2], p_data->cch_open_ind.bd_addr[3],
                              p_data->cch_open_ind.bd_addr[4], p_data->cch_open_ind.bd_addr[5]);

            btif_hl_proc_cch_open_ind(p_data);
            break;

        case BTA_HL_DCH_CREATE_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_CREATE_IND_EVT");
            BTIF_TRACE_DEBUG1("mcl_handle=%d",
                              p_data->dch_create_ind.mcl_handle );
            BTIF_TRACE_DEBUG3("local_mdep_id =%d mdl_id=%d cfg=%d",
                              p_data->dch_create_ind.local_mdep_id,
                              p_data->dch_create_ind.mdl_id,
                              p_data->dch_create_ind.cfg);
            btif_hl_proc_create_ind(p_data);
            break;

        case BTA_HL_DCH_OPEN_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_OPEN_IND_EVT");
            BTIF_TRACE_DEBUG2("mcl_handle=%d mdl_handle=0x%x",
                              p_data->dch_open_ind.mcl_handle,
                              p_data->dch_open_ind.mdl_handle );
            BTIF_TRACE_DEBUG5("first_reliable =%d dch_mode=%d local_mdep_id=%d mdl_id=%d mtu=%d",
                              p_data->dch_open_ind.first_reliable,
                              p_data->dch_open_ind.dch_mode,
                              p_data->dch_open_ind.local_mdep_id,
                              p_data->dch_open_ind.mdl_id,
                              p_data->dch_open_ind.mtu);

            btif_hl_proc_dch_open_ind(p_data);
            break;

        case BTA_HL_DELETE_MDL_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DELETE_MDL_IND_EVT");
            BTIF_TRACE_DEBUG2("mcl_handle=%d mdl_id=0x%x",
                              p_data->delete_mdl_ind.mcl_handle,
                              p_data->delete_mdl_ind.mdl_id);
            if (btif_hl_find_mcl_idx_using_handle( p_data->delete_mdl_ind.mcl_handle, &app_idx, &mcl_idx))
            {
                p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
                p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
                /* todo send callback find channel id from NV? */
            }
            break;

        case BTA_HL_DELETE_MDL_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DELETE_MDL_CFM_EVT");
            BTIF_TRACE_DEBUG3("mcl_handle=%d mdl_id=0x%x status=%d",
                              p_data->delete_mdl_cfm.mcl_handle,
                              p_data->delete_mdl_cfm.mdl_id,
                              p_data->delete_mdl_cfm.status);


            if (btif_hl_find_mcl_idx_using_handle( p_data->delete_mdl_cfm.mcl_handle, &app_idx,&mcl_idx))
            {
                p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);

                btif_hl_send_destroyed_cb(p_acb);
                btif_hl_clean_delete_mdl(&p_acb->delete_mdl);
                /* todo if delete mdl failed we still report mdl delete ok and remove the mld_id from NV*/
            }
            break;

        case BTA_HL_DCH_RECONNECT_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_RECONNECT_CFM_EVT");
            BTIF_TRACE_DEBUG3("mcl_handle=%d mdl_handle=%d status=%d   ",
                              p_data->dch_reconnect_cfm.mcl_handle,
                              p_data->dch_reconnect_cfm.mdl_handle,
                              p_data->dch_reconnect_cfm.status);
            BTIF_TRACE_DEBUG4("first_reliable =%d dch_mode=%d mdl_id=%d mtu=%d",
                              p_data->dch_reconnect_cfm.first_reliable,
                              p_data->dch_reconnect_cfm.dch_mode,
                              p_data->dch_reconnect_cfm.mdl_id,
                              p_data->dch_reconnect_cfm.mtu);
            /* sot supported */
            /* will be supported in next phase */
            break;

        case BTA_HL_CCH_CLOSE_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_CCH_CLOSE_CFM_EVT");
            BTIF_TRACE_DEBUG2("mcl_handle=%d status =%d",
                              p_data->cch_close_cfm.mcl_handle,
                              p_data->cch_close_cfm.status);
            if (p_data->cch_close_cfm.status == BTA_HL_STATUS_OK)
            {
                btif_hl_proc_cch_close_cfm(p_data);
            }
            break;

        case BTA_HL_CCH_CLOSE_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_CCH_CLOSE_IND_EVT");
            BTIF_TRACE_DEBUG2("mcl_handle =%d intentional_close=%s",
                              p_data->cch_close_ind.mcl_handle,
                              (p_data->cch_close_ind.intentional?"Yes":"No"));

            btif_hl_proc_cch_close_ind(p_data);
            break;

        case BTA_HL_DCH_CLOSE_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_CLOSE_IND_EVT");
            BTIF_TRACE_DEBUG2("mdl_handle=%d intentional_close=%s",
                              p_data->dch_close_ind.mdl_handle,
                              (p_data->dch_close_ind.intentional?"Yes":"No") );

            btif_hl_proc_dch_close_ind(p_data);
            break;

        case BTA_HL_DCH_CLOSE_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_CLOSE_CFM_EVT");
            BTIF_TRACE_DEBUG2("mdl_handle=%d status=%d ",
                              p_data->dch_close_cfm.mdl_handle,
                              p_data->dch_close_cfm.status);

            if (p_data->dch_close_cfm.status == BTA_HL_STATUS_OK)
            {
                btif_hl_proc_dch_close_cfm(p_data);
            }
            break;

        case BTA_HL_DCH_ECHO_TEST_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_ECHO_TEST_CFM_EVT");
            BTIF_TRACE_DEBUG2("mcl_handle=%d    status=%d",
                              p_data->echo_test_cfm.mcl_handle,
                              p_data->echo_test_cfm.status );
            /* not supported */
            break;


        case BTA_HL_DCH_RECONNECT_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_RECONNECT_IND_EVT");

            BTIF_TRACE_DEBUG2("mcl_handle=%d mdl_handle=5d",
                              p_data->dch_reconnect_ind.mcl_handle,
                              p_data->dch_reconnect_ind.mdl_handle );
            BTIF_TRACE_DEBUG4("first_reliable =%d dch_mode=%d mdl_id=%d mtu=%d",
                              p_data->dch_reconnect_ind.first_reliable,
                              p_data->dch_reconnect_ind.dch_mode,
                              p_data->dch_reconnect_ind.mdl_id,
                              p_data->dch_reconnect_ind.mtu);

            btif_hl_proc_dch_reconnect_ind(p_data);
            break;

        case BTA_HL_CONG_CHG_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_CONG_CHG_IND_EVT");
            BTIF_TRACE_DEBUG2("mdl_handle=%d cong =%d",
                              p_data->dch_cong_ind.mdl_handle,
                              p_data->dch_cong_ind.cong);
            btif_hl_proc_dch_cong_ind(p_data);
            break;

        case BTA_HL_DCH_ABORT_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_ABORT_IND_EVT");
            BTIF_TRACE_DEBUG1("mcl_handle=%d",
                              p_data->dch_abort_ind.mcl_handle );
            btif_hl_proc_abort_ind(p_data->dch_abort_ind.mcl_handle);
            break;
        case BTA_HL_DCH_ABORT_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_ABORT_CFM_EVT");
            BTIF_TRACE_DEBUG2("mcl_handle=%d status =%d",
                              p_data->dch_abort_cfm.mcl_handle,
                              p_data->dch_abort_cfm.status);
            if (p_data->dch_abort_cfm.status == BTA_HL_STATUS_OK)
            {
                btif_hl_proc_abort_cfm(p_data->dch_abort_ind.mcl_handle);
            }
            break;

        case BTA_HL_DCH_SEND_DATA_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_SEND_DATA_CFM_EVT");
            BTIF_TRACE_DEBUG2("mdl_handle=0x%x status =%d",
                              p_data->dch_send_data_cfm.mdl_handle,
                              p_data->dch_send_data_cfm.status);
            btif_hl_proc_send_data_cfm(p_data->dch_send_data_cfm.mdl_handle,
                                       p_data->dch_send_data_cfm.status);
            break;

        case BTA_HL_DCH_RCV_DATA_IND_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_DCH_RCV_DATA_IND_EVT");
            BTIF_TRACE_DEBUG1("mdl_handle=0x%x ",
                              p_data->dch_rcv_data_ind.mdl_handle);
            /* do nothing here */
            break;

        default:
            BTIF_TRACE_DEBUG1("Unknown Event (0x%02x)...", event);
            break;
    }


}

/*******************************************************************************
**
** Function         btif_hl_cback
**
** Description      Callback function for HL events
**
** Returns          void
**
*******************************************************************************/
static void btif_hl_cback(tBTA_HL_EVT event, tBTA_HL *p_data){
    bt_status_t status;
    int param_len = 0;

    switch (event)
    {
        case BTA_HL_REGISTER_CFM_EVT:
            param_len = sizeof(tBTA_HL_REGISTER_CFM);
            break;
        case BTA_HL_SDP_INFO_IND_EVT:
            param_len = sizeof(tBTA_HL_SDP_INFO_IND);
            break;
        case BTA_HL_DEREGISTER_CFM_EVT:
            param_len = sizeof(tBTA_HL_DEREGISTER_CFM);
            break;
        case BTA_HL_SDP_QUERY_CFM_EVT:
            param_len = sizeof(tBTA_HL_SDP_QUERY_CFM);
            break;
        case BTA_HL_CCH_OPEN_CFM_EVT:
            param_len = sizeof(tBTA_HL_CCH_OPEN_CFM);
            break;
        case BTA_HL_DCH_OPEN_CFM_EVT:
            param_len = sizeof(tBTA_HL_DCH_OPEN_CFM);
            break;
        case BTA_HL_CCH_OPEN_IND_EVT:
            param_len = sizeof(tBTA_HL_CCH_OPEN_IND);
            break;
        case BTA_HL_DCH_CREATE_IND_EVT:
            param_len = sizeof(tBTA_HL_DCH_CREATE_IND);
            break;
        case BTA_HL_DCH_OPEN_IND_EVT:
            param_len = sizeof(tBTA_HL_DCH_OPEN_IND);
            break;
        case BTA_HL_DELETE_MDL_IND_EVT:
            param_len = sizeof(tBTA_HL_MDL_IND);
            break;
        case BTA_HL_DELETE_MDL_CFM_EVT:
            param_len = sizeof(tBTA_HL_MDL_CFM);
            break;
        case BTA_HL_DCH_RECONNECT_CFM_EVT:
            param_len = sizeof(tBTA_HL_DCH_OPEN_CFM);
            break;
        case BTA_HL_CCH_CLOSE_CFM_EVT:
            param_len = sizeof(tBTA_HL_MCL_CFM);
            break;
        case BTA_HL_CCH_CLOSE_IND_EVT:
            param_len = sizeof(tBTA_HL_CCH_CLOSE_IND);
            break;
        case BTA_HL_DCH_CLOSE_IND_EVT:
            param_len = sizeof(tBTA_HL_DCH_CLOSE_IND);
            break;
        case BTA_HL_DCH_CLOSE_CFM_EVT:
            param_len = sizeof(tBTA_HL_MDL_CFM);
            break;
        case BTA_HL_DCH_ECHO_TEST_CFM_EVT:
            param_len = sizeof(tBTA_HL_MCL_CFM);
            break;
        case BTA_HL_DCH_RECONNECT_IND_EVT:
            param_len = sizeof(tBTA_HL_DCH_OPEN_IND);
            break;
        case BTA_HL_CONG_CHG_IND_EVT:
            param_len = sizeof(tBTA_HL_DCH_CONG_IND);
            break;
        case BTA_HL_DCH_ABORT_IND_EVT:
            param_len = sizeof(tBTA_HL_MCL_IND);
            break;
        case BTA_HL_DCH_ABORT_CFM_EVT:
            param_len = sizeof(tBTA_HL_MCL_CFM);
            break;
        case BTA_HL_DCH_SEND_DATA_CFM_EVT:
            param_len = sizeof(tBTA_HL_MDL_CFM);
            break;
        case BTA_HL_DCH_RCV_DATA_IND_EVT:
            param_len = sizeof(tBTA_HL_MDL_IND);
            break;
        default:
            param_len = sizeof(tBTA_HL_MDL_IND);
            break;
    }
    /* TODO: BTA sends the union members and not tBTA_HL. If using param_len=sizeof(tBTA_HL), we get a crash on memcpy */
    /* switch context to btif task context (copy full union size for convenience) */
    status = btif_transfer_context(btif_hl_upstreams_evt, (uint16_t)event, (void*)p_data, param_len, NULL);

    /* catch any failed context transfers */
    ASSERTC(status == BT_STATUS_SUCCESS, "context transfer failed", status);


}

/*******************************************************************************
**
** Function         btif_hl_ctrl_cback
**
** Description      Callback function for HL control events
**
** Returns          void
**
*******************************************************************************/
static void btif_hl_ctrl_cback(tBTA_HL_CTRL_EVT event, tBTA_HL_CTRL *p_data){
    UINT8               i;
    tBTA_HL_REG_PARAM   reg_param;
    btif_hl_app_cb_t    *p_acb;

    BTIF_TRACE_DEBUG2("%s event %d", __FUNCTION__, event);
    switch ( event )
    {
        case BTA_HL_CTRL_ENABLE_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_CTRL_ENABLE_CFM_EVT");
            BTIF_TRACE_DEBUG1("status=%d", p_data->enable_cfm.status);

            if (p_data->enable_cfm.status == BTA_HL_STATUS_OK)
            {
                btif_hl_set_state(BTIF_HL_STATE_ENABLED);

                for (i=0; i < BTA_HL_NUM_APPS ; i ++)
                {
                    if (btif_hl_cb.pcb[i].in_use)
                    {
                        p_acb = BTIF_HL_GET_APP_CB_PTR(btif_hl_cb.pcb[i].app_idx);
                        btif_hl_free_pending_reg_idx(i);
                        reg_param.dev_type = p_acb->dev_type;
                        reg_param.sec_mask = BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT;
                        reg_param.p_srv_name = p_acb->srv_name;
                        reg_param.p_srv_desp = p_acb->srv_desp;
                        reg_param.p_provider_name = p_acb->provider_name;

                        BTIF_TRACE_DEBUG1("Register pending app_id=%d", p_acb->app_id);

                        BTA_HlRegister(p_acb->app_id, &reg_param, btif_hl_cback);
                    }
                }
            }

            break;
        case BTA_HL_CTRL_DISABLE_CFM_EVT:
            BTIF_TRACE_DEBUG0("Rcv BTA_HL_CTRL_DISABLE_CFM_EVT");
            BTIF_TRACE_DEBUG1("status=%d",
                              p_data->disable_cfm.status);

            if (p_data->disable_cfm.status == BTA_HL_STATUS_OK)
            {
                memset(p_btif_hl_cb, 0, sizeof(btif_hl_cb_t));
                btif_hl_set_state(BTIF_HL_STATE_DISABLED);
            }

            break;
        default:
            break;
    }


}



/*******************************************************************************
**
** Function         connect_channel
**
** Description     connect a data channel
**
** Returns         bt_status_t
**
*******************************************************************************/
static bt_status_t connect_channel(int app_id, bt_bdaddr_t *bd_addr, int mdep_cfg_index, int *channel_id){
    UINT8                   app_idx, mcl_idx;
    btif_hl_app_cb_t        *p_acb = NULL;
    btif_hl_mcl_cb_t        *p_mcb=NULL;
    BOOLEAN                 status = FALSE;
    tBTA_HL_DCH_OPEN_PARAM  dch_open;
    BD_ADDR                 bda;
    UINT8 i;

    CHECK_BTHL_INIT();
    BTIF_TRACE_EVENT1("%s", __FUNCTION__);


    for (i=0; i<6; i++)
    {
        bda[i] = (UINT8) bd_addr->address[i];
    }
    if (btif_hl_find_app_idx(((UINT8)app_id), &app_idx))
    {
        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
        if (btif_hl_find_mcl_idx(app_idx, bda , &mcl_idx))
        {
            p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);

            if (p_mcb->is_connected)
            {
                dch_open.ctrl_psm = p_mcb->ctrl_psm;
                dch_open.local_mdep_id = p_acb->sup_feature.mdep[mdep_cfg_index].mdep_id;
                if (btif_hl_find_peer_mdep_id(p_acb->app_id, p_mcb->bd_addr,
                                              p_acb->sup_feature.mdep[mdep_cfg_index].mdep_cfg.mdep_role,
                                              p_acb->sup_feature.mdep[mdep_cfg_index].mdep_cfg.data_cfg[0].data_type, &dch_open.peer_mdep_id ))
                {
                    dch_open.local_cfg = p_acb->channel_type[mdep_cfg_index];
                    if ((p_acb->sup_feature.mdep[mdep_cfg_index].mdep_cfg.mdep_role == BTA_HL_MDEP_ROLE_SOURCE)
                        && !btif_hl_is_the_first_reliable_existed(app_idx,mcl_idx))
                    {
                        dch_open.local_cfg = BTA_HL_DCH_CFG_RELIABLE;
                    }
                    dch_open.sec_mask = (BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT);

                    status = btif_hl_dch_open(p_acb->app_id, bda, &dch_open,
                                              mdep_cfg_index, BTIF_HL_PEND_DCH_OP_OPEN, channel_id );
                }
                else
                {
                    status = BT_STATUS_FAIL;
                }
            }
            else
            {
                status = BT_STATUS_FAIL;
            }
        }
        else
        {
            p_acb->filter.num_elems =1;
            p_acb->filter.elem[0].data_type = p_acb->sup_feature.mdep[mdep_cfg_index].mdep_cfg.data_cfg[mdep_cfg_index].data_type;
            if (p_acb->sup_feature.mdep[mdep_cfg_index].mdep_cfg.mdep_role == BTA_HL_MDEP_ROLE_SINK)
                p_acb->filter.elem[0].peer_mdep_role = BTA_HL_MDEP_ROLE_SOURCE;
            else
                p_acb->filter.elem[0].peer_mdep_role = BTA_HL_MDEP_ROLE_SINK;

            if ( !btif_hl_cch_open(p_acb->app_id, bda, 0, mdep_cfg_index,
                                   BTIF_HL_PEND_DCH_OP_OPEN,
                                   channel_id))
            {
                status = BT_STATUS_FAIL;
            }
        }
    }
    else
    {
        status = BT_STATUS_FAIL;
    }

    BTIF_TRACE_DEBUG3("%s status=%d channel_id=0x%08x", __FUNCTION__, status, *channel_id);

    return status;
}
/*******************************************************************************
**
** Function         destroy_channel
**
** Description      destroy a data channel
**
** Returns         bt_status_t
**
*******************************************************************************/
static bt_status_t destroy_channel(int channel_id){
    UINT8 app_idx, mcl_idx, mdl_idx, mdl_cfg_idx, app_id, mdep_cfg_idx;
    bt_status_t status = BT_STATUS_SUCCESS;
    btif_hl_mdl_cfg_t     *p_mdl;
    btif_hl_mcl_cb_t     *p_mcb;
    btif_hl_app_cb_t     *p_acb;

    CHECK_BTHL_INIT();
    BTIF_TRACE_EVENT2("%s channel_id=0x%08x", __FUNCTION__, channel_id);

    if (btif_hl_find_mdl_cfg_idx_using_channel_id(channel_id, &app_idx, &mdl_cfg_idx))
    {
        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
        if (!p_acb->delete_mdl.active)
        {
            p_mdl =BTIF_HL_GET_MDL_CFG_PTR(app_idx, mdl_cfg_idx);
            p_acb->delete_mdl.active = TRUE;
            p_acb->delete_mdl.mdl_id = p_mdl->base.mdl_id;
            p_acb->delete_mdl.channel_id = channel_id;
            p_acb->delete_mdl.mdep_cfg_idx = p_mdl->extra.mdep_cfg_idx;
            memcpy(p_acb->delete_mdl.bd_addr, p_mdl->base.peer_bd_addr,sizeof(BD_ADDR));

            if (btif_hl_find_mcl_idx(app_idx, p_mdl->base.peer_bd_addr, &mcl_idx))
            {
                p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
                if (p_mcb->is_connected)
                {
                    BTIF_TRACE_DEBUG1("calling BTA_HlDeleteMdl mdl_id=%d",p_acb->delete_mdl.mdl_id );
                    BTA_HlDeleteMdl(p_mcb->mcl_handle, p_acb->delete_mdl.mdl_id);
                }
                else
                {
                    status = BT_STATUS_FAIL;
                }
            }
            else
            {
                BTIF_TRACE_DEBUG0("btif_hl_delete_mdl calling btif_hl_cch_open"  );
                mdep_cfg_idx = p_mdl->extra.mdep_cfg_idx;
                p_acb->filter.num_elems =1;
                p_acb->filter.elem[0].data_type = p_acb->sup_feature.mdep[mdep_cfg_idx].mdep_cfg.data_cfg[mdep_cfg_idx].data_type;
                if (p_acb->sup_feature.mdep[mdep_cfg_idx].mdep_cfg.mdep_role == BTA_HL_MDEP_ROLE_SINK)
                    p_acb->filter.elem[0].peer_mdep_role = BTA_HL_MDEP_ROLE_SOURCE;
                else
                    p_acb->filter.elem[0].peer_mdep_role = BTA_HL_MDEP_ROLE_SINK;
                if (btif_hl_cch_open(p_acb->app_id, p_acb->delete_mdl.bd_addr, 0,
                                     mdep_cfg_idx,
                                     BTIF_HL_PEND_DCH_OP_DELETE_MDL, NULL))
                {
                    status = BT_STATUS_FAIL;
                }
            }

            if (  status == BT_STATUS_FAIL)
            {
                /* fail for now  */
                btif_hl_clean_delete_mdl(&p_acb->delete_mdl);
            }
        }
        else
        {
            status = BT_STATUS_BUSY;
        }
    }
    else
    {
        /* todo fail for now later need to consider channel setup abort */
        status = BT_STATUS_FAIL;
    }

    return status;
}
/*******************************************************************************
**
** Function         unregister_application
**
** Description     unregister an HDP application
**
** Returns         bt_status_t
**
*******************************************************************************/
static bt_status_t unregister_application(int app_id){
    btif_hl_app_cb_t            *p_acb;
    UINT8                       app_idx;
    bt_status_t                 status = BT_STATUS_SUCCESS;

    CHECK_BTHL_INIT();
    BTIF_TRACE_EVENT2("%s app_id=%d", __FUNCTION__, app_id);

    if (btif_hl_find_app_idx(((UINT8)app_id), &app_idx))
    {
        p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
        BTA_HlDeregister(p_acb->app_handle);
    }
    else
    {
        status  = BT_STATUS_FAIL;
    }

    BTIF_TRACE_DEBUG1("de-reg return status=%d", status);
    return status;


}
/*******************************************************************************
**
** Function         register_application
**
** Description     register an HDP application
**
** Returns         bt_status_t
**
*******************************************************************************/
static bt_status_t register_application(bthl_reg_param_t *p_reg_param, int *app_id){
    btif_hl_app_cb_t            *p_acb;
    tBTA_HL_SUP_FEATURE         *p_sup;
    tBTA_HL_MDEP_CFG            *p_cfg;
    tBTA_HL_MDEP_DATA_TYPE_CFG  *p_data;
    tBTA_HL_REG_PARAM           reg_param;
    UINT8                       app_idx=0, i=0, pending_reg_idx=0;
    bthl_mdep_cfg_t             *p_mdep_cfg;
    bt_status_t                 status = BT_STATUS_SUCCESS;

    CHECK_BTHL_INIT();
    BTIF_TRACE_EVENT1("%s", __FUNCTION__);

    if (btif_hl_get_state() == BTIF_HL_STATE_DISABLED)
    {
        btif_hl_init();
        btif_hl_set_state(BTIF_HL_STATE_ENABLING);
        BTA_HlEnable(btif_hl_ctrl_cback);
    }

    if (!btif_hl_find_avail_app_idx(&app_idx))
    {
        BTIF_TRACE_ERROR0("Unable to allocate a new application control block");
        return BT_STATUS_FAIL;
    }

    p_acb = BTIF_HL_GET_APP_CB_PTR(app_idx);
    p_acb->in_use = TRUE;

    /* todo
       check whether this appication exists in the NV or not if it exists
       then use its own app_id
    */
    p_acb->app_id = btif_hl_get_next_app_id();

    if (p_reg_param->application_name != NULL )
        strncpy(p_acb->application_name, p_reg_param->application_name, BTIF_HL_APPLICATION_NAME_LEN);

    if (p_reg_param->provider_name != NULL )
        strncpy(p_acb->provider_name, p_reg_param->provider_name, BTA_PROVIDER_NAME_LEN);

    if (p_reg_param->srv_name != NULL )
        strncpy(p_acb->srv_name, p_reg_param->srv_name, BTA_SERVICE_NAME_LEN);

    if (p_reg_param->srv_desp != NULL )
        strncpy(p_acb->srv_desp, p_reg_param->srv_desp, BTA_SERVICE_DESP_LEN);

    p_sup = &p_acb->sup_feature;
    p_sup->advertize_source_sdp = TRUE;
    p_sup->echo_cfg.max_rx_apdu_size = 0;
    p_sup->echo_cfg.max_tx_apdu_size = 0;
    p_sup->num_of_mdeps = p_reg_param->number_of_mdeps;

    for (i=0, p_mdep_cfg = p_reg_param->mdep_cfg ; i<  p_sup->num_of_mdeps; i++, p_mdep_cfg++  )
    {
        p_cfg = &p_sup->mdep[i].mdep_cfg;
        p_cfg->num_of_mdep_data_types = 1;
        p_data  = &p_cfg->data_cfg[0];

        if ( !btif_hl_get_bta_mdep_role(p_mdep_cfg->mdep_role, &(p_cfg->mdep_role)))
        {
            BTIF_TRACE_ERROR1("Invalid mdep_role=%d", p_mdep_cfg->mdep_role);
            status = BT_STATUS_FAIL;

            break;
        }
        else
        {
            if (p_cfg->mdep_role == BTA_HL_MDEP_ROLE_SINK )
                p_sup->app_role_mask |= BTA_HL_MDEP_ROLE_MASK_SINK;
            else
                p_sup->app_role_mask |=  BTA_HL_MDEP_ROLE_MASK_SOURCE;

            if ( (p_sup->app_role_mask & BTA_HL_MDEP_ROLE_MASK_SINK) &&
                 (p_sup->app_role_mask & BTA_HL_MDEP_ROLE_MASK_SINK) )
            {
                p_acb->dev_type = BTA_HL_DEVICE_TYPE_DUAL;
            }
            else if ( p_sup->app_role_mask & BTA_HL_MDEP_ROLE_MASK_SINK )
                p_acb->dev_type = BTA_HL_DEVICE_TYPE_SINK;
            else

                p_acb->dev_type = BTA_HL_DEVICE_TYPE_SOURCE;

            p_data->data_type = (UINT16) p_mdep_cfg->data_type;
            p_data->max_rx_apdu_size = btif_hl_get_max_rx_apdu_size(p_cfg->mdep_role, p_data->data_type);
            p_data->max_tx_apdu_size = btif_hl_get_max_tx_apdu_size(p_cfg->mdep_role, p_data->data_type);

            if (p_mdep_cfg->mdep_description != NULL )
                strncpy(p_data->desp, p_mdep_cfg->mdep_description, BTA_SERVICE_DESP_LEN);

            if ( !btif_hl_get_bta_channel_type(p_mdep_cfg->channel_type, &(p_acb->channel_type[i])))
            {
                BTIF_TRACE_ERROR1("Invalid channel_type=%d", p_mdep_cfg->channel_type);
                status = BT_STATUS_FAIL;
                break;
            }
        }
    }

    if (status == BT_STATUS_SUCCESS)
    {
        *app_id = (int) p_acb->app_id;
        if (btif_hl_get_state() == BTIF_HL_STATE_ENABLED )
        {
            reg_param.dev_type = p_acb->dev_type;
            reg_param.sec_mask = BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT;
            reg_param.p_srv_name = p_acb->srv_name;
            reg_param.p_srv_desp = p_acb->srv_desp;
            reg_param.p_provider_name = p_acb->provider_name;
            BTA_HlRegister(p_acb->app_id, &reg_param, btif_hl_cback);
        }
        else
        {
            if ((btif_hl_get_state() == BTIF_HL_STATE_ENABLING) && btif_hl_find_avail_pending_reg_idx(&pending_reg_idx))
            {
                BTIF_TRACE_DEBUG1("registration is delayed until HL is enabled app_id=%d",  *app_id);
                btif_hl_cb.pcb[i].in_use = TRUE;
                btif_hl_cb.pcb[i].app_idx = app_idx;
            }
            else
            {
                BTIF_TRACE_ERROR0("Unable to enqueue this reg request");
                btif_hl_free_app_idx(app_idx);
                status = BT_STATUS_FAIL;
            }
        }
    }
    else
    {
        btif_hl_free_app_idx(app_idx);
    }

    BTIF_TRACE_DEBUG2("register_application status=%d app_id=%d", status, *app_id);
    return status;
}
/*******************************************************************************
**
** Function         init
**
** Description     initializes the hl interface
**
** Returns         bt_status_t
**
*******************************************************************************/
static bt_status_t init( bthl_callbacks_t* callbacks ){
    bt_status_t status = BT_STATUS_SUCCESS;
    BTIF_TRACE_EVENT1("%s", __FUNCTION__);

    bt_hl_callbacks_cb = *callbacks;
    bt_hl_callbacks = &bt_hl_callbacks_cb;
    btif_hl_soc_thread_init();

    return status;
}
/*******************************************************************************
**
** Function         cleanup
**
** Description      Closes the HL interface
**
** Returns          void
**
*******************************************************************************/
static void  cleanup( void ){
    BTIF_TRACE_EVENT1("%s", __FUNCTION__);
    if (bt_hl_callbacks)
    {
        btif_disable_service(BTA_HDP_SERVICE_ID);
        bt_hl_callbacks = NULL;
    }

    btif_hl_disable();
}

static const bthl_interface_t bthlInterface = {
    sizeof(bthl_interface_t),
    init,
    register_application,
    unregister_application,
    connect_channel,
    destroy_channel,
    cleanup,
};


/*******************************************************************************
**
** Function         btif_hl_get_interface
**
** Description      Get the hl callback interface
**
** Returns          bthf_interface_t
**
*******************************************************************************/
const bthl_interface_t *btif_hl_get_interface(){
    BTIF_TRACE_EVENT1("%s", __FUNCTION__);
    return &bthlInterface;
}

int btif_hl_update_maxfd( int max_org_s){
    btif_hl_soc_cb_t      *p_scb = NULL;
    int maxfd=0;

    BTIF_TRACE_DEBUG1("btif_hl_update_maxfd max_org_s= %d", max_org_s);

    maxfd = max_org_s;
    if (!GKI_queue_is_empty(&soc_queue))
    {
        p_scb = (btif_hl_soc_cb_t *)GKI_getfirst((void *)&soc_queue);
        if (maxfd < p_scb->max_s)
        {
            maxfd = p_scb->max_s;
            BTIF_TRACE_DEBUG1("btif_hl_update_maxfd 1 maxfd=%d", maxfd);
        }
        while (p_scb != NULL)
        {
            if (maxfd < p_scb->max_s)
            {
                maxfd = p_scb->max_s;
                BTIF_TRACE_DEBUG1("btif_hl_update_maxfd 2 maxfd=%d", maxfd);
            }
            p_scb = (btif_hl_soc_cb_t *)GKI_getnext((void *)p_scb );
        }


    }

    BTIF_TRACE_DEBUG1("btif_hl_update_maxfd final *p_max_s=%d", maxfd);
    return maxfd;
}

btif_hl_soc_state_t btif_hl_get_socket_state(btif_hl_soc_cb_t *p_scb){
    BTIF_TRACE_DEBUG1("btif_hl_get_socket_state state=%d", p_scb->state);
    return p_scb->state;
}

void btif_hl_set_socket_state(btif_hl_soc_cb_t *p_scb, btif_hl_soc_state_t new_state){
    BTIF_TRACE_DEBUG2("btif_hl_set_socket_state %d---->%d", p_scb->state, new_state);
    p_scb->state = new_state;
}

void btif_hl_release_mcl_sockets(UINT8 app_idx, UINT8 mcl_idx){
    btif_hl_soc_cb_t    *p_scb = NULL;
    UINT8               i;
    btif_hl_mdl_cb_t    *p_dcb;
    BOOLEAN             found= FALSE;
    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);
    for (i=0; i < BTA_HL_NUM_MDLS_PER_MCL ; i ++)
    {
        p_dcb = BTIF_HL_GET_MDL_CB_PTR(app_idx, mcl_idx, i);
        if (p_dcb && p_dcb->in_use && p_dcb->p_scb)
        {
            BTIF_TRACE_DEBUG3("found socket for app_idx=%d mcl_id=%d, mdl_idx=%d", app_idx, mcl_idx, i);
            btif_hl_set_socket_state (p_dcb->p_scb, BTIF_HL_SOC_STATE_W4_REL);
            p_dcb->p_scb = NULL;
            found = TRUE;
        }
    }
    if (found)
        btif_hl_select_close_connected();
}

void btif_hl_release_socket(UINT8 app_idx, UINT8 mcl_idx, UINT8 mdl_idx){
    btif_hl_soc_cb_t       *p_scb = NULL;
    btif_hl_mdl_cb_t      *p_dcb = BTIF_HL_GET_MDL_CB_PTR(app_idx, mcl_idx, mdl_idx);

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);
    BTIF_TRACE_DEBUG3("app_idx=%d mcl_idx=%d mdl_idx=%d",  app_idx, mcl_idx, mdl_idx  );

    if (p_dcb && p_dcb->p_scb)
    {
        p_scb = p_dcb->p_scb;
        btif_hl_set_socket_state(p_scb,  BTIF_HL_SOC_STATE_W4_REL);
        p_dcb->p_scb = NULL;
        btif_hl_select_close_connected();
    }
}

BOOLEAN btif_hl_create_socket(UINT8 app_idx, UINT8 mcl_idx, UINT8 mdl_idx){
    btif_hl_mcl_cb_t      *p_mcb = BTIF_HL_GET_MCL_CB_PTR(app_idx, mcl_idx);
    btif_hl_mdl_cb_t      *p_dcb = BTIF_HL_GET_MDL_CB_PTR(app_idx, mcl_idx, mdl_idx);
    btif_hl_soc_cb_t      *p_scb = NULL;
    UINT8                 soc_idx;
    BOOLEAN               status = FALSE;

    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);

    if (p_dcb && ((p_scb = (btif_hl_soc_cb_t *)GKI_getbuf((UINT16)sizeof(btif_hl_soc_cb_t)))!=NULL))
    {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, p_scb->socket_id) >= 0)
        {
            BTIF_TRACE_DEBUG2("socket id[0]=%d id[1]=%d",p_scb->socket_id[0], p_scb->socket_id[1] );
            p_dcb->p_scb = p_scb;
            p_scb->app_idx = app_idx;
            p_scb->mcl_idx = mcl_idx;
            p_scb->mdl_idx = mdl_idx;
            p_scb->channel_id = p_dcb->channel_id;
            p_scb->mdep_cfg_idx = p_dcb->local_mdep_cfg_idx;
            memcpy(p_scb->bd_addr, p_mcb->bd_addr,sizeof(BD_ADDR));
            btif_hl_set_socket_state(p_scb,  BTIF_HL_SOC_STATE_W4_ADD);
            p_scb->max_s = p_scb->socket_id[1];
            GKI_enqueue(&soc_queue, (void *) p_scb);
            btif_hl_select_wakeup();
            status = TRUE;
        }
        else
        {

            btif_hl_free_buf((void **)&p_scb);
        }
    }

    BTIF_TRACE_DEBUG2("%s status=%d", __FUNCTION__, status);
    return status;
}

void btif_hl_add_socket_to_set( fd_set *p_org_set)
{
    btif_hl_soc_cb_t                *p_scb = NULL;
    btif_hl_mdl_cb_t                *p_dcb = NULL;
    btif_hl_mcl_cb_t                *p_mcb = NULL;
    btif_hl_send_chan_state_cb_t    send_cb_param;
    bt_status_t                     status;
    int                             len;

    BTIF_TRACE_DEBUG1("entering %s",__FUNCTION__);

    if (!GKI_queue_is_empty(&soc_queue))
    {
        p_scb = (btif_hl_soc_cb_t *)GKI_getfirst((void *)&soc_queue);
        BTIF_TRACE_DEBUG1("btif_hl_add_socket_to_set first p_scb=0x%x", p_scb);
        while (p_scb != NULL)
        {
            if (btif_hl_get_socket_state(p_scb) == BTIF_HL_SOC_STATE_W4_ADD)
            {
                btif_hl_set_socket_state(p_scb,   BTIF_HL_SOC_STATE_W4_READ);
                FD_SET(p_scb->socket_id[1], p_org_set);
                BTIF_TRACE_DEBUG2("found and set socket_id=%d is_set=%d", p_scb->socket_id[1], FD_ISSET(p_scb->socket_id[1], p_org_set));
                p_mcb = BTIF_HL_GET_MCL_CB_PTR(p_scb->app_idx, p_scb->mcl_idx);
                p_dcb = BTIF_HL_GET_MDL_CB_PTR(p_scb->app_idx, p_scb->mcl_idx, p_scb->mdl_idx);
                if (p_mcb && p_dcb)
                {
                    send_cb_param.app_id = (int) btif_hl_get_app_id(p_dcb->channel_id);
                    memcpy(send_cb_param.bd_addr, p_mcb->bd_addr, sizeof(BD_ADDR));
                    send_cb_param.channel_id = p_dcb->channel_id;
                    send_cb_param.fd = p_scb->socket_id[0];
                    send_cb_param.mdep_cfg_index = (int ) p_dcb->local_mdep_cfg_idx;
                    send_cb_param.cb_state = BTIF_HL_CHAN_CB_STATE_CONNECTED_PENDING;
                    len = sizeof(btif_hl_send_chan_state_cb_t);
                    status = btif_transfer_context (btif_hl_send_chan_state_cb_evt, BTIF_HL_SEND_CONNECTED_CB,
                                                    (char*) &send_cb_param, len, NULL);
                    ASSERTC(status == BT_STATUS_SUCCESS, "context transfer failed", status);

                    //btif_hl_send_connected_cb (p_scb->app_idx, p_scb->mcl_idx, p_scb->mdl_idx);
                }


            }
            p_scb = (btif_hl_soc_cb_t *)GKI_getnext((void *)p_scb );
            BTIF_TRACE_DEBUG1("next p_scb=0x%x", p_scb);
        }
    }

    BTIF_TRACE_DEBUG1("leaving %s",__FUNCTION__);
}

void btif_hl_close_socket( fd_set *p_org_set){
    btif_hl_soc_cb_t                *p_scb = NULL;
    BOOLEAN                         element_removed = FALSE;
    btif_hl_mdl_cb_t                *p_dcb = NULL ;
    btif_hl_send_chan_state_cb_t    send_cb_param;
    int                             len;
    bt_status_t                     status;

    BTIF_TRACE_DEBUG1("entering %s",__FUNCTION__);
    if (!GKI_queue_is_empty(&soc_queue))
    {
        p_scb = (btif_hl_soc_cb_t *)GKI_getfirst((void *)&soc_queue);
        while (p_scb != NULL)
        {
            if (btif_hl_get_socket_state(p_scb) == BTIF_HL_SOC_STATE_W4_REL)
            {
                BTIF_TRACE_DEBUG3("app_idx=%d mcl_id=%d, mdl_idx=%d",
                                  p_scb->app_idx, p_scb->mcl_idx, p_scb->mdl_idx);
                btif_hl_set_socket_state(p_scb,   BTIF_HL_SOC_STATE_IDLE);
                if (p_scb->socket_id[1] != -1)
                {
                    FD_CLR(p_scb->socket_id[1] , p_org_set);
                    shutdown(p_scb->socket_id[1], SHUT_RDWR);
                    close(p_scb->socket_id[1]);

                    send_cb_param.app_id = (int) btif_hl_get_app_id(p_scb->channel_id);
                    memcpy(send_cb_param.bd_addr, p_scb->bd_addr, sizeof(BD_ADDR));
                    send_cb_param.channel_id = p_scb->channel_id;
                    send_cb_param.fd = p_scb->socket_id[0];
                    send_cb_param.mdep_cfg_index = (int ) p_scb->mdep_cfg_idx;
                    send_cb_param.cb_state = BTIF_HL_CHAN_CB_STATE_DISCONNECTED_PENDING;
                    len = sizeof(btif_hl_send_chan_state_cb_t);
                    status = btif_transfer_context (btif_hl_send_chan_state_cb_evt, BTIF_HL_SEND_DISCONNECTED_CB,
                                                    (char*) &send_cb_param, len, NULL);
                    ASSERTC(status == BT_STATUS_SUCCESS, "context transfer failed", status);

                    //btif_hl_send_disconnected_cb(p_scb);
                }
            }
            p_scb = (btif_hl_soc_cb_t *)GKI_getnext((void *)p_scb );
            BTIF_TRACE_DEBUG1("while loop next p_scb=0x%x", p_scb);
        }

        p_scb = (btif_hl_soc_cb_t *)GKI_getfirst((void *)&soc_queue);
        while (p_scb != NULL)
        {
            if (btif_hl_get_socket_state(p_scb) == BTIF_HL_SOC_STATE_IDLE)
            {
                p_dcb = BTIF_HL_GET_MDL_CB_PTR(p_scb->app_idx, p_scb->mcl_idx, p_scb->mdl_idx);
                BTIF_TRACE_DEBUG4("idle socket app_idx=%d mcl_id=%d, mdl_idx=%d p_dcb->in_use=%d",
                                  p_scb->app_idx, p_scb->mcl_idx, p_scb->mdl_idx, p_dcb->in_use);
                GKI_remove_from_queue((void *)&soc_queue, p_scb);
                btif_hl_free_buf((void **)&p_scb);
                p_dcb->p_scb = NULL;
                element_removed = TRUE;
            }
            BTIF_TRACE_DEBUG2("element_removed=%d p_scb=0x%x", element_removed, p_scb);
            if (element_removed)
            {
                element_removed = FALSE;
                p_scb = (btif_hl_soc_cb_t *)GKI_getfirst((void *)&soc_queue);
            }
            else
                p_scb = (btif_hl_soc_cb_t *)GKI_getnext((void *)p_scb );

            BTIF_TRACE_DEBUG1("while loop p_scb=0x%x", p_scb);
        }


    }
    BTIF_TRACE_DEBUG1("leaving %s",__FUNCTION__);
}


void btif_hl_select_wakeup_callback( fd_set *p_org_set ,  int wakeup_signal){
    BTIF_TRACE_DEBUG2("entering %s wakeup_signal=0x%04x",__FUNCTION__, wakeup_signal);

    if (wakeup_signal == btif_hl_signal_select_wakeup )
    {
        btif_hl_add_socket_to_set(p_org_set);
    }
    else if (wakeup_signal == btif_hl_signal_select_close_connected)
    {
        btif_hl_close_socket(p_org_set);
    }
    BTIF_TRACE_DEBUG1("leaving %s",__FUNCTION__);
}


void btif_hl_select_monitor_callback( fd_set *p_cur_set , fd_set *p_org_set){
    btif_hl_soc_cb_t      *p_scb = NULL;
    btif_hl_mdl_cb_t      *p_dcb = NULL;
    int r;

    BTIF_TRACE_DEBUG1("entering %s",__FUNCTION__);

    if (!GKI_queue_is_empty(&soc_queue))
    {
        p_scb = (btif_hl_soc_cb_t *)GKI_getfirst((void *)&soc_queue);
        BTIF_TRACE_DEBUG0(" GKI queue is not empty ");
        while (p_scb != NULL)
        {
            if (btif_hl_get_socket_state(p_scb) == BTIF_HL_SOC_STATE_W4_READ)
            {
                if (FD_ISSET(p_scb->socket_id[1], p_cur_set))
                {
                    BTIF_TRACE_DEBUG0("read data");
                    BTIF_TRACE_DEBUG0("state= BTIF_HL_SOC_STATE_W4_READ");
                    p_dcb = BTIF_HL_GET_MDL_CB_PTR(p_scb->app_idx, p_scb->mcl_idx, p_scb->mdl_idx);
                    if (p_dcb->p_tx_pkt)
                    {
                        BTIF_TRACE_ERROR1("Rcv new pkt but the last pkt is still not been sent tx_size=%d", p_dcb->tx_size);
                        btif_hl_free_buf((void **) &p_dcb->p_tx_pkt);
                    }
                    p_dcb->p_tx_pkt =  btif_hl_get_buf (p_dcb->mtu);
                    if (p_dcb )
                    {
                        //do
                        // {
                        //     r = recv(p_scb->socket_id[1], p_dcb->p_tx_pkt, p_dcb->mtu , MSG_DONTWAIT));
                        // } while (r == SOCKET_ERROR && errno == EINTR);

                        if ((r = (int)recv(p_scb->socket_id[1], p_dcb->p_tx_pkt, p_dcb->mtu , MSG_DONTWAIT)) > 0)
                        {
                            BTIF_TRACE_DEBUG1("btif_hl_select_monitor_callback send data r =%d", r);
                            p_dcb->tx_size = r;
                            BTIF_TRACE_DEBUG1("btif_hl_select_monitor_callback send data tx_size=%d", p_dcb->tx_size );
                            BTA_HlSendData(p_dcb->mdl_handle, p_dcb->tx_size  );
                        }

                        if (r <= 0 )
                        {
                            BTIF_TRACE_DEBUG1("btif_hl_select_monitor_callback  receive failed r=%d",r);
                            BTA_HlDchClose(p_dcb->mdl_handle );
                        }
                    }
                }
            }
            p_scb = (btif_hl_soc_cb_t *)GKI_getnext((void *)p_scb );
        }
    }
    else
    {
        BTIF_TRACE_DEBUG0("btif_hl_select_monitor_queue is empty");
    }
    BTIF_TRACE_DEBUG1("leaving %s",__FUNCTION__);
}

/* create dummy socket pair used to wake up select loop */
static inline int btif_hl_select_wakeup_init(fd_set* set){
    BTIF_TRACE_DEBUG0("btif_hl_select_wakeup_init");
    //if (signal_fds[0] == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, signal_fds) < 0)
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, signal_fds) < 0)
    {
        BTIF_TRACE_ERROR1("socketpair failed: %s", strerror(errno));
        return -1;
    }

    BTIF_TRACE_DEBUG2("btif_hl_select_wakeup_init signal_fds[0]=%d signal_fds[1]=%d",signal_fds[0], signal_fds[1] );
    FD_SET(signal_fds[0], set);

    return signal_fds[0];
}


static inline int btif_hl_select_wakeup(void){
    char sig_on = btif_hl_signal_select_wakeup;
    BTIF_TRACE_DEBUG0("btif_hl_select_wakeup");
    return send(signal_fds[1], &sig_on, sizeof(sig_on), 0);
}
static inline int btif_hl_select_exit(void){
    char sig_on = btif_hl_signal_select_exit;
    BTIF_TRACE_DEBUG0("btif_hl_select_exit");
    return send(signal_fds[1], &sig_on, sizeof(sig_on), 0);
}
static inline int btif_hl_select_close_connected(void){
    char sig_on = btif_hl_signal_select_close_connected;
    BTIF_TRACE_DEBUG0("btif_hl_select_close_connected");
    return send(signal_fds[1], &sig_on, sizeof(sig_on), 0);
}
/* clear signal by reading signal */
static inline int btif_hl_select_wake_reset(void){
    char sig_recv = 0;

    BTIF_TRACE_DEBUG0("btif_hl_select_wake_reset");
    recv(signal_fds[0], &sig_recv, sizeof(sig_recv), MSG_WAITALL);
    return(int)sig_recv;
}

static inline int btif_hl_select_wake_signaled(fd_set* set){
    BTIF_TRACE_DEBUG0("btif_hl_select_wake_signaled");
    return FD_ISSET(signal_fds[0], set);
}

static void btif_hl_thread_cleanup(){
    if (listen_s != -1)
        close(listen_s);
    if (connected_s != -1)
    {
        shutdown(connected_s, SHUT_RDWR);
        close(connected_s);
    }
    listen_s = connected_s = -1;
    select_thread_id = -1;
    BTIF_TRACE_DEBUG0("hl thread cleanup");
}

static void *btif_hl_select_thread(void *arg){
    fd_set org_set, curr_set;
    int r, max_curr_s, max_org_s;

    BTIF_TRACE_DEBUG0("entered btif_hl_select_thread");
    FD_ZERO(&org_set);
    max_org_s = btif_hl_select_wakeup_init(&org_set);
    BTIF_TRACE_DEBUG1("max_s=%d ", max_org_s);

    for (;;)
    {
        r = 0;
        BTIF_TRACE_DEBUG0("set curr_set = org_set ");
        curr_set = org_set;
        max_curr_s = max_org_s;
        int ret = select((max_curr_s + 1), &curr_set, NULL, NULL, NULL);
        BTIF_TRACE_DEBUG1("select unblocked ret=%d", ret);
        if (ret == -1)
        {
            BTIF_TRACE_DEBUG0("select() ret -1, exit the thread");
            btif_hl_thread_cleanup();
            return 0;
        }
        else if (ret)
        {
            BTIF_TRACE_DEBUG1("btif_hl_select_wake_signaled, signal ret=%d", ret);
            if (btif_hl_select_wake_signaled(&curr_set))
            {
                r = btif_hl_select_wake_reset();
                BTIF_TRACE_DEBUG1("btif_hl_select_wake_signaled, signal:%d", r);
                if (r == btif_hl_signal_select_wakeup || r == btif_hl_signal_select_close_connected )
                {
                    btif_hl_select_wakeup_callback(&org_set, r);
                }


            }

            btif_hl_select_monitor_callback(&curr_set, &org_set);
            max_org_s = btif_hl_update_maxfd(max_org_s);
        }
        else
            BTIF_TRACE_DEBUG1("no data, select ret: %d\n", ret);
    }
    BTIF_TRACE_DEBUG0("leaving hl_select_thread");
    return 0;
}


static inline pthread_t create_thread(void *(*start_routine)(void *), void * arg){
    BTIF_TRACE_DEBUG0("create_thread: entered");
    pthread_attr_t thread_attr;

    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);
    pthread_t thread_id = -1;
    if ( pthread_create(&thread_id, &thread_attr, start_routine, arg)!=0 )
    {
        BTIF_TRACE_ERROR1("pthread_create : %s", strerror(errno));
        return -1;
    }
    BTIF_TRACE_DEBUG0("create_thread: thread created successfully");
    return thread_id;
}

/*******************************************************************************
**
** Function         btif_hl_soc_thread_init
**
** Description      HL socket thread init function.
**
** Returns          void
**
*******************************************************************************/
void btif_hl_soc_thread_init(void){
    BTIF_TRACE_DEBUG1("%s", __FUNCTION__);
    GKI_init_q(&soc_queue);
    select_thread_id = create_thread(btif_hl_select_thread, NULL);
}

