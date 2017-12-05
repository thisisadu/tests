#include <assert.h>
#include <math.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include "jtbuf.h"

#define MIN(a,b) (((a) < (b)) ? (a):(b))
#define MAX(a,b) MIN(b,a)

/* Invalid sequence number, used as the initial value. */
#define INVALID_OFFSET -9999

/* Maximum burst length, whenever an operation is bursting longer than
 * this value, JB will assume that the opposite operation was idle.
 */
#define MAX_BURST_MSEC 1000

/* Number of OP switches to be performed in JB_STATUS_INITIALIZING, before
 * JB can switch its states to JB_STATUS_PROCESSING.
 */
#define INIT_CYCLE 10


/* Minimal difference between JB size and 2*burst-level to perform
 * JB shrinking in static discard algorithm.
 */
#define STA_DISC_SAFE_SHRINKING_DIFF 1


#define JBUF_DISC_MIN_GAP                200

#define JBUF_PRO_DISC_MIN_BURST          1

#define JBUF_PRO_DISC_MAX_BURST          100

#define JBUF_PRO_DISC_T1                 2000

#define JBUF_PRO_DISC_T2                 10000

/* Struct of JB internal buffer, represented in a circular buffer containing
 * frame content, frame type, frame length, and frame bit info.
 */
typedef struct jb_framelist_t
{
    /* Settings */
    unsigned     frame_size;  /**< maximum size of frame    */
    unsigned     max_count;   /**< maximum number of frames    */

    /* Buffers */
    char         *content;    /**< frame content array    */
    int          *frame_type; /**< frame type array    */
    size_t       *content_len; /**< frame length array    */
    uint32_t     *bit_info; /**< frame bit info array    */
    uint32_t     *ts;  /**< timestamp array    */

    /* States */
    unsigned     head; /**< index of head, pointed frame
                         will be returned by next GET   */
    unsigned     size; /**< current size of framelist,
                         including discarded frames.    */
    unsigned	 discarded_num;	/**< current number of discarded
                                   frames.			    */
    int          origin; /**< original index of flist_head   */

} jb_framelist_t;

struct jbuf;

typedef void (*discard_algo)(struct jbuf *jb);

typedef struct jbuf
{
    pthread_mutex_t lock;
    
    /* Settings (consts) */
    size_t    jb_frame_size;/**< frame size    */
    unsigned  jb_frame_ptime;/**< frame duration.    */
    size_t    jb_max_count;/**< capacity of jitter buffer,
                                 in frames    */
    int    jb_init_prefetch;/**< Initial prefetch    */
    int    jb_min_prefetch;/**< Minimum allowable prefetch    */
    int    jb_max_prefetch;/**< Maximum allowable prefetch    */
    int    jb_max_burst;/**< maximum possible burst, whenever
                                burst exceeds this value, it
                                     won't be included in level
                                     calculation    */
    int    jb_min_shrink_gap;/**< How often can we shrink    */
    discard_algo    jb_discard_algo;/**< Discard algorithm    */

    /* Buffer */
    jb_framelist_t  jb_framelist;/**< the buffer    */

    /* States */
    int    jb_level;/**< delay between source &
                            destination (calculated according
                                 of the number of burst get/put
                                 operations)    */
    int    jb_max_hist_level;  /**< max level during the last level
                                  calculations    */
    int    jb_stable_hist;/**< num of times the delay hasbeen
                             lower then the prefetch num    */
    int    jb_last_op;/**< last operation executed
                         (put/get)    */
    int    jb_eff_level;/**< effective burst level    */
    int    jb_prefetch;/**< no. of frame to insert before
                               removing some (at the beginning
                                    of the framelist->content
                                         operation), the value may be
                                              continuously updated based on
                                              current frame burst level.    */
    int    jb_prefetching;/**< flag if jbuf_t is prefetching.   */
    int    jb_status;/**< status is 'init' until thefirst
                        'put' operation    */
    int    jb_init_cycle_cnt;/**< status is 'init' until thefirst
                                'put' operation    */

    int    jb_discard_ref;/**< Seq # of last frame deleted or
                             discarded    */
    unsigned    jb_discard_dist;/**< Distance from jb_discard_ref
                                   to perform discard (in frm)    */

} jbuf_t;

static void jbuf_discard_static(jbuf_t *jb);
static void jbuf_discard_progressive(jbuf_t *jb);


#define JB_STATUS_INITIALIZING 0
#define JB_STATUS_PROCESSING 1


/* Progressive discard algorithm introduced to reduce JB latency
 * by discarding incoming frames with adaptive aggressiveness based on
 * actual burst level.
 */
#define PROGRESSIVE_DISCARD 1

/* Internal JB frame flag, discarded frame will not be returned by JB to
 * application, it's just simply discarded.
 */
#define JB_DISCARDED_FRAME 1024


static int jb_framelist_reset(jb_framelist_t *framelist);
static unsigned jb_framelist_remove_head(jb_framelist_t *framelist,
                                         unsigned count);

static int jb_framelist_init(jb_framelist_t *framelist,
                             unsigned frame_size,
                             unsigned max_count)
{
    bzero(framelist, sizeof(jb_framelist_t));

    framelist->frame_size   = frame_size;
    framelist->max_count    = max_count;
    framelist->content    = (char*)malloc(framelist->frame_size*
                                          framelist->max_count);
    framelist->frame_type   = (int*)malloc(sizeof(framelist->frame_type[0])*
                                            framelist->max_count);
    framelist->content_len  = (size_t*)malloc(sizeof(framelist->content_len[0])*
                                            framelist->max_count);
    framelist->bit_info    = (uint32_t*)malloc(sizeof(framelist->bit_info[0])*
                                           framelist->max_count);
    framelist->ts    = (uint32_t*)malloc(sizeof(framelist->ts[0])*
                                     framelist->max_count);

    return jb_framelist_reset(framelist);

}

static int jb_framelist_destroy(jb_framelist_t *framelist)
{
    return 0;
}

static int jb_framelist_reset(jb_framelist_t *framelist)
{
    framelist->head = 0;
    framelist->origin = INVALID_OFFSET;
    framelist->size = 0;
    framelist->discarded_num = 0;

    memset(framelist->frame_type, JB_MISSING_FRAME, sizeof(framelist->frame_type[0]) * framelist->max_count);

    bzero(framelist->content_len, sizeof(framelist->content_len[0]) * framelist->max_count);

    return 0;
}


static unsigned jb_framelist_size(const jb_framelist_t *framelist)
{
    return framelist->size;
}


static unsigned jb_framelist_eff_size(const jb_framelist_t *framelist)
{
    return (framelist->size - framelist->discarded_num);
}

static int jb_framelist_origin(const jb_framelist_t *framelist)
{
    return framelist->origin;
}


static int jb_framelist_get(jb_framelist_t *framelist,
                            void *frame, size_t *size,
                            jb_frame_type_t *p_type,
                            uint32_t *bit_info,
                            uint32_t *ts,
                            int *seq)
{
    if (framelist->size) {
        int prev_discarded = 0;

        /* Skip discarded frames */
        while (framelist->frame_type[framelist->head] ==
               JB_DISCARDED_FRAME)
        {
            jb_framelist_remove_head(framelist, 1);
            prev_discarded = 1;
            // printf("discarded\n");
        }

        /* Return the head frame if any */
        if (framelist->size) {
            if (prev_discarded) {
                /*  when previous frame(s) was discarded, return
                 * 'missing' frame to trigger PLC to get smoother signal.
                 */
                *p_type = JB_MISSING_FRAME;
                if (size)
                    *size = 0;
                if (bit_info)
                    *bit_info = 0;
            } else {
                memcpy(frame,
                       framelist->content + framelist->head * framelist->frame_size,
                       framelist->frame_size);
                *p_type = (jb_frame_type_t)framelist->frame_type[framelist->head];
                if (size)
                    *size   = framelist->content_len[framelist->head];
                if (bit_info)
                    *bit_info = framelist->bit_info[framelist->head];
            }
            if (ts)
                *ts = framelist->ts[framelist->head];
            if (seq)
                *seq = framelist->origin;

            //bzero(framelist->content +
            // framelist->head * framelist->frame_size,
            // framelist->frame_size);
            framelist->frame_type[framelist->head] = JB_MISSING_FRAME;
            framelist->content_len[framelist->head] = 0;
            framelist->bit_info[framelist->head] = 0;
            framelist->ts[framelist->head] = 0;

            framelist->origin++;
            framelist->head = (framelist->head + 1) % framelist->max_count;
            framelist->size--;

            return 1;
        }
    }

    /* No frame available */
    bzero(frame, framelist->frame_size);

    return 0;
}


static int jb_framelist_peek(jb_framelist_t *framelist,
                             unsigned offset,
                             const void **frame,
                             size_t *size,
                             jb_frame_type_t *type,
                             uint32_t *bit_info,
                             uint32_t *ts,
                             int *seq)
{
    unsigned pos, idx;

    if (offset >= jb_framelist_eff_size(framelist))
        return 0;

    pos = framelist->head;
    idx = offset;

    /* Find actual peek position, note there may be discarded frames */
    while (1) {
        if (framelist->frame_type[pos] != JB_DISCARDED_FRAME) {
            if (idx == 0)
                break;
            else
                --idx;
        }
        pos = (pos + 1) % framelist->max_count;
    }

    /* Return the frame pointer */
    if (frame)
        *frame = framelist->content + pos*framelist->frame_size;
    if (type)
        *type = (jb_frame_type_t)framelist->frame_type[pos];
    if (size)
        *size = framelist->content_len[pos];
    if (bit_info)
        *bit_info = framelist->bit_info[pos];
    if (ts)
        *ts = framelist->ts[pos];
    if (seq)
        *seq = framelist->origin + offset;

    return 1;
}


/* Remove oldest frames as many as param 'count' */
static unsigned jb_framelist_remove_head(jb_framelist_t *framelist,
                                         unsigned count)
{
    if (count > framelist->size)
        count = framelist->size;

    if (count) {
        /* may be done in two steps if overlapping */
        unsigned step1,step2;
        unsigned tmp = framelist->head+count;
        unsigned i;

        if (tmp > framelist->max_count) {
            step1 = framelist->max_count - framelist->head;
            step2 = count-step1;
        } else {
            step1 = count;
            step2 = 0;
        }

        for (i = framelist->head; i < (framelist->head + step1); ++i) {
            if (framelist->frame_type[i] == JB_DISCARDED_FRAME) {
                assert(framelist->discarded_num > 0);
                framelist->discarded_num--;
            }
        }

        //bzero(framelist->content +
        //    framelist->head * framelist->frame_size,
        //          step1*framelist->frame_size);
        memset(framelist->frame_type+framelist->head,
               JB_MISSING_FRAME,
               step1*sizeof(framelist->frame_type[0]));
        bzero(framelist->content_len+framelist->head,
              step1*sizeof(framelist->content_len[0]));

        if (step2) {
            for (i = 0; i < step2; ++i) {
                if (framelist->frame_type[i] == JB_DISCARDED_FRAME) {
                    assert(framelist->discarded_num > 0);
                    framelist->discarded_num--;
                }
            }
            //bzero( framelist->content,
            //      step2*framelist->frame_size);
            memset(framelist->frame_type,
                      JB_MISSING_FRAME,
                      step2*sizeof(framelist->frame_type[0]));
            bzero (framelist->content_len,
                      step2*sizeof(framelist->content_len[0]));
        }

        /* update states */
        framelist->origin += count;
        framelist->head = (framelist->head + count) % framelist->max_count;
        framelist->size -= count;
    }

    return count;
}


static int jb_framelist_put_at(jb_framelist_t *framelist,
                               int index,
                               const void *frame,
                               unsigned frame_size,
                               uint32_t bit_info,
                               uint32_t ts,
                               unsigned frame_type)
{
    int distance;
    unsigned pos;
    enum { MAX_MISORDER = 100 };
    enum { MAX_DROPOUT = 3000 };

    if (frame_size > framelist->frame_size)
        return -1;

    /* too late or sequence restart */
    if (index < framelist->origin) {
        if (framelist->origin - index < MAX_MISORDER) {
            /* too late */
            return -1;
        } else {
            /* sequence restart */
            framelist->origin = index - framelist->size;
        }
    }

    /* if jbuf_t is empty, just reset the origin */
    if (framelist->size == 0) {
        assert(framelist->discarded_num == 0);
        framelist->origin = index;
    }

    /* get distance of this frame to the first frame in the buffer */
    distance = index - framelist->origin;

    /* far jump, the distance is greater than buffer capacity */
    if (distance >= (int)framelist->max_count) {
        if (distance > MAX_DROPOUT) {
            /* jump too far, reset the buffer */
            jb_framelist_reset(framelist);
            framelist->origin = index;
            distance = 0;
        } else {
            /* otherwise, reject the frame */
            return -2;
        }
    }

    /* get the slot position */
    pos = (framelist->head + distance) % framelist->max_count;

    /* if the slot is occupied, it must be duplicated frame, ignore it. */
    if (framelist->frame_type[pos] != JB_MISSING_FRAME)
        return -1;

    /* put the frame into the slot */
    framelist->frame_type[pos] = frame_type;
    framelist->content_len[pos] = frame_size;
    framelist->bit_info[pos] = bit_info;
    framelist->ts[pos] = ts;

    /* update framelist size */
    if (framelist->origin + (int)framelist->size <= index)
        framelist->size = distance + 1;

    if(JB_NORMAL_FRAME == frame_type) {
        /* copy frame content */
        memcpy(framelist->content + pos * framelist->frame_size,
                  frame, frame_size);
    }

    return 0;
}


static int jb_framelist_discard(jb_framelist_t *framelist,
                                        int index)
{
    unsigned pos;

    
    if (index < framelist->origin ||
        index >= framelist->origin + (int)framelist->size)
        return -1;

    /* Get the slot position */
    pos = (framelist->head + (index - framelist->origin)) %
          framelist->max_count;

    /* Discard the frame */
    framelist->frame_type[pos] = JB_DISCARDED_FRAME;
    framelist->discarded_num++;

    // printf("discard frame\n");

    return 0;
}


enum jb_op
    {
        JB_OP_INIT  = -1,
        JB_OP_PUT   = 1,
        JB_OP_GET   = 2
    };



int jbuf_create(unsigned frame_size,
                unsigned ptime,
                unsigned max_count,
                jbuf_t **p_jb)
{
    jbuf_t *jb;
    int status;

    jb = (jbuf_t*)calloc(1, sizeof(jbuf_t));

    status = jb_framelist_init(&jb->jb_framelist, frame_size, max_count);
    if (status)
        return status;

    status = pthread_mutex_init(&jb->lock, NULL);
    if (status < 0)
        return status;
    
    jb->jb_frame_size = frame_size;
    jb->jb_frame_ptime = ptime;
    jb->jb_prefetch = MIN(JB_DEFAULT_INIT_DELAY, max_count*4/5);
    jb->jb_min_prefetch  = 0;
    jb->jb_max_prefetch  = max_count*4/5;
    jb->jb_max_count = max_count;
    jb->jb_min_shrink_gap = JBUF_DISC_MIN_GAP / ptime;
    jb->jb_max_burst = MAX(MAX_BURST_MSEC / ptime, max_count*3/4);

    jbuf_set_discard(jb, JB_DISCARD_PROGRESSIVE);
    
    jbuf_reset(jb);

    *p_jb = jb;
    
    return 0;
}


/*
 * Set the jitter buffer to fixed delay mode. The default behavior
 * is to adapt the delay with actual packet delay.
 *
 */
int jbuf_set_fixed(jbuf_t *jb,
                   unsigned prefetch)
{
    if (!jb)
        return -1;
    if (prefetch > jb->jb_max_count)
        return -1;

    pthread_mutex_lock(&jb->lock);
    
    jb->jb_min_prefetch = jb->jb_max_prefetch =
        jb->jb_prefetch = jb->jb_init_prefetch = prefetch;

    jbuf_set_discard(jb, JB_DISCARD_NONE);

    pthread_mutex_unlock(&jb->lock);    
    
    return 0;
}


/*
 * Set the jitter buffer to adaptive mode.
 */
int jbuf_set_adaptive(jbuf_t *jb,
                      unsigned prefetch,
                      unsigned min_prefetch,
                      unsigned max_prefetch)
{
    if (!jb)
        return -1;
    
    if (min_prefetch > max_prefetch ||
          prefetch > max_prefetch ||
          max_prefetch > jb->jb_max_count)
        return -1;

    pthread_mutex_lock(&jb->lock);
    
    jb->jb_prefetch = jb->jb_init_prefetch = prefetch;
    jb->jb_min_prefetch = min_prefetch;
    jb->jb_max_prefetch = max_prefetch;

    pthread_mutex_unlock(&jb->lock);
    
    return 0;
}


int jbuf_set_discard(jbuf_t *jb,
                     jb_discard_algo_t algo)
{
    if (!jb) return -1;
    if (algo < JB_DISCARD_NONE || algo > JB_DISCARD_PROGRESSIVE)
        return -1;

    pthread_mutex_lock(&jb->lock);
    
    switch(algo) {
    case JB_DISCARD_PROGRESSIVE:
        jb->jb_discard_algo = &jbuf_discard_progressive;
        break;
    case JB_DISCARD_STATIC:
        jb->jb_discard_algo = &jbuf_discard_static;
        break;
    default:
        jb->jb_discard_algo = NULL;
        break;
    }

    pthread_mutex_unlock(&jb->lock);
    
    return 0;
}


int jbuf_reset(jbuf_t *jb)
{
    pthread_mutex_lock(&jb->lock);
    
    jb->jb_level = 0;
    jb->jb_last_op = JB_OP_INIT;
    jb->jb_stable_hist = 0;
    jb->jb_status = JB_STATUS_INITIALIZING;
    jb->jb_init_cycle_cnt= 0;
    jb->jb_max_hist_level= 0;
    jb->jb_prefetching   = (jb->jb_prefetch != 0);
    jb->jb_discard_dist  = 0;

    jb_framelist_reset(&jb->jb_framelist);

    pthread_mutex_unlock(&jb->lock);
    
    return 0;
}


int jbuf_destroy(jbuf_t *jb)
{
    int rc;
    
    pthread_mutex_lock(&jb->lock);    
    rc = jb_framelist_destroy(&jb->jb_framelist);
    pthread_mutex_unlock(&jb->lock);

    return rc;
}

int jbuf_is_full(jbuf_t *jb)
{
    int rc ;
    
    pthread_mutex_lock(&jb->lock);
    
    rc = (jb->jb_framelist.size == jb->jb_framelist.max_count);

    pthread_mutex_unlock(&jb->lock);

    return rc;
}

static void jbuf_calculate_jitter(jbuf_t *jb)
{
    int diff, cur_size;

    cur_size = jb_framelist_eff_size(&jb->jb_framelist);
    jb->jb_max_hist_level = MAX(jb->jb_max_hist_level, jb->jb_level);

    /* Burst level is decreasing */
    if (jb->jb_level < jb->jb_eff_level) {

        enum { STABLE_HISTORY_LIMIT = 20 };

        jb->jb_stable_hist++;

        /* Only update the effective level (and prefetch) if 'stable'
         * condition is reached (not just short time impulse)
         */
        if (jb->jb_stable_hist > STABLE_HISTORY_LIMIT) {

            diff = (jb->jb_eff_level - jb->jb_max_hist_level) / 3;

            if (diff < 1)
                diff = 1;

            /* Update effective burst level */
            jb->jb_eff_level -= diff;

            /* Update prefetch based on level */
            if (jb->jb_init_prefetch) {
                jb->jb_prefetch = jb->jb_eff_level;
                if (jb->jb_prefetch < jb->jb_min_prefetch)
                    jb->jb_prefetch = jb->jb_min_prefetch;
                if (jb->jb_prefetch > jb->jb_max_prefetch)
                    jb->jb_prefetch = jb->jb_max_prefetch;
            }

            /* Reset history */
            jb->jb_max_hist_level = 0;
            jb->jb_stable_hist = 0;
        }
    }

    /* Burst level is increasing */
    else if (jb->jb_level > jb->jb_eff_level) {

        /* Instaneous set effective burst level to recent maximum level */
        jb->jb_eff_level = MIN(jb->jb_max_hist_level,
                                  (int)(jb->jb_max_count*4/5));

        /* Update prefetch based on level */
        if (jb->jb_init_prefetch) {
            jb->jb_prefetch = jb->jb_eff_level;
            if (jb->jb_prefetch > jb->jb_max_prefetch)
                jb->jb_prefetch = jb->jb_max_prefetch;
            if (jb->jb_prefetch < jb->jb_min_prefetch)
                jb->jb_prefetch = jb->jb_min_prefetch;
        }

        jb->jb_stable_hist = 0;
        /* Do not reset max_hist_level. */
        //jb->jb_max_hist_level = 0;
    }

    /* Level is unchanged */
    else {
        jb->jb_stable_hist = 0;
    }
}

static void jbuf_update(jbuf_t *jb, int oper)
{
    if(jb->jb_last_op != oper) {
        jb->jb_last_op = oper;

        if (jb->jb_status == JB_STATUS_INITIALIZING) {
            /* Switch status 'initializing' -> 'processing' after some OP
             * switch cycles and current OP is GET (burst level is calculated
             * based on PUT burst), so burst calculation is guaranted to be
             * performed right after the status switching.
             */
            if (++jb->jb_init_cycle_cnt >= INIT_CYCLE && oper == JB_OP_GET) {
                jb->jb_status = JB_STATUS_PROCESSING;
                /* To make sure the burst calculation will be done right after
                 * this, adjust burst level if it exceeds max burst level.
                 */
                jb->jb_level = MIN(jb->jb_level, jb->jb_max_burst);
            } else {
                jb->jb_level = 0;
                return;
            }
        }

        /* Perform jitter calculation based on PUT burst-level only, since
         * GET burst-level may not be accurate, e.g: when VAD is active.
         * Note that when burst-level is too big, i.e: exceeds jb_max_burst,
         * the GET op may be idle, in this case, we better skip the jitter
         * calculation.
         */
        if (oper == JB_OP_GET && jb->jb_level <= jb->jb_max_burst)
            jbuf_calculate_jitter(jb);

        jb->jb_level = 0;
    }

    /* Call discard algorithm */
    if (jb->jb_status == JB_STATUS_PROCESSING && jb->jb_discard_algo) {
        (*jb->jb_discard_algo)(jb);
    }
}



static void jbuf_discard_static(jbuf_t *jb)
{
    /* These code is used for shortening the delay in the jitter buffer.
     * It needs shrink only when there is possibility of drift. Drift
     * detection is performed by inspecting the jitter buffer size, if
     * its size is twice of current burst level, there can be drift.
     *
     * Moreover, normally drift level is quite low, so JB shouldn't need
     * to shrink aggresively, it will shrink maximum one frame per
     * JBUF_DISC_MIN_GAP ms. Theoritically, JB may handle drift level
     * as much as = FRAME_PTIME/JBUF_DISC_MIN_GAP * 100%
     *
     * Whenever there is drift, where PUT > GET, this method will keep
     * the latency (JB size) as much as twice of burst level.
     */

    /* Shrinking due of drift will be implicitly done by progressive discard,
     * so just disable it when progressive discard is active.
     */
    int diff, burst_level;

    burst_level = MAX(jb->jb_eff_level, jb->jb_level);
    diff = jb_framelist_eff_size(&jb->jb_framelist) - burst_level*2;

    if (diff >= STA_DISC_SAFE_SHRINKING_DIFF) {
        int seq_origin;

        /* Check and adjust jb_discard_ref, in case there was
         * seq restart
         */
        seq_origin = jb_framelist_origin(&jb->jb_framelist);
        if (seq_origin < jb->jb_discard_ref)
            jb->jb_discard_ref = seq_origin;

        if (seq_origin - jb->jb_discard_ref >= jb->jb_min_shrink_gap)
        {
            /* Shrink slowly, one frame per cycle */
            diff = 1;

            /* Drop frame(s)! */
            diff = jb_framelist_remove_head(&jb->jb_framelist, diff);
            jb->jb_discard_ref = jb_framelist_origin(&jb->jb_framelist);
        }
    }
}


static void jbuf_discard_progressive(jbuf_t *jb)
{
    unsigned cur_size, burst_level, overflow, T, discard_dist;
    int last_seq;

    /* Should be done in PUT operation */
    if (jb->jb_last_op != JB_OP_PUT)
        return;

    /* Check if latency is longer than burst */
    cur_size = jb_framelist_eff_size(&jb->jb_framelist);
    burst_level = MAX(jb->jb_eff_level, jb->jb_level);
    if (cur_size <= burst_level) {
        /* Reset any scheduled discard */
        jb->jb_discard_dist = 0;
        return;
    }

    /* Estimate discard duration needed for adjusting latency */
    if (burst_level <= JBUF_PRO_DISC_MIN_BURST)
        T = JBUF_PRO_DISC_T1;
    else if (burst_level >= JBUF_PRO_DISC_MAX_BURST)
        T = JBUF_PRO_DISC_T2;
    else
        T = JBUF_PRO_DISC_T1 +
            (JBUF_PRO_DISC_T2 - JBUF_PRO_DISC_T1) *
            (burst_level - JBUF_PRO_DISC_MIN_BURST) /
            (JBUF_PRO_DISC_MAX_BURST-JBUF_PRO_DISC_MIN_BURST);

    /* Calculate current discard distance */
    overflow = cur_size - burst_level;
    discard_dist = T / overflow / jb->jb_frame_ptime;

    /* Get last seq number in the JB */
    last_seq = jb_framelist_origin(&jb->jb_framelist) +
               jb_framelist_size(&jb->jb_framelist) - 1;

    /* Setup new discard schedule if none, otherwise, update the existing
     * discard schedule (can be delayed or accelerated).
     */
    if (jb->jb_discard_dist == 0) {
        /* Setup new discard schedule */
        jb->jb_discard_ref = last_seq;
    } else if (last_seq < jb->jb_discard_ref) {
        /* Seq restarted, update discard reference */
        jb->jb_discard_ref = last_seq;
    }
    jb->jb_discard_dist = MAX(jb->jb_min_shrink_gap, (int)discard_dist);

    /* Check if we need to discard now */
    if (last_seq >= (jb->jb_discard_ref + (int)jb->jb_discard_dist)) {
        int discard_seq;

        discard_seq = jb->jb_discard_ref + jb->jb_discard_dist;
        if (discard_seq < jb_framelist_origin(&jb->jb_framelist))
            discard_seq = jb_framelist_origin(&jb->jb_framelist);

        jb_framelist_discard(&jb->jb_framelist, discard_seq);

        /* Update discard reference */
        jb->jb_discard_ref = discard_seq;
    }
}


void jbuf_put_frame(jbuf_t *jb,
                    const void *frame,
                    size_t frame_size,
                    int frame_seq)
{
    jbuf_put_frame3(jb, frame, frame_size, 0, frame_seq, 0, NULL);
}

void jbuf_put_frame2(jbuf_t *jb,
                     const void *frame,
                     size_t frame_size,
                     uint32_t bit_info,
                     int frame_seq,
                     int *discarded)
{
    jbuf_put_frame3(jb, frame, frame_size, bit_info, frame_seq, 0,
                            discarded);
}

void jbuf_put_frame3(jbuf_t *jb,
                     const void *frame,
                     size_t frame_size,
                     uint32_t bit_info,
                     int frame_seq,
                     uint32_t ts,
                     int *discarded)
{
    size_t min_frame_size;
    int new_size, cur_size;
    int status;

    pthread_mutex_lock(&jb->lock);
    
    cur_size = jb_framelist_eff_size(&jb->jb_framelist);

    /* Attempt to store the frame */
    min_frame_size = MIN(frame_size, jb->jb_frame_size);
    status = jb_framelist_put_at(&jb->jb_framelist, frame_seq, frame,
                                 (unsigned)min_frame_size, bit_info, ts,
                                 JB_NORMAL_FRAME);

    /* Jitter buffer is full, remove some older frames */
    while (status == -2) {
        int distance;
        unsigned removed;

        //        printf("jt buf full\n");
        /* Remove as few as possible just to make this frame in. Note that
         * the cases of seq-jump, out-of-order, and seq restart should have
         * been handled/normalized by previous call of jb_framelist_put_at().
         * So we're confident about 'distance' value here.
         */
        distance = (frame_seq - jb_framelist_origin(&jb->jb_framelist)) -
                   (int)jb->jb_max_count + 1;
        assert(distance > 0);

        removed = jb_framelist_remove_head(&jb->jb_framelist, distance);
        status = jb_framelist_put_at(&jb->jb_framelist, frame_seq, frame,
                                     (unsigned)min_frame_size, bit_info, ts,
                                     JB_NORMAL_FRAME);

    }

    /* Get new JB size after PUT */
    new_size = jb_framelist_eff_size(&jb->jb_framelist);

    /* Return the flag if this frame is discarded */
    if (discarded)
        *discarded = (status != 0);

    if (status == 0) {
        if (jb->jb_prefetching) {
            if (new_size >= jb->jb_prefetch)
                jb->jb_prefetching = 0;
        }
        jb->jb_level += (new_size > cur_size ? new_size-cur_size : 1);
        jbuf_update(jb, JB_OP_PUT);
    }

    pthread_mutex_unlock(&jb->lock);    
}

/*
 * Get frame from jitter buffer.
 */
void jbuf_get_frame(jbuf_t *jb,
                    void *frame,
                    char *p_frame_type)
{
    jbuf_get_frame3(jb, frame, NULL, p_frame_type, NULL,
                            NULL, NULL);
}

/*
 * Get frame from jitter buffer.
 */
void jbuf_get_frame2(jbuf_t *jb,
                     void *frame,
                     size_t *size,
                     char *p_frame_type,
                     uint32_t *bit_info)
{
    jbuf_get_frame3(jb, frame, size, p_frame_type, bit_info,
                            NULL, NULL);
}

/*
 * Get frame from jitter buffer.
 */
void jbuf_get_frame3(jbuf_t *jb,
                     void *frame,
                     size_t *size,
                     char *p_frame_type,
                     uint32_t *bit_info,
                     uint32_t *ts,
                     int *seq)
{

    pthread_mutex_lock(&jb->lock);
    
    if (jb->jb_prefetching) {

        /* Can't return frame because jitter buffer is filling up
         * minimum prefetch.
         */

        //bzero(frame, jb->jb_frame_size);
        *p_frame_type = JB_ZERO_PREFETCH_FRAME;
        if (size)
            *size = 0;
    } else {
        jb_frame_type_t ftype = JB_NORMAL_FRAME;
        int res;

        /* Try to retrieve a frame from frame list */
        res = jb_framelist_get(&jb->jb_framelist, frame, size, &ftype,
                               bit_info, ts, seq);
        if (res) {
            /* We've successfully retrieved a frame from the frame list, but
             * the frame could be a blank frame!
             */
            if (ftype == JB_NORMAL_FRAME) {
                *p_frame_type = JB_NORMAL_FRAME;
                //                printf("normal packet\n");                
            } else {
                *p_frame_type = JB_MISSING_FRAME;
                //                printf("missing packet\n");
            }


            
            /* Store delay history at the first GET */
            if (jb->jb_last_op == JB_OP_PUT) {
                unsigned cur_size;

                /* We've just retrieved one frame, so add one to cur_size */
                cur_size = jb_framelist_eff_size(&jb->jb_framelist) + 1;
            }
        } else {
            /* Jitter buffer is empty */
            if (jb->jb_prefetch)
                jb->jb_prefetching = 1;

            //bzero(frame, jb->jb_frame_size);
            *p_frame_type = JB_ZERO_EMPTY_FRAME;
            if (size)
                *size = 0;

            // printf("jtbuf empty\n");
        }
    }

    jb->jb_level++;
    jbuf_update(jb, JB_OP_GET);

    pthread_mutex_unlock(&jb->lock);    
}


void jbuf_peek_frame(jbuf_t *jb,
                     unsigned offset,
                     const void **frame,
                     size_t *size,
                     char *p_frm_type,
                     uint32_t *bit_info,
                     uint32_t *ts,
                     int *seq)
{
    jb_frame_type_t ftype;
    int res;

    pthread_mutex_lock(&jb->lock);
    
    res = jb_framelist_peek(&jb->jb_framelist, offset, frame, size, &ftype,
                            bit_info, ts, seq);
    if (!res)
        *p_frm_type = JB_ZERO_EMPTY_FRAME;
    else if (ftype == JB_NORMAL_FRAME)
        *p_frm_type = JB_NORMAL_FRAME;
    else
        *p_frm_type = JB_MISSING_FRAME;

    pthread_mutex_unlock(&jb->lock);    
}


unsigned jbuf_remove_frame(jbuf_t *jb,
                           unsigned frame_cnt)
{
    unsigned count, last_discard_num;

    pthread_mutex_lock(&jb->lock);
    
    last_discard_num = jb->jb_framelist.discarded_num;
    count = jb_framelist_remove_head(&jb->jb_framelist, frame_cnt);

    /* Remove some more when there were discarded frames included */
    while (jb->jb_framelist.discarded_num < last_discard_num) {
        /* Calculate frames count to be removed next */
        frame_cnt = last_discard_num - jb->jb_framelist.discarded_num;

        /* Normalize non-discarded frames count just been removed */
        count -= frame_cnt;

        /* Remove more frames */
        last_discard_num = jb->jb_framelist.discarded_num;
        count += jb_framelist_remove_head(&jb->jb_framelist, frame_cnt);
    }

    pthread_mutex_unlock(&jb->lock);
    
    return count;
}
