/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Network filesystem support services.
 *
 * Copyright (C) 2021 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * See:
 *
 *	Documentation/filesystems/netfs_library.rst
 *
 * for a description of the network filesystem interface declared here.
 */

#ifndef _LINUX_NETFS_H
#define _LINUX_NETFS_H

#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/pagemap.h>

enum netfs_sreq_ref_trace;

/*
 * Overload PG_private_2 to give us PG_fscache - this is used to indicate that
 * a page is currently backed by a local disk cache
 */
#define folio_test_fscache(folio)	folio_test_private_2(folio)
#define PageFsCache(page)		PagePrivate2((page))
#define SetPageFsCache(page)		SetPagePrivate2((page))
#define ClearPageFsCache(page)		ClearPagePrivate2((page))
#define TestSetPageFsCache(page)	TestSetPagePrivate2((page))
#define TestClearPageFsCache(page)	TestClearPagePrivate2((page))

/**
 * folio_start_fscache - Start an fscache write on a folio.
 * @folio: The folio.
 *
 * Call this function before writing a folio to a local cache.  Starting a
 * second write before the first one finishes is not allowed.
 */
static inline void folio_start_fscache(struct folio *folio)
{
	VM_BUG_ON_FOLIO(folio_test_private_2(folio), folio);
	folio_get(folio);
	folio_set_private_2(folio);
}

/**
 * folio_end_fscache - End an fscache write on a folio.
 * @folio: The folio.
 *
 * Call this function after the folio has been written to the local cache.
 * This will wake any sleepers waiting on this folio.
 */
static inline void folio_end_fscache(struct folio *folio)
{
	folio_end_private_2(folio);
}

/**
 * folio_wait_fscache - Wait for an fscache write on this folio to end.
 * @folio: The folio.
 *
 * If this folio is currently being written to a local cache, wait for
 * the write to finish.  Another write may start after this one finishes,
 * unless the caller holds the folio lock.
 */
static inline void folio_wait_fscache(struct folio *folio)
{
	folio_wait_private_2(folio);
}

/**
 * folio_wait_fscache_killable - Wait for an fscache write on this folio to end.
 * @folio: The folio.
 *
 * If this folio is currently being written to a local cache, wait
 * for the write to finish or for a fatal signal to be received.
 * Another write may start after this one finishes, unless the caller
 * holds the folio lock.
 *
 * Return:
 * - 0 if successful.
 * - -EINTR if a fatal signal was encountered.
 */
static inline int folio_wait_fscache_killable(struct folio *folio)
{
	return folio_wait_private_2_killable(folio);
}

static inline void set_page_fscache(struct page *page)
{
	folio_start_fscache(page_folio(page));
}

static inline void end_page_fscache(struct page *page)
{
	folio_end_private_2(page_folio(page));
}

static inline void wait_on_page_fscache(struct page *page)
{
	folio_wait_private_2(page_folio(page));
}

static inline int wait_on_page_fscache_killable(struct page *page)
{
	return folio_wait_private_2_killable(page_folio(page));
}

enum netfs_io_source {
	NETFS_FILL_WITH_ZEROES,
	NETFS_DOWNLOAD_FROM_SERVER,
	NETFS_READ_FROM_CACHE,
	NETFS_INVALID_READ,
} __mode(byte);

typedef void (*netfs_io_terminated_t)(void *priv, ssize_t transferred_or_error,
				      bool was_async);

/*
 * Per-inode description.  This must be directly after the inode struct.
 */
struct netfs_i_context {
	const struct netfs_request_ops *ops;
#if IS_ENABLED(CONFIG_FSCACHE)
	struct fscache_cookie	*cache;
#endif
	loff_t			remote_i_size;	/* Size of the remote file */
};

/*
 * Resources required to do operations on a cache.
 */
struct netfs_cache_resources {
	const struct netfs_cache_ops	*ops;
	void				*cache_priv;
	void				*cache_priv2;
	unsigned int			debug_id;	/* Cookie debug ID */
	unsigned int			inval_counter;	/* object->inval_counter at begin_op */
};

/*
 * Descriptor for a single component subrequest.
 */
struct netfs_io_subrequest {
	struct netfs_io_request *rreq;		/* Supervising I/O request */
	struct list_head	rreq_link;	/* Link in rreq->subrequests */
	loff_t			start;		/* Where to start the I/O */
	size_t			len;		/* Size of the I/O */
	size_t			transferred;	/* Amount of data transferred */
	refcount_t		ref;
	short			error;		/* 0 or error that occurred */
	unsigned short		debug_index;	/* Index in list (for debugging output) */
	enum netfs_io_source	source;		/* Where to read from/write to */
	unsigned long		flags;
#define NETFS_SREQ_COPY_TO_CACHE	0	/* Set if should copy the data to the cache */
#define NETFS_SREQ_CLEAR_TAIL		1	/* Set if the rest of the read should be cleared */
#define NETFS_SREQ_SHORT_IO		2	/* Set if the I/O was short */
#define NETFS_SREQ_SEEK_DATA_READ	3	/* Set if ->read() should SEEK_DATA first */
#define NETFS_SREQ_NO_PROGRESS		4	/* Set if we didn't manage to read any data */
#define NETFS_SREQ_ONDEMAND		5	/* Set if it's from on-demand read mode */
};

enum netfs_io_origin {
	NETFS_READAHEAD,		/* This read was triggered by readahead */
	NETFS_READPAGE,			/* This read is a synchronous read */
	NETFS_READ_FOR_WRITE,		/* This read is to prepare a write */
} __mode(byte);

/*
 * Descriptor for an I/O helper request.  This is used to make multiple I/O
 * operations to a variety of data stores and then stitch the result together.
 */
struct netfs_io_request {
	struct work_struct	work;
	struct inode		*inode;		/* The file being accessed */
	struct address_space	*mapping;	/* The mapping being accessed */
	struct netfs_cache_resources cache_resources;
	struct list_head	subrequests;	/* Contributory I/O operations */
	void			*netfs_priv;	/* Private data for the netfs */
	unsigned int		debug_id;
	atomic_t		nr_outstanding;	/* Number of ops in progress */
	atomic_t		nr_copy_ops;	/* Number of copy-to-cache ops in progress */
	size_t			submitted;	/* Amount submitted for I/O so far */
	size_t			len;		/* Length of the request */
	short			error;		/* 0 or error that occurred */
	enum netfs_io_origin	origin;		/* Origin of the request */
	loff_t			i_size;		/* Size of the file */
	loff_t			start;		/* Start position */
	pgoff_t			no_unlock_folio; /* Don't unlock this folio after read */
	refcount_t		ref;
	unsigned long		flags;
#define NETFS_RREQ_INCOMPLETE_IO	0	/* Some ioreqs terminated short or with error */
#define NETFS_RREQ_COPY_TO_CACHE	1	/* Need to write to the cache */
#define NETFS_RREQ_NO_UNLOCK_FOLIO	2	/* Don't unlock no_unlock_folio on completion */
#define NETFS_RREQ_DONT_UNLOCK_FOLIOS	3	/* Don't unlock the folios on completion */
#define NETFS_RREQ_FAILED		4	/* The request failed */
#define NETFS_RREQ_IN_PROGRESS		5	/* Unlocked when the request completes */
	const struct netfs_request_ops *netfs_ops;
};

/*
 * Operations the network filesystem can/must provide to the helpers.
 */
struct netfs_request_ops {
	int (*init_request)(struct netfs_io_request *rreq, struct file *file);
	int (*begin_cache_operation)(struct netfs_io_request *rreq);
	void (*expand_readahead)(struct netfs_io_request *rreq);
	bool (*clamp_length)(struct netfs_io_subrequest *subreq);
	void (*issue_read)(struct netfs_io_subrequest *subreq);
	bool (*is_still_valid)(struct netfs_io_request *rreq);
	int (*check_write_begin)(struct file *file, loff_t pos, unsigned len,
				 struct folio *folio, void **_fsdata);
	void (*done)(struct netfs_io_request *rreq);
	void (*cleanup)(struct address_space *mapping, void *netfs_priv);
};

/*
 * How to handle reading from a hole.
 */
enum netfs_read_from_hole {
	NETFS_READ_HOLE_IGNORE,
	NETFS_READ_HOLE_CLEAR,
	NETFS_READ_HOLE_FAIL,
};

/*
 * Table of operations for access to a cache.  This is obtained by
 * rreq->ops->begin_cache_operation().
 */
struct netfs_cache_ops {
	/* End an operation */
	void (*end_operation)(struct netfs_cache_resources *cres);

	/* Read data from the cache */
	int (*read)(struct netfs_cache_resources *cres,
		    loff_t start_pos,
		    struct iov_iter *iter,
		    enum netfs_read_from_hole read_hole,
		    netfs_io_terminated_t term_func,
		    void *term_func_priv);

	/* Write data to the cache */
	int (*write)(struct netfs_cache_resources *cres,
		     loff_t start_pos,
		     struct iov_iter *iter,
		     netfs_io_terminated_t term_func,
		     void *term_func_priv);

	/* Expand readahead request */
	void (*expand_readahead)(struct netfs_cache_resources *cres,
				 loff_t *_start, size_t *_len, loff_t i_size);

	/* Prepare a read operation, shortening it to a cached/uncached
	 * boundary as appropriate.
	 */
	enum netfs_io_source (*prepare_read)(struct netfs_io_subrequest *subreq,
					       loff_t i_size);

	/* Prepare a write operation, working out what part of the write we can
	 * actually do.
	 */
	int (*prepare_write)(struct netfs_cache_resources *cres,
			     loff_t *_start, size_t *_len, loff_t i_size,
			     bool no_space_allocated_yet);

	/* Query the occupancy of the cache in a region, returning where the
	 * next chunk of data starts and how long it is.
	 */
	int (*query_occupancy)(struct netfs_cache_resources *cres,
			       loff_t start, size_t len, size_t granularity,
			       loff_t *_data_start, size_t *_data_len);
};

struct readahead_control;
extern void netfs_readahead(struct readahead_control *);
int netfs_read_folio(struct file *, struct folio *);
extern int netfs_write_begin(struct file *, struct address_space *,
			     loff_t, unsigned int, struct folio **,
			     void **);

extern void netfs_subreq_terminated(struct netfs_io_subrequest *, ssize_t, bool);
extern void netfs_get_subrequest(struct netfs_io_subrequest *subreq,
				 enum netfs_sreq_ref_trace what);
extern void netfs_put_subrequest(struct netfs_io_subrequest *subreq,
				 bool was_async, enum netfs_sreq_ref_trace what);
extern void netfs_stats_show(struct seq_file *);

/**
 * netfs_i_context - Get the netfs inode context from the inode
 * @inode: The inode to query
 *
 * Get the netfs lib inode context from the network filesystem's inode.  The
 * context struct is expected to directly follow on from the VFS inode struct.
 */
static inline struct netfs_i_context *netfs_i_context(struct inode *inode)
{
	return (struct netfs_i_context *)(inode + 1);
}

/**
 * netfs_inode - Get the netfs inode from the inode context
 * @ctx: The context to query
 *
 * Get the netfs inode from the netfs library's inode context.  The VFS inode
 * is expected to directly precede the context struct.
 */
static inline struct inode *netfs_inode(struct netfs_i_context *ctx)
{
	return ((struct inode *)ctx) - 1;
}

/**
 * netfs_i_context_init - Initialise a netfs lib context
 * @inode: The inode with which the context is associated
 * @ops: The netfs's operations list
 *
 * Initialise the netfs library context struct.  This is expected to follow on
 * directly from the VFS inode struct.
 */
static inline void netfs_i_context_init(struct inode *inode,
					const struct netfs_request_ops *ops)
{
	struct netfs_i_context *ctx = netfs_i_context(inode);

	memset(ctx, 0, sizeof(*ctx));
	ctx->ops = ops;
	ctx->remote_i_size = i_size_read(inode);
}

/**
 * netfs_resize_file - Note that a file got resized
 * @inode: The inode being resized
 * @new_i_size: The new file size
 *
 * Inform the netfs lib that a file got resized so that it can adjust its state.
 */
static inline void netfs_resize_file(struct inode *inode, loff_t new_i_size)
{
	struct netfs_i_context *ctx = netfs_i_context(inode);

	ctx->remote_i_size = new_i_size;
}

/**
 * netfs_i_cookie - Get the cache cookie from the inode
 * @inode: The inode to query
 *
 * Get the caching cookie (if enabled) from the network filesystem's inode.
 */
static inline struct fscache_cookie *netfs_i_cookie(struct inode *inode)
{
#if IS_ENABLED(CONFIG_FSCACHE)
	struct netfs_i_context *ctx = netfs_i_context(inode);
	return ctx->cache;
#else
	return NULL;
#endif
}

#endif /* _LINUX_NETFS_H */
