/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2025

#include "internals.h"

int tbl_setup(const MDBX_env *env, volatile kvx_t *const kvx, const tree_t *const db) {
  osal_memory_fence(mo_AcquireRelease, false);

  if (unlikely(!check_table_flags(db->flags))) {
    ERROR("incompatible or invalid db.flags (0x%x) ", db->flags);
    return MDBX_INCOMPATIBLE;
  }

  size_t v_lmin = valsize_min(db->flags);
  size_t v_lmax = env_valsize_max(env, db->flags);
  if ((db->flags & (MDBX_DUPFIXED | MDBX_INTEGERDUP)) != 0 && db->dupfix_size) {
    if (!MDBX_DISABLE_VALIDATION && unlikely(db->dupfix_size < v_lmin || db->dupfix_size > v_lmax)) {
      ERROR("db.dupfix_size (%u) <> min/max value-length (%zu/%zu)", db->dupfix_size, v_lmin, v_lmax);
      return MDBX_CORRUPTED;
    }
    v_lmin = v_lmax = db->dupfix_size;
  }

  kvx->clc.k.lmin = keysize_min(db->flags);
  kvx->clc.k.lmax = env_keysize_max(env, db->flags);
  if (unlikely(!kvx->clc.k.cmp)) {
    kvx->clc.v.cmp = builtin_datacmp(db->flags);
    kvx->clc.k.cmp = builtin_keycmp(db->flags);
  }
  kvx->clc.v.lmin = v_lmin;
  osal_memory_fence(mo_Relaxed, true);
  kvx->clc.v.lmax = v_lmax;
  osal_memory_fence(mo_AcquireRelease, true);

  eASSERT(env, kvx->clc.k.lmax >= kvx->clc.k.lmin);
  eASSERT(env, kvx->clc.v.lmax >= kvx->clc.v.lmin);
  return MDBX_SUCCESS;
}

int tbl_fetch(MDBX_txn *txn, MDBX_cursor *mc, size_t dbi, const MDBX_val *name, unsigned wanna_flags) {
  int err = cursor_init(mc, txn, MAIN_DBI);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  err = tree_search(mc, name, 0);
  if (unlikely(err != MDBX_SUCCESS)) {
    if (err == MDBX_NOTFOUND)
      goto notfound;
    return err;
  }

  struct node_search_result nsr = node_search(mc, name);
  if (unlikely(!nsr.exact)) {
  notfound:
    if (dbi < txn->env->n_dbi && (txn->env->dbs_flags[dbi] & DB_VALID) && !(wanna_flags & MDBX_CREATE))
      NOTICE("dbi %zu refs to non-existing table `%.*s` for txn %" PRIaTXN " (err %d)", dbi, (int)name->iov_len,
             (const char *)name->iov_base, txn->txnid, err);
    return MDBX_NOTFOUND;
  }

  if (unlikely((node_flags(nsr.node) & (N_DUP | N_TREE)) != N_TREE)) {
    NOTICE("dbi %zu refs to not a named table `%.*s` for txn %" PRIaTXN " (%s)", dbi, (int)name->iov_len,
           (const char *)name->iov_base, txn->txnid, "wrong node-flags");
    return MDBX_INCOMPATIBLE /* not a named DB */;
  }

  MDBX_val data;
  err = node_read(mc, nsr.node, &data, mc->pg[mc->top]);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  if (unlikely(data.iov_len < sizeof(tree_t))) {
    NOTICE("dbi %zu refs to not a named table `%.*s` for txn %" PRIaTXN " (%s)", dbi, (int)name->iov_len,
           (const char *)name->iov_base, txn->txnid, "wrong record-size");
    return MDBX_INCOMPATIBLE /* not a named DB */;
  }

  const unsigned db_flags = UNALIGNED_PEEK_16(data.iov_base, tree_t, flags);
  const pgno_t db_root_pgno = peek_pgno(ptr_disp(data.iov_base, offsetof(tree_t, root)));
  /* The txn may not know this DBI, or another process may
   * have dropped and recreated the DB with other flags. */
  if (unlikely((wanna_flags ^ db_flags) & DB_PERSISTENT_FLAGS) && !(wanna_flags & MDBX_DB_ACCEDE) &&
      !((wanna_flags & MDBX_CREATE) && db_root_pgno == P_INVALID)) {
    NOTICE("dbi %zu refs to the re-created table `%.*s` for txn %" PRIaTXN
           " with different flags (present 0x%X != wanna 0x%X)",
           dbi, (int)name->iov_len, (const char *)name->iov_base, txn->txnid, db_flags & DB_PERSISTENT_FLAGS,
           wanna_flags & DB_PERSISTENT_FLAGS);
    return MDBX_INCOMPATIBLE /* not a named DB */;
  }

  tree_t *const db = &txn->dbs[dbi];
  memcpy(db, data.iov_base, sizeof(tree_t));
#if !MDBX_DISABLE_VALIDATION
  const txnid_t maindb_leafpage_txnid = mc->pg[mc->top]->txnid;
  tASSERT(txn, txn->front_txnid >= maindb_leafpage_txnid);
  if (unlikely(db->mod_txnid > maindb_leafpage_txnid)) {
    ERROR("db.mod_txnid (%" PRIaTXN ") > page-txnid (%" PRIaTXN ")", db->mod_txnid, maindb_leafpage_txnid);
    return MDBX_CORRUPTED;
  }
#endif /* !MDBX_DISABLE_VALIDATION */

  return MDBX_SUCCESS;
}

int tbl_create(MDBX_txn *txn, MDBX_cursor *mc, size_t slot, const MDBX_val *name, unsigned db_flags) {
  tASSERT(txn, db_flags & MDBX_CREATE);
  MDBX_val body;
  body.iov_base = memset(&txn->dbs[slot], 0, body.iov_len = sizeof(tree_t));
  txn->dbs[slot].root = P_INVALID;
  txn->dbs[slot].mod_txnid = txn->txnid;
  txn->dbs[slot].flags = db_flags & DB_PERSISTENT_FLAGS;
  mc->next = txn->cursors[MAIN_DBI];
  txn->cursors[MAIN_DBI] = mc;
  int err = cursor_put_checklen(mc, name, &body, N_TREE | MDBX_NOOVERWRITE);
  txn->cursors[MAIN_DBI] = mc->next;
  if (likely(err == MDBX_SUCCESS)) {
    txn->flags |= MDBX_TXN_DIRTY;
    tASSERT(txn, (txn->dbi_state[MAIN_DBI] & DBI_DIRTY) != 0);
  }
  return err;
}

int tbl_refresh(MDBX_txn *txn, size_t dbi) {
  cursor_couple_t couple;
  kvx_t *const kvx = &txn->env->kvs[dbi];
  int rc = tbl_fetch(txn, &couple.outer, dbi, &kvx->name, txn->dbs[dbi].flags);
  if (likely(rc != MDBX_SUCCESS))
    return dbi_gone(txn, dbi, rc);

  rc = tbl_setup_ifneed(txn->env, kvx, &txn->dbs[dbi]);
  if (unlikely(rc != MDBX_SUCCESS))
    return dbi_gone(txn, dbi, rc);

  if (unlikely(dbi_changed(txn, dbi)))
    return MDBX_BAD_DBI;

  txn->dbi_state[dbi] &= ~DBI_STALE;
  return MDBX_SUCCESS;
}
