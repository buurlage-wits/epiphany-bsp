/*
File: e_bsp.c

This file is part of the Epiphany BSP library.

Copyright (C) 2014 Buurlage Wits
Support e-mail: <info@buurlagewits.nl>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License (LGPL)
as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
and the GNU Lesser General Public License along with this program,
see the files COPYING and COPYING.LESSER. If not, see
<http://www.gnu.org/licenses/>.
*/

#include "e_bsp.h"
#include <e-lib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

//
// All internal bsp variables for this core
//

typedef struct {
    // ARM core will set this, epiphany will poll this
    volatile int        syncstate;

    int                 pid;
    int                 nprocs;

    // time_passed is epiphany cpu time (so not walltime) in seconds
    volatile float        time_passed;
    volatile unsigned int last_timer_value;

    // counter for ebsp_comm_buf::data_requests[pid]
    unsigned int        request_counter;

    // if this core has done a bsp_push_reg
    int                 var_pushed;

    // message_index is an index into an epiphany<->epiphany queue and
    // when it reached the end, it is an index into the arm->epiphany queue
    unsigned int        tag_size;
    unsigned int        tag_size_next; // next superstep
    unsigned int        queue_index;
    unsigned int        message_index;

    // bsp_sync barrier
    volatile e_barrier_t sync_barrier[_NPROCS];
    e_barrier_t*        sync_barrier_tgt[_NPROCS];

    // Mutex is used for message_queue and data_payloads
    e_mutex_t           payload_mutex;

    // Mutex for ebsp_message
    e_mutex_t           ebsp_message_mutex;
} ebsp_core_data;

ebsp_core_data coredata;
ebsp_comm_buf* const comm_buf = (ebsp_comm_buf*)COMMBUF_EADDR;

// The following variables belong in ebsp_core_data but since
// we do not want to include e-lib.h in common.h (as it is used by host)
// we place these variables here

// All error messages are written here so that we can store
// them in external ram if needed
const char err_pushreg_multiple[] = "BSP ERROR: multiple bsp_push_reg calls within one sync";
const char err_pushreg_overflow[] = "BSP ERROR: Trying to push more than MAX_BSP_VARS vars";
const char err_var_not_found[]    = "BSP ERROR: could not find bsp var. targetpid %d, addr = %p";
const char err_get_overflow[]     = "BSP ERROR: too many bsp_get requests per sync";
const char err_put_overflow[]     = "BSP ERROR: too many bsp_put requests per sync";
const char err_put_overflow2[]    = "BSP ERROR: too large bsp_put payload per sync";
const char err_send_overflow[]    = "BSP ERROR: too many bsp_send requests per sync";

void _write_syncstate(int state);
int row_from_pid(int pid);
int col_from_pid(int pid);

void bsp_begin()
{
    int row = e_group_config.core_row;
    int col = e_group_config.core_col;
    int cols = e_group_config.group_cols;

    // Initialize local data
    coredata.pid = col + cols * row;
    coredata.nprocs = comm_buf->nprocs;
    coredata.request_counter = 0;
    coredata.var_pushed = 0;
    coredata.tag_size = comm_buf->initial_tagsize;
    coredata.tag_size_next = coredata.tag_size;
    coredata.queue_index = 0;
    coredata.message_index = 0;

    // Initialize the barrier used during syncs
    e_barrier_init(coredata.sync_barrier, coredata.sync_barrier_tgt);

    // Initialize the mutex for bsp_put, bsp_send
    e_mutex_init(0, 0, &coredata.payload_mutex, MUTEXATTR_NULL);

    // Mutex for ebsp_message
    e_mutex_init(0, 0, &coredata.ebsp_message_mutex, MUTEXATTR_NULL);

    // Send &syncstate to ARM
    if (coredata.pid == 0)
        comm_buf->syncstate_ptr = (int*)&coredata.syncstate;

#ifdef DEBUG
    // Wait for ARM before starting
    _write_syncstate(STATE_INIT);
    while (coredata.syncstate != STATE_CONTINUE) {}
#endif
    _write_syncstate(STATE_RUN);

    // Initialize epiphany timer
    coredata.time_passed = 0.0f;
    e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);
    coredata.last_timer_value = e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);
}

void bsp_end()
{
    _write_syncstate(STATE_FINISH);
    // Finish execution
    __asm__("trap 3");
}

int bsp_nprocs()
{
    return coredata.nprocs;
}


int bsp_pid()
{
    return coredata.pid;
}

float bsp_time()
{
    // TODO: Add timer overhead the calculation
    unsigned int cur_time = e_ctimer_get(E_CTIMER_0);
    coredata.time_passed += (coredata.last_timer_value - cur_time) / CLOCKSPEED;
    e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);
    // Tested: between setting E_CTIMER_MAX and 
    // reading the timer, it decreased by 23 clockcycles
    coredata.last_timer_value = e_ctimer_get(E_CTIMER_0);
    //coredata.last_timer_value = cur_time;

#ifdef DEBUG
    if (cur_time == 0)
        return -1.0f;
#endif
    return coredata.time_passed;
}

float bsp_remote_time()
{
    return comm_buf->remotetimer;
}

// Sync
void bsp_sync()
{
    int i;
    ebsp_data_request* reqs = &comm_buf->data_requests[coredata.pid][0];

    // First handle all bsp_get requests
    // Then handle all bsp_put requests (because of bsp specifications)
    // They are stored in the same list and recognized by the
    // highest bit of nbytes

    e_barrier(coredata.sync_barrier, coredata.sync_barrier_tgt);
    for (i = 0; i < coredata.request_counter; ++i)
    {
        // Check if this is a get
        if ((reqs[i].nbytes & DATA_PUT_BIT) == 0)
            memcpy(reqs[i].dst,
                    reqs[i].src,
                    reqs[i].nbytes & ~DATA_PUT_BIT);
    }
    e_barrier(coredata.sync_barrier, coredata.sync_barrier_tgt);
    for (i = 0; i < coredata.request_counter; ++i)
    {
        // Check if this is a put
        if ((reqs[i].nbytes & DATA_PUT_BIT) != 0)
            memcpy(reqs[i].dst,
                    reqs[i].src,
                    reqs[i].nbytes & ~DATA_PUT_BIT);
    }
    coredata.request_counter = 0;

    // This can be done at any point during the sync
    // (as long as it is after the first barrier so all cores are syncing)
    // and only one core needs to set this, but this also works
    comm_buf->data_payloads.buffer_size = 0;

    if (coredata.var_pushed)
    {
        coredata.var_pushed = 0;
        if (coredata.pid == 0)
            comm_buf->bsp_var_counter++;
    }

    // Switch queue between 0 and 1
    coredata.queue_index++;
    if (coredata.queue_index == 2)
        coredata.queue_index = 0;

    coredata.tag_size = coredata.tag_size_next;
    coredata.message_index = 0;

    // Synchronize with host
    //_write_syncstate(STATE_SYNC);
    //while (coredata.syncstate != STATE_CONTINUE) {}
    //_write_syncstate(STATE_RUN);
    e_barrier(coredata.sync_barrier, coredata.sync_barrier_tgt);
}

void _write_syncstate(int state)
{
    coredata.syncstate = state; // local variable
    comm_buf->syncstate[coredata.pid] = state; // being polled by ARM
}

void bsp_push_reg(const void* variable, const int nbytes)
{
    if (coredata.var_pushed)
        return ebsp_message(err_pushreg_multiple);

    if (comm_buf->bsp_var_counter == MAX_BSP_VARS)
        return ebsp_message(err_pushreg_overflow);

    comm_buf->bsp_var_list[comm_buf->bsp_var_counter][coredata.pid] =
        (void*)variable;

    coredata.var_pushed = 1;
}

int row_from_pid(int pid)
{
    return pid / e_group_config.group_cols;
}

int col_from_pid(int pid)
{
    return pid % e_group_config.group_cols;
}

// This incoroporates the bsp_var_list as well as
// the epiphany global address system
// The resulting address can be written to directly
void* _get_remote_addr(int pid, const void *addr, int offset)
{
    // Find the slot for our local pid
    // And return the entry for the remote pid including the epiphany mapping
    int slot;
    for(slot = 0; slot < MAX_BSP_VARS; ++slot)
        if (comm_buf->bsp_var_list[slot][coredata.pid] == addr)
            return e_get_global_address(row_from_pid(pid),
                    col_from_pid(pid),
                    (void*)((int)comm_buf->bsp_var_list[slot][pid] + offset));
    ebsp_message(err_var_not_found, pid, addr);
    return 0;
}

void bsp_put(int pid, const void *src, void *dst, int offset, int nbytes)
{
    // Check if we can store the request
    if (coredata.request_counter >= MAX_DATA_REQUESTS)
        return ebsp_message(err_put_overflow);

    // Find remote address
    void* dst_remote = _get_remote_addr(pid, dst, offset);
    if (!dst_remote) return;

    // Check if we can store the payload
    // A mutex is needed for this.
    // While holding the mutex this core checks if it can store
    // the payload and if so, updates the buffer
    // Note that the mutex is NOT held while writing the payload itself
    // A possible error message is given after unlocking
    unsigned int payload_offset;

    e_mutex_lock(0, 0, &coredata.payload_mutex);

    payload_offset = comm_buf->data_payloads.buffer_size;

    if (payload_offset + nbytes > MAX_PAYLOAD_SIZE)
        payload_offset = -1;
    else
        comm_buf->data_payloads.buffer_size += nbytes;

    e_mutex_unlock(0, 0, &coredata.payload_mutex);

    if (payload_offset == -1)
        return ebsp_message(err_put_overflow2);

    // We are now ready to save the request and payload
    void* payload_ptr = &comm_buf->data_payloads.buf[payload_offset];

    // TODO: Measure if e_dma_copy is faster here for both request and payload

    // Save request
    ebsp_data_request* req = &comm_buf->data_requests[coredata.pid][coredata.request_counter];
    req->src = payload_ptr;
    req->dst = dst_remote;
    req->nbytes = nbytes | DATA_PUT_BIT;
    coredata.request_counter++;

    // Save payload
    memcpy(payload_ptr, src, nbytes);
}

void bsp_hpput(int pid, const void *src, void *dst, int offset, int nbytes)
{
    void* dst_remote = _get_remote_addr(pid, dst, offset);
    if (!dst_remote) return;
    memcpy(dst_remote, src, nbytes);
}

void bsp_get(int pid, const void *src, int offset, void *dst, int nbytes)
{
    if (coredata.request_counter >= MAX_DATA_REQUESTS)
        return ebsp_message(err_get_overflow);
    const void* src_remote = _get_remote_addr(pid, src, offset);
    if (!src_remote) return;

    ebsp_data_request* req = &comm_buf->data_requests[coredata.pid][coredata.request_counter];
    req->src = src_remote;
    req->dst = dst;
    req->nbytes = nbytes;
    coredata.request_counter++;
}

void bsp_hpget(int pid, const void *src, int offset, void *dst, int nbytes)
{
    const void* src_remote = _get_remote_addr(pid, src, offset);
    if (!src_remote) return;
    memcpy(dst, src_remote, nbytes);
}

void bsp_set_tagsize(int *tag_bytes)
{
    coredata.tag_size_next = *tag_bytes;
    *tag_bytes = coredata.tag_size;
}

void bsp_send(int pid, const void *tag, const void *payload, int nbytes)
{
    unsigned int index;
    unsigned int payload_offset;

    ebsp_message_queue* q = &comm_buf->message_queue[coredata.queue_index];

    e_mutex_lock(0, 0, &coredata.payload_mutex);

    index = q->count;
    payload_offset = comm_buf->data_payloads.buffer_size;

    if ((payload_offset + coredata.tag_size + nbytes > MAX_PAYLOAD_SIZE)
            || (index >= MAX_MESSAGES))
    {
        index = -1;
        payload_offset = -1;
    }
    else
    {
        q->count++;
        comm_buf->data_payloads.buffer_size += nbytes;
    }

    e_mutex_unlock(0, 0, &coredata.payload_mutex);

    if (index == -1)
        return ebsp_message(err_send_overflow);

    // We are now ready to save the request and payload
    void* tag_ptr = &comm_buf->data_payloads.buf[payload_offset];
    payload_offset += coredata.tag_size;
    void* payload_ptr = &comm_buf->data_payloads.buf[payload_offset];

    q->message[index].pid = pid;
    q->message[index].tag = tag_ptr;
    q->message[index].payload = payload_ptr;
    q->message[index].nbytes = nbytes;

    memcpy(tag_ptr, tag, coredata.tag_size);
    memcpy(payload_ptr, payload, nbytes);
}

// Gets the next message from the queue, does not pop
// Returns 0 if no message
// Checks both epiphany<->epiphany queue
// and ARM->epiphany queue
ebsp_message_header* _next_queue_message()
{
    ebsp_message_queue* q = &comm_buf->message_queue[coredata.queue_index];
    int qsize = q->count;

    // currently searching at message_index
    for (; coredata.message_index < qsize; coredata.message_index++)
    {
        if (q->message[coredata.message_index].pid != coredata.pid)
            continue;
        return &q->message[coredata.message_index];
    }
    return 0;
}

void _pop_queue_message()
{
    coredata.message_index++;
}

void bsp_qsize(int *packets, int *accum_bytes)
{
    *packets = 0;
    *accum_bytes = 0;

    ebsp_message_queue* q = &comm_buf->message_queue[coredata.queue_index];
    int qsize = q->count;

    // currently searching at message_index
    for (; coredata.message_index < qsize; coredata.message_index++)
    {
        if (q->message[coredata.message_index].pid != coredata.pid)
            continue;
        *packets++;
        *accum_bytes += q->message[coredata.message_index].nbytes;
    }
    return;
}

void bsp_get_tag(int *status, void *tag)
{
    ebsp_message_header* m = _next_queue_message();
    if (m == 0)
    {
        *status = -1;
        return;
    }
    *status = m->nbytes;
    memcpy(tag, m->tag, coredata.tag_size);
}

void bsp_move(void *payload, int buffer_size)
{
    ebsp_message_header* m = _next_queue_message();
    _pop_queue_message();
    if (m == 0)
    {
        // This part is not defined by the BSP standard
        return;
    }

    if (buffer_size == 0) // Specified by BSP standard
        return;

    if (m->nbytes < buffer_size)
        buffer_size = m->nbytes;

    memcpy(payload, m->payload, buffer_size);
}

int bsp_hpmove(void **tag_ptr_buf, void **payload_ptr_buf)
{
    ebsp_message_header* m = _next_queue_message();
    _pop_queue_message();
    if (m == 0) return -1;
    *tag_ptr_buf = m->tag;
    *payload_ptr_buf = m->payload;
    return m->nbytes;
}

void ebsp_message(const char* format, ... )
{
    // Write the message to a buffer
    char buf[128];
    va_list args;
    va_start(args, format);
    vsnprintf(&buf[0], sizeof(buf), format, args);
    va_end(args);

    // Lock mutex
    e_mutex_lock(0, 0, &coredata.ebsp_message_mutex);
    // Write the message
    memcpy(&comm_buf->msgbuf[0], &buf[0], sizeof(buf));
    comm_buf->msgflag = coredata.pid+1;
    // Wait for it to be printed
    while(comm_buf->msgflag != 0){}
    // Unlock mutex
    e_mutex_unlock(0, 0, &coredata.ebsp_message_mutex);
}

