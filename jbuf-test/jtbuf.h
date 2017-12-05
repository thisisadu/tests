#ifndef JTBUF_H
#define JTBUF_H

/**
 * Types of frame returned by the jitter buffer.
 */
typedef enum jb_frame_type
{
    JB_MISSING_FRAME   = 0, /**< No frame because it's missing  */
    JB_NORMAL_FRAME   = 1, /**< Normal frame is being returned */
    JB_ZERO_PREFETCH_FRAME = 2, /**< Zero frame is being returned  
                                   because JB is bufferring.    */
    JB_ZERO_EMPTY_FRAME   = 3/**< Zero frame is being returned
                                    because JB is empty.    */
} jb_frame_type_t;


/**
 * Enumeration of jitter buffer discard algorithm. The jitter buffer
 * continuously calculates the jitter level to get the optimum latency at
 * any time and in order to adjust the latency, the jitter buffer may need
 * to discard some frames.
 */
typedef enum jb_discard_algo
{
    /**
     * Jitter buffer should not discard any frame, except when the jitter
     * buffer is full and a new frame arrives, one frame will be discarded
     * to make space for the new frame.
     */
    JB_DISCARD_NONE   = 0,
    
    /**
     * Only discard one frame in at least 200ms when the latency is considered
     * much higher than it should be. When the jitter buffer is full and a new
     * frame arrives, one frame will be discarded to make space for the new
     * frame.
     */
    JB_DISCARD_STATIC,
    
    /**
     * The discard rate is dynamically calculated based on actual parameters
     * such as jitter level and latency. When the jitter buffer is full and
     * a new frame arrives, one frame will be discarded to make space for the
     * new frame.
     */
    JB_DISCARD_PROGRESSIVE

} jb_discard_algo_t;


/**
 * This structure describes jitter buffer state.
 */
typedef struct jb_state
{
    /* Setting */
    unsigned frame_size;    /**< Individual frame size, in bytes.   */
    unsigned min_prefetch;    /**< Minimum allowed prefetch, in frms. */
    unsigned max_prefetch;    /**< Maximum allowed prefetch, in frms. */

    /* Status */
    unsigned burst;    /**< Current burst level, in frames    */
    unsigned prefetch;    /**< Current prefetch value, in frames  */
    unsigned size;    /**< Current buffer size, in frames.    */

    /* Statistic */
    unsigned avg_delay;    /**< Average delay, in ms.    */
    unsigned min_delay;    /**< Minimum delay, in ms.    */
    unsigned max_delay;    /**< Maximum delay, in ms.    */
    unsigned dev_delay;    /**< Standard deviation of delay, in ms.*/
    unsigned avg_burst;    /**< Average burst, in frames.    */
    unsigned lost;    /**< Number of lost frames.    */
    unsigned discard;    /**< Number of discarded frames.    */
    unsigned empty;    /**< Number of empty on GET events.    */
    
} jb_state_t;


typedef struct jbuf jbuf_t;



/**
 * The constant JB_DEFAULT_INIT_DELAY specifies default jitter
 * buffer prefetch count during jitter buffer creation.
 */
#define JB_DEFAULT_INIT_DELAY    15

extern int jbuf_set_fixed(jbuf_t *, unsigned);
extern int jbuf_set_adaptive(jbuf_t *,
                             unsigned,
                             unsigned,
                             unsigned);
extern int jbuf_set_discard(jbuf_t *,
                            jb_discard_algo_t);

extern int jbuf_create(unsigned,
                       unsigned,
                       unsigned,
                       jbuf_t **);
extern int jbuf_reset(jbuf_t *);

extern int jbuf_destroy(jbuf_t *);

extern int jbuf_is_full(jbuf_t *);

extern void jbuf_put_frame(jbuf_t *, const void *, size_t, int);

extern void jbuf_put_frame2(jbuf_t *,
                            const void *,
                            size_t,
                            uint32_t,
                            int,
                            int *);
extern void jbuf_put_frame3(jbuf_t *,
                            const void *,
                            size_t,
                            uint32_t,
                            int,
                            uint32_t,
                            int *);
extern void jbuf_get_frame(jbuf_t *, void *, char *);

extern void jbuf_get_frame2(jbuf_t *, void *, size_t*, char *, uint32_t*);

extern void jbuf_get_frame3(jbuf_t *, void *, size_t*, char *, uint32_t*, uint32_t*, int*);



#endif
