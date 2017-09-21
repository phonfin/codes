/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <assert.h>
#include <math.h>

#include "codes/codes-workload.h"
#include "codes/quickhash.h"

#include "darshan-logutils.h"

#define DEF_INTER_IO_DELAY_PCT 0.2
#define DEF_INTER_CYC_DELAY_PCT 0.4

#define DARSHAN_NEGLIGIBLE_DELAY 0.00001

#define RANK_HASH_TABLE_SIZE 397

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#define ALIGN_BY_8(x) ((x) + ((x) % 8))

/* structure for storing a darshan workload operation (a codes op with 2 timestamps) */
struct darshan_io_op
{
    struct codes_workload_op codes_op;
    double start_time;
    double end_time;
};

/* I/O context structure managed by each rank in the darshan workload */
struct rank_io_context
{
    int64_t my_rank;
    double last_op_time;
    void *io_op_dat;

    off_t next_off;

    struct qhash_head hash_link;
};

struct darshan_unified_record
{
    struct darshan_posix_file psx_file_rec;
    struct darshan_mpiio_file mpiio_file_rec;
    struct darshan_unified_record *next;
};

static void * darshan_io_workload_read_config(
        ConfigHandle * handle,
        char const * section_name,
        char const * annotation,
        int num_ranks);
/* Darshan workload generator's implementation of the CODES workload API */
static int darshan_psx_io_workload_load(const char *params, int app_id, int rank);
static void darshan_psx_io_workload_get_next(int app_id, int rank, struct codes_workload_op *op);
static int darshan_psx_io_workload_get_rank_cnt(const char *params, int app_id);
static int darshan_rank_hash_compare(void *key, struct qhash_head *link);

/* Darshan I/O op data structure access (insert, remove) abstraction */
static void *darshan_init_io_op_dat(void);
static void darshan_insert_next_io_op(void *io_op_dat, struct darshan_io_op *io_op);
static void darshan_remove_next_io_op(void *io_op_dat, struct darshan_io_op *io_op,
                                      double last_op_time);
static void darshan_finalize_io_op_dat(void *io_op_dat);
static int darshan_io_op_compare(const void *p1, const void *p2);

/* Helper functions for implementing the Darshan workload generator */
static void generate_psx_ind_file_events(struct darshan_posix_file *file,
                                         struct rank_io_context *io_context);
static double generate_psx_open_event(struct darshan_posix_file *file, int create_flag,
                                      double meta_op_time, double cur_time,
                                      struct rank_io_context *io_context, int insert_flag);
static double generate_psx_close_event(struct darshan_posix_file *file, double meta_op_time,
                                       double cur_time, struct rank_io_context *io_context,
                                       int insert_flag);
static double generate_psx_ind_io_events(struct darshan_posix_file *file, int64_t io_ops_this_cycle,
                                         double inter_io_delay, double cur_time,
                                         struct rank_io_context *io_context);
static void determine_ind_io_params(struct darshan_posix_file *file, int write_flag, size_t *io_sz,
                                    off_t *io_off, struct rank_io_context *io_context);
static void calc_io_delays(struct darshan_posix_file *file, int64_t num_opens, int64_t num_io_ops,
                           double total_delay, double *first_io_delay, double *close_delay,
                           double *inter_open_delay, double *inter_io_delay);
static void file_sanity_check(struct darshan_posix_file *file, struct darshan_job *job, darshan_fd fd);

static int darshan_psx_io_workload_get_time(const char *params, int app_id, int rank, double *read_time, double *write_time,
												int64_t *read_bytes, int64_t *written_bytes);
/* workload method name and function pointers for the CODES workload API */
struct codes_workload_method darshan_posix_io_workload_method =
{
    .method_name = "darshan_io_workload",
    .codes_workload_read_config = darshan_io_workload_read_config,
    .codes_workload_load = darshan_psx_io_workload_load,
    .codes_workload_get_next = darshan_psx_io_workload_get_next,
    .codes_workload_get_rank_cnt = darshan_psx_io_workload_get_rank_cnt,
	.codes_workload_get_time = darshan_psx_io_workload_get_time,
};

/* posix_logutils functions */

extern struct darshan_mod_logutil_funcs posix_logutils;
static struct darshan_mod_logutil_funcs *psx_utils = &posix_logutils;
static struct darshan_mod_logutil_funcs *mpiio_utils = &mpiio_logutils;
static int total_rank_cnt = 0;

/* hash table to store per-rank workload contexts */
static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;

static void * darshan_io_workload_read_config(
        ConfigHandle * handle,
        char const * section_name,
        char const * annotation,
        int num_ranks)
{
    darshan_params *d = malloc(sizeof(*d));
    assert(d);
    d->log_file_path[0] = '\0';
    
    /* silence warning */
    (void)num_ranks;

    int rc = configuration_get_value_relpath(handle, section_name,
            "darshan_log_file", annotation, d->log_file_path,
            MAX_NAME_LENGTH_WKLD);
    assert(rc > 0);
   
    return d;
}

static int darshan_psx_io_workload_get_time(const char *params, int app_id, int rank, double *read_time, double *write_time,
		int64_t *read_bytes, int64_t *written_bytes)
{
	struct darshan_posix_file * psx_file_rec;
	darshan_fd logfile_fd = NULL;
	psx_file_rec = (struct darshan_posix_file *)calloc(1, sizeof(struct darshan_posix_file));
	darshan_params *d_params = (darshan_params *)params;

        /* silence warning */
        (void)app_id;

	int ret;
	if (!d_params)
		return -1;

	/* open the darshan log to begin reading in file i/o info */
	//logfile_fd = darshan_log_open(d_params->log_file_path, "r");
	logfile_fd = darshan_log_open(d_params->log_file_path);
	if (!logfile_fd)
		return -1;
	while ((ret = psx_utils->log_get_record(logfile_fd, (void **)&psx_file_rec)) > 0)
	{
		 /* generate all i/o events contained in this independent file */
		if (psx_file_rec->base_rec.rank == rank)
		{
			*read_time += psx_file_rec->fcounters[POSIX_F_READ_TIME];
			*write_time += psx_file_rec->fcounters[POSIX_F_WRITE_TIME];
			*read_bytes += psx_file_rec->counters[POSIX_BYTES_READ];
			*written_bytes += psx_file_rec->counters[POSIX_BYTES_WRITTEN];
		}
	}
	darshan_log_close(logfile_fd);
	return 0;
}

/* load the workload generator for this rank, given input params */
static int darshan_psx_io_workload_load(const char *params, int app_id, int rank)
{
    darshan_params *d_params = (darshan_params *)params;
    darshan_fd logfile_fd = NULL;
    struct darshan_job job;
    //struct darshan_file next_file;
    /* open posix log file */
    struct darshan_posix_file *psx_file_rec;
    struct darshan_mpiio_file *mpiio_file_rec;
    struct rank_io_context *my_ctx;
    int ret;
    struct darshan_unified_record *dur_new = NULL;
    struct darshan_unified_record *dur_head = NULL;
    struct darshan_unified_record *dur_cur = NULL;
    struct darshan_unified_record *dur_tmp = NULL;

    psx_file_rec = (struct darshan_posix_file *) calloc(1, sizeof(struct darshan_posix_file));
    assert(psx_file_rec);
    mpiio_file_rec = (struct darshan_mpiio_file *) calloc(1, sizeof(struct darshan_mpiio_file));
    assert(mpiio_file_rec);

    APP_ID_UNSUPPORTED(app_id, "darshan")

    if (!d_params)
        return -1;

    /* open the darshan log to begin reading in file i/o info */
    //logfile_fd = darshan_log_open(d_params->log_file_path, "r");
    logfile_fd = darshan_log_open(d_params->log_file_path);
    if (!logfile_fd)
        return -1;

    /* get the per-job stats from the log */
    ret = darshan_log_get_job(logfile_fd, &job);
    if (ret < 0)
    {
        darshan_log_close(logfile_fd);
        return -1;
    }
    if (!total_rank_cnt)
    {
        total_rank_cnt = job.nprocs;
    }
    //printf("rank = %d, total_rank_cnt = %d\n", rank, total_rank_cnt);
    //printf("job: (%ld, %ld, %ld, %ld, %s)\n", job.jobid, job.uid, job.start_time, job.end_time, job.metadata);
    assert(rank < total_rank_cnt);

    /* allocate the i/o context needed by this rank */
    my_ctx = malloc(sizeof(struct rank_io_context));
    if (!my_ctx)
    {
        darshan_log_close(logfile_fd);
        return -1;
    }
    my_ctx->my_rank = (int64_t)rank;
    my_ctx->last_op_time = 0.0;
    my_ctx->io_op_dat = darshan_init_io_op_dat();
    my_ctx->next_off = 0;

    /* loop over all files contained in the log file */
    /* build a linked list to preserve order, we will make another pass over
     * mpiio records and combine them
     */
    while ((ret = psx_utils->log_get_record(logfile_fd, (void **)&psx_file_rec)) > 0)
    {
        dur_new = calloc(1, sizeof(*dur_new));
        assert(dur_new);

        dur_new->psx_file_rec = *psx_file_rec;

        if(!dur_head)
            dur_head = dur_new;
        if(dur_cur)
            dur_cur->next = dur_new;
        dur_cur = dur_new;
    }

    /* now loop over mpiio records (if present) and match them up with the
     * posix records
     */
    while ((ret = mpiio_utils->log_get_record(logfile_fd, (void **)&mpiio_file_rec)) > 0)
    {
        for(dur_cur = dur_head; dur_cur; dur_cur = dur_cur->next)
        {
            if((dur_cur->psx_file_rec.base_rec.rank ==
                mpiio_file_rec->base_rec.rank) &&
               (dur_cur->psx_file_rec.base_rec.id ==
                mpiio_file_rec->base_rec.id))
            {
                dur_cur->mpiio_file_rec = *mpiio_file_rec;
                break;
            }
        }
        /* if we exit loop with null dur_cur, that means that an mpiio record is present
         * for which there is no exact match in the posix records.  This
         * could (for example) happen if mpiio was using deferred opens,
         * producing a shared record in mpi and unique records in posix.  Or
         * if mpiio is using a non-posix back end
         */
        assert(dur_cur);
    }

    /* file records have all been retrieved from darshan log.  Now we loop
     * through them and generate workload events from each record
     */
    for(dur_cur = dur_head; dur_cur; dur_cur = dur_cur->next)
    {
        /* generate all i/o events contained in this independent file */
        if (dur_cur->psx_file_rec.base_rec.rank == rank)
        {
            /* make sure the file i/o counters are valid */
            file_sanity_check(&dur_cur->psx_file_rec, &job, logfile_fd);

            /* generate i/o events and store them in this rank's workload context */
            generate_psx_ind_file_events(&dur_cur->psx_file_rec, my_ctx);
        }
        else
        {
            /* TODO: implement */
            assert(0);
        }

        assert(dur_cur->psx_file_rec.counters[POSIX_OPENS] == 0);
        assert(dur_cur->psx_file_rec.counters[POSIX_READS] == 0);
        assert(dur_cur->psx_file_rec.counters[POSIX_WRITES] == 0);
    }
    if (ret < 0)
        return -1;

    darshan_log_close(logfile_fd);

    /* finalize the rank's i/o context so i/o ops may be retrieved later (in order) */
    darshan_finalize_io_op_dat(my_ctx->io_op_dat);
    if (!rank_tbl)
    {
        rank_tbl = qhash_init(darshan_rank_hash_compare, quickhash_64bit_hash, RANK_HASH_TABLE_SIZE);
        if (!rank_tbl)
            return -1;
    }

    /* add this rank context to the hash table */
    qhash_add(rank_tbl, &(my_ctx->my_rank), &(my_ctx->hash_link));
    rank_tbl_pop++;

    /* free linked list */
    for(dur_cur = dur_head; dur_cur;)
    {
        dur_tmp = dur_cur;
        dur_cur = dur_cur->next;
        free(dur_tmp);
    }
    free(psx_file_rec);
    free(mpiio_file_rec);
    return 0;
}

/* pull the next event (independent or collective) for this rank from its event context */
static void darshan_psx_io_workload_get_next(int app_id, int rank, struct codes_workload_op *op)
{
    int64_t my_rank = (int64_t)rank;
    struct qhash_head *hash_link = NULL;
    struct rank_io_context *tmp = NULL;
    struct darshan_io_op next_io_op;

    assert(rank < total_rank_cnt);

    /* find i/o context for this rank in the rank hash table */
    hash_link = qhash_search(rank_tbl, &my_rank);
    /* terminate the workload if there is no valid rank context */
    if (!hash_link)
    {
        op->op_type = CODES_WK_END;
        return;
    }

    /* get access to the rank's io_context data */
    tmp = qhash_entry(hash_link, struct rank_io_context, hash_link);
    assert(tmp->my_rank == my_rank);

    /* get the next darshan i/o op out of this rank's context */
    darshan_remove_next_io_op(tmp->io_op_dat, &next_io_op, tmp->last_op_time);

    /* free the rank's i/o context if this is the last i/o op */
    if (next_io_op.codes_op.op_type == CODES_WK_END)
    {
        qhash_del(hash_link);
        free(tmp);
        rank_tbl_pop--;
        if (!rank_tbl_pop)
        {
            qhash_finalize(rank_tbl);
            rank_tbl = NULL;
        }
    }
    else
    {
        /* else, set the last op time to be the end of the returned op */
        tmp->last_op_time = next_io_op.end_time;
    }

    /* return the codes op contained in the darshan i/o op */
    *op = next_io_op.codes_op;

    return;
}

static int darshan_psx_io_workload_get_rank_cnt(const char *params, int app_id)
{
    darshan_params *d_params = (darshan_params *)params;
    darshan_fd logfile_fd = NULL;
    struct darshan_job job;
    int ret;

    /* silence warning */
    (void)app_id;

    if (!d_params)
        return -1;
    //printf("opening log ... \n");
    /* open the darshan log to begin reading in file i/o info */
    logfile_fd = darshan_log_open(d_params->log_file_path);
    if (!logfile_fd)
        return -1;
    //printf("log open done, start getting job \n");
    /* get the per-job stats from the log */
    ret = darshan_log_get_job(logfile_fd, &job);
    if (ret < 0)
    {
        darshan_log_close(logfile_fd);
        return -1;
    }
    //printf("get_job done\n");
    darshan_log_close(logfile_fd);

    return job.nprocs;
}

/* comparison function for comparing two hash keys (used for storing multiple io contexts) */
static int darshan_rank_hash_compare(
    void *key, struct qhash_head *link)
{
    int64_t *in_rank = (int64_t *)key;
    struct rank_io_context *tmp;

    tmp = qhash_entry(link, struct rank_io_context, hash_link);
    if (tmp->my_rank == *in_rank)
        return 1;

    return 0;
}

/*****************************************/
/*                                       */
/*   Darshan I/O op storage abstraction  */
/*                                       */
/*****************************************/

#define DARSHAN_IO_OP_INC_CNT 100000

/* dynamically allocated array data structure for storing darshan i/o events */
struct darshan_io_dat_array
{
    struct darshan_io_op *op_array;
    int64_t op_arr_ndx;
    int64_t op_arr_cnt;
};

/* initialize the dynamic array data structure */
static void *darshan_init_io_op_dat()
{
    struct darshan_io_dat_array *tmp;

    /* initialize the array data structure */
    tmp = malloc(sizeof(struct darshan_io_dat_array));
    assert(tmp);
    tmp->op_array = malloc(DARSHAN_IO_OP_INC_CNT * sizeof(struct darshan_io_op));
    assert(tmp->op_array);
    tmp->op_arr_ndx = 0;
    tmp->op_arr_cnt = DARSHAN_IO_OP_INC_CNT;

    /* return the array info for this rank's i/o context */
    return (void *)tmp;
}

/* store the i/o event in this rank's i/o context */
static void darshan_insert_next_io_op(
    void *io_op_dat, struct darshan_io_op *io_op)
{
    struct darshan_io_dat_array *array = (struct darshan_io_dat_array *)io_op_dat;
    struct darshan_io_op *tmp;

    assert(io_op->start_time >= 0);

    /* realloc array if it is already full */
    if (array->op_arr_ndx == array->op_arr_cnt)
    {
        tmp = malloc((array->op_arr_cnt + DARSHAN_IO_OP_INC_CNT) * sizeof(struct darshan_io_op));
        assert(tmp);
        memcpy(tmp, array->op_array, array->op_arr_cnt * sizeof(struct darshan_io_op));
        free(array->op_array);
        array->op_array = tmp;
        array->op_arr_cnt += DARSHAN_IO_OP_INC_CNT;
    }

    /* add the darshan i/o op to the array */
    array->op_array[array->op_arr_ndx++] = *io_op;

    return;
}

/* pull the next i/o event out of this rank's i/o context */
static void darshan_remove_next_io_op(
    void *io_op_dat, struct darshan_io_op *io_op, double last_op_time)
{
    struct darshan_io_dat_array *array = (struct darshan_io_dat_array *)io_op_dat;

    /* if the array has been scanned completely already */
    if (array->op_arr_ndx == array->op_arr_cnt)
    {
        /* no more events just end the workload */
        io_op->codes_op.op_type = CODES_WK_END;
    }
    else
    {
        struct darshan_io_op *tmp = &(array->op_array[array->op_arr_ndx]);

        if ((tmp->start_time - last_op_time) <= DARSHAN_NEGLIGIBLE_DELAY)
        {
            /* there is no delay, just return the next op in the array */
            *io_op = *tmp;
            array->op_arr_ndx++;
        }
        else
        {
            /* there is a nonnegligible delay, so generate and return a delay event */
            io_op->codes_op.op_type = CODES_WK_DELAY;
            io_op->codes_op.u.delay.seconds = tmp->start_time - last_op_time;
            io_op->start_time = last_op_time;
            io_op->end_time = tmp->start_time;
        }
    }

    /* if this is the end op, free data structures */
    if (io_op->codes_op.op_type == CODES_WK_END)
    {
        free(array->op_array);
        free(array);
    }

    return;
}

/* sort the dynamic array in order of i/o op start time */
static void darshan_finalize_io_op_dat(
    void *io_op_dat)
{
    struct darshan_io_dat_array *array = (struct darshan_io_dat_array *)io_op_dat;

    /* sort this rank's i/o op list */
    qsort(array->op_array, array->op_arr_ndx, sizeof(struct darshan_io_op), darshan_io_op_compare);
    array->op_arr_cnt = array->op_arr_ndx;
    array->op_arr_ndx = 0;

    return;
}

/* comparison function for sorting darshan_io_ops in order of start timestamps */
static int darshan_io_op_compare(
    const void *p1, const void *p2)
{
    struct darshan_io_op *a = (struct darshan_io_op *)p1;
    struct darshan_io_op *b = (struct darshan_io_op *)p2;

    if (a->start_time < b->start_time)
        return -1;
    else if (a->start_time > b->start_time)
        return 1;
    else
        return 0;
}

/*****************************************/
/*                                       */
/* Darshan workload generation functions */
/*                                       */
/*****************************************/

/* generate events for an independently opened file, and store these events */
static void generate_psx_ind_file_events(
    struct darshan_posix_file *file, struct rank_io_context *io_context)
{
    int64_t io_ops_this_cycle;
    double cur_time = file->fcounters[POSIX_F_OPEN_START_TIMESTAMP];
    double total_delay;
    double first_io_delay = 0.0;
    double close_delay = 0.0;
    double inter_open_delay = 0.0;
    double inter_io_delay = 0.0;
    double meta_op_time;
    int create_flag;
    int64_t i;

    /* if the file was never really opened, just return because we have no timing info */
    //printf("file->counters[POSIX_OPENS] = %d\n", file->counters[POSIX_OPENS]);
    if (file->counters[POSIX_OPENS] == 0)
        return;

    /* determine delay available per open-io-close cycle */
    total_delay = file->fcounters[POSIX_F_CLOSE_END_TIMESTAMP] - file->fcounters[POSIX_F_OPEN_START_TIMESTAMP] -
                  file->fcounters[POSIX_F_READ_TIME] - file->fcounters[POSIX_F_WRITE_TIME] -
                  file->fcounters[POSIX_F_META_TIME];

    /* calculate synthetic delay values */
    calc_io_delays(file, file->counters[POSIX_OPENS],
                   file->counters[POSIX_READS] + file->counters[POSIX_WRITES], total_delay,
                   &first_io_delay, &close_delay, &inter_open_delay, &inter_io_delay);

    /* calculate average meta op time (for i/o and opens/closes) */
    /* TODO: this needs to be updated when we add in stat, seek, etc. */
    meta_op_time = file->fcounters[POSIX_F_META_TIME] / (2 * file->counters[POSIX_OPENS]);

    /* set the create flag if the file was written to */
    if (file->counters[POSIX_BYTES_WRITTEN])
    {
        create_flag = 1;
    }

    /* generate open/io/close events for all cycles */
    /* TODO: add stats */
    for (i = 0; file->counters[POSIX_OPENS]; i++, file->counters[POSIX_OPENS]--)
    {
        /* generate an open event */
        cur_time = generate_psx_open_event(file, create_flag, meta_op_time, cur_time,
                                           io_context, 1);
        create_flag = 0;

        /* account for potential delay from first open to first io */
        cur_time += first_io_delay;

        io_ops_this_cycle = ceil((double)(file->counters[POSIX_READS] +
                                 file->counters[POSIX_WRITES]) /
                                 file->counters[POSIX_OPENS]);

        /* perform the calculated number of i/o operations for this file open */
        cur_time = generate_psx_ind_io_events(file, io_ops_this_cycle, inter_io_delay,
                                              cur_time, io_context);

        /* account for potential delay from last io to close */
        cur_time += close_delay;

        /* generate a close for the open event at the start of the loop */
        cur_time = generate_psx_close_event(file, meta_op_time, cur_time, io_context, 1);

        /* account for potential interopen delay if more than one open */
        if (file->counters[POSIX_OPENS] > 1)
        {
            cur_time += inter_open_delay;
        }
    }

    return;
}

/* fill in an open event structure and store it with the rank context */
static double generate_psx_open_event(
    struct darshan_posix_file *file, int create_flag, double meta_op_time,
    double cur_time, struct rank_io_context *io_context, int insert_flag)
{
    struct darshan_io_op next_io_op = 
    {
        .codes_op.op_type = CODES_WK_OPEN,
        .codes_op.u.open.file_id = file->base_rec.id,
        .codes_op.u.open.create_flag = create_flag,
        .start_time = cur_time
    };
    //printf("OPEN event\n");
    /* set the end time of the event based on time spent in POSIX meta operations */
    cur_time += meta_op_time;
    next_io_op.end_time = cur_time;

    /* store the open event (if this rank performed it) */
    if (insert_flag)
        darshan_insert_next_io_op(io_context->io_op_dat, &next_io_op);

    return cur_time;
}

/* fill in a close event structure and store it with the rank context */
static double generate_psx_close_event(
    struct darshan_posix_file *file, double meta_op_time, double cur_time,
    struct rank_io_context *io_context, int insert_flag)
{
    struct darshan_io_op next_io_op =
    {
        .codes_op.op_type = CODES_WK_CLOSE,
        .codes_op.u.close.file_id = file->base_rec.id,
        .start_time = cur_time
    };

    /* set the end time of the event based on time spent in POSIX meta operations */
    cur_time += meta_op_time;
    next_io_op.end_time = cur_time;

    //printf("CLOSE event\n");
    /* store the close event (if this rank performed it) */
    if (insert_flag)
        darshan_insert_next_io_op(io_context->io_op_dat, &next_io_op);

    io_context->next_off = 0;

    return cur_time;
}

/* generate all i/o events for one independent file open and store them with the rank context */
static double generate_psx_ind_io_events(
    struct darshan_posix_file *file, int64_t io_ops_this_cycle, double inter_io_delay,
    double cur_time, struct rank_io_context *io_context)
{
    static int rw = -1; /* rw = 1 for write, 0 for read, -1 for uninitialized */
    static int64_t io_ops_this_rw;
    static double rd_bw = 0.0, wr_bw = 0.0;
    int64_t psx_rw_ops_remaining = file->counters[POSIX_READS] + file->counters[POSIX_WRITES];
    double io_op_time;
    size_t io_sz;
    off_t io_off;
    int64_t i;
    struct darshan_io_op next_io_op;

    /* if there are no i/o ops, just return immediately */
    if (!io_ops_this_cycle)
        return cur_time;

    /* initialze static variables when a new file is opened */
    if (rw == -1)
    {
        /* initialize rw to be the first i/o operation found in the log */
        if (file->fcounters[POSIX_F_WRITE_START_TIMESTAMP] == 0.0)
            rw = 0;
        else if (file->fcounters[POSIX_F_READ_START_TIMESTAMP] == 0.0)
            rw = 1;
        else
            rw = (file->fcounters[POSIX_F_READ_START_TIMESTAMP] <
                  file->fcounters[POSIX_F_WRITE_START_TIMESTAMP]) ? 0 : 1;

        /* determine how many io ops to do before next rw switch */
        if (!rw)
            io_ops_this_rw = file->counters[POSIX_READS] /
                             ((file->counters[POSIX_RW_SWITCHES] / 2) + 1);
        else
            io_ops_this_rw = file->counters[POSIX_WRITES] /
                             ((file->counters[POSIX_RW_SWITCHES] / 2) + 1);

        /* initialize the rd and wr bandwidth values using total io size and time */
        if (file->fcounters[POSIX_F_READ_TIME])
            rd_bw = file->counters[POSIX_BYTES_READ] / file->fcounters[POSIX_F_READ_TIME];
        if (file->fcounters[POSIX_F_WRITE_TIME])
            wr_bw = file->counters[POSIX_BYTES_WRITTEN] / file->fcounters[POSIX_F_WRITE_TIME];
    }

    /* loop to generate all reads/writes for this open/close sequence */
    for (i = 0; i < io_ops_this_cycle; i++)
    {
        /* calculate what value to use for i/o size and offset */
        determine_ind_io_params(file, rw, &io_sz, &io_off, io_context);
        if (!rw)
        {
            /* generate a read event */
            next_io_op.codes_op.op_type = CODES_WK_READ;
            next_io_op.codes_op.u.read.file_id = file->base_rec.id;
            next_io_op.codes_op.u.read.size = io_sz;
            next_io_op.codes_op.u.read.offset = io_off;
            next_io_op.start_time = cur_time;
            next_io_op.codes_op.start_time = cur_time;
            /* set the end time based on observed bandwidth and io size */
            if (rd_bw == 0.0)
                io_op_time = 0.0;
            else
                io_op_time = (io_sz / rd_bw);

            /* update time, accounting for metadata time */
            cur_time += io_op_time;
            next_io_op.end_time = cur_time;
            next_io_op.codes_op.end_time = cur_time;
            file->counters[POSIX_READS]--;
            //printf("end_time, start_time = %f, %f, rd_bw = %f\n", next_io_op.end_time, next_io_op.start_time, rd_bw);
        }
        else
        {
            /* generate a write event */
            next_io_op.codes_op.op_type = CODES_WK_WRITE;
            next_io_op.codes_op.u.write.file_id = file->base_rec.id;
            next_io_op.codes_op.u.write.size = io_sz;
            next_io_op.codes_op.u.write.offset = io_off;
            next_io_op.start_time = cur_time;
            next_io_op.codes_op.start_time = cur_time;

            /* set the end time based on observed bandwidth and io size */
            if (wr_bw == 0.0)
                io_op_time = 0.0;
            else
                io_op_time = (io_sz / wr_bw);

            /* update time, accounting for metadata time */
            cur_time += io_op_time;
            next_io_op.end_time = cur_time;
            next_io_op.codes_op.end_time = cur_time;
            file->counters[POSIX_WRITES]--;
        }
        psx_rw_ops_remaining--;
        io_ops_this_rw--;
        assert(file->counters[POSIX_READS] >= 0);
        assert(file->counters[POSIX_WRITES] >= 0);
        //printf("%s event\n", next_io_op.codes_op.op_type == CODES_WK_WRITE ? "WRITE" : "READ");
        /* store the i/o event */
        darshan_insert_next_io_op(io_context->io_op_dat, &next_io_op);

        /* determine whether to toggle between reads and writes */
        if (!io_ops_this_rw && psx_rw_ops_remaining)
        {
            /* toggle the read/write flag */
            rw ^= 1;
            file->counters[POSIX_RW_SWITCHES]--;

            /* determine how many io ops to do before next rw switch */
            if (!rw)
                io_ops_this_rw = file->counters[POSIX_READS] /
                                 ((file->counters[POSIX_RW_SWITCHES] / 2) + 1);
            else
                io_ops_this_rw = file->counters[POSIX_WRITES] /
                                 ((file->counters[POSIX_RW_SWITCHES] / 2) + 1);
        }

        if (i != (io_ops_this_cycle - 1))
        {
            /* update current time to account for possible delay between i/o operations */
            cur_time += inter_io_delay;
        }
    }

    /* reset the static rw flag if this is the last open-close cycle for this file */
    if (file->counters[POSIX_OPENS] == 1)
    {
        rw = -1;
    }

    return cur_time;
}

static void determine_ind_io_params(
    struct darshan_posix_file *file, int write_flag, size_t *io_sz, off_t *io_off,
    struct rank_io_context *io_context)
{
    int size_bin_ndx = 0;
    int64_t *rd_size_bins = &(file->counters[POSIX_SIZE_READ_0_100]);
    int64_t *wr_size_bins = &(file->counters[POSIX_SIZE_WRITE_0_100]);
    int64_t *size_bins = NULL;
    int64_t *common_accesses = &(file->counters[POSIX_ACCESS1_ACCESS]); /* 4 common accesses */
    int64_t *common_access_counts = &(file->counters[POSIX_ACCESS1_COUNT]); /* common access counts */
    int64_t *total_io_size = NULL;
    int i, j = 0;
    const int64_t size_bin_min_vals[10] = { 0, 100, 1024, 10 * 1024, 100 * 1024, 1024 * 1024,
                                            4 * 1024 * 1024, 10 * 1024 * 1024, 100 * 1024 * 1024,
                                            1024 * 1024 * 1024 };
    const int64_t size_bin_max_vals[10] = { 100, 1024, 10 * 1024, 100 * 1024, 1024 * 1024,
                                            4 * 1024 * 1024, 10 * 1024 * 1024, 100 * 1024 * 1024,
                                            1024 * 1024 * 1024, INT64_MAX };

    /* assign data values depending on whether the operation is a read or write */
    if (write_flag)
    {
        total_io_size = &(file->counters[POSIX_BYTES_WRITTEN]);
        size_bins = wr_size_bins;
    }
    else
    {
        total_io_size = &(file->counters[POSIX_BYTES_READ]);
        size_bins = rd_size_bins;
    }

    for (i = 0; i < 10; i++)
    {
        if (size_bins[i])
        {
            size_bin_ndx = i;
            break;
        }
    }

    *io_sz = 0;
    if (*total_io_size > 0)
    {
        /* try to assign a common access first (intelligently) */
        for (j = 0; j < 4; j++)
        {
            if (common_access_counts[j] &&
                (common_accesses[j] >= size_bin_min_vals[size_bin_ndx]) &&
                (common_accesses[j] <= size_bin_max_vals[size_bin_ndx]))
            {
                *io_sz = common_accesses[j];
                common_access_counts[j]--;
                break;
            }
        }

        /* if no common accesses left, then assign default size for this bin */
        if (*io_sz == 0)
        {
            size_t gen_size;
            gen_size = (size_bin_max_vals[size_bin_ndx] - size_bin_min_vals[size_bin_ndx]) / 2;
            *io_sz = ALIGN_BY_8(gen_size);
        }
        assert(*io_sz);
    }

    *total_io_size -= *io_sz;
    size_bins[size_bin_ndx]--;

    *io_off = io_context->next_off;
    io_context->next_off += *io_sz;

    return;
}

/* calculate the simulated "delay" between different i/o events using delay info
 * from the file counters */
static void calc_io_delays(
    struct darshan_posix_file *file, int64_t num_opens, int64_t num_io_ops, double total_delay,
    double *first_io_delay, double *close_delay, double *inter_open_delay, double *inter_io_delay)
{
    double first_io_time, last_io_time;
    double first_io_pct, close_pct, inter_open_pct, inter_io_pct = 0;
    double total_delay_pct = 0;
    double tmp_inter_io_pct, tmp_inter_open_pct = 0;

    if (total_delay > 0.0)
    {
        /* determine the time of the first io operation */
        if (!file->fcounters[POSIX_F_WRITE_START_TIMESTAMP])
            first_io_time = file->fcounters[POSIX_F_READ_START_TIMESTAMP];
        else if (!file->fcounters[POSIX_F_READ_START_TIMESTAMP])
            first_io_time = file->fcounters[POSIX_F_WRITE_START_TIMESTAMP];
        else if (file->fcounters[POSIX_F_READ_START_TIMESTAMP] <
                 file->fcounters[POSIX_F_WRITE_START_TIMESTAMP])
            first_io_time = file->fcounters[POSIX_F_READ_START_TIMESTAMP];
        else
            first_io_time = file->fcounters[POSIX_F_WRITE_START_TIMESTAMP];

        /* determine the time of the last io operation */
        if (file->fcounters[POSIX_F_READ_END_TIMESTAMP] > file->fcounters[POSIX_F_WRITE_END_TIMESTAMP])
            last_io_time = file->fcounters[POSIX_F_READ_END_TIMESTAMP];
        else
            last_io_time = file->fcounters[POSIX_F_WRITE_END_TIMESTAMP];

        /* no delay contribution for inter-open delay if there is only a single open */
        if (num_opens > 1)
            inter_open_pct = DEF_INTER_CYC_DELAY_PCT;

        /* no delay contribution for inter-io delay if there is one or less io op */
        if ((num_io_ops - num_opens) > 0)
            inter_io_pct = DEF_INTER_IO_DELAY_PCT;

        /* determine delay contribution for first io and close delays */
        if (first_io_time != 0.0)
        {
            first_io_pct = (first_io_time - file->fcounters[POSIX_F_OPEN_START_TIMESTAMP]) *
                           (num_opens / total_delay);
            close_pct = (file->fcounters[POSIX_F_CLOSE_END_TIMESTAMP] - last_io_time) *
                        (num_opens / total_delay);
        }
        else
        {
            first_io_pct = 0.0;
            close_pct = 1 - inter_open_pct;
        }

        /* Safety check; adjust approximation if the percentages are
         * negative.  This can happen if (for example) the same file is
         * opened and closed multiple times.
         */
        if(first_io_pct < 0)
            first_io_pct = 0;
        if(close_pct < 0)
            close_pct = 1 - inter_open_pct;

        /* adjust per open delay percentages using a simple heuristic */
        total_delay_pct = inter_open_pct + inter_io_pct + first_io_pct + close_pct;
        if ((total_delay_pct < 1) && (inter_open_pct || inter_io_pct))
        {
            /* only adjust inter-open and inter-io delays if we underestimate */
            tmp_inter_open_pct = (inter_open_pct / (inter_open_pct + inter_io_pct)) *
                                 (1 - first_io_pct - close_pct);
            tmp_inter_io_pct = (inter_io_pct / (inter_open_pct + inter_io_pct)) *
                               (1 - first_io_pct - close_pct);
            inter_open_pct = tmp_inter_open_pct;
            inter_io_pct = tmp_inter_io_pct;
        }
        else
        {
            inter_open_pct += (inter_open_pct / total_delay_pct) * (1 - total_delay_pct);
            inter_io_pct += (inter_io_pct / total_delay_pct) * (1 - total_delay_pct);
            first_io_pct += (first_io_pct / total_delay_pct) * (1 - total_delay_pct);
            close_pct += (close_pct / total_delay_pct) * (1 - total_delay_pct);
        }

        *first_io_delay = (first_io_pct * total_delay) / num_opens;
        *close_delay = (close_pct * total_delay) / num_opens;

        if (num_opens > 1)
            *inter_open_delay = (inter_open_pct * total_delay) / (num_opens - 1);
        if ((num_io_ops - num_opens) > 0)
            *inter_io_delay = (inter_io_pct * total_delay) / (num_io_ops - num_opens);
    }

    return;
}

/* check to make sure file stats are valid and properly formatted */
static void file_sanity_check(
    struct darshan_posix_file *file, struct darshan_job *job, darshan_fd fd)
{
    int64_t ops_not_implemented;
    int64_t ops_total;

    /* make sure we have log version 3.00 or greater */
    if (strcmp(fd->version, "3.00") < 0)
    {
        fprintf(stderr, "Error: Darshan log version must be >= 3.00 (using %s)\n",
                fd->version);
        exit(EXIT_FAILURE);
    }

    /* these counters should not be negative */
    assert(file->counters[POSIX_OPENS] >= 0);
    //assert(file->counters[POSIX_COLL_OPENS] >= 0);
    assert(file->counters[POSIX_READS] >= 0);
    assert(file->counters[POSIX_WRITES] >= 0);
    assert(file->counters[POSIX_BYTES_READ] >= 0);
    assert(file->counters[POSIX_BYTES_WRITTEN] >= 0);
    assert(file->counters[POSIX_RW_SWITCHES] >= 0);

    /* set any timestamps that happen to be negative to 0 */
    if (file->fcounters[POSIX_F_READ_START_TIMESTAMP] < 0.0)
        file->fcounters[POSIX_F_READ_START_TIMESTAMP] = 0.0;
    if (file->fcounters[POSIX_F_WRITE_START_TIMESTAMP] < 0.0)
        file->fcounters[POSIX_F_WRITE_START_TIMESTAMP] = 0.0;
    if (file->fcounters[POSIX_F_READ_END_TIMESTAMP] < 0.0)
        file->fcounters[POSIX_F_READ_END_TIMESTAMP] = 0.0;
    if (file->fcounters[POSIX_F_WRITE_END_TIMESTAMP] < 0.0)
        file->fcounters[POSIX_F_WRITE_END_TIMESTAMP] = 0.0;

    /* set file close time to the end of execution if it is not given */
    if (file->fcounters[POSIX_F_CLOSE_END_TIMESTAMP] == 0.0)
        file->fcounters[POSIX_F_CLOSE_END_TIMESTAMP] = job->end_time - job->start_time + 1;

    /* collapse fopen/fread/etc. calls into the corresponding open/read/etc. counters */
    /*fopens etc are removed in darshan3.1.3*/
    /*
    file->counters[POSIX_OPENS] += file->counters[POSIX_FOPENS];
    file->counters[POSIX_READS] += file->counters[POSIX_FREADS];
    file->counters[POSIX_WRITES] += file->counters[POSIX_FWRITES];
    file->counters[POSIX_SEEKS] += file->counters[POSIX_FSEEKS];
	*/
    /* reduce total meta time by percentage of ops not currently implemented */
    /* NOTE: we lump fseeks and r/w operations together when setting op durations ... */
    ops_not_implemented = file->counters[POSIX_STATS] + file->counters[POSIX_FSYNCS];
    ops_total = ops_not_implemented + (2 * file->counters[POSIX_OPENS]) +
                file->counters[POSIX_SEEKS];
    file->fcounters[POSIX_F_META_TIME] *= (1 - ((double)ops_not_implemented / ops_total));

    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */