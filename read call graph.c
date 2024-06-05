struct file {
	...
	struct list_head	f_list;
	struct dentry		*f_dentry;

	struct file_operations	*f_op;
	atomic_t		f_count;
	unsigned int 		f_flags;
	mode_t			f_mode;
	loff_t			f_pos;
	...
};

/*
1. System call of Linux Kernel
*/
asmlinkage ssize_t sys_read(unsigned int fd, char * buf, size_t count)
{
	ssize_t ret;
	struct file * file;

	ret = -EBADF;
	file = fget(fd);
	if (file) {
		if (file->f_mode & FMODE_READ) {
			ret = locks_verify_area(FLOCK_VERIFY_READ, file->f_dentry->d_inode,
						file, file->f_pos, count);
			if (!ret) {
				ssize_t (*read)(struct file *, char *, size_t, loff_t *);
				ret = -EINVAL;
				if (file->f_op && (read = file->f_op->read) != NULL)
					ret = read(file, buf, count, &file->f_pos);     // ###############################################
			}
		}
		if (ret > 0)
			dnotify_parent(file->f_dentry, DN_ACCESS);
		fput(file);
	}
	return ret;
}

/*
1.5. Connect to Uxfs-specfic read function
*/
struct file_operations ux_file_operations = {
        llseek:    generic_file_llseek,
        read:      generic_file_read,	// #################### here
        write:     generic_file_write,
        mmap:      generic_file_mmap,
};

/*
2. Uxfs-specific read function
*/
ssize_t generic_file_read(struct file * filp, char * buf, size_t count, loff_t *ppos)
{
	ssize_t retval;

	if ((ssize_t) count < 0)
		return -EINVAL;

	if (filp->f_flags & O_DIRECT)	// Direct IO인지 체크
		goto o_direct;

	retval = -EFAULT;
	if (access_ok(VERIFY_WRITE, buf, count)) {
		retval = 0;

		if (count) {
			read_descriptor_t desc;

			desc.written = 0;
			desc.count = count;
			desc.buf = buf;
			desc.error = 0;
			do_generic_file_read(filp, ppos, &desc, file_read_actor);

			retval = desc.written;
			if (!retval)
				retval = desc.error;
		}
	}
 out:
	return retval;

 o_direct:
	{
		loff_t pos = *ppos, size;
		struct address_space *mapping = filp->f_dentry->d_inode->i_mapping;
		struct inode *inode = mapping->host;

		retval = 0;
		if (!count)
			goto out; /* skip atime */
		size = inode->i_size;
		if (pos < size) {
			if (pos + count > size)
				count = size - pos;
			retval = generic_file_direct_IO(READ, filp, buf, count, pos);
			if (retval > 0)
				*ppos = pos + retval;
		}
		UPDATE_ATIME(filp->f_dentry->d_inode);
		goto out;
	}
}

/*
3. Find page to read from page cache
*/
void do_generic_file_read(struct file * filp, loff_t *ppos, read_descriptor_t * desc, read_actor_t actor)
{
	struct address_space *mapping = filp->f_dentry->d_inode->i_mapping;
	struct inode *inode = mapping->host;
	unsigned long index, offset;
	struct page *cached_page;	// page cache를 체크하는 데에 사용되는 page
	int reada_ok;
	int error;
	int max_readahead = get_max_readahead(inode);

	cached_page = NULL;	// 초기화
	index = *ppos >> PAGE_CACHE_SHIFT;
	offset = *ppos & ~PAGE_CACHE_MASK;

/*
 * If the current position is outside the previous read-ahead window, 
 * we reset the current read-ahead context and set read ahead max to zero
 * (will be set to just needed value later),
 * otherwise, we assume that the file accesses are sequential enough to
 * continue read-ahead.
 */
	if (index > filp->f_raend || index + filp->f_rawin < filp->f_raend) {
		reada_ok = 0;
		filp->f_raend = 0;
		filp->f_ralen = 0;
		filp->f_ramax = 0;
		filp->f_rawin = 0;
	} else {
		reada_ok = 1;
	}
/*
 * Adjust the current value of read-ahead max.
 * If the read operation stay in the first half page, force no readahead.
 * Otherwise try to increase read ahead max just enough to do the read request.
 * Then, at least MIN_READAHEAD if read ahead is ok,
 * and at most MAX_READAHEAD in all cases.
 */
	if (!index && offset + desc->count <= (PAGE_CACHE_SIZE >> 1)) {
		filp->f_ramax = 0;
	} else {
		unsigned long needed;

		needed = ((offset + desc->count) >> PAGE_CACHE_SHIFT) + 1;

		if (filp->f_ramax < needed)
			filp->f_ramax = needed;

		if (reada_ok && filp->f_ramax < vm_min_readahead)
				filp->f_ramax = vm_min_readahead;
		if (filp->f_ramax > max_readahead)
			filp->f_ramax = max_readahead;
	}

	// page cache에서 page를 찾는 loop #############################################################################
	for (;;) {
		struct page *page, **hash;
		unsigned long end_index, nr, ret;

		end_index = inode->i_size >> PAGE_CACHE_SHIFT;
			
		if (index > end_index)
			break;
		nr = PAGE_CACHE_SIZE;
		if (index == end_index) {
			nr = inode->i_size & ~PAGE_CACHE_MASK;
			if (nr <= offset)
				break;
		}

		nr = nr - offset;

		/*
		 * Try to find the data in the page cache..
		 */
		hash = page_hash(mapping, index);

		spin_lock(&pagecache_lock);
		page = __find_page_nolock(mapping, index, *hash);	// hash값을 이용해서 page cache로부터 page 찾기
		if (!page)
			goto no_cached_page;	// page cache에 원하는 page가 없다!!
found_page:
		page_cache_get(page);
		spin_unlock(&pagecache_lock);

		if (!Page_Uptodate(page))
			goto page_not_up_to_date;
		generic_file_readahead(reada_ok, filp, inode, page);
page_ok:
		/* If users can be writing to this page using arbitrary
		 * virtual addresses, take care about potential aliasing
		 * before reading the page on the kernel side.
		 */
		if (mapping->i_mmap_shared != NULL)
			flush_dcache_page(page);

		/*
		 * Mark the page accessed if we read the
		 * beginning or we just did an lseek.
		 */
		if (!offset || !filp->f_reada)
			mark_page_accessed(page);

		/*
		 * Ok, we have the page, and it's up-to-date, so
		 * now we can copy it to user space...
		 *
		 * The actor routine returns how many bytes were actually used..
		 * NOTE! This may not be the same as how much of a user buffer
		 * we filled up (we may be padding etc), so we can only update
		 * "pos" here (the actor routine has to update the user buffer
		 * pointers and the remaining count).
		 */
		ret = actor(desc, page, offset, nr);
		offset += ret;
		index += offset >> PAGE_CACHE_SHIFT;
		offset &= ~PAGE_CACHE_MASK;

		page_cache_release(page);
		if (ret == nr && desc->count)
			continue;
		break;

/*
 * Ok, the page was not immediately readable, so let's try to read ahead while we're at it..
 */
page_not_up_to_date:
		generic_file_readahead(reada_ok, filp, inode, page);

		if (Page_Uptodate(page))
			goto page_ok;

		/* Get exclusive access to the page ... */
		lock_page(page);

		/* Did it get unhashed before we got the lock? */
		if (!page->mapping) {
			UnlockPage(page);
			page_cache_release(page);
			continue;
		}

		/* Did somebody else fill it already? */
		if (Page_Uptodate(page)) {
			UnlockPage(page);
			goto page_ok;
		}

readpage:
		/* ... and start the actual read. The read will unlock the page. */
		error = mapping->a_ops->readpage(filp, page);   // disk에서 page를 읽는다. (ux_readpage)

		if (!error) {
			if (Page_Uptodate(page))
				goto page_ok;

			/* Again, try some read-ahead while waiting for the page to finish.. */
			generic_file_readahead(reada_ok, filp, inode, page);
			wait_on_page(page);
			if (Page_Uptodate(page))
				goto page_ok;
			error = -EIO;
		}

		/* UHHUH! A synchronous read error occurred. Report it */
		desc->error = error;
		page_cache_release(page);
		break;

no_cached_page:
		/*
		 * Ok, it wasn't cached, so we need to create a new
		 * page..
		 *
		 * We get here with the page cache lock held.
		 */
		if (!cached_page) {
			spin_unlock(&pagecache_lock);
			cached_page = page_cache_alloc(mapping);	// 새로운 page 할당
			if (!cached_page) {
				desc->error = -ENOMEM;
				break;
			}

			/*
			 * Somebody may have added the page while we
			 * dropped the page cache lock. Check for that.
			 */
			spin_lock(&pagecache_lock);
			page = __find_page_nolock(mapping, index, *hash);
			if (page)
				goto found_page;
		}

		/*
		 * Ok, add the new page to the hash-queues...
		 */
		page = cached_page;
		__add_to_page_cache(page, mapping, index, hash);	// page cache에 새로 만든 page의 hash 추가
		spin_unlock(&pagecache_lock);
		lru_cache_add(page);		
		cached_page = NULL;

		goto readpage;
	}

	*ppos = ((loff_t) index << PAGE_CACHE_SHIFT) + offset;
	filp->f_reada = 1;
	if (cached_page)
		page_cache_release(cached_page);
	UPDATE_ATIME(inode);
}

/*
3.5. Connect to Uxfs's readpage function
*/
struct address_space_operations ux_aops = {
        readpage:         ux_readpage,	// #################### here
        writepage:        ux_writepage,
        sync_page:        block_sync_page,
        prepare_write:    ux_prepare_write,
        commit_write:     generic_commit_write,
        bmap:             ux_bmap,
};

/*
4. Uxfs's readpage function
*/
int
ux_readpage(struct file *file, struct page *page)
{
        return block_read_full_page(page, ux_get_block);
}

/*
5. Generic readpage function
	- IO를 수행하기 위한 적절한 개수의 buffer heads를 할당한다.
*/
int block_read_full_page(struct page *page, get_block_t *get_block)
{
	struct inode *inode = page->mapping->host;
	unsigned long iblock, lblock;
	struct buffer_head *bh, *head, *arr[MAX_BUF_PER_PAGE];
	unsigned int blocksize, blocks;
	int nr, i;

	if (!PageLocked(page))
		PAGE_BUG(page);
	blocksize = 1 << inode->i_blkbits;
	if (!page->buffers)
		create_empty_buffers(page, inode->i_dev, blocksize);
	head = page->buffers;

	blocks = PAGE_CACHE_SIZE >> inode->i_blkbits;
	iblock = page->index << (PAGE_CACHE_SHIFT - inode->i_blkbits);
	lblock = (inode->i_size+blocksize-1) >> inode->i_blkbits;
	bh = head;
	nr = 0;
	i = 0;

	do {
		if (buffer_uptodate(bh))
			continue;

		if (!buffer_mapped(bh)) {
			if (iblock < lblock) {
				if (get_block(inode, iblock, bh, 0))	// ux_get_block()
					continue;
			}
			if (!buffer_mapped(bh)) {
				memset(kmap(page) + i*blocksize, 0, blocksize);
				flush_dcache_page(page);
				kunmap(page);
				set_bit(BH_Uptodate, &bh->b_state);
				continue;
			}
			/* get_block() might have updated the buffer synchronously */
			if (buffer_uptodate(bh))
				continue;
		}

		arr[nr] = bh;
		nr++;
	} while (i++, iblock++, (bh = bh->b_this_page) != head);

	if (!nr) {
		/*
		 * all buffers are uptodate - we can set the page
		 * uptodate as well.
		 */
		SetPageUptodate(page);
		UnlockPage(page);
		return 0;
	}

	/* Stage two: lock the buffers */
	for (i = 0; i < nr; i++) {
		struct buffer_head * bh = arr[i];
		lock_buffer(bh);
		set_buffer_async_io(bh);
	}

	/* Stage 3: start the IO */
	for (i = 0; i < nr; i++)
		submit_bh(READ, arr[i]); 	// submit buffer head

	return 0;
}

int
ux_get_block(struct inode *inode, long block, 
             struct buffer_head *bh_result, int create)
{
        struct super_block *sb = inode->i_sb;
        struct ux_fs       *fs = (struct ux_fs *)
                                  sb->s_private;
        struct ux_inode    *uip = (struct ux_inode *)
                                   &inode->i_private;
        __u32              blk;

        /*
         * First check to see is the file can be extended.
         */

        if (block >= UX_DIRECT_BLOCKS) {
                return -EFBIG;
        }

        /*
         * If we're creating, we must allocate a new block.
         */

        if (create) {
                blk = ux_block_alloc(sb);	// 새로운 block 할당
                if (blk == 0) {
                        printk("uxfs: ux_get_block - "
                               "Out of space\n");
                        return -ENOSPC;
                }
                uip->i_addr[block] = blk;	// inode에 새 block 주소 연결
                uip->i_blocks++;
                uip->i_size = inode->i_size;
                mark_inode_dirty(inode);
        }

        bh_result->b_dev = inode->i_dev;
        bh_result->b_blocknr = uip->i_addr[block];
        bh_result->b_state |= (1UL << BH_Mapped);
        return 0;
}

/*
 * The on-disk inode.
 */
struct ux_inode {
        __u32        i_mode;
        __u32        i_nlink;
        __u32        i_atime;
        __u32        i_mtime;
        __u32        i_ctime;
        __s32        i_uid;
        __s32        i_gid;
        __u32        i_size;
        __u32        i_blocks;
        __u32        i_addr[UX_DIRECT_BLOCKS];
};