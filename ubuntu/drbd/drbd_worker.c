/*
   drbd_worker.c

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/version.h>

#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/drbd_config.h>
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/scatterlist.h>

#include <linux/drbd.h>
#include "drbd_int.h"
#include "drbd_req.h"

#define SLEEP_TIME (HZ/10)

STATIC int w_make_ov_request(struct drbd_conf *mdev, struct drbd_work *w, int cancel);



/* defined here:
   drbd_md_io_complete
   drbd_endio_write_sec
   drbd_endio_read_sec
   drbd_endio_pri

 * more endio handlers:
   atodb_endio in drbd_actlog.c
   drbd_bm_async_io_complete in drbd_bitmap.c

 * For all these callbacks, note the follwing:
 * The callbacks will be called in irq context by the IDE drivers,
 * and in Softirqs/Tasklets/BH context by the SCSI drivers.
 * Try to get the locking right :)
 *
 */


/* About the global_state_lock
   Each state transition on an device holds a read lock. In case we have
   to evaluate the sync after dependencies, we grab a write lock, because
   we need stable states on all devices for that.  */
rwlock_t global_state_lock;

/* used for synchronous meta data and bitmap IO
 * submitted by drbd_md_sync_page_io()
 */
void drbd_md_io_complete(struct bio *bio, int error)
{
	struct drbd_md_io *md_io;

	/* error parameter ignored:
	 * drbd_md_sync_page_io explicitly tests bio_uptodate(bio); */

	md_io = (struct drbd_md_io *)bio->bi_private;

	md_io->error = error;

	dump_internal_bio("Md", md_io->mdev, bio, 1);

	complete(&md_io->event);
}

/* reads on behalf of the partner,
 * "submitted" by the receiver
 */
void drbd_endio_read_sec(struct bio *bio, int error) __releases(local)
{
	unsigned long flags = 0;
	struct Tl_epoch_entry *e = NULL;
	struct drbd_conf *mdev;
	int uptodate = bio_flagged(bio, BIO_UPTODATE);

	e = bio->bi_private;
	mdev = e->mdev;

	if (!error && !uptodate) {
		/* strange behaviour of some lower level drivers...
		 * fail the request by clearing the uptodate flag,
		 * but do not return any error?!
		 * do we want to drbd_WARN() on this? */
		error = -EIO;
	}

	D_ASSERT(e->block_id != ID_VACANT);

	dump_internal_bio("Sec", mdev, bio, 1);

	spin_lock_irqsave(&mdev->req_lock, flags);
	mdev->read_cnt += e->size >> 9;
	list_del(&e->w.list);
	if (list_empty(&mdev->read_ee))
		wake_up(&mdev->ee_wait);
	spin_unlock_irqrestore(&mdev->req_lock, flags);

	drbd_chk_io_error(mdev, error, FALSE);
	drbd_queue_work(&mdev->data.work, &e->w);
	dec_local(mdev);

	MTRACE(TraceTypeEE, TraceLvlAll,
	       INFO("Moved EE (READ) to worker sec=%llus size=%u ee=%p\n",
		    (unsigned long long)e->sector, e->size, e);
	       );
}

/* writes on behalf of the partner, or resync writes,
 * "submitted" by the receiver.
 */
void drbd_endio_write_sec(struct bio *bio, int error) __releases(local)
{
	unsigned long flags = 0;
	struct Tl_epoch_entry *e = NULL;
	struct drbd_conf *mdev;
	sector_t e_sector;
	int do_wake;
	int is_syncer_req;
	int do_al_complete_io;
	int uptodate = bio_flagged(bio, BIO_UPTODATE);

	e = bio->bi_private;
	mdev = e->mdev;

	if (!error && !uptodate) {
		/* strange behaviour of some lower level drivers...
		 * fail the request by clearing the uptodate flag,
		 * but do not return any error?!
		 * do we want to drbd_WARN() on this? */
		error = -EIO;
	}

	/* error == -ENOTSUPP would be a better test,
	 * alas it is not reliable */
	if (error && e->flags & EE_IS_BARRIER) {
		drbd_bump_write_ordering(mdev, WO_bdev_flush);
		spin_lock_irqsave(&mdev->req_lock, flags);
		list_del(&e->w.list);
		e->w.cb = w_e_reissue;
		__release(local); /* Actually happens in w_e_reissue. */
		spin_unlock_irqrestore(&mdev->req_lock, flags);
		drbd_queue_work(&mdev->data.work, &e->w);
		return;
	}

	D_ASSERT(e->block_id != ID_VACANT);

	dump_internal_bio("Sec", mdev, bio, 1);

	spin_lock_irqsave(&mdev->req_lock, flags);
	mdev->writ_cnt += e->size >> 9;
	is_syncer_req = is_syncer_block_id(e->block_id);

	/* after we moved e to done_ee,
	 * we may no longer access it,
	 * it may be freed/reused already!
	 * (as soon as we release the req_lock) */
	e_sector = e->sector;
	do_al_complete_io = e->flags & EE_CALL_AL_COMPLETE_IO;

	list_del(&e->w.list); /* has been on active_ee or sync_ee */
	list_add_tail(&e->w.list, &mdev->done_ee);

	MTRACE(TraceTypeEE, TraceLvlAll,
	       INFO("Moved EE (WRITE) to done_ee sec=%llus size=%u ee=%p\n",
		    (unsigned long long)e->sector, e->size, e);
	       );

	/* No hlist_del_init(&e->colision) here, we did not send the Ack yet,
	 * neither did we wake possibly waiting conflicting requests.
	 * done from "drbd_process_done_ee" within the appropriate w.cb
	 * (e_end_block/e_end_resync_block) or from _drbd_clear_done_ee */

	do_wake = is_syncer_req
		? list_empty(&mdev->sync_ee)
		: list_empty(&mdev->active_ee);

	if (error)
		__drbd_chk_io_error(mdev, FALSE);
	spin_unlock_irqrestore(&mdev->req_lock, flags);

	if (is_syncer_req)
		drbd_rs_complete_io(mdev, e_sector);

	if (do_wake)
		wake_up(&mdev->ee_wait);

	if (do_al_complete_io)
		drbd_al_complete_io(mdev, e_sector);

	wake_asender(mdev);
	dec_local(mdev);

}

/* read, readA or write requests on Primary comming from drbd_make_request
 */
void drbd_endio_pri(struct bio *bio, int error)
{
	unsigned long flags;
	struct drbd_request *req = bio->bi_private;
	struct drbd_conf *mdev = req->mdev;
	enum drbd_req_event what;
	int uptodate = bio_flagged(bio, BIO_UPTODATE);

	if (!error && !uptodate) {
		/* strange behaviour of some lower level drivers...
		 * fail the request by clearing the uptodate flag,
		 * but do not return any error?!
		 * do we want to drbd_WARN() on this? */
		error = -EIO;
	}

	dump_internal_bio("Pri", mdev, bio, 1);

	/* to avoid recursion in _req_mod */
	what = error
	       ? (bio_data_dir(bio) == WRITE)
		 ? write_completed_with_error
		 : read_completed_with_error
	       : completed_ok;
	spin_lock_irqsave(&mdev->req_lock, flags);
	_req_mod(req, what, error);
	spin_unlock_irqrestore(&mdev->req_lock, flags);
}

int w_io_error(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct drbd_request *req = (struct drbd_request *)w;
	int ok;

	/* NOTE: mdev->bc can be NULL by the time we get here! */
	/* D_ASSERT(mdev->bc->dc.on_io_error != PassOn); */

	/* the only way this callback is scheduled is from _req_may_be_done,
	 * when it is done and had a local write error, see comments there */
	drbd_req_free(req);

	ok = drbd_io_error(mdev, FALSE);
	if (unlikely(!ok))
		ERR("Sending in w_io_error() failed\n");
	return ok;
}

int w_read_retry_remote(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct drbd_request *req = (struct drbd_request *)w;

	/* We should not detach for read io-error,
	 * but try to WRITE the DataReply to the failed location,
	 * to give the disk the chance to relocate that block */
	drbd_io_error(mdev, FALSE); /* tries to schedule a detach and notifies peer */

	spin_lock_irq(&mdev->req_lock);
	if (cancel ||
	    mdev->state.conn < Connected ||
	    mdev->state.pdsk <= Inconsistent) {
		_req_mod(req, send_canceled, 0);
		spin_unlock_irq(&mdev->req_lock);
		ALERT("WE ARE LOST. Local IO failure, no peer.\n");
		return 1;
	}
	spin_unlock_irq(&mdev->req_lock);

	return w_send_read_req(mdev, w, 0);
}

int w_resync_inactive(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	ERR_IF(cancel) return 1;
	ERR("resync inactive, but callback triggered??\n");
	return 1; /* Simply ignore this! */
}

STATIC void drbd_csum(struct drbd_conf *mdev, struct crypto_hash *tfm, struct bio *bio, void *digest)
{
	struct hash_desc desc;
	struct scatterlist sg;
	struct bio_vec *bvec;
	int i;

	desc.tfm = tfm;
	desc.flags = 0;

	sg_init_table(&sg, 1);
	crypto_hash_init(&desc);

	__bio_for_each_segment(bvec, bio, i, 0) {
		sg_set_page(&sg, bvec->bv_page, bvec->bv_len, bvec->bv_offset);
		crypto_hash_update(&desc, &sg, sg.length);
	}
	crypto_hash_final(&desc, digest);
}

STATIC int w_e_send_csum(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry *)w;
	int digest_size;
	void *digest;
	int ok;

	D_ASSERT(e->block_id == DRBD_MAGIC + 0xbeef);

	if (unlikely(cancel)) {
		drbd_free_ee(mdev, e);
		return 1;
	}

	if (likely(drbd_bio_uptodate(e->private_bio))) {
		digest_size = crypto_hash_digestsize(mdev->csums_tfm);
		digest = kmalloc(digest_size, GFP_KERNEL);
		if (digest) {
			drbd_csum(mdev, mdev->csums_tfm, e->private_bio, digest);

			inc_rs_pending(mdev);
			ok = drbd_send_drequest_csum(mdev,
						     e->sector,
						     e->size,
						     digest,
						     digest_size,
						     CsumRSRequest);
			kfree(digest);
		} else {
			ERR("kmalloc() of digest failed.\n");
			ok = 0;
		}
	} else {
		drbd_io_error(mdev, FALSE);
		ok = 1;
	}

	drbd_free_ee(mdev, e);

	if (unlikely(!ok))
		ERR("drbd_send_drequest(..., csum) failed\n");
	return ok;
}

#define GFP_TRY	(__GFP_HIGHMEM | __GFP_NOWARN)

STATIC int read_for_csum(struct drbd_conf *mdev, sector_t sector, int size)
{
	struct Tl_epoch_entry *e;

	if (!inc_local(mdev))
		return 0;

	if (FAULT_ACTIVE(mdev, DRBD_FAULT_AL_EE))
		return 2;

	e = drbd_alloc_ee(mdev, DRBD_MAGIC+0xbeef, sector, size, GFP_TRY);
	if (!e) {
		dec_local(mdev);
		return 2;
	}

	spin_lock_irq(&mdev->req_lock);
	list_add(&e->w.list, &mdev->read_ee);
	spin_unlock_irq(&mdev->req_lock);

	e->private_bio->bi_end_io = drbd_endio_read_sec;
	e->private_bio->bi_rw = READ;
	e->w.cb = w_e_send_csum;

	mdev->read_cnt += size >> 9;
	drbd_generic_make_request(mdev, DRBD_FAULT_RS_RD, e->private_bio);

	return 1;
}

void resync_timer_fn(unsigned long data)
{
	unsigned long flags;
	struct drbd_conf *mdev = (struct drbd_conf *) data;
	int queue;

	spin_lock_irqsave(&mdev->req_lock, flags);

	if (likely(!test_and_clear_bit(STOP_SYNC_TIMER, &mdev->flags))) {
		queue = 1;
		if (mdev->state.conn == VerifyS)
			mdev->resync_work.cb = w_make_ov_request;
		else
			mdev->resync_work.cb = w_make_resync_request;
	} else {
		queue = 0;
		mdev->resync_work.cb = w_resync_inactive;
	}

	spin_unlock_irqrestore(&mdev->req_lock, flags);

	/* harmless race: list_empty outside data.work.q_lock */
	if (list_empty(&mdev->resync_work.list) && queue)
		drbd_queue_work(&mdev->data.work, &mdev->resync_work);
}

int w_make_resync_request(struct drbd_conf *mdev,
		struct drbd_work *w, int cancel)
{
	unsigned long bit;
	sector_t sector;
	const sector_t capacity = drbd_get_capacity(mdev->this_bdev);
	int max_segment_size = queue_max_segment_size(mdev->rq_queue);
	int number, i, size;
	int align;

	if (unlikely(cancel))
		return 1;

	if (unlikely(mdev->state.conn < Connected)) {
		ERR("Confused in w_make_resync_request()! cstate < Connected");
		return 0;
	}

	if (mdev->state.conn != SyncTarget)
		ERR("%s in w_make_resync_request\n",
			conns_to_name(mdev->state.conn));

	if (!inc_local(mdev)) {
		/* Since we only need to access mdev->rsync a
		   inc_local_if_state(mdev,Failed) would be sufficient, but
		   to continue resync with a broken disk makes no sense at
		   all */
		ERR("Disk broke down during resync!\n");
		mdev->resync_work.cb = w_resync_inactive;
		return 1;
	}
	/* All goto requeses have to happend after this block: inc_local() */

	number = SLEEP_TIME*mdev->sync_conf.rate / ((BM_BLOCK_SIZE/1024)*HZ);

	if (atomic_read(&mdev->rs_pending_cnt) > number)
		goto requeue;
	number -= atomic_read(&mdev->rs_pending_cnt);

	for (i = 0; i < number; i++) {
next_sector:
		size = BM_BLOCK_SIZE;
		bit  = drbd_bm_find_next(mdev, mdev->bm_resync_fo);

		if (bit == -1UL) {
			mdev->bm_resync_fo = drbd_bm_bits(mdev);
			mdev->resync_work.cb = w_resync_inactive;
			dec_local(mdev);
			return 1;
		}

		sector = BM_BIT_TO_SECT(bit);

		if (drbd_try_rs_begin_io(mdev, sector)) {
			mdev->bm_resync_fo = bit;
			goto requeue;
		}
		mdev->bm_resync_fo = bit + 1;

		if (unlikely(drbd_bm_test_bit(mdev, bit) == 0)) {
			drbd_rs_complete_io(mdev, sector);
			goto next_sector;
		}

#if DRBD_MAX_SEGMENT_SIZE > BM_BLOCK_SIZE
		/* try to find some adjacent bits.
		 * we stop if we have already the maximum req size.
		 *
		 * Aditionally always align bigger requests, in order to
		 * be prepared for all stripe sizes of software RAIDs.
		 *
		 * we _do_ care about the agreed-uppon q->max_segment_size
		 * here, as splitting up the requests on the other side is more
		 * difficult.  the consequence is, that on lvm and md and other
		 * "indirect" devices, this is dead code, since
		 * q->max_segment_size will be PAGE_SIZE.
		 */
		align = 1;
		for (;;) {
			if (size + BM_BLOCK_SIZE > max_segment_size)
				break;

			/* Be always aligned */
			if (sector & ((1<<(align+3))-1))
				break;

			/* do not cross extent boundaries */
			if (((bit+1) & BM_BLOCKS_PER_BM_EXT_MASK) == 0)
				break;
			/* now, is it actually dirty, after all?
			 * caution, drbd_bm_test_bit is tri-state for some
			 * obscure reason; ( b == 0 ) would get the out-of-band
			 * only accidentally right because of the "oddly sized"
			 * adjustment below */
			if (drbd_bm_test_bit(mdev, bit+1) != 1)
				break;
			bit++;
			size += BM_BLOCK_SIZE;
			if ((BM_BLOCK_SIZE << align) <= size)
				align++;
			i++;
		}
		/* if we merged some,
		 * reset the offset to start the next drbd_bm_find_next from */
		if (size > BM_BLOCK_SIZE)
			mdev->bm_resync_fo = bit + 1;
#endif

		/* adjust very last sectors, in case we are oddly sized */
		if (sector + (size>>9) > capacity)
			size = (capacity-sector)<<9;
		if (mdev->agreed_pro_version >= 89 && mdev->csums_tfm) {
			switch (read_for_csum(mdev, sector, size)) {
			case 0: /* Disk failure*/
				dec_local(mdev);
				return 0;
			case 2: /* Allocation failed */
				drbd_rs_complete_io(mdev, sector);
				mdev->bm_resync_fo = BM_SECT_TO_BIT(sector);
				goto requeue;
			/* case 1: everything ok */
			}
		} else {
			inc_rs_pending(mdev);
			if (!drbd_send_drequest(mdev, RSDataRequest,
					       sector, size, ID_SYNCER)) {
				ERR("drbd_send_drequest() failed, aborting...\n");
				dec_rs_pending(mdev);
				dec_local(mdev);
				return 0;
			}
		}
	}

	if (mdev->bm_resync_fo >= drbd_bm_bits(mdev)) {
		/* last syncer _request_ was sent,
		 * but the RSDataReply not yet received.  sync will end (and
		 * next sync group will resume), as soon as we receive the last
		 * resync data block, and the last bit is cleared.
		 * until then resync "work" is "inactive" ...
		 */
		mdev->resync_work.cb = w_resync_inactive;
		dec_local(mdev);
		return 1;
	}

 requeue:
	mod_timer(&mdev->resync_timer, jiffies + SLEEP_TIME);
	dec_local(mdev);
	return 1;
}

int w_make_ov_request(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	int number, i, size;
	sector_t sector;
	const sector_t capacity = drbd_get_capacity(mdev->this_bdev);

	if (unlikely(cancel))
		return 1;

	if (unlikely(mdev->state.conn < Connected)) {
		ERR("Confused in w_make_ov_request()! cstate < Connected");
		return 0;
	}

	number = SLEEP_TIME*mdev->sync_conf.rate / ((BM_BLOCK_SIZE/1024)*HZ);
	if (atomic_read(&mdev->rs_pending_cnt) > number)
		goto requeue;

	number -= atomic_read(&mdev->rs_pending_cnt);

	sector = mdev->ov_position;
	for (i = 0; i < number; i++) {
		size = BM_BLOCK_SIZE;

		if (drbd_try_rs_begin_io(mdev, sector)) {
			mdev->ov_position = sector;
			goto requeue;
		}

		if (sector + (size>>9) > capacity)
			size = (capacity-sector)<<9;

		inc_rs_pending(mdev);
		if (!drbd_send_ov_request(mdev, sector, size)) {
			dec_rs_pending(mdev);
			return 0;
		}
		sector += BM_SECT_PER_BIT;
		if (sector >= capacity) {
			mdev->resync_work.cb = w_resync_inactive;

			return 1;
		}
	}
	mdev->ov_position = sector;

 requeue:
	mod_timer(&mdev->resync_timer, jiffies + SLEEP_TIME);
	return 1;
}


int w_ov_finished(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	kfree(w);
	ov_oos_print(mdev);
	drbd_resync_finished(mdev);

	return 1;
}

STATIC int w_resync_finished(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	kfree(w);

	drbd_resync_finished(mdev);

	return 1;
}

int drbd_resync_finished(struct drbd_conf *mdev)
{
	unsigned long db, dt, dbdt;
	unsigned long n_oos;
	union drbd_state_t os, ns;
	struct drbd_work *w;
	char *khelper_cmd = NULL;

	/* Remove all elements from the resync LRU. Since future actions
	 * might set bits in the (main) bitmap, then the entries in the
	 * resync LRU would be wrong. */
	if (drbd_rs_del_all(mdev)) {
		/* In case this is not possible now, most probabely because
		 * there are RSDataReply Packets lingering on the worker's
		 * queue (or even the read operations for those packets
		 * is not finished by now).   Retry in 100ms. */

		drbd_kick_lo(mdev);
		__set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ / 10);
		w = kmalloc(sizeof(struct drbd_work), GFP_ATOMIC);
		if (w) {
			w->cb = w_resync_finished;
			drbd_queue_work(&mdev->data.work, w);
			return 1;
		}
		ERR("Warn failed to drbd_rs_del_all() and to kmalloc(w).\n");
	}

	dt = (jiffies - mdev->rs_start - mdev->rs_paused) / HZ;
	if (dt <= 0)
		dt = 1;
	db = mdev->rs_total;
	dbdt = Bit2KB(db/dt);
	mdev->rs_paused /= HZ;

	if (!inc_local(mdev))
		goto out;

	spin_lock_irq(&mdev->req_lock);
	os = mdev->state;

	/* This protects us against multiple calls (that can happen in the presence
	   of application IO), and against connectivity loss just before we arrive here. */
	if (os.conn <= Connected)
		goto out_unlock;

	ns = os;
	ns.conn = Connected;

	INFO("%s done (total %lu sec; paused %lu sec; %lu K/sec)\n",
	     (os.conn == VerifyS || os.conn == VerifyT) ?
	     "Online verify " : "Resync",
	     dt + mdev->rs_paused, mdev->rs_paused, dbdt);

	n_oos = drbd_bm_total_weight(mdev);

	if (os.conn == VerifyS || os.conn == VerifyT) {
		if (n_oos) {
			ALERT("Online verify found %lu %dk block out of sync!\n",
			      n_oos, Bit2KB(1));
			khelper_cmd = "out-of-sync";
		}
	} else {
		D_ASSERT((n_oos - mdev->rs_failed) == 0);

		if (os.conn == SyncTarget || os.conn == PausedSyncT)
			khelper_cmd = "after-resync-target";

		if (mdev->csums_tfm && mdev->rs_total) {
			const unsigned long s = mdev->rs_same_csum;
			const unsigned long t = mdev->rs_total;
			const int ratio =
				(t == 0)     ? 0 :
			(t < 100000) ? ((s*100)/t) : (s/(t/100));
			INFO("%u %% had equal check sums, eliminated: %luK; "
			     "transferred %luK total %luK\n",
			     ratio,
			     Bit2KB(mdev->rs_same_csum),
			     Bit2KB(mdev->rs_total - mdev->rs_same_csum),
			     Bit2KB(mdev->rs_total));
		}
	}

	if (mdev->rs_failed) {
		INFO("            %lu failed blocks\n", mdev->rs_failed);

		if (os.conn == SyncTarget || os.conn == PausedSyncT) {
			ns.disk = Inconsistent;
			ns.pdsk = UpToDate;
		} else {
			ns.disk = UpToDate;
			ns.pdsk = Inconsistent;
		}
	} else {
		ns.disk = UpToDate;
		ns.pdsk = UpToDate;

		if (os.conn == SyncTarget || os.conn == PausedSyncT) {
			if (mdev->p_uuid) {
				int i;
				for (i = Bitmap ; i <= History_end ; i++)
					_drbd_uuid_set(mdev, i, mdev->p_uuid[i]);
				drbd_uuid_set(mdev, Bitmap, mdev->bc->md.uuid[Current]);
				_drbd_uuid_set(mdev, Current, mdev->p_uuid[Current]);
			} else {
				ERR("mdev->p_uuid is NULL! BUG\n");
			}
		}

		drbd_uuid_set_bm(mdev, 0UL);

		if (mdev->p_uuid) {
			/* Now the two UUID sets are equal, update what we
			 * know of the peer. */
			int i;
			for (i = Current ; i <= History_end ; i++)
				mdev->p_uuid[i] = mdev->bc->md.uuid[i];
		}
	}

	_drbd_set_state(mdev, ns, ChgStateVerbose, NULL);
out_unlock:
	spin_unlock_irq(&mdev->req_lock);
	dec_local(mdev);
out:
	mdev->rs_total  = 0;
	mdev->rs_failed = 0;
	mdev->rs_paused = 0;

	if (test_and_clear_bit(WRITE_BM_AFTER_RESYNC, &mdev->flags)) {
		drbd_WARN("Writing the whole bitmap, due to failed kmalloc\n");
		drbd_queue_bitmap_io(mdev, &drbd_bm_write, NULL, "write from resync_finished");
	}

	drbd_bm_recount_bits(mdev);

	if (khelper_cmd)
		drbd_khelper(mdev, khelper_cmd);

	return 1;
}

/**
 * w_e_end_data_req: Send the answer (DataReply) in response to a DataRequest.
 */
int w_e_end_data_req(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry *)w;
	int ok;

	if (unlikely(cancel)) {
		drbd_free_ee(mdev, e);
		dec_unacked(mdev);
		return 1;
	}

	if (likely(drbd_bio_uptodate(e->private_bio))) {
		ok = drbd_send_block(mdev, DataReply, e);
	} else {
		if (__ratelimit(&drbd_ratelimit_state))
			ERR("Sending NegDReply. sector=%llus.\n",
			    (unsigned long long)e->sector);

		ok = drbd_send_ack(mdev, NegDReply, e);

		drbd_io_error(mdev, FALSE);
	}

	dec_unacked(mdev);

	spin_lock_irq(&mdev->req_lock);
	if (drbd_bio_has_active_page(e->private_bio)) {
		/* This might happen if sendpage() has not finished */
		list_add_tail(&e->w.list, &mdev->net_ee);
	} else {
		drbd_free_ee(mdev, e);
	}
	spin_unlock_irq(&mdev->req_lock);

	if (unlikely(!ok))
		ERR("drbd_send_block() failed\n");
	return ok;
}

/**
 * w_e_end_rsdata_req: Send the answer (RSDataReply) to a RSDataRequest.
 */
int w_e_end_rsdata_req(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry *)w;
	int ok;

	if (unlikely(cancel)) {
		drbd_free_ee(mdev, e);
		dec_unacked(mdev);
		return 1;
	}

	if (inc_local_if_state(mdev, Failed)) {
		drbd_rs_complete_io(mdev, e->sector);
		dec_local(mdev);
	}

	if (likely(drbd_bio_uptodate(e->private_bio))) {
		if (likely(mdev->state.pdsk >= Inconsistent)) {
			inc_rs_pending(mdev);
			ok = drbd_send_block(mdev, RSDataReply, e);
		} else {
			if (__ratelimit(&drbd_ratelimit_state))
				ERR("Not sending RSDataReply, "
				    "partner DISKLESS!\n");
			ok = 1;
		}
	} else {
		if (__ratelimit(&drbd_ratelimit_state))
			ERR("Sending NegRSDReply. sector %llus.\n",
			    (unsigned long long)e->sector);

		ok = drbd_send_ack(mdev, NegRSDReply, e);

		drbd_io_error(mdev, FALSE);

		/* update resync data with failure */
		drbd_rs_failed_io(mdev, e->sector, e->size);
	}

	dec_unacked(mdev);

	spin_lock_irq(&mdev->req_lock);
	if (drbd_bio_has_active_page(e->private_bio)) {
		/* This might happen if sendpage() has not finished */
		list_add_tail(&e->w.list, &mdev->net_ee);
	} else {
		drbd_free_ee(mdev, e);
	}
	spin_unlock_irq(&mdev->req_lock);

	if (unlikely(!ok))
		ERR("drbd_send_block() failed\n");
	return ok;
}

int w_e_end_csum_rs_req(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry *)w;
	struct digest_info *di;
	int digest_size;
	void *digest = NULL;
	int ok, eq = 0;

	if (unlikely(cancel)) {
		drbd_free_ee(mdev, e);
		dec_unacked(mdev);
		return 1;
	}

	drbd_rs_complete_io(mdev, e->sector);

	di = (struct digest_info *)(unsigned long)e->block_id;

	if (likely(drbd_bio_uptodate(e->private_bio))) {
		/* quick hack to try to avoid a race against reconfiguration.
		 * a real fix would be much more involved,
		 * introducing more locking mechanisms */
		if (mdev->csums_tfm) {
			digest_size = crypto_hash_digestsize(mdev->csums_tfm);
			D_ASSERT(digest_size == di->digest_size);
			digest = kmalloc(digest_size, GFP_KERNEL);
		}
		if (digest) {
			drbd_csum(mdev, mdev->csums_tfm, e->private_bio, digest);
			eq = !memcmp(digest, di->digest, digest_size);
			kfree(digest);
		}

		if (eq) {
			drbd_set_in_sync(mdev, e->sector, e->size);
			mdev->rs_same_csum++;
			ok = drbd_send_ack(mdev, RSIsInSync, e);
		} else {
			inc_rs_pending(mdev);
			e->block_id = ID_SYNCER;
			ok = drbd_send_block(mdev, RSDataReply, e);
		}
	} else {
		ok = drbd_send_ack(mdev, NegRSDReply, e);
		if (__ratelimit(&drbd_ratelimit_state))
			ERR("Sending NegDReply. I guess it gets messy.\n");
		drbd_io_error(mdev, FALSE);
	}

	dec_unacked(mdev);

	kfree(di);

	spin_lock_irq(&mdev->req_lock);
	if (drbd_bio_has_active_page(e->private_bio)) {
		/* This might happen if sendpage() has not finished */
		list_add_tail(&e->w.list, &mdev->net_ee);
	} else {
		drbd_free_ee(mdev, e);
	}
	spin_unlock_irq(&mdev->req_lock);

	if (unlikely(!ok))
		ERR("drbd_send_block/ack() failed\n");
	return ok;
}

int w_e_end_ov_req(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry *)w;
	int digest_size;
	void *digest;
	int ok = 1;

	if (unlikely(cancel)) {
		drbd_free_ee(mdev, e);
		dec_unacked(mdev);
		return 1;
	}

	if (likely(drbd_bio_uptodate(e->private_bio))) {
		digest_size = crypto_hash_digestsize(mdev->verify_tfm);
		digest = kmalloc(digest_size, GFP_KERNEL);
		if (digest) {
			drbd_csum(mdev, mdev->verify_tfm, e->private_bio, digest);
			ok = drbd_send_drequest_csum(mdev, e->sector, e->size,
						     digest, digest_size, OVReply);
			if (ok)
				inc_rs_pending(mdev);
			kfree(digest);
		}
	}

	dec_unacked(mdev);

	spin_lock_irq(&mdev->req_lock);
	drbd_free_ee(mdev, e);
	spin_unlock_irq(&mdev->req_lock);

	return ok;
}

void drbd_ov_oos_found(struct drbd_conf *mdev, sector_t sector, int size)
{
	if (mdev->ov_last_oos_start + mdev->ov_last_oos_size == sector) {
		mdev->ov_last_oos_size += size>>9;
	} else {
		mdev->ov_last_oos_start = sector;
		mdev->ov_last_oos_size = size>>9;
	}
	drbd_set_out_of_sync(mdev, sector, size);
	set_bit(WRITE_BM_AFTER_RESYNC, &mdev->flags);
}

int w_e_end_ov_reply(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry *)w;
	struct digest_info *di;
	int digest_size;
	void *digest;
	int ok, eq = 0;

	if (unlikely(cancel)) {
		drbd_free_ee(mdev, e);
		dec_unacked(mdev);
		return 1;
	}

	/* after "cancel", because after drbd_disconnect/drbd_rs_cancel_all
	 * the resync lru has been cleaned up already */
	drbd_rs_complete_io(mdev, e->sector);

	di = (struct digest_info *)(unsigned long)e->block_id;

	if (likely(drbd_bio_uptodate(e->private_bio))) {
		digest_size = crypto_hash_digestsize(mdev->verify_tfm);
		digest = kmalloc(digest_size, GFP_KERNEL);
		if (digest) {
			drbd_csum(mdev, mdev->verify_tfm, e->private_bio, digest);

			D_ASSERT(digest_size == di->digest_size);
			eq = !memcmp(digest, di->digest, digest_size);
			kfree(digest);
		}
	} else {
		ok = drbd_send_ack(mdev, NegRSDReply, e);
		if (__ratelimit(&drbd_ratelimit_state))
			ERR("Sending NegDReply. I guess it gets messy.\n");
		drbd_io_error(mdev, FALSE);
	}

	dec_unacked(mdev);

	kfree(di);

	if (!eq)
		drbd_ov_oos_found(mdev, e->sector, e->size);
	else
		ov_oos_print(mdev);

	ok = drbd_send_ack_ex(mdev, OVResult, e->sector, e->size,
			      eq ? ID_IN_SYNC : ID_OUT_OF_SYNC);

	spin_lock_irq(&mdev->req_lock);
	drbd_free_ee(mdev, e);
	spin_unlock_irq(&mdev->req_lock);

	if (--mdev->ov_left == 0) {
		ov_oos_print(mdev);
		drbd_resync_finished(mdev);
	}

	return ok;
}

int w_prev_work_done(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	clear_bit(WORK_PENDING, &mdev->flags);
	wake_up(&mdev->misc_wait);
	return 1;
}

int w_send_barrier(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct drbd_barrier *b = (struct drbd_barrier *)w;
	struct Drbd_Barrier_Packet *p = &mdev->data.sbuf.Barrier;
	int ok = 1;

	/* really avoid racing with tl_clear.  w.cb may have been referenced
	 * just before it was reassigned and requeued, so double check that.
	 * actually, this race was harmless, since we only try to send the
	 * barrier packet here, and otherwise do nothing with the object.
	 * but compare with the head of w_clear_epoch */
	spin_lock_irq(&mdev->req_lock);
	if (w->cb != w_send_barrier || mdev->state.conn < Connected)
		cancel = 1;
	spin_unlock_irq(&mdev->req_lock);
	if (cancel)
		return 1;

	if (!drbd_get_data_sock(mdev))
		return 0;
	p->barrier = b->br_number;
	/* inc_ap_pending was done where this was queued.
	 * dec_ap_pending will be done in got_BarrierAck
	 * or (on connection loss) in w_clear_epoch.  */
	ok = _drbd_send_cmd(mdev, mdev->data.socket, Barrier,
				(struct Drbd_Header *)p, sizeof(*p), 0);
	drbd_put_data_sock(mdev);

	return ok;
}

int w_send_write_hint(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	if (cancel)
		return 1;
	return drbd_send_short_cmd(mdev, UnplugRemote);
}

/**
 * w_send_dblock: Send a mirrored write request.
 */
int w_send_dblock(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct drbd_request *req = (struct drbd_request *)w;
	int ok;

	if (unlikely(cancel)) {
		req_mod(req, send_canceled, 0);
		return 1;
	}

	ok = drbd_send_dblock(mdev, req);
	req_mod(req, ok ? handed_over_to_network : send_failed, 0);

	return ok;
}

/**
 * w_send_read_req: Send a read requests.
 */
int w_send_read_req(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct drbd_request *req = (struct drbd_request *)w;
	int ok;

	if (unlikely(cancel)) {
		req_mod(req, send_canceled, 0);
		return 1;
	}

	ok = drbd_send_drequest(mdev, DataRequest, req->sector, req->size,
				(unsigned long)req);

	if (!ok) {
		/* ?? we set Timeout or BrokenPipe in drbd_send();
		 * so this is probably redundant */
		if (mdev->state.conn >= Connected)
			drbd_force_state(mdev, NS(conn, NetworkFailure));
	}
	req_mod(req, ok ? handed_over_to_network : send_failed, 0);

	return ok;
}

STATIC int _drbd_may_sync_now(struct drbd_conf *mdev)
{
	struct drbd_conf *odev = mdev;

	while (1) {
		if (odev->sync_conf.after == -1)
			return 1;
		odev = minor_to_mdev(odev->sync_conf.after);
		ERR_IF(!odev) return 1;
		if ((odev->state.conn >= SyncSource &&
		     odev->state.conn <= PausedSyncT) ||
		    odev->state.aftr_isp || odev->state.peer_isp ||
		    odev->state.user_isp)
			return 0;
	}
}

/**
 * _drbd_pause_after:
 * Finds all devices that may not resync now, and causes them to
 * pause their resynchronisation.
 * Called from process context only (admin command and after_state_ch).
 */
STATIC int _drbd_pause_after(struct drbd_conf *mdev)
{
	struct drbd_conf *odev;
	int i, rv = 0;

	for (i = 0; i < minor_count; i++) {
		odev = minor_to_mdev(i);
		if (!odev)
			continue;
		if (odev->state.conn == StandAlone && odev->state.disk == Diskless)
			continue;
		if (!_drbd_may_sync_now(odev))
			rv |= (__drbd_set_state(_NS(odev, aftr_isp, 1), ChgStateHard, NULL)
			       != SS_NothingToDo);
	}

	return rv;
}

/**
 * _drbd_resume_next:
 * Finds all devices that can resume resynchronisation
 * process, and causes them to resume.
 * Called from process context only (admin command and worker).
 */
STATIC int _drbd_resume_next(struct drbd_conf *mdev)
{
	struct drbd_conf *odev;
	int i, rv = 0;

	for (i = 0; i < minor_count; i++) {
		odev = minor_to_mdev(i);
		if (!odev)
			continue;
		if (odev->state.conn == StandAlone && odev->state.disk == Diskless)
			continue;
		if (odev->state.aftr_isp) {
			if (_drbd_may_sync_now(odev))
				rv |= (__drbd_set_state(_NS(odev, aftr_isp, 0),
							ChgStateHard, NULL)
				       != SS_NothingToDo) ;
		}
	}
	return rv;
}

void resume_next_sg(struct drbd_conf *mdev)
{
	write_lock_irq(&global_state_lock);
	_drbd_resume_next(mdev);
	write_unlock_irq(&global_state_lock);
}

void suspend_other_sg(struct drbd_conf *mdev)
{
	write_lock_irq(&global_state_lock);
	_drbd_pause_after(mdev);
	write_unlock_irq(&global_state_lock);
}

void drbd_alter_sa(struct drbd_conf *mdev, int na)
{
	int changes;

	write_lock_irq(&global_state_lock);
	mdev->sync_conf.after = na;

	do {
		changes  = _drbd_pause_after(mdev);
		changes |= _drbd_resume_next(mdev);
	} while (changes);

	write_unlock_irq(&global_state_lock);
}

/**
 * drbd_start_resync:
 * @side: Either SyncSource or SyncTarget
 * Start the resync process. Called from process context only,
 * either admin command or drbd_receiver.
 * Note, this function might bring you directly into one of the
 * PausedSync* states.
 */
void drbd_start_resync(struct drbd_conf *mdev, enum drbd_conns side)
{
	union drbd_state_t ns;
	int r;

	MTRACE(TraceTypeResync, TraceLvlSummary,
	       INFO("Resync starting: side=%s\n",
		    side == SyncTarget ? "SyncTarget" : "SyncSource");
	    );

	drbd_bm_recount_bits(mdev);

	/* In case a previous resync run was aborted by an IO error... */
	drbd_rs_cancel_all(mdev);

	if (side == SyncTarget) {
		/* Since application IO was locked out during WFBitMapT and
		   WFSyncUUID we are still unmodified. Before going to SyncTarget
		   we check that we might make the data inconsistent. */
		r = drbd_khelper(mdev, "before-resync-target");
		r = (r >> 8) & 0xff;
		if (r > 0) {
			INFO("before-resync-target handler returned %d, "
			     "dropping connection.\n", r);
			drbd_force_state(mdev, NS(conn, Disconnecting));
			return;
		}
	}

	drbd_state_lock(mdev);

	if (!inc_local_if_state(mdev, Negotiating)) {
		drbd_state_unlock(mdev);
		return;
	}

	if (side == SyncTarget) {
		mdev->bm_resync_fo = 0;
	} else /* side == SyncSource */ {
		u64 uuid;

		get_random_bytes(&uuid, sizeof(u64));
		drbd_uuid_set(mdev, Bitmap, uuid);
		drbd_send_sync_uuid(mdev, uuid);

		D_ASSERT(mdev->state.disk == UpToDate);
	}

	write_lock_irq(&global_state_lock);
	ns = mdev->state;

	ns.aftr_isp = !_drbd_may_sync_now(mdev);

	ns.conn = side;

	if (side == SyncTarget)
		ns.disk = Inconsistent;
	else /* side == SyncSource */
		ns.pdsk = Inconsistent;

	r = __drbd_set_state(mdev, ns, ChgStateVerbose, NULL);
	ns = mdev->state;

	if (ns.conn < Connected)
		r = SS_UnknownError;

	if (r == SS_Success) {
		mdev->rs_total     =
		mdev->rs_mark_left = drbd_bm_total_weight(mdev);
		mdev->rs_failed    = 0;
		mdev->rs_paused    = 0;
		mdev->rs_start     =
		mdev->rs_mark_time = jiffies;
		mdev->rs_same_csum = 0;
		_drbd_pause_after(mdev);
	}
	write_unlock_irq(&global_state_lock);
	drbd_state_unlock(mdev);
	dec_local(mdev);

	if (r == SS_Success) {
		INFO("Began resync as %s (will sync %lu KB [%lu bits set]).\n",
		     conns_to_name(ns.conn),
		     (unsigned long) mdev->rs_total << (BM_BLOCK_SIZE_B-10),
		     (unsigned long) mdev->rs_total);

		if (mdev->rs_total == 0) {
			drbd_resync_finished(mdev);
			return;
		}

		if (ns.conn == SyncTarget) {
			D_ASSERT(!test_bit(STOP_SYNC_TIMER, &mdev->flags));
			mod_timer(&mdev->resync_timer, jiffies);
		}

		drbd_md_sync(mdev);
	}
}

int drbd_worker(struct Drbd_thread *thi)
{
	struct drbd_conf *mdev = thi->mdev;
	struct drbd_work *w = NULL;
	LIST_HEAD(work_list);
	int intr = 0, i;

	sprintf(current->comm, "drbd%d_worker", mdev_to_minor(mdev));

	while (get_t_state(thi) == Running) {
		drbd_thread_current_set_cpu(mdev);

		if (down_trylock(&mdev->data.work.s)) {
			mutex_lock(&mdev->data.mutex);
			if (mdev->data.socket && !mdev->net_conf->no_cork)
				drbd_tcp_uncork(mdev->data.socket);
			mutex_unlock(&mdev->data.mutex);

			intr = down_interruptible(&mdev->data.work.s);

			mutex_lock(&mdev->data.mutex);
			if (mdev->data.socket  && !mdev->net_conf->no_cork)
				drbd_tcp_cork(mdev->data.socket);
			mutex_unlock(&mdev->data.mutex);
		}

		if (intr) {
			D_ASSERT(intr == -EINTR);
			flush_signals(current);
			ERR_IF (get_t_state(thi) == Running)
				continue;
			break;
		}

		if (get_t_state(thi) != Running)
			break;
		/* With this break, we have done a down() but not consumed
		   the entry from the list. The cleanup code takes care of
		   this...   */

		w = NULL;
		spin_lock_irq(&mdev->data.work.q_lock);
		ERR_IF(list_empty(&mdev->data.work.q)) {
			/* something terribly wrong in our logic.
			 * we were able to down() the semaphore,
			 * but the list is empty... doh.
			 *
			 * what is the best thing to do now?
			 * try again from scratch, restarting the receiver,
			 * asender, whatnot? could break even more ugly,
			 * e.g. when we are primary, but no good local data.
			 *
			 * I'll try to get away just starting over this loop.
			 */
			spin_unlock_irq(&mdev->data.work.q_lock);
			continue;
		}
		w = list_entry(mdev->data.work.q.next, struct drbd_work, list);
		list_del_init(&w->list);
		spin_unlock_irq(&mdev->data.work.q_lock);

		if (!w->cb(mdev, w, mdev->state.conn < Connected)) {
			/* drbd_WARN("worker: a callback failed! \n"); */
			if (mdev->state.conn >= Connected)
				drbd_force_state(mdev,
						NS(conn, NetworkFailure));
		}
	}

	spin_lock_irq(&mdev->data.work.q_lock);
	i = 0;
	while (!list_empty(&mdev->data.work.q)) {
		list_splice_init(&mdev->data.work.q, &work_list);
		spin_unlock_irq(&mdev->data.work.q_lock);

		while (!list_empty(&work_list)) {
			w = list_entry(work_list.next, struct drbd_work, list);
			list_del_init(&w->list);
			w->cb(mdev, w, 1);
			i++; /* dead debugging code */
		}

		spin_lock_irq(&mdev->data.work.q_lock);
	}
	sema_init(&mdev->data.work.s, 0);
	/* DANGEROUS race: if someone did queue his work within the spinlock,
	 * but up() ed outside the spinlock, we could get an up() on the
	 * semaphore without corresponding list entry.
	 * So don't do that.
	 */
	spin_unlock_irq(&mdev->data.work.q_lock);

	D_ASSERT(mdev->state.disk == Diskless && mdev->state.conn == StandAlone);
	/* _drbd_set_state only uses stop_nowait.
	 * wait here for the Exiting receiver. */
	drbd_thread_stop(&mdev->receiver);
	drbd_mdev_cleanup(mdev);

	INFO("worker terminated\n");

	return 0;
}