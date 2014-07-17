/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_pop_entry(WT_SESSION_IMPL *, uint32_t , WT_LSM_WORK_UNIT **);
static void * __lsm_worker(void *);
static void * __lsm_worker_manager(void *);

/*
 * __wt_lsm_manager_start --
 *	Start the LSM management infrastructure.
 */
int
__wt_lsm_manager_start(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_LSM_MANAGER *manager;
	WT_SESSION_IMPL *worker_session;

	manager = &S2C(session)->lsm_manager;

	/*
	 * We need at least a manager, a switch thread and a generic
	 * worker.
	 */
	WT_ASSERT(session, manager->lsm_workers_max > 2);
	WT_RET(__wt_calloc_def(session,
	    manager->lsm_workers_max, &manager->lsm_worker_tids));
	/*
	 * All the LSM worker threads do their operations on read-only
	 * files. Use read-uncommitted isolation to avoid keeping
	 * updates in cache unnecessarily.
	 */
	WT_ERR(__wt_open_session(S2C(session), 1, NULL,
	    "isolation=read-uncommitted", &worker_session));
	worker_session->name = "lsm-worker-manager";
	WT_ERR(__wt_thread_create(session, &manager->lsm_worker_tids[0],
	    __lsm_worker_manager, worker_session));

	if (0) {
err:		__wt_free(session, manager->lsm_worker_tids);
	}
	return (ret);
}

/*
 * __lsm_worker_manager --
 *	A thread that manages all open LSM trees, and the shared LSM worker
 *	threads.
 */
static void *
__lsm_worker_manager(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LSM_MANAGER *manager;
	WT_LSM_TREE *next_tree;
	WT_LSM_WORK_UNIT *merge_op;
	WT_LSM_WORKER_ARGS *worker_args;
	WT_SESSION_IMPL *session, *worker_session;
	int queued;

	session = (WT_SESSION_IMPL *)arg;
	conn = S2C(session);
	manager = &conn->lsm_manager;

	WT_ASSERT(session, manager->lsm_workers == 0);
	manager->lsm_workers = 1; /* We are the first worker */

	/* Setup the spin locks for the queues. */
	WT_ERR(__wt_spin_init(
	    session, &manager->app_lock, "LSM application queue lock"));
	WT_ERR(__wt_spin_init(
	    session, &manager->manager_lock, "LSM manager queue lock"));
	WT_ERR(__wt_spin_init(
	    session, &manager->switch_lock, "LSM switch queue lock"));

	/*
	 * All the LSM worker threads do their operations on read-only files.
	 * Use read-uncommitted isolation to avoid keeping updates in cache
	 * unnecessarily.
	 */
	WT_ERR(__wt_open_session(
	    conn, 1, NULL, "isolation=read-uncommitted", &worker_session));
	worker_session->name = "lsm-worker-switch";
	/* Freed by the worker thread when it shuts down */
	WT_ERR(__wt_calloc_def(session, 1, &worker_args));
	worker_args->session = worker_session;
	worker_args->id = manager->lsm_workers++;
	worker_args->flags = WT_LSM_WORK_SWITCH;
	/* Start the switch thread. */
	WT_ERR(__wt_thread_create(session, &manager->lsm_worker_tids[1],
	    __lsm_worker, worker_args));

	/* Start a generic worker thread. */
	WT_ERR(__wt_open_session(
	    conn, 1, NULL, "isolation=read-uncommitted", &worker_session));
	worker_session->name = "lsm-worker-1";
	/* Freed by the worker thread when it shuts down */
	WT_ERR(__wt_calloc_def(session, 1, &worker_args));
	worker_args->session = worker_session;
	worker_args->id = manager->lsm_workers++;
	worker_args->flags = WT_LSM_WORK_BLOOM | WT_LSM_WORK_FLUSH |
	    WT_LSM_WORK_MERGE | WT_LSM_WORK_SWITCH;
	WT_ERR(__wt_thread_create(session, &manager->lsm_worker_tids[2],
	    __lsm_worker, worker_args));

	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		if (TAILQ_EMPTY(&conn->lsmqh)) {
			__wt_sleep(0, 10000);
			continue;
		}
		queued = 0;
		TAILQ_FOREACH(next_tree, &conn->lsmqh, q) {
			if (next_tree->nchunks > 1 &&
			    next_tree->merge_throttle > 0) {
				++queued;
				WT_ERR(__wt_calloc_def(session, 1, &merge_op));
				F_SET(merge_op, WT_LSM_WORK_MERGE);
				merge_op->lsm_tree = next_tree;
				STAILQ_INSERT_TAIL(
				    &manager->managerqh, merge_op, q);
			}
		}
		/* Don't busy loop if we aren't finding work. */
		if (queued == 0)
			__wt_sleep(0, 1000);
	}

	/* Wait for the rest of the LSM workers to shutdown. */

	if (ret != 0) {
err:		__wt_err(session, ret, "LSM worker manager thread error");
	}
	return (NULL);
}

/*
 * __lsm_pop_entry --
 *	Retrieve the head of the queue, if it matches the requested work
 *	unit type.
 */
static int
__lsm_pop_entry(
    WT_SESSION_IMPL *session, uint32_t type, WT_LSM_WORK_UNIT **entryp)
{
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *entry;

	manager = &S2C(session)->lsm_manager;
	*entryp = NULL;
	entry = NULL;

	switch (type) {
	case WT_LSM_WORK_SWITCH:
		if (STAILQ_EMPTY(&manager->switchqh))
			return (0);

		__wt_spin_lock(session, &manager->switch_lock);
		if (!STAILQ_EMPTY(&manager->switchqh)) {
			entry = STAILQ_FIRST(&manager->switchqh);
			WT_ASSERT(session, entry != NULL);
			STAILQ_REMOVE_HEAD(&manager->switchqh, q);
		}

		__wt_spin_unlock(session, &manager->switch_lock);
		break;
	case WT_LSM_WORK_MERGE:
		if (STAILQ_EMPTY(&manager->managerqh))
			return (0);

		__wt_spin_lock(session, &manager->manager_lock);
		if (!STAILQ_EMPTY(&manager->managerqh)) {
			entry = STAILQ_FIRST(&manager->managerqh);
			WT_ASSERT(session, entry != NULL);
			if (F_ISSET(entry, type))
				STAILQ_REMOVE_HEAD(&manager->managerqh, q);
			else
				entry = NULL;
		}

		__wt_spin_unlock(session, &manager->manager_lock);
		break;
	default:
		/*
		 * The app queue is the only one that has multiple different
		 * work unit types, allow a request for a variety.
		 */
		WT_ASSERT(session, FLD_ISSET(type, WT_LSM_WORK_BLOOM) ||
		    FLD_ISSET(type, WT_LSM_WORK_FLUSH));
		if (STAILQ_EMPTY(&manager->appqh))
			return (0);

		__wt_spin_lock(session, &manager->app_lock);
		if (!STAILQ_EMPTY(&manager->appqh)) {
			entry = STAILQ_FIRST(&manager->appqh);
			WT_ASSERT(session, entry != NULL);
			if (FLD_ISSET(type, entry->flags))
				STAILQ_REMOVE_HEAD(&manager->appqh, q);
			else
				entry = NULL;
		}
		__wt_spin_unlock(session, &manager->app_lock);
		break;
	}
	*entryp = entry;
	return (0);
}

/*
 * __wt_lsm_push_entry --
 *	Add an entry to the end of the switch queue.
 */
int
__wt_lsm_push_entry(
    WT_SESSION_IMPL *session, uint32_t type, WT_LSM_TREE *lsm_tree)
{
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *entry;

	manager = &S2C(session)->lsm_manager;

	WT_RET(__wt_calloc_def(session, 1, &entry));
	entry->flags = type;
	entry->lsm_tree = lsm_tree;

	switch (type) {
	case WT_LSM_WORK_SWITCH:
		__wt_spin_lock(session, &manager->switch_lock);
		STAILQ_INSERT_TAIL(&manager->switchqh, entry, q);
		__wt_spin_unlock(session, &manager->switch_lock);
		break;
	case WT_LSM_WORK_BLOOM:
	case WT_LSM_WORK_FLUSH:
		__wt_spin_lock(session, &manager->app_lock);
		STAILQ_INSERT_TAIL(&manager->appqh, entry, q);
		__wt_spin_unlock(session, &manager->app_lock);
		break;
	case WT_LSM_WORK_MERGE:
		__wt_spin_lock(session, &manager->manager_lock);
		STAILQ_INSERT_TAIL(&manager->managerqh, entry, q);
		__wt_spin_unlock(session, &manager->manager_lock);
		break;
	}

	return (0);
}

/*
 * __lsm_get_chunk_to_flush --
 *	Find and pin a chunk in the LSM tree that is likely to need flushing.
 */
static int
__lsm_get_chunk_to_flush(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK **chunkp)
{
	u_int i;

	*chunkp = NULL;

	WT_RET(__wt_lsm_tree_lock(session, lsm_tree, 0));
	if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
		return (__wt_lsm_tree_unlock(session, lsm_tree));

	for (i = 0;
	    i < lsm_tree->nchunks &&
	    F_ISSET(lsm_tree->chunk[i], WT_LSM_CHUNK_ONDISK);
	    i++) {}

	if (!F_ISSET(lsm_tree->chunk[i], WT_LSM_CHUNK_ONDISK)) {
		/* We should never be asked to flush the primary chunk */
		WT_ASSERT(session, i < lsm_tree->nchunks);
		(void)WT_ATOMIC_ADD(lsm_tree->chunk[i]->refcnt, 1);
		*chunkp = lsm_tree->chunk[i];
	}

	WT_RET(__wt_lsm_tree_unlock(session, lsm_tree));

	return (0);
}

/*
 * __lsm_worker --
 *	A thread that executes switches for all open LSM trees.
 */
static void *
__lsm_worker(void *arg) {
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *entry;
	WT_LSM_WORKER_ARGS *cookie;
	WT_SESSION_IMPL *session;
	int flushed;

	cookie = (WT_LSM_WORKER_ARGS *)arg;
	session = cookie->session;
	conn = S2C(session);
	manager = &conn->lsm_manager;

	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		/* Don't busy wait if there aren't any LSM trees. */
		if (TAILQ_EMPTY(&conn->lsmqh)) {
			__wt_sleep(0, 10000);
			continue;
		}

		/* Switches are always a high priority */
		while (F_ISSET(cookie, WT_LSM_WORK_SWITCH) &&
		    (ret = __lsm_pop_entry(
		    session, WT_LSM_WORK_SWITCH, &entry)) == 0 &&
		    entry != NULL) {
			/*
			 * Don't exit the switch thread because a single
			 * switch fails. Keep trying until we are told to
			 * shut down.
			 */
			if ((ret = __wt_lsm_tree_switch(
			    session, entry->lsm_tree)) != 0) {
				__wt_err(session, ret, "Error in LSM switch");
				ret = 0;
			}
		}
		/* Flag an error if the pop failed. */
		WT_ERR(ret);

		if ((F_ISSET(cookie, WT_LSM_WORK_FLUSH) ||
		    F_ISSET(cookie, WT_LSM_WORK_BLOOM)) &&
		    (ret = __lsm_pop_entry(
		    session, cookie->flags, &entry)) == 0 &&
		    entry != NULL) {
			if (entry->flags == WT_LSM_WORK_FLUSH) {
				WT_ERR(__lsm_get_chunk_to_flush(
				    session, entry->lsm_tree, &chunk));
				WT_ERR(__wt_lsm_checkpoint_chunk(session,
				    entry->lsm_tree, chunk, &flushed));
			} else if (entry->flags == WT_LSM_WORK_BLOOM) {
			}
		}
		/* Flag an error if the pop failed. */
		WT_ERR(ret);

		if (F_ISSET(cookie, WT_LSM_WORK_MERGE) &&
		    (ret = __lsm_pop_entry(
		    session, WT_LSM_WORK_MERGE, &entry)) == 0 &&
		    entry != NULL) {
			WT_ASSERT(session, entry->flags == WT_LSM_WORK_MERGE);
			(void)__wt_lsm_merge(session,
			    entry->lsm_tree, cookie->id, 0);
			/* Clear any state */
			WT_CLEAR_BTREE_IN_SESSION(session);
		}
		/* Flag an error if the pop failed. */
		WT_ERR(ret);
	}

	if (ret != 0) {
err:		__wt_err(session, ret, "Error in LSM switch thread");
	}
	--manager->lsm_workers;
	__wt_free(session, cookie);
	/* TODO: There has to be a better way? */
	((WT_SESSION *)session)->close(((WT_SESSION *)session), NULL);
	return (NULL);
}
