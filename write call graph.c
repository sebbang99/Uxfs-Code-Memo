/*
1. System call of Linux Kernel
*/
asmlinkage ssize_t sys_write(unsigned int fd, const char * buf, size_t count)
{
	ssize_t ret;
	struct file * file;

	ret = -EBADF;
	file = fget(fd);
	if (file) {
		if (file->f_mode & FMODE_WRITE) {
			struct inode *inode = file->f_dentry->d_inode;
			ret = locks_verify_area(FLOCK_VERIFY_WRITE, inode, file,
				file->f_pos, count);
			if (!ret) {
				ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
				ret = -EINVAL;
				if (file->f_op && (write = file->f_op->write) != NULL)
					ret = write(file, buf, count, &file->f_pos);
			}
		}
		if (ret > 0)
			dnotify_parent(file->f_dentry, DN_MODIFY);
		fput(file);
	}
	return ret;
}

/*
1.5. Connect to Uxfs-specfic write function
*/
struct file_operations ux_file_operations = {
        llseek:    generic_file_llseek,
        read:      generic_file_read,
        write:     generic_file_write,  // #################### here
        mmap:      generic_file_mmap,
};

/*
2. Uxfs-specific write function
*/
ssize_t
generic_file_write(struct file *file,const char *buf,size_t count, loff_t *ppos)
{
	struct address_space *mapping = file->f_dentry->d_inode->i_mapping;
	struct inode	*inode = mapping->host;
	unsigned long	limit = current->rlim[RLIMIT_FSIZE].rlim_cur;
	loff_t		pos;
	struct page	*page, *cached_page;
	ssize_t		written;
	long		status = 0;
	int		err;
	unsigned	bytes;

	if ((ssize_t) count < 0)
		return -EINVAL;

	if (!access_ok(VERIFY_READ, buf, count))
		return -EFAULT;

	cached_page = NULL;

	down(&inode->i_sem);

	pos = *ppos;
	err = -EINVAL;
	if (pos < 0)
		goto out;

	err = file->f_error;
	if (err) {
		file->f_error = 0;
		goto out;
	}

	written = 0;

	/* FIXME: this is for backwards compatibility with 2.4 */
	if (!S_ISBLK(inode->i_mode) && file->f_flags & O_APPEND)
		pos = inode->i_size;

	/*
	 * Check whether we've reached the file size limit.
	 */
	err = -EFBIG;
	
	if (limit != RLIM_INFINITY) {
		if (pos >= limit) {
			send_sig(SIGXFSZ, current, 0);
			goto out;
		}
		if (pos > 0xFFFFFFFFULL || count > limit - (u32)pos) {
			/* send_sig(SIGXFSZ, current, 0); */
			count = limit - (u32)pos;
		}
	}

	/*
	 *	LFS rule 
	 */
	if ( pos + count > MAX_NON_LFS && !(file->f_flags&O_LARGEFILE)) {
		if (pos >= MAX_NON_LFS) {
			send_sig(SIGXFSZ, current, 0);
			goto out;
		}
		if (count > MAX_NON_LFS - (u32)pos) {
			/* send_sig(SIGXFSZ, current, 0); */
			count = MAX_NON_LFS - (u32)pos;
		}
	}

	/*
	 *	Are we about to exceed the fs block limit ?
	 *
	 *	If we have written data it becomes a short write
	 *	If we have exceeded without writing data we send
	 *	a signal and give them an EFBIG.
	 *
	 *	Linus frestrict idea will clean these up nicely..
	 */
	 
	if (!S_ISBLK(inode->i_mode)) {
		if (pos >= inode->i_sb->s_maxbytes)
		{
			if (count || pos > inode->i_sb->s_maxbytes) {
				send_sig(SIGXFSZ, current, 0);
				err = -EFBIG;
				goto out;
			}
			/* zero-length writes at ->s_maxbytes are OK */
		}

		if (pos + count > inode->i_sb->s_maxbytes)
			count = inode->i_sb->s_maxbytes - pos;
	} else {
		if (is_read_only(inode->i_rdev)) {
			err = -EPERM;
			goto out;
		}
		if (pos >= inode->i_size) {
			if (count || pos > inode->i_size) {
				err = -ENOSPC;
				goto out;
			}
		}

		if (pos + count > inode->i_size)
			count = inode->i_size - pos;
	}

	err = 0;
	if (count == 0)
		goto out;

	remove_suid(inode);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	mark_inode_dirty_sync(inode);

	if (file->f_flags & O_DIRECT)
		goto o_direct;

    // ######################################################################## do-while loop
	do {
		unsigned long index, offset;
		long page_fault;
		char *kaddr;

		/*
		 * Try to find the page in the cache. If it isn't there,
		 * allocate a free page.
		 */
		offset = (pos & (PAGE_CACHE_SIZE -1)); /* Within page */
		index = pos >> PAGE_CACHE_SHIFT;
		bytes = PAGE_CACHE_SIZE - offset;
		if (bytes > count)
			bytes = count;

		/*
		 * Bring in the user page that we will copy from _first_.
		 * Otherwise there's a nasty deadlock on copying from the
		 * same page as we're writing to, without it being marked
		 * up-to-date.
		 */
		{ volatile unsigned char dummy;
			__get_user(dummy, buf);
			__get_user(dummy, buf+bytes-1);
		}

		status = -ENOMEM;	/* we'll assign it later anyway */
		page = __grab_cache_page(mapping, index, &cached_page); // write할 page 찾기. 리턴값으로 page 받음.
		if (!page)
			break;

		/* We have exclusive IO access to the page.. */
		if (!PageLocked(page)) {
			PAGE_BUG(page);
		}

		kaddr = kmap(page);
		status = mapping->a_ops->prepare_write(file, page, offset, offset+bytes);   // Uxfs의 prepare write
		if (status)
			goto sync_failure;
		page_fault = __copy_from_user(kaddr+offset, buf, bytes);    // user space에서 kernel space로 data copy
		flush_dcache_page(page);
		status = mapping->a_ops->commit_write(file, page, offset, offset+bytes);    // Uxfs의 commit write
		if (page_fault)
			goto fail_write;
		if (!status)
			status = bytes;

		if (status >= 0) {
			written += status;
			count -= status;
			pos += status;
			buf += status;
		}
unlock:
		kunmap(page);
		/* Mark it unlocked again and drop the page.. */
		SetPageReferenced(page);
		UnlockPage(page);
		page_cache_release(page);

		if (status < 0)
			break;
	} while (count);
done:
	*ppos = pos;

	if (cached_page)
		page_cache_release(cached_page);

	/* For now, when the user asks for O_SYNC, we'll actually
	 * provide O_DSYNC. */
	if (status >= 0) {
		if ((file->f_flags & O_SYNC) || IS_SYNC(inode))
			status = generic_osync_inode(inode, OSYNC_METADATA|OSYNC_DATA); // dish로 flush!!
	}
	
out_status:	
	err = written ? written : status;
out:

	up(&inode->i_sem);
	return err;
fail_write:
	status = -EFAULT;
	goto unlock;

sync_failure:
	/*
	 * If blocksize < pagesize, prepare_write() may have instantiated a
	 * few blocks outside i_size.  Trim these off again.
	 */
	kunmap(page);
	UnlockPage(page);
	page_cache_release(page);
	if (pos + bytes > inode->i_size)
		vmtruncate(inode, inode->i_size);
	goto done;

o_direct:
	written = generic_file_direct_IO(WRITE, file, (char *) buf, count, pos);
	if (written > 0) {
		loff_t end = pos + written;
		if (end > inode->i_size && !S_ISBLK(inode->i_mode)) {
			inode->i_size = end;
			mark_inode_dirty(inode);
		}
		*ppos = end;
		invalidate_inode_pages2(mapping);
	}
	/*
	 * Sync the fs metadata but not the minor inode changes and
	 * of course not the data as we did direct DMA for the IO.
	 */
	if (written >= 0 && file->f_flags & O_SYNC)
		status = generic_osync_inode(inode, OSYNC_METADATA);
	goto out_status;
}

/*
3.5. Connect to Uxfs's functions 
*/
struct address_space_operations ux_aops = {
        readpage:         ux_readpage,
        writepage:        ux_writepage,
        sync_page:        block_sync_page,
        prepare_write:    ux_prepare_write, // ##################### write 준비
        commit_write:     generic_commit_write, // ################# 커밋
        bmap:             ux_bmap,
};

/*
prepare_write의 flow ------------------------------------------------------------------------------------------------------------
*/
int
ux_prepare_write(struct file *file, struct page *page,
                 unsigned from, unsigned to)
{
        return block_prepare_write(page, from, to, ux_get_block);
}

int block_prepare_write(struct page *page, unsigned from, unsigned to,
			get_block_t *get_block)
{
	struct inode *inode = page->mapping->host;
	int err = __block_prepare_write(inode, page, from, to, get_block);
	if (err) {
		ClearPageUptodate(page);
		kunmap(page);
	}
	return err;
}

static int __block_prepare_write(struct inode *inode, struct page *page,
		unsigned from, unsigned to, get_block_t *get_block)
{
	unsigned block_start, block_end;
	unsigned long block;
	int err = 0;
	unsigned blocksize, bbits;
	struct buffer_head *bh, *head, *wait[2], **wait_bh=wait;    // buffer head (buffer는 disk block에 대응되는 memory object)
	char *kaddr = kmap(page);

	blocksize = 1 << inode->i_blkbits;
	if (!page->buffers)
		create_empty_buffers(page, inode->i_dev, blocksize);
	head = page->buffers;

	bbits = inode->i_blkbits;
	block = page->index << (PAGE_CACHE_SHIFT - bbits);

	for(bh = head, block_start = 0; bh != head || !block_start;
	    block++, block_start=block_end, bh = bh->b_this_page) {
		if (!bh)
			BUG();
		block_end = block_start+blocksize;
		if (block_end <= from)
			continue;
		if (block_start >= to)
			break;
		clear_bit(BH_New, &bh->b_state);
		if (!buffer_mapped(bh)) {
			err = get_block(inode, block, bh, 1);	// ux_get_block()
			if (err)
				goto out;
			if (buffer_new(bh)) {
				unmap_underlying_metadata(bh);
				if (Page_Uptodate(page)) {
					set_bit(BH_Uptodate, &bh->b_state);
					continue;
				}
				if (block_end > to)
					memset(kaddr+to, 0, block_end-to);
				if (block_start < from)
					memset(kaddr+block_start, 0, from-block_start);
				if (block_end > to || block_start < from)
					flush_dcache_page(page);
				continue;
			}
		}
		if (Page_Uptodate(page)) {
			set_bit(BH_Uptodate, &bh->b_state);
			continue; 
		}
		if (!buffer_uptodate(bh) &&
		     (block_start < from || block_end > to)) {
			ll_rw_block(READ, 1, &bh);
			*wait_bh++=bh;
		}
	}
	/*
	 * If we issued read requests - let them complete.
	 */
	while(wait_bh > wait) {
		wait_on_buffer(*--wait_bh);
		if (!buffer_uptodate(*wait_bh))
			return -EIO;
	}
	return 0;
out:
	/*
	 * Zero out any newly allocated blocks to avoid exposing stale
	 * data.  If BH_New is set, we know that the block was newly
	 * allocated in the above loop.
	 */
	bh = head;
	block_start = 0;
	do {
		block_end = block_start+blocksize;
		if (block_end <= from)
			goto next_bh;
		if (block_start >= to)
			break;
		if (buffer_new(bh)) {
			if (buffer_uptodate(bh))
				printk(KERN_ERR "%s: zeroing uptodate buffer!\n", __FUNCTION__);
			memset(kaddr+block_start, 0, bh->b_size);
			set_bit(BH_Uptodate, &bh->b_state);
			mark_buffer_dirty(bh);
		}
next_bh:
		block_start = block_end;
		bh = bh->b_this_page;
	} while (bh != head);
	return err;
}
// ------------------------------------------------------------------------------------------------------------------------------------

/*
commit write의 flow -------------------------------------------------------------------------------------------------------------------
*/
int generic_commit_write(struct file *file, struct page *page,
		unsigned from, unsigned to)
{
	struct inode *inode = page->mapping->host;
	loff_t pos = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;
	__block_commit_write(inode,page,from,to);
	kunmap(page);
	if (pos > inode->i_size) {
		inode->i_size = pos;
		mark_inode_dirty(inode);
	}
	return 0;
}

static int __block_commit_write(struct inode *inode, struct page *page,
		unsigned from, unsigned to)
{
	unsigned block_start, block_end;
	int partial = 0, need_balance_dirty = 0;
	unsigned blocksize;
	struct buffer_head *bh, *head;

	blocksize = 1 << inode->i_blkbits;

	for(bh = head = page->buffers, block_start = 0;
	    bh != head || !block_start;
	    block_start=block_end, bh = bh->b_this_page) {
		block_end = block_start + blocksize;
		if (block_end <= from || block_start >= to) {
			if (!buffer_uptodate(bh))
				partial = 1;
		} else {
			set_bit(BH_Uptodate, &bh->b_state);
			if (!atomic_set_buffer_dirty(bh)) {
				__mark_dirty(bh);   // 나중에 flush 될 수 있게 dirty 마킹
				buffer_insert_inode_data_queue(bh, inode);
				need_balance_dirty = 1;
			}
		}
	}

	if (need_balance_dirty)
		balance_dirty();
	/*
	 * is this a partial write that happened to make all buffers
	 * uptodate then we can optimize away a bogus readpage() for
	 * the next read(). Here we 'discover' wether the page went
	 * uptodate as a result of this (potentially partial) write.
	 */
	if (!partial)
		SetPageUptodate(page);
	return 0;
}

/**
 * generic_osync_inode - flush all dirty data for a given inode to disk
 * @inode: inode to write
 * @datasync: if set, don't bother flushing timestamps
 *
 * This can be called by file_write functions for files which have the
 * O_SYNC flag set, to flush dirty writes to disk.  
 */

int generic_osync_inode(struct inode *inode, int what)
{
	int err = 0, err2 = 0, need_write_inode_now = 0;
	
	/* 
	 * WARNING
	 *
	 * Currently, the filesystem write path does not pass the
	 * filp down to the low-level write functions.  Therefore it
	 * is impossible for (say) __block_commit_write to know if
	 * the operation is O_SYNC or not.
	 *
	 * Ideally, O_SYNC writes would have the filesystem call
	 * ll_rw_block as it went to kick-start the writes, and we
	 * could call osync_inode_buffers() here to wait only for
	 * those IOs which have already been submitted to the device
	 * driver layer.  As it stands, if we did this we'd not write
	 * anything to disk since our writes have not been queued by
	 * this point: they are still on the dirty LRU.
	 * 
	 * So, currently we will call fsync_inode_buffers() instead,
	 * to flush _all_ dirty buffers for this inode to disk on 
	 * every O_SYNC write, not just the synchronous I/Os.  --sct
	 */

	if (what & OSYNC_METADATA)
		err = fsync_inode_buffers(inode);
	if (what & OSYNC_DATA)
		err2 = fsync_inode_data_buffers(inode);
	if (!err)
		err = err2;

	spin_lock(&inode_lock);
	if ((inode->i_state & I_DIRTY) &&
	    ((what & OSYNC_INODE) || (inode->i_state & I_DIRTY_DATASYNC)))
		need_write_inode_now = 1;
	spin_unlock(&inode_lock);

	if (need_write_inode_now)
		write_inode_now(inode, 1);
	else
		wait_on_inode(inode);

	return err;
}