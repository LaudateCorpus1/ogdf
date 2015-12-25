/*===========================================================================*/
/*                                                                           */
/* This file is part of the SYMPHONY MILP Solver Framework.                  */
/*                                                                           */
/* SYMPHONY was jointly developed by Ted Ralphs (ted@lehigh.edu) and         */
/* Laci Ladanyi (ladanyi@us.ibm.com).                                        */
/*                                                                           */
/* (c) Copyright 2000-2011 Ted Ralphs. All Rights Reserved.                  */
/*                                                                           */
/* This software is licensed under the Eclipse Public License. Please see    */
/* accompanying file for terms.                                              */
/*                                                                           */
/*===========================================================================*/

#include <stdlib.h>
#include <memory.h>
#include <math.h>
#if !defined (_MSC_VER)
#include <unistd.h>            /* this defines sleep() */
#endif

#include "sym_lp.h"
#include "sym_timemeas.h"
#include "sym_proccomm.h"
#include "sym_constants.h"
#include "sym_macros.h"
#include "sym_types.h"
#include "sym_messages.h"
#include "sym_pack_cut.h"
#include "sym_pack_array.h"
#include "sym_lp_solver.h"

/*===========================================================================*/

/*===========================================================================*\
 * This file contains the functions related to LP process communication.
\*===========================================================================*/

/*---------------------------------------------------------------------------*\
 * This little bit of code checks to see whether a new upper bound has
 * been received
\*---------------------------------------------------------------------------*/

void check_ub(lp_prob* p)
{
#ifdef COMPILE_IN_LP
    if(p->tm->has_ub)
    {
        p->has_ub = TRUE;
        p->ub = p->tm->ub;
    }
#else
    int r_bufid  = 0;
    r_bufid = nreceive_msg(ANYONE, UPPER_BOUND);
    if(r_bufid)
    {
        lp_process_ub_message(p);
        freebuf(r_bufid);
    }
#endif
}

/*===========================================================================*/

/*===========================================================================*\
 * This function processes the messages arriving to the LP. It is invoked
 * first from
 * This function returns TRUE only if an ACTIVE_NODE_DATA has been received
 * and it will be processed (i.e., not too expensive, or has to be priced)
\*===========================================================================*/

int process_message(lp_prob* p, int r_bufid, int* pindex, int* pitnum)
{
    int s_bufid, bytes, msgtag, sender;
    int i, cut_pool_cuts, new_row_num;
    cut_data* cut;
    double cut_time;
    waiting_row** wrows = p->waiting_rows, **new_rows;

    if(! r_bufid)
    {
        if(pstat(p->tree_manager) == PROCESS_OK)
            /* PROCESS_OK, it's a long wait, but carry on */
            return(FALSE);
        /* Oops, TM died. We should comit harakiri. */
        printf("TM has died -- LP exiting\n\n");
        msgtag = YOU_CAN_DIE;
    }
    else
    {
        /* Get the info for real messages */
        bufinfo(r_bufid, &bytes, &msgtag, &sender);
    }

    switch(msgtag)
    {
    case PACKED_CUT:
        /* receive a packed cut and add it to the current LP */
        cut = unpack_cut(NULL);

        if(pindex)   /* we are receiving cuts */
        {
            unpack_cuts_u(p, (sender == p->cut_pool ? CUT_FROM_CP : CUT_FROM_CG),
                          UNPACK_CUTS_MULTIPLE, 1, &cut, &new_row_num, &new_rows);
            if(new_row_num)
            {
                new_rows[0]->source_pid =
                    ((sender == p->cut_pool) ? EXTERNAL_CUT_POOL : EXTERNAL_CUT_GEN);
                for(i = p->waiting_row_num - 1; i >= 0; i--)
                {
                    if(same_cuts_u(p, wrows[i], new_rows[0]) !=
                            DIFFERENT_CUTS)
                    {
                        free_waiting_row(new_rows);
                        break;
                    }
                }
                if(i < 0)
                {
                    add_new_rows_to_waiting_rows(p, new_rows, new_row_num);
                }
                FREE(new_rows);
            }
        }
        else
        {
            /* a cut has arrived when we are waiting for a new LP. Store it
            among the waiting rows */
            REALLOC(p->waiting_rows, waiting_row*, p->waiting_rows_size,
                    p->waiting_row_num + 1, BB_BUNCH);
            p->waiting_rows[p->waiting_row_num] =
                (waiting_row*) calloc(1, sizeof(waiting_row));
            p->waiting_rows[p->waiting_row_num]->source_pid = sender;
            p->waiting_rows[p->waiting_row_num++]->cut = cut;
        }
        return(FALSE);

    case NO_MORE_CUTS:
        /* this message type says that all cuts generated by the current
        LP solution have been received and hence calculation can resume */
        receive_int_array(&cut_pool_cuts, 1);
        receive_dbl_array(&cut_time, 1);
        p->comp_times.cut_pool += cut_time;
        if(pindex)
        {
            receive_int_array(pindex, 1);
            receive_int_array(pitnum, 1);
        }
        return(FALSE);

    case UPPER_BOUND:
        lp_process_ub_message(p);
        return(FALSE);

    case LP__ACTIVE_NODE_DATA:
#ifdef DO_TESTS
        if(pindex)
        {
            printf("Error: ACTIVE_NODE_DATA arrived in receive_cuts()!!!\n\n");
            exit(-2);
        }
#endif
        return(receive_active_node(p));

    case LP__SECOND_PHASE_STARTS:
        /* Send back the timing data for the first phase */
        s_bufid = init_send(DataInPlace);
        send_char_array((char*)&p->comp_times, sizeof(node_times));
        send_char_array((char*)&p->lp_stat, sizeof(lp_stat_desc));
        send_msg(p->tree_manager, LP__TIMING);
#ifdef DO_TESTS
        if(pindex)
        {
            printf("Error: SECOND_PHASE_STARTS arrived in receive_cuts!!!\n\n");
            exit(-2);
        }
#endif
        p->phase = 1;
        return(FALSE);

    case YOU_CAN_DIE:
#if defined(COMPILE_IN_TM) && !defined(COMPILE_IN_LP) && 0
        /* This is not needed anymore */
        send_feasible_solution_u(p, p->best_sol.xlevel, p->best_sol.xindex,
                                 p->best_sol.xiter_num, p->best_sol.lpetol,
                                 p->best_sol.objval, p->best_sol.xlength,
                                 p->best_sol.xind, p->best_sol.xval);
        FREE(p->best_sol.xind);
        FREE(p->best_sol.xval);
#endif
        p->comp_times.communication += used_time(&p->tt);
        freebuf(r_bufid);
        lp_close(p);
        comm_exit();
        exit(0);

    default:
        printf("Unknown message type!! (%i)\n", msgtag);
        return(FALSE);
    }

    return(FALSE); /* fake return */
}

/*===========================================================================*/

void lp_process_ub_message(lp_prob* p)
{
    double new_ub;

    receive_dbl_array(&new_ub, 1);
    if(!p->has_ub || (p->has_ub && (new_ub < p->ub)))
    {
        p->has_ub = TRUE;
        p->ub = new_ub;
        if(p->par.set_obj_upper_lim)
            set_obj_upper_lim(p->lp_data, p->ub - p->par.granularity);
    }
}

/*===========================================================================*/

int receive_active_node(lp_prob* p)
{
    int i, s_bufid;
    node_desc* desc;
    char ch;

    desc = p->desc = (node_desc*) malloc(sizeof(node_desc));

    receive_int_array(&p->cut_pool, 1);
    receive_int_array(&p->bc_index, 1);
    receive_int_array(&p->bc_level, 1);
    receive_dbl_array(&p->lp_data->objval, 1);
    receive_int_array(&p->colgen_strategy, 1);
    receive_int_array(&desc->nf_status, 1);

    if(!(p->colgen_strategy & COLGEN_REPRICING) &&
            p->has_ub && p->lp_data->objval > p->ub - p->par.granularity)
    {
        if(desc->nf_status == NF_CHECK_NOTHING ||
                (p->colgen_strategy & FATHOM__DO_NOT_GENERATE_COLS__DISCARD))
        {
            s_bufid = init_send(DataInPlace);
            send_msg(p->tree_manager, LP__NODE_DISCARDED);
            if(p->par.verbosity > 0)
            {
                printf("****************************************************\n");
                printf("* Immediately pruning NODE %i LEVEL %i\n",
                       p->bc_index, p->bc_level);
                printf("****************************************************\n");
            }
            FREE(p->desc);
            return(FALSE);
        }
        if(p->colgen_strategy & FATHOM__DO_NOT_GENERATE_COLS__SEND)
        {
            s_bufid = init_send(DataInPlace);
            send_msg(p->tree_manager, LP__NODE_RESHELVED);
            if(p->par.verbosity > 0)
            {
                printf("****************************************************\n");
                printf("* Sending back NODE %i LEVEL %i\n",
                       p->bc_index, p->bc_level);
                printf("****************************************************\n");
            }
            FREE(p->desc);
            return(FALSE);
        }
    }

    /*------------------------------------------------------------------------*\
     * EXPLICIT_LIST  must arrive everywhere where list arrives,
     * -- except -- for basis, which might be NO_DATA_ARE_STORED
    \*------------------------------------------------------------------------*/
    unpack_basis(&desc->basis, TRUE);
    if(desc->nf_status == NF_CHECK_AFTER_LAST ||
            desc->nf_status == NF_CHECK_UNTIL_LAST)
        unpack_array_desc(&desc->not_fixed);
    unpack_array_desc(&desc->uind);
    unpack_array_desc(&desc->cutind);

#ifdef DO_TESTS
    for(i = 1; i < desc->uind.size; i++)
        if(desc->uind.list[i] <= desc->uind.list[i - 1])
        {
            printf("\nProblems creating uind list! Exiting now.\n\n");
            exit(-129);
        }

    for(i = 1; i < desc->cutind.size; i++)
        if(desc->cutind.list[i] <= desc->cutind.list[i - 1])
        {
            printf("\nProblems creating cutind list! Exiting now.\n\n");
            exit(-129);
        }
#endif

    if(desc->cutind.size > 0)
    {
        desc->cuts = (cut_data**) malloc(desc->cutind.size * sizeof(cut_data*));
        for(i = 0; i < desc->cutind.size; i++)
            desc->cuts[i] = unpack_cut(NULL);
    }

    /*------------------------------------------------------------------------*\
     * Unpack the chain of branching information
    \*------------------------------------------------------------------------*/
    if(p->bc_level > 0)
    {
        REMALLOC(p->bdesc, branch_desc, p->bdesc_size, p->bc_level, BB_BUNCH);
        receive_char_array((char*)p->bdesc,
                           p->bc_level * (int)sizeof(branch_desc));
    }

    receive_char_array(&ch, 1);
    p->dive = (int) ch;

    /*------------------------------------------------------------------------*\
     * Unpack the user defined description
    \*------------------------------------------------------------------------*/
    receive_int_array(&desc->desc_size, 1);
    if(desc->desc_size > 0)
    {
        desc->desc = (char*) malloc(desc->desc_size * CSIZE);
        receive_char_array(desc->desc, desc->desc_size);
    }

    return(TRUE);
}

/*===========================================================================*/

int receive_cuts(lp_prob* p, int first_lp, int no_more_cuts_count)
{
    LPdata* lp_data = p->lp_data;
    int r_bufid, bytes, msgtag, sender;
    double start, timeout, diff;
    double first_cut_time_out, all_cuts_time_out;
    struct timeval tvtimeout, *ptimeout;
    int added_rows, old_waiting_row_num;
    int bc_index, itnum;
    int termcode = 0;

    PRINT(p->par.verbosity, 3, ("Receiving/creating cuts...\n"));

    /*------------------------------------------------------------------------*\
     * Test whether the rows in p->waiting_rows are still valid, i.e., whether
     * the column set of the matrix has changed.
     * For the rows where they are not valid, regenerate the rows by
     * calling unpack_cuts_u().
     * After that compute the violations for every row.
    \*------------------------------------------------------------------------*/
    if(p->waiting_row_num > 0)
    {
        if(lp_data->col_set_changed)
        {
            /* regenerate the rows */
            int i;
            int           new_row_num,    wrnum = p->waiting_row_num;
            waiting_row** new_rows,     **wrows = p->waiting_rows;
            cut_data** cuts;

            PRINT(p->par.verbosity, 10, ("Regenerating rows in waiting_rows.\n"));
            cuts = (cut_data**) lp_data->tmp.p1;  /* m */
            for(i = wrnum - 1; i >= 0; i--)
            {
                cuts[i] = wrows[i]->cut;
                wrows[i]->cut = NULL;
            }
            free_waiting_rows(p->waiting_rows, p->waiting_row_num);

            unpack_cuts_u(p, CUT_LEFTOVER, UNPACK_CUTS_MULTIPLE,
                          wrnum, cuts, &new_row_num, &new_rows);
            p->waiting_row_num = new_row_num;
            if(new_row_num > 0)
            {
                /* for 'why MAXINT' see comment in
                   order_waiting_rows_based_on_sender() */
                for(i = new_row_num - 1; i >= 0; --i)
                    new_rows[i]->source_pid = LEFTOVER;
                memcpy(p->waiting_rows, new_rows,
                       new_row_num * sizeof(waiting_row*));
                free(new_rows);
            }
        }
        /* calculate the violations */
        p->waiting_row_num =
            compute_violations(p, p->waiting_row_num, p->waiting_rows);

        PRINT(p->par.verbosity, 4,
              ("   Cuts in local pool: %i\n", p->waiting_row_num));
    }

    p->comp_times.lp += used_time(&p->tt);

    /* Generate cuts within the LP process if needed. Post-processing in the
     * ..._u function vill check whether the new cuts are distinct from the old
     * ones. (Computing the violations is left to the user! After all, she just
     * generated these cuts, she must have tested whether the violation is
     * positive, i.e., she must knew the violations.) Also, the generated cuts
     * are added to the list of waiting rows. */
    CALL_WRAPPER_FUNCTION(generate_cuts_in_lp_u(p));
    p->comp_times.separation += used_time(&p->tt);
    if(no_more_cuts_count > 0)
    {
        /* Receive cuts if we have sent out the lp solution somewhere. */

        if(first_lp)
        {
            first_cut_time_out = p->par.first_lp.first_cut_time_out;
            all_cuts_time_out = p->par.first_lp.all_cuts_time_out;
        }
        else
        {
            first_cut_time_out = p->par.later_lp.first_cut_time_out;
            all_cuts_time_out = p->par.later_lp.all_cuts_time_out;
        }

        timeout = (old_waiting_row_num = p->waiting_row_num) == 0 ?
                  first_cut_time_out : all_cuts_time_out;
        DBLTOTV(timeout, tvtimeout);
        ptimeout = timeout ? (&tvtimeout) : NULL;

        start = wall_clock(NULL);
        while(TRUE)
        {
            r_bufid = treceive_msg(ANYONE, ANYTHING, ptimeout);
            if(! r_bufid)
            {
                /* Check that TM is still alive */
                if(pstat(p->tree_manager) != PROCESS_OK)
                {
                    printf("TM has died -- LP exiting\n\n");
                    exit(-302);
                }
                /* Message queue is empty and we have waited enough, so exit */
                if(old_waiting_row_num == p->waiting_row_num)
                {
                    PRINT(p->par.verbosity, 1,
                          ("   Receive cuts timed out after %.3f seconds\n",
                           first_cut_time_out));
                }
                else
                {
                    PRINT(p->par.verbosity, 1,
                          ("   Receive cuts timed out after %.3f seconds\n",
                           all_cuts_time_out));
                }
                break;
            }

            bufinfo(r_bufid, &bytes, &msgtag, &sender);
            process_message(p, r_bufid, &bc_index, &itnum);
            freebuf(r_bufid);

            if(msgtag == NO_MORE_CUTS &&
                    bc_index == p->bc_index && itnum == p->iter_num)
                no_more_cuts_count--;
            if(!no_more_cuts_count)
            {
                /* If we have already received enough NO_MORE_CUTS then (since
                   there is nothing in the message queue, we exit. */
                break;
            }

            /* Reset timeout */
            timeout = (p->waiting_row_num == old_waiting_row_num) ?
                      first_cut_time_out : all_cuts_time_out;
            DBLTOTV(timeout, tvtimeout);
            ptimeout = timeout ? (&tvtimeout) : NULL;
            if(ptimeout)
            {
                diff = wall_clock(NULL) - start;
                if(diff > timeout)
                {
                    /* we have waited enough */
                    break;
                }
                timeout -= diff;
            }
        }
        p->comp_times.idle_cuts += wall_clock(NULL) - start;

        if(p->cut_gen && (pstat(p->cut_gen) != PROCESS_OK))
        {
            /* Before declaring death check that maybe we have to die! Wait for
               that message a few seconds, though */
            tvtimeout.tv_sec = 15;
            tvtimeout.tv_usec = 0;
            r_bufid = treceive_msg(ANYONE, YOU_CAN_DIE, &tvtimeout);
            if(! r_bufid)
            {
                /* well, the sym_cg.has really died and the TM did not send a you can
                   die message to us. Just comit harakiri. */
                printf("   Cut generator died -- halting machine\n\n");
                lp_exit(p);
            }
            else
            {
                /* Hah! we got to die. process the message. */
                process_message(p, r_bufid, NULL, NULL);
            }
        }
        else if(p->cut_pool && (pstat(p->cut_pool) != PROCESS_OK))
        {
            /* Before declaring death check that maybe we have to die! Wait for
               that message a few seconds, though */
            tvtimeout.tv_sec = 15;
            tvtimeout.tv_usec = 0;
            r_bufid = treceive_msg(ANYONE, YOU_CAN_DIE, &tvtimeout);
            if(! r_bufid)
            {
                /* well, the sym_cp.has really died and the TM did not send a you can
                   die message to us. Just comit harakiri. */
                printf("   Cut Pool died -- halting machine\n\n");
                lp_exit(p);
            }
            else
            {
                /* Hah! we got to die. process the message. */
                process_message(p, r_bufid, NULL, NULL);
            }
        }
    } /* endif   (no_more_cuts_count > 0) */

    PRINT(p->par.verbosity, 3, ("\nCuts in the local pool: %i\n\n",
                                p->waiting_row_num));

    p->comp_times.communication += used_time(&p->tt);

    if(p->waiting_row_num == 0)
        return(0);

    order_waiting_rows_based_on_sender(p);
    added_rows = add_best_waiting_rows(p);
    purge_waiting_rows_u(p);
    lp_data->col_set_changed = FALSE;
    return(added_rows);
}

/*****************************************************************************
 *****************************************************************************
 *                                                                           *
 *                        Now the outgoing messages                          *
 *                                                                           *
 *****************************************************************************
 *****************************************************************************/

#define MAKE_TM_ARRAY_DESC(newad, oldad)                                    \
if (newad.size > 0){                                                        \
   newad.list = (int *) malloc(newad.size*ISIZE);                           \
   if (lp_data->tmp.i1[0] >= 0)                                             \
      memcpy((char *)newad.list, lp_data->tmp.i1, newad.size*ISIZE);        \
   else                                                                     \
      memcpy((char *)newad.list, oldad.list, newad.size*ISIZE);             \
   if (newad.type == EXPLICIT_LIST)                                         \
      newad.added = newad.size;                                             \
}

/*===========================================================================*/

void send_node_desc(lp_prob* p, int node_type)
{
    node_desc* new_lp_desc = NULL, *new_tm_desc = NULL;
    node_desc* lp_desc = p->desc;
    char repricing = (p->colgen_strategy & COLGEN_REPRICING) ? 1 : 0;
    int deal_with_nf;

    LPdata* lp_data = p->lp_data;

#ifdef COMPILE_IN_LP
    tm_prob* tm = p->tm;
    bc_node* n = repricing ? (bc_node*) calloc(1, sizeof(bc_node)) :
                 tm->active_nodes[p->proc_index];
    node_desc* tm_desc = &n->desc;

    if(p->bc_level > 0)
    {
        n->num_cut_iters_in_path =
            p->lp_stat.num_cut_iters_in_path;
        n->num_cuts_added_in_path =
            p->lp_stat.num_cuts_added_in_path;
        n->num_cuts_slacked_out_in_path =
            p->lp_stat.num_cuts_slacked_out_in_path;
        n->avg_cuts_obj_impr_in_path =
            p->lp_stat.avg_cuts_obj_impr_in_path;

        n->avg_br_obj_impr_in_path =
            p->lp_stat.avg_br_obj_impr_in_path;

    }
    else
    {
        n->num_cut_iters_in_path = 0;
        n->num_cuts_added_in_path = 0;
        n->num_cuts_slacked_out_in_path = 0;
        n->avg_cuts_obj_impr_in_path = 0;

        n->num_str_br_cands_in_path = 0;
        n->avg_br_obj_impr_in_path = 0;

        n->num_fp_calls_in_path = 0;
    }

    n->start_objval = p->lp_stat.start_objval;
    n->end_objval = p->lp_stat.end_objval;
    n->num_str_br_cands_in_path =
        p->lp_stat.num_str_br_cands_in_path;
    n->num_fp_calls_in_path =
        p->lp_stat.num_fp_calls_in_path;

#else
    int s_bufid;
    char ch;
#endif

#ifdef SENSITIVITY_ANALYSIS
    if(tm->par.sensitivity_analysis &&
            !(node_type == INFEASIBLE_PRUNED || node_type == DISCARDED_NODE))
    {
        if(n->duals)
        {
            FREE(n->duals);
        }
        n->duals = (double*) malloc(DSIZE * p->base.cutnum);
        memcpy(n->duals, lp_data->dualsol, DSIZE * p->base.cutnum);
    }
#endif

#ifdef COMPILE_IN_LP
    int* indices;
    double* values;
    if(node_type == INFEASIBLE_PRUNED || node_type == OVER_UB_PRUNED ||
            node_type == DISCARDED_NODE || node_type == FEASIBLE_PRUNED)
    {

        n->node_status = NODE_STATUS__PRUNED;

        if(tm->par.keep_description_of_pruned == KEEP_IN_MEMORY)
        {
            if(node_type == INFEASIBLE_PRUNED || node_type == DISCARDED_NODE)
            {
                if(n->feasibility_status != NOT_PRUNED_HAS_CAN_SOLUTION)
                {
                    n->feasibility_status = INFEASIBLE_PRUNED;
                }
            }
            if(node_type == FEASIBLE_PRUNED)
            {
                indices = lp_data->tmp.i1;
                values = lp_data->tmp.d;

                n->sol_size = collect_nonzeros(p, lp_data->x, indices, values);
                n->sol_ind = (int*) malloc(ISIZE * n->sol_size);
                n->sol = (double*) malloc(DSIZE * n->sol_size);
                memcpy(n->sol, values, DSIZE * n->sol_size);
                memcpy(n->sol_ind, indices, ISIZE * n->sol_size);
                n->feasibility_status = FEASIBLE_PRUNED;
            }

            if(node_type == OVER_UB_PRUNED)
            {
                n->feasibility_status = OVER_UB_PRUNED;
                if(n->feasibility_status == NOT_PRUNED_HAS_CAN_SOLUTION)
                {
                    n->feasibility_status = FEASIBLE_PRUNED;
                }
            }
        }

#ifdef TRACE_PATH
        if(n->optimal_path)
        {
            printf("\n\nAttempting to prune the optimal path!!!!!!!!!\n\n");
            sleep(600);
            if(tm->par.logging)
            {
                write_tm_info(tm, tm->par.tree_log_file_name, NULL, FALSE);
                write_subtree(tm->rootnode, tm->par.tree_log_file_name, NULL,
                              TRUE, tm->par.logging);
                write_tm_cut_list(tm, tm->par.cut_log_file_name, FALSE);
            }
            exit(-10);
        }
#endif
        if(tm->par.keep_description_of_pruned == KEEP_ON_DISK_VBC_TOOL)
            #pragma omp critical (write_pruned_node_file)
            write_pruned_nodes(tm, n);
        if(tm->par.keep_description_of_pruned == DISCARD ||
                tm->par.keep_description_of_pruned == KEEP_ON_DISK_VBC_TOOL)
        {
            if(tm->par.vbc_emulation == VBC_EMULATION_FILE_NEW)
            {
                int vbc_node_pr_reason;
                switch(node_type)
                {
                case INFEASIBLE_PRUNED:
                    vbc_node_pr_reason = VBC_PRUNED_INFEASIBLE;
                    break;
                case OVER_UB_PRUNED:
                    vbc_node_pr_reason = VBC_PRUNED_FATHOMED;
                    break;
                case FEASIBLE_PRUNED:
                    vbc_node_pr_reason = VBC_FEAS_SOL_FOUND;
                    break;
                default:
                    vbc_node_pr_reason = VBC_PRUNED;
                }
                #pragma omp critical (tree_update)
                purge_pruned_nodes(tm, n, vbc_node_pr_reason);
            }
            else
            {
                #pragma omp critical (tree_update)
                purge_pruned_nodes(tm, n, node_type == FEASIBLE_PRUNED ?
                                   VBC_FEAS_SOL_FOUND : VBC_PRUNED);
            }

            if(!repricing)
                return;
        }
    }

    if(node_type == INTERRUPTED_NODE)
    {
        n->node_status = NODE_STATUS__INTERRUPTED;
        n->lower_bound = lp_data->objval;
        #pragma omp critical (tree_update)
        insert_new_node(tm, n);
        if(!repricing)
            return;
    }

    if(!repricing || n->node_status != NODE_STATUS__PRUNED)
    {

        n->lower_bound = lp_data->objval;

        new_lp_desc = create_explicit_node_desc(p);

        deal_with_nf = (new_lp_desc->nf_status == NF_CHECK_AFTER_LAST ||
                        new_lp_desc->nf_status == NF_CHECK_UNTIL_LAST) ?
                       TRUE : FALSE;

        new_tm_desc = (node_desc*) calloc(1, sizeof(node_desc));

        if(p->bc_level == 0)
        {
            COPY_ARRAY_DESC(new_tm_desc->uind, new_lp_desc->uind);
            COPY_ARRAY_DESC(new_tm_desc->cutind, new_lp_desc->cutind);
            new_tm_desc->nf_status = new_lp_desc->nf_status;
            if(deal_with_nf)
                COPY_ARRAY_DESC(new_tm_desc->not_fixed, new_lp_desc->not_fixed);
            new_tm_desc->basis = new_lp_desc->basis;
            COPY_STAT(new_tm_desc->basis.basevars, new_lp_desc->basis.basevars);
            COPY_STAT(new_tm_desc->basis.extravars, new_lp_desc->basis.extravars);
            COPY_STAT(new_tm_desc->basis.baserows, new_lp_desc->basis.baserows);
            COPY_STAT(new_tm_desc->basis.extrarows, new_lp_desc->basis.extrarows);
        }
        else  /* we may want to pack the differences */
        {
            new_tm_desc->uind = pack_array_desc_diff(&lp_desc->uind,
                                &new_lp_desc->uind,
                                lp_data->tmp.i1); /* n */
            MAKE_TM_ARRAY_DESC(new_tm_desc->uind, new_lp_desc->uind);
            new_tm_desc->nf_status = new_lp_desc->nf_status;
            if(deal_with_nf)
            {
                new_tm_desc->not_fixed = pack_array_desc_diff(&lp_desc->not_fixed,
                                         &new_lp_desc->not_fixed,
                                         lp_data->tmp.iv);
                MAKE_TM_ARRAY_DESC(new_tm_desc->not_fixed, new_lp_desc->not_fixed);
            }
            new_tm_desc->cutind = pack_array_desc_diff(&lp_desc->cutind,
                                  &new_lp_desc->cutind,
                                  lp_data->tmp.i1); /* m */
            MAKE_TM_ARRAY_DESC(new_tm_desc->cutind, new_lp_desc->cutind);

            if(!new_lp_desc->basis.basis_exists ||
                    !lp_desc->basis.basis_exists)
            {
                new_tm_desc->basis = new_lp_desc->basis;

                COPY_STAT(new_tm_desc->basis.basevars,
                          new_lp_desc->basis.basevars);
                COPY_STAT(new_tm_desc->basis.extravars,
                          new_lp_desc->basis.extravars);
                COPY_STAT(new_tm_desc->basis.baserows,
                          new_lp_desc->basis.baserows);
                COPY_STAT(new_tm_desc->basis.extrarows,
                          new_lp_desc->basis.extrarows);
            }
            else
            {
                new_tm_desc->basis = pack_basis_diff(lp_desc, new_lp_desc,
                                                     new_tm_desc->uind.type,
                                                     new_tm_desc->cutind.type,
                                                     lp_data->tmp.i1);
                new_tm_desc->basis.basis_exists = new_lp_desc->basis.basis_exists;
            }
        }

        tm_desc->desc_size = new_lp_desc->desc_size;
        FREE(tm_desc->desc);

        if(new_lp_desc->desc_size > 0)
            memcpy((char*)tm_desc->desc, (char*)new_lp_desc->desc,
                   new_lp_desc->desc_size);

        merge_descriptions(tm_desc, new_tm_desc);
        /*
         * new_lp_desc used by "lp_prob p" does not need bnd_change. it is meant
         * only for bc_node->node_desc. hence we insert bnd_change in tm_desc
         * only
         */
        add_bound_changes_to_desc(tm_desc, p);
        free_node_desc(&new_tm_desc);

        if(p->par.verbosity > 10)
        {
            printf("TM: node %4i: ", n->bc_index);
            if(tm_desc->uind.type == WRT_PARENT)
                printf("uind:WRT(%i,%i) ", tm_desc->uind.size, tm_desc->uind.added);
            else
                printf("uind:EXP(%i) ", tm_desc->uind.size);
            printf("nf:%s ",
                   (deal_with_nf ?
                    (tm_desc->not_fixed.type == EXPLICIT_LIST ? "EXP" : "WRT") :
                    "N/A"));
            if(tm_desc->cutind.type == WRT_PARENT)
                printf("cind:WRT(%i,%i)\n", tm_desc->cutind.size,
                       tm_desc->cutind.added);
            else
                printf("cind:EXP(%i)\n", tm_desc->cutind.size);
            printf("               bvar:%s evar:%s brow:%s erow:%s\n",
                   tm_desc->basis.basevars.type == EXPLICIT_LIST ? "EXP" : "WRT",
                   tm_desc->basis.extravars.type == EXPLICIT_LIST ? "EXP" : "WRT",
                   tm_desc->basis.baserows.type == EXPLICIT_LIST ? "EXP" : "WRT",
                   tm_desc->basis.extrarows.type == EXPLICIT_LIST ? "EXP" : "WRT");
        }
    }

    if(! repricing)
    {
        /* If it's not repricing then we have to insert the node into the
         * appropriate heap */
        switch(node_type)
        {
        case INFEASIBLE_HOLD_FOR_NEXT_PHASE:
        case OVER_UB_HOLD_FOR_NEXT_PHASE:
            n->node_status = NODE_STATUS__HELD;
            REALLOC(tm->nextphase_cand, bc_node*,
                    tm->nextphase_cand_size, tm->nextphase_candnum + 1, BB_BUNCH);
            tm->nextphase_cand[tm->nextphase_candnum++] = n;
            /* update the nodes_per_... stuff */
            /* the active_nodes_per_... will be updated when the LP__IS_FREE
               message comes */
            if(n->cp)
#ifdef COMPILE_IN_CP
                tm->nodes_per_cp[n->cp]++;
#else
                tm->nodes_per_cp[find_process_index(&tm->cp, n->cp)]++;
#endif
            break;
        case NODE_BRANCHED_ON:
            n->node_status = NODE_STATUS__BRANCHED_ON;
            if(p->tm->par.vbc_emulation == VBC_EMULATION_FILE)
            {
                FILE* f;
                #pragma omp critical(write_vbc_emulation_file)
                if(!(f = fopen(p->tm->par.vbc_emulation_file_name, "a")))
                {
                    printf("\nError opening vbc emulation file\n\n");
                }
                else
                {
                    PRINT_TIME(p->tm, f);
                    fprintf(f, "P %i %i\n", n->bc_index + 1, VBC_INTERIOR_NODE);
                    fclose(f);
                }
            }
            else if(p->tm->par.vbc_emulation == VBC_EMULATION_FILE_NEW)
            {
                FILE* f;
                #pragma omp critical(write_vbc_emulation_file)
                if(!(f = fopen(p->tm->par.vbc_emulation_file_name, "a")))
                {
                    printf("\nError opening vbc emulation file\n\n");
                }
                else
                {
                    /* calculate measures of infeasibility */
                    double sum_inf = 0;
                    int num_inf = 0;

                    for(int i = 0; i < lp_data->n; i++)
                    {
                        double v = lp_data->x[i];
                        if(lp_data->vars[i]->is_int)
                        {
                            if(fabs(v - floor(v + 0.5)) > lp_data->lpetol)
                            {
                                num_inf++;
                                sum_inf = sum_inf + fabs(v - floor(v + 0.5));
                            }
                        }
                    }

                    char* reason = (char*)malloc(50 * CSIZE);
                    PRINT_TIME2(p->tm, f);
                    sprintf(reason, "%s %i", "branched", n->bc_index + 1);
                    if(n->bc_index == 0)
                    {
                        sprintf(reason, "%s %i", reason, 0);
                    }
                    else
                    {
                        sprintf(reason, "%s %i", reason, n->parent->bc_index + 1);
                    }

                    char branch_dir = 'M';
                    if(n->bc_index > 0)
                    {
                        if(n->parent->children[0] == n)
                        {
                            branch_dir = n->parent->bobj.sense[0];
                        }
                        else
                        {
                            branch_dir = n->parent->bobj.sense[1];
                        }
                        if(branch_dir == 'G')
                        {
                            branch_dir = 'R';
                        }
                    }
                    sprintf(reason, "%s %c %f %f %i", reason, branch_dir,
                            lp_data->objval + p->mip->obj_offset, sum_inf, num_inf);
                    fprintf(f, "%s\n", reason);
                    FREE(reason);
                    fclose(f);
                }
            }
            else if(p->tm->par.vbc_emulation == VBC_EMULATION_LIVE)
            {
                printf("$P %i %i\n", n->bc_index + 1, VBC_INTERIOR_NODE);
            }
            break;
        case ROOT_NODE:
            tm->rootnode = n;
            n->bc_index = tm->stat.created++;
            tm->stat.tree_size++;
            tm->stat.root_lb = n->lower_bound;
            /* these are set by calloc:
               n->bc_level = 0;
               n->lp = n->cg = n->cp = n->sp = 0;
               n->parent = NULL;
               */
            n->node_status = NODE_STATUS__ROOT;
            #pragma omp critical (tree_update)
            insert_new_node(tm, n);
            break;
        }
    }
    else
    {
        int nsize, nf_status;

        tm->stat.root_lb = n->lower_bound;
        if(n->node_status == NODE_STATUS__PRUNED)
        {
            /* Field day! Proved optimality! */
            free_subtree(tm->rootnode);
            tm->rootnode = n;
            tm->samephase_candnum = tm->nextphase_candnum = 0;
            return;
        }
        if(n->desc.uind.size > 0)
        {
            array_desc* uind = &n->desc.uind;
            array_desc* ruind = &tm->rootnode->desc.uind;
            int usize = uind->size;
            int rusize = ruind->size;
            int* ulist = uind->list;
            int* rulist = ruind->list;
            int i, j, k, not_fixed_size;
            /* Kick out from uind those in root's uind */
            for(i = 0, j = 0, k = 0; i < usize && j < rusize;)
            {
                if(ulist[i] < rulist[j])
                {
                    /* a new element in uind */
                    ulist[k++] = ulist[i++];
                }
                else if(ulist[i] < rulist[j])
                {
                    /* something got kicked out of ruind */
                    j++;
                }
                else   /* ulist[i] == rulist[j] */
                {
                    /* It just stayed there peacefully */
                    i++;
                    j++;
                }
            }
            if(i < usize)
            {
                /* The rest are new */
                for(; i < usize; i++, k++)
                    ulist[k] = ulist[i];
            }

            if((usize = k) > 0)
            {
                if((nsize = n->desc.not_fixed.size) == 0)
                {
                    /* All we got is from uind */
                    n->desc.not_fixed.size = usize;
                    n->desc.not_fixed.list = ulist;
                    uind->list = NULL;
                }
                else
                {
                    /* Now merge whatever is left in ulist with not_fixed.
                    Note that the two lists are disjoint. */
                    int* not_fixed = (int*) malloc((usize + nsize) * ISIZE);
                    int* nlist = n->desc.not_fixed.list;
                    for(not_fixed_size = i = j = k = 0; i < usize && j < nsize;
                            not_fixed_size++)
                    {
                        if(ulist[i] < nlist[j])
                        {
                            not_fixed[k++] = ulist[i++];
                        }
                        else if(ulist[i] > nlist[j])
                        {
                            not_fixed[k++] = nlist[j++];
                        }
                        else
                        {
                            not_fixed[k++] = nlist[j++];
                            i++;
                        }
                    }
                    if(i < usize)
                        memcpy(not_fixed + k, ulist + i, (usize - i)*ISIZE);
                    if(j < nsize)
                        memcpy(not_fixed + k, nlist + j, (nsize - j)*ISIZE);
                    FREE(nlist);
                    n->desc.not_fixed.size = not_fixed_size;
                    n->desc.not_fixed.list = not_fixed;
                }
            }
        }

        /* PROCESS_OK, now every new thingy is in n->desc.not_fixed */
        nsize = n->desc.not_fixed.size;
        if(nsize == 0)
        {
            /* Field day! Proved optimality!
               Caveats:
               This proves optimality, but the current tree may not contain
               this proof, since the cuts used in pricing out might be
               different from those originally in the root.
               For now just accept this sad fact and report optimality.
               Later, when the tree could be written out on disk, take care
               of writing out BOTH root descriptions to prove optimality.
               FIXME */
            if(tm->par.keep_description_of_pruned)
            {
                /* We got to write it out here. */
            }
            free_tree_node(n);
            tm->samephase_candnum = tm->nextphase_candnum = 0;
            return;
        }
        else
        {
            tm->rootnode->desc.not_fixed.list = n->desc.not_fixed.list;
            n->desc.not_fixed.list = NULL;
            if(nsize > tm->par.not_fixed_storage_size)
            {
                tm->rootnode->desc.not_fixed.size =
                    tm->par.not_fixed_storage_size;
                nf_status = NF_CHECK_AFTER_LAST;
            }
            else
            {
                tm->rootnode->desc.not_fixed.size = nsize;
                nf_status = NF_CHECK_UNTIL_LAST;
            }
        }
        propagate_nf_status(tm->rootnode, nf_status);
        tm->stat.nf_status = nf_status;
        tm->stat.vars_not_priced = tm->rootnode->desc.not_fixed.size;
        free_tree_node(n);
    }

    if(n->node_status == NODE_STATUS__PRUNED)
    {
#ifdef TRACE_PATH
        if(n->optimal_path)
        {
            printf("\n\nAttempting to prune the optimal path!!!!!!!!!\n\n");
            sleep(600);
            if(tm->par.logging)
            {
                write_tm_info(tm, tm->par.tree_log_file_name, NULL, FALSE);
                write_subtree(tm->rootnode, tm->par.tree_log_file_name, NULL,
                              TRUE, tm->par.logging);
                write_tm_cut_list(tm, tm->par.cut_log_file_name, FALSE);
            }
            exit(-10);
        }
#endif
        if(tm->par.keep_description_of_pruned == KEEP_ON_DISK_FULL ||
                tm->par.keep_description_of_pruned == KEEP_ON_DISK_VBC_TOOL)
        {
            #pragma omp critical (write_pruned_node_file)
            write_pruned_nodes(tm, n);
            #pragma omp critical (tree_update)
            if(tm->par.vbc_emulation == VBC_EMULATION_FILE_NEW)
            {
                int vbc_node_pr_reason;
                switch(node_type)
                {
                case INFEASIBLE_PRUNED:
                    vbc_node_pr_reason = VBC_PRUNED_INFEASIBLE;
                    break;
                case OVER_UB_PRUNED:
                    vbc_node_pr_reason = VBC_PRUNED_FATHOMED;
                    break;
                case FEASIBLE_PRUNED:
                    vbc_node_pr_reason = VBC_FEAS_SOL_FOUND;
                    break;
                default:
                    vbc_node_pr_reason = VBC_PRUNED;
                }
                purge_pruned_nodes(tm, n, vbc_node_pr_reason);
            }
            else
            {
                purge_pruned_nodes(tm, n, node_type == FEASIBLE_PRUNED ?
                                   VBC_FEAS_SOL_FOUND : VBC_PRUNED);
            }
        }
    }
#else
#ifdef SENSITIVITY_ANALYSIS
    if(p->par.sensitivity_analysis)
    {
        send_int_array(&p->desc->uind.size, 1);
        send_dbl_array(lp_data->x, p->desc->uind.size);
        //send_int_array(&p->base.cutnum, 1);
        send_dbl_array(lp_data->dualsol, p->base.cutnum);
    }
#endif
    if((node_type == INFEASIBLE_PRUNED || node_type == OVER_UB_PRUNED ||
            node_type == DISCARDED_NODE || node_type == FEASIBLE_PRUNED) &&
            !p->par.keep_description_of_pruned)
    {
        s_bufid = init_send(DataInPlace);
        send_char_array(&repricing, 1);
        ch = (char) node_type;
        send_char_array(&ch, 1);
        if(node_type == FEASIBLE_PRUNED)
        {
            if(!p->par.sensitivity_analysis)
            {
                send_int_array(&(p->desc->uind.size), 1);
                send_dbl_array(lp_data->x, p->desc->uind.size);
            }
        }
        send_msg(p->tree_manager, LP__NODE_DESCRIPTION);
        freebuf(s_bufid);
        return;
    }

    new_lp_desc = create_explicit_node_desc(p);

    new_lp_desc->bnd_change = NULL; /* TODO: implement this */

    /* Now start the real message */
    s_bufid = init_send(DataInPlace);
    send_char_array(&repricing, 1);
    ch = (char) node_type;
    send_char_array(&ch, 1);
    send_dbl_array(&lp_data->objval, 1);
    if(node_type == INTERRUPTED_NODE)
    {
        send_msg(p->tree_manager, LP__NODE_DESCRIPTION);
        freebuf(s_bufid);
        return;
    }

    send_int_array(&new_lp_desc->nf_status, 1);

    deal_with_nf = (new_lp_desc->nf_status == NF_CHECK_AFTER_LAST ||
                    new_lp_desc->nf_status == NF_CHECK_UNTIL_LAST) ? TRUE : FALSE;

    if(p->bc_level == 0)   /*we have the root node: send back explicit lists*/
    {
        pack_array_desc(&new_lp_desc->uind);
        if(deal_with_nf)
            pack_array_desc(&new_lp_desc->not_fixed);
        pack_array_desc(&new_lp_desc->cutind);
        pack_basis(&new_lp_desc->basis, FALSE);
    }
    else   /* we may want to pack the differences */
    {

        new_tm_desc = (node_desc*) calloc(1, sizeof(node_desc));

        new_tm_desc->uind = pack_array_desc_diff(&lp_desc->uind,
                            &new_lp_desc->uind,
                            lp_data->tmp.i1); /* n */
        if(deal_with_nf)
            pack_array_desc_diff(&lp_desc->not_fixed, &new_lp_desc->not_fixed,
                                 lp_data->tmp.iv); /* not_fixed_storage_size */
        new_tm_desc->cutind = pack_array_desc_diff(&lp_desc->cutind,
                              &new_lp_desc->cutind,
                              lp_data->tmp.i1); /* m */
        if(!new_lp_desc->basis.basis_exists ||
                !lp_desc->basis.basis_exists)
        {
            pack_basis(&new_lp_desc->basis, FALSE);
        }
        else
        {
            pack_basis_diff(lp_desc, new_lp_desc, new_tm_desc->uind.type,
                            new_tm_desc->cutind.type,
                            lp_data->tmp.i1); /* max(m,n)+1 */
        }

        FREE(new_tm_desc);
    }

    send_int_array(&new_lp_desc->desc_size, 1);
    if(new_lp_desc->desc_size > 0)
    {
        send_char_array(new_lp_desc->desc, new_lp_desc->desc_size);
    }
    /* Send it off */
    send_msg(p->tree_manager, LP__NODE_DESCRIPTION);
    freebuf(s_bufid);
    /* Now update the description in p */

#endif
    free_node_desc(&p->desc);
    p->desc = new_lp_desc;
}

/*===========================================================================*/

array_desc pack_array_desc_diff(array_desc* ad, array_desc* new_ad, int* itmp)
{
    /* Note that new_ad cannot be WRT_PARENT */

#ifndef COMPILE_IN_LP
    if(new_ad->type == NO_DATA_STORED)
    {
        pack_array_desc(new_ad);
    }
    else if(new_ad->size == 0)
    {
        /* No WRT can beat a 0 length explicit list */
        pack_array_desc(new_ad);
    }
    else
    {
#else
    itmp[0] = -1;
    if(new_ad->type != NO_DATA_STORED && new_ad->size > 0)
    {
#endif
        int  origsize = ad->size,  newsize = new_ad->size;
        int* origlist = ad->list, *newlist = new_ad->list;
        int i, j, k, l;
        int* iadd = itmp, *isub = itmp + newsize;

        for(k = 0, l = 0, i = 0, j = 0;
                i < origsize && j < newsize && k + l < newsize;)
        {
            if(origlist[i] < newlist[j])
            {
                isub[l++] = origlist[i++];
            }
            else if(origlist[i] == newlist[j])
            {
                i++;
                j++;
            }
            else
            {
                iadd[k++] = newlist[j++];
            }
        }
        if(origsize - i - j + k + l >= 0)
        {
            /* (origsize-i + newsize-j >= newsize - (k + l)) :
               The rest of the change is more than the free space in the
               change area ==> send explicitly, so just send new_ad */
#ifndef COMPILE_IN_LP
            pack_array_desc(new_ad);
#else
            itmp[0] = -1;
#endif
            return(*new_ad);
        }
        else
        {
            /* we want to send the difference */
            array_desc desc;
            desc.type = WRT_PARENT;
            desc.size = origsize - i + l + newsize - j + k;
            desc.added = newsize - j + k;
            /* addind */
            desc.list = desc.size > 0 ? iadd : NULL;
            if(newsize > j)
                memcpy(iadd + k, newlist + j, (newsize - j) * ISIZE);
            /* subind */
            if(l > 0)
                memcpy(desc.list + desc.added, isub, l * ISIZE);
            if(origsize > i)
                memcpy(desc.list + desc.added + l, origlist + i, (origsize - i) * ISIZE);
#ifndef COMPILE_IN_LP
            pack_array_desc(&desc);
#endif
            return(desc);
        }
    }
    return(*new_ad);
}

/*===========================================================================*/

#define PACK_BASE_DIFF(newdesc, olddesc, which)                              \
   orig_size = size = newdesc->which.size;                                   \
   type = pack_base_diff(&size,olddesc->which.stat,newdesc->which.stat,itmp);\
   if ((which.type = type) == WRT_PARENT){                                   \
      if ((which.size = size) > 0){                                          \
     which.list = (int *) malloc(size*ISIZE);                            \
     which.stat = (int *) malloc(size*ISIZE);                            \
     memcpy((char *)which.list, (char *)itmp, size*ISIZE);               \
     memcpy((char *)which.stat, (char *)(itmp+orig_size), size*ISIZE);   \
      }                                                                      \
   }else if ((which.size=newdesc->which.size) > 0){                          \
      which.stat = (int *) malloc(which.size*ISIZE);                         \
      memcpy((char *)which.stat,(char *)newdesc->which.stat,which.size*ISIZE);\
   }

#define PACK_EXTRA_DIFF(newdesc, olddesc, which1, which2, var)               \
   orig_size = newdesc->which1.size/2 + 1;                                   \
   type = pack_extra_diff(&olddesc->which1, olddesc->which2.stat,            \
              &newdesc->which1, newdesc->which2.stat,            \
              olddesc->which2.type, var, itmp, &size);           \
   if ((which2.type = type) == WRT_PARENT){                                  \
      if ((which2.size = size) > 0){                                         \
     which2.list = (int *) malloc(size*ISIZE);                           \
     which2.stat = (int *) malloc(size*ISIZE);                           \
     memcpy((char *)which2.list, (char *)itmp, size*ISIZE);              \
     memcpy((char *)which2.stat, (char *)(itmp+orig_size), size*ISIZE);  \
      }                                                                      \
   }else if ((which2.size = newdesc->which2.size) > 0){                      \
      which2.stat = (int *) malloc(which2.size*ISIZE);                       \
      memcpy(which2.stat, newdesc->which2.stat, which2.size*ISIZE);          \
   }

/*===========================================================================*/

basis_desc pack_basis_diff(node_desc* oldnode, node_desc* newnode,
                           char uind_type, char cutind_type, int* itmp)
{
    int size;
    basis_desc basis;
#ifdef COMPILE_IN_LP
    int orig_size;
    char type;
#else
    send_char_array(&newnode->basis.basis_exists, 1);
#endif
    if(! newnode->basis.basis_exists)
        return(basis);

    memset((char*)(&basis), 0, sizeof(basis_desc));

#ifdef COMPILE_IN_LP
    /* take care of the base rows */
    PACK_BASE_DIFF(newnode, oldnode, basis.baserows);

    /* take care of extra rows */
#ifdef DO_TESTS
    if(oldnode->basis.extrarows.size != oldnode->cutind.size ||
            newnode->basis.extrarows.size != newnode->cutind.size)
    {
        printf("pack_basis_diff: size differences!!!\n\n");
        exit(-5);
    }
#endif
    PACK_EXTRA_DIFF(newnode, oldnode, cutind, basis.extrarows, cutind_type);

    /* take care of base variables */
    PACK_BASE_DIFF(newnode, oldnode, basis.basevars);

    /* take care of extra variable */
#ifdef DO_TESTS
    if(oldnode->basis.extravars.size != oldnode->uind.size ||
            newnode->basis.extravars.size != newnode->uind.size)
    {
        printf("pack_basis_diff: size differences!!!\n\n");
        exit(-5);
    }
#endif

    PACK_EXTRA_DIFF(newnode, oldnode, uind, basis.extravars, uind_type);

#else
    /* take care of the base rows */
    pack_base_diff(&(newnode->basis.baserows.size), oldnode->basis.baserows.stat,
                   newnode->basis.baserows.stat, itmp); /* m */

    /* take care of extra rows */
#ifdef DO_TESTS
    if(oldnode->basis.extrarows.size != oldnode->cutind.size ||
            newnode->basis.extrarows.size != newnode->cutind.size)
    {
        printf("pack_basis_diff: size differences!!!\n\n");
        exit(-5);
    }
#endif
    pack_extra_diff(&oldnode->cutind, oldnode->basis.extrarows.stat,
                    &newnode->cutind, newnode->basis.extrarows.stat,
                    oldnode->basis.extrarows.type, cutind_type, itmp, &size);
    /*m*/

    /* take care of base variables */
    pack_base_diff(&(newnode->basis.basevars.size), oldnode->basis.basevars.stat,
                   newnode->basis.basevars.stat, itmp); /* n */

    /* take care of extra variable */
#ifdef DO_TESTS
    if(oldnode->basis.extravars.size != oldnode->uind.size ||
            newnode->basis.extravars.size != newnode->uind.size)
    {
        printf("pack_basis_diff: size differences!!!\n\n");
        exit(-5);
    }
#endif
    pack_extra_diff(&oldnode->uind, oldnode->basis.extravars.stat,
                    &newnode->uind, newnode->basis.extravars.stat,
                    oldnode->basis.extravars.type, uind_type, itmp, &size);/*n*/
#endif
    return(basis);
}

/*===========================================================================*/

char pack_base_diff(int* size, int* oldstat, int* newstat, int* itmp)
{
    int* list = itmp;
    int* stat = itmp + *size;
    int i, k;
    char type;

    for(k = 0, i = 0; i < *size && 2 * k < *size; i++)
    {
        if(oldstat[i] != newstat[i])
        {
            list[k] = i;
            stat[k++] = newstat[i];
        }
    }
    if(2 * k < *size)   /*changes are shorter */
    {
        *size = k;
        type = WRT_PARENT;
#ifndef COMPILE_IN_LP
        send_char_array(&type, 1);
        send_int_array(size, 1); /* size */
        if(k > 0)
        {
            send_int_array(list, k);
            send_int_array(stat, k);
        }
#endif
    }
    else   /* explicit shorter */
    {
        type = EXPLICIT_LIST;
#ifndef COMPILE_IN_LP
        send_char_array(&type, 1);
        send_int_array(size, 1);
        if(*size > 0)
        {
            send_int_array(newstat, *size);
        }
#endif
    }
    return(type);
}

/*===========================================================================*/

char pack_extra_diff(array_desc* olddesc, int* oldstat,
                     array_desc* newdesc, int* newstat,
                     char oldbasis_type_in_tm, char newdesc_type_in_tm,
                     int* itmp, int* size)
{
    char type;
    int oldsize = olddesc->size;
    int* oldlist = olddesc->list;
    int newsize = newdesc->size;
    int* newlist = newdesc->list;

    int tmp, i, j, k, l;
    int* modlist = itmp;
    int* modstat = itmp + newsize / 2 + 1;

    /* We must send explicit list if either
       - newdesc's type is EXPLICIT_LIST; or
       - this extra was stored as an explicit list in TM. This is indicated
         in type */
    if(newdesc_type_in_tm == EXPLICIT_LIST ||
            oldbasis_type_in_tm == EXPLICIT_LIST)
    {
        type = EXPLICIT_LIST;
#ifndef COMPILE_IN_LP
        send_char_array(&type, 1);
        send_int_array(&newsize, 1);
        if(newsize > 0)
            send_int_array(newstat, newsize);
#endif
        return(type);
    }

    /* OK, so in TM the old description is stored as wrt parent AND
       in newdesc the list is wrt parent */
    /* newsize must be positive, since if it is 0 then surely it is sent as
       EXP, so we'd never come here */

#if DO_TESTS
    if(newsize == 0)
    {
        printf("This can't be!!! newsize == 0 !!!\n\n");
        exit(11000);
    }
#endif

    for(k = l = 0, i = j = 0;
            i < oldsize && j < newsize && 2 * l < newsize;)
    {
        tmp = oldlist[i] - newlist[j];
        if(tmp < 0)
        {
            i++;
        }
        else if(tmp > 0)
        {
            modlist[l] = newlist[j];
            modstat[l++] = newstat[j++];
        }
        else
        {
            if(oldstat[i] != newstat[j])
            {
                modlist[l] = newlist[j];
                modstat[l++] = newstat[j];
            }
            i++;
            j++;
        }
    }

    if(2 * (*size = newsize - j + l) < newsize)
    {
        /* changes smaller than explicit */
        type = WRT_PARENT;
#ifdef COMPILE_IN_LP
        if(newsize - j > 0)
        {
            memcpy((char*)(modlist + l), (char*)(newlist + j), (newsize - j)*ISIZE);
            memcpy((char*)(modstat + l), (char*)(newstat + j), (newsize - j)*ISIZE);
        }
#else
        send_char_array(&type, 1);
        send_int_array(size, 1); /* size */
        if(*size > 0)
        {
            if(l > 0)
                send_int_array(modlist, l);
            if(newsize - j > 0)
                send_int_array(newlist + j, newsize - j);

            if(l > 0)
                send_int_array(modstat, l);
            if(newsize - j > 0)
                send_int_array(newstat + j, newsize - j);
        }
#endif
    }
    else
    {
        /* EXPLICIT_LIST is shorter */
        type = EXPLICIT_LIST;
#ifndef COMPILE_IN_LP
        send_char_array(&type, 1);
        send_int_array(&newsize, 1);
        if(newsize > 0)
        {
            send_int_array(newstat, newsize);
        }
#endif
    }
    return(type);
}

/*===========================================================================*/

void send_branching_info(lp_prob* p, branch_obj* can, char* action, int* keep)
{
    LPdata* lp_data = p->lp_data;
#ifndef COMPILE_IN_LP
    int s_bufid, r_bufid, name;
#endif
    int i = 0, pos = can->position;
    cut_data* brcut;
    char dive = p->dive, olddive = p->dive;
    char fractional_dive = FALSE;

#ifdef COMPILE_IN_LP
    tm_prob* tm = p->tm;
    bc_node* node = tm->active_nodes[p->proc_index];
    branch_obj* bobj = &node->bobj;
    int old_cut_name = 0;

    node->bobj = *can;

    switch(can->type)
    {
    case CANDIDATE_VARIABLE:
        bobj->name = pos < p->base.varnum ?
                     - pos - 1 : lp_data->vars[pos]->userind;
        break;
    case CANDIDATE_CUT_IN_MATRIX:
        brcut = lp_data->rows[pos].cut;
        bobj->name = pos < p->base.cutnum ?
                     - pos - 1 : (brcut->name < 0 ? - p->base.cutnum - 1 : brcut->name);
        i = (brcut->branch & CUT_BRANCHED_ON) ? FALSE : TRUE;
        if((old_cut_name = bobj->name) == -tm->bcutnum - 1)
        {
            bobj->name = add_cut_to_list(tm, lp_data->rows[pos].cut);
        }
        break;
    }

#ifdef COMPILE_FRAC_BRANCHING
    if(can->frac_num[*keep] < ((double)lp_data->n)*p->par.fractional_diving_ratio
            || can->frac_num[*keep] < p->par.fractional_diving_num)
    {
        dive = DO_DIVE;
        fractional_dive = TRUE;
    }
#endif

    dive = generate_children(tm, node, bobj, can->objval, can->feasible,
                             action, dive, keep, i);

    if(*keep >= 0 && (p->dive == CHECK_BEFORE_DIVE || p->dive == DO_DIVE))
    {
        *can = node->bobj;

#ifndef MAX_CHILDREN_NUM
        can->sense = malloc(can->child_num);
        can->rhs = (double*) malloc(can->child_num * DSIZE);
        can->range = (double*) malloc(can->child_num * DSIZE);
        can->branch = (int*) malloc(can->child_num * ISIZE);
        memcpy(can->sense, bobj->sense, bobj->child_num);
        memcpy((char*)can->rhs, (char*)bobj->rhs, bobj->child_num * DSIZE);
        memcpy((char*)can->range, (char*)bobj->range, bobj->child_num * DSIZE);
        memcpy((char*)can->branch, (char*)bobj->branch, bobj->child_num * ISIZE);
#endif
        p->dive = fractional_dive ? olddive : dive;
        if(dive == DO_DIVE || dive == CHECK_BEFORE_DIVE /*next time*/)
        {
            /* get the new node index */
            p->bc_index = node->children[*keep]->bc_index;
            if(can->type == CANDIDATE_CUT_IN_MATRIX &&
                    bobj->name == -p->base.cutnum - 1)
            {
                /* in this case we must have a branching cut */
                lp_data->rows[pos].cut->name = bobj->name;
                PRINT(p->par.verbosity, 4,
                      ("The real cut name is %i \n", lp_data->rows[pos].cut->name));
            }
            node->children[*keep]->cg = node->cg;
            tm->active_nodes[p->proc_index] = node->children[*keep];
            tm->stat.analyzed++;
            PRINT(p->par.verbosity, 1, ("Decided to dive...\n"));
        }
        else
        {
            PRINT(p->par.verbosity, 1, ("Decided not to dive...\n"));
        }
    }
    if(*keep < 0)
    {
        can->child_num = 0;
    }

#else

    s_bufid = init_send(DataInPlace);
    /* Type of the object */
    send_char_array(&can->type, 1);
    switch(can->type)
    {
    case CANDIDATE_VARIABLE:
        /* For branching variable pack only the name */
        name = pos < p->base.varnum ? - pos - 1 : lp_data->vars[pos]->userind;
        send_int_array(&name, 1);
        break;
    case CANDIDATE_CUT_IN_MATRIX:
        /* For branching cut pack the name, whether it is a new branching cut
        and if necessary the cut itself. */
        brcut = lp_data->rows[pos].cut;
        name = pos < p->base.cutnum ?
               - pos - 1 : (brcut->name < 0 ? - p->base.cutnum - 1 : brcut->name);
        send_int_array(&name, 1);
        i = (brcut->branch & CUT_BRANCHED_ON) ? FALSE : TRUE;
        send_int_array(&i, 1);
        if(name == - p->base.cutnum - 1)
            /* a branching cut without name. Pack the cut, too.*/
            pack_cut(lp_data->rows[pos].cut);
        break;
    }

#ifdef COMPILE_FRAC_BRANCHING
    if(can->frac_num[*keep] < ((double)lp_data->n)*p->par.fractional_diving_ratio
            || can->frac_num[*keep] < p->par.fractional_diving_num)
    {
        dive = DO_DIVE;
        fractional_dive = TRUE;
    }
#endif

    /* Number of descendants */
    send_int_array(&can->child_num, 1);

    /* The describing arrays */
    send_char_array(can->sense, can->child_num);
    send_dbl_array(can->rhs, can->child_num);
    send_dbl_array(can->range, can->child_num);
    send_int_array(can->branch, can->child_num);
    send_dbl_array(can->objval, can->child_num);
    send_int_array(can->feasible, can->child_num);
    for(i = 0; i < can->child_num; i++)
    {
        if(can->feasible[i])
        {
#if 0
            send_dbl_array(can->solutions[i], lp_data->n);
#endif
        }
    }
    /* the action for each descendant */
    send_char_array(action, can->child_num);

    /* Our diving status and what we would keep */
    send_char_array(&dive, 1);
    send_int_array(keep, 1);

    send_msg(p->tree_manager, LP__BRANCHING_INFO);
    freebuf(s_bufid);

    /* We can expect a reply only in this case */
    if(*keep >= 0 && (dive == CHECK_BEFORE_DIVE || dive == DO_DIVE))
    {
        double start;
        struct timeval timeout = {15, 0};
        start = wall_clock(NULL);
        do
        {
            r_bufid = treceive_msg(p->tree_manager, LP__DIVING_INFO, &timeout);
            if(! r_bufid)
            {
                if(pstat(p->tree_manager) != PROCESS_OK)
                {
                    printf("TM has died -- LP exiting\n\n");
                    exit(-301);
                }
            }
        }
        while(! r_bufid);
        receive_char_array(&dive, 1);
        /* get the new nodenum (and the index of the branching cut if unknown)
         * if we dive */
        p->comp_times.idle_diving += wall_clock(NULL) - start;
        if(dive == DO_DIVE || dive == CHECK_BEFORE_DIVE /*next time*/)
        {
            /* get the new node index */
            receive_int_array(&p->bc_index, 1);
            if(can->type == CANDIDATE_CUT_IN_MATRIX &&
                    name == -p->base.cutnum - 1)
            {
                /* in this case we must have a branching cut */
                receive_int_array(&lp_data->rows[pos].cut->name, 1);
                PRINT(p->par.verbosity, 4,
                      ("The real cut name is %i \n", lp_data->rows[pos].cut->name));
            }
            PRINT(p->par.verbosity, 1, ("Decided to dive...\n"));
        }
        else
        {
            PRINT(p->par.verbosity, 1, ("Decided not to dive...\n"));
        }
        freebuf(r_bufid);
        p->dive = fractional_dive ? olddive : dive;
    }
#endif

    /* Print some statistics */
    for(i = can->child_num - 1; i >= 0; i--)
    {
        switch(action[i])
        {
        case KEEP_THIS_CHILD:
            break;
        case RETURN_THIS_CHILD:
            break;
        case PRUNE_THIS_CHILD:
            PRINT(p->par.verbosity, 2, ("child %i is pruned by rule\n", i));
            break;
        case PRUNE_THIS_CHILD_FATHOMABLE:
        case PRUNE_THIS_CHILD_INFEASIBLE:
            PRINT(p->par.verbosity, 2, ("child %i is fathomed [%i, %i]\n",
                                        i, can->termcode[i], can->iterd[i]));
            break;
        }
    }
}

/*===========================================================================*/

void send_lp_is_free(lp_prob* p)
{
    int s_bufid;

    s_bufid = init_send(DataInPlace);
    send_int_array(&p->cut_pool, 1);
    send_msg(p->tree_manager, LP__IS_FREE);
    freebuf(s_bufid);

    /* clear out stuff here */
    free_node_dependent(p);
}

/*===========================================================================*/

void send_cuts_to_pool(lp_prob* p, int eff_cnt_limit)
{
    int i, cnt = 0;
    row_data* extrarows = p->lp_data->rows + p->base.cutnum;
#if defined(COMPILE_IN_CP) && defined(COMPILE_IN_LP)

    cut_pool* cp = p->tm->cpp[p->cut_pool];

    if(!cp)
        return;
#else
    int s_bufid;

    if(! p->cut_pool)
        return;
#endif

    /* Count how many to send */
    for(i = p->lp_data->m - p->base.cutnum - 1; i >= 0; i--)
    {
        if(!(extrarows[i].cut->name != CUT__SEND_TO_CP || extrarows[i].free ||
                extrarows[i].eff_cnt < eff_cnt_limit))
            cnt++;
    }

#if defined(COMPILE_IN_CP) && defined(COMPILE_IN_LP)

    if(cnt > 0)
    {
        REALLOC(cp->cuts_to_add, cut_data*, cp->cuts_to_add_size,
                cnt, BB_BUNCH);
        for(i = p->lp_data->m - p->base.cutnum - 1; i >= 0; i--)
        {
            if(!(extrarows[i].cut->name != CUT__SEND_TO_CP ||
                    extrarows[i].free || extrarows[i].eff_cnt < eff_cnt_limit))
            {
                cp->cuts_to_add[cp->cuts_to_add_num] =
                    (cut_data*) malloc(sizeof(cut_data));
                memcpy((char*)cp->cuts_to_add[cp->cuts_to_add_num],
                       (char*)extrarows[i].cut, sizeof(cut_data));
                if(extrarows[i].cut->size > 0)
                {
                    cp->cuts_to_add[cp->cuts_to_add_num]->coef =
                        (char*) malloc(extrarows[i].cut->size * sizeof(char));
                    memcpy((char*)cp->cuts_to_add[cp->cuts_to_add_num++]->coef,
                           extrarows[i].cut->coef,
                           extrarows[i].cut->size * sizeof(char));
                }
                extrarows[i].cut->name = CUT__DO_NOT_SEND_TO_CP;
            }
        }
        cut_pool_receive_cuts(cp, p->bc_level);
        cp->cuts_to_add_num = 0;
    }
#else

    if(cnt > 0)
    {
        s_bufid = init_send(DataInPlace);
        send_int_array(&cnt, 1);
        /* whatever is sent to the CP must have been generated at this level */
        send_int_array(&p->bc_level, 1);
        for(i = p->lp_data->m - p->base.cutnum - 1; i >= 0; i--)
        {
            if(!(extrarows[i].cut->name != CUT__SEND_TO_CP ||
                    extrarows[i].free || extrarows[i].eff_cnt < eff_cnt_limit))
            {
                pack_cut(extrarows[i].cut);
                extrarows[i].cut->name = CUT__DO_NOT_SEND_TO_CP;
            }
        }
        send_msg(p->cut_pool, PACKED_CUTS_TO_CP);
        freebuf(s_bufid);
        PRINT(p->par.verbosity, 4, ("%i cuts sent to cutpool\n", cnt));
    }

#endif
}
/*===========================================================================*/
/*===========================================================================*/
