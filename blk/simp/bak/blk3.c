/*
 * A simple block device based on local memory
 * Use self-defined q->make_request_fn: fool_make_request() to 
 *  handle I/O directly, skipping request and elevator layer
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/spinlock_types.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>	/* blk_queue_logical_block_size */
#include <linux/hdreg.h>	/* hd_geometry */
#include <linux/bio.h>		/* bio_for_each_segment */

#define KERNEL_SECTOR_SIZE 512

static struct md_device {
	unsigned int size;
	spinlock_t lock;
	u8 *data;
	struct gendisk *gd;
} md;


static struct request_queue *mdqueue;
static int blocksize;
static int blocks;
static int major_num;

#define MD_MAX_SIZE (blocksize * blocks)
#define MD_NAME "memdisk"

/* 
 * HDIO_GETGEO ioctl is handled in blkdev_ioctl(), which call this.
 * Tools , like fdisk, will call ioctl.
 */
static int md_getgeo(struct block_device *bdev, struct hd_geometry *hdgeo)
{
	struct md_device *md = bdev->bd_disk->private_data;
	int sectors;
	if (md == NULL) {
		printk(KERN_ALERT "error getgeo\n");
		return -1;
	}	
	sectors = md->size >> 9;
	hdgeo->cylinders = sectors / 4 / 16;
	hdgeo->heads = 4;
	hdgeo->sectors = 16;
	//hdgeo->start = 0;
	return 0;
}

struct block_device_operations md_ops = {
	.owner = THIS_MODULE,
	.getgeo = md_getgeo,
};


/*
 * handle I/O operations via bio, not using request_queue
 */
static int fool_make_request(struct request_queue *q, struct bio *bio)
{
	struct bio_vec *bv;
	int i;
	unsigned int offset, len;
	void *disk_mem, *iovec_mem;

	if ((bio->bi_sector << 9) + bio->bi_size > MD_MAX_SIZE) {
		printk(KERN_ERR MD_NAME ":bas request: block=%llu, count=%u\n",
			(unsigned long long)bio->bi_sector ,bio->bi_size);	
		bio_endio(bio, -EIO);
	}

	disk_mem = md.data + (bio->bi_sector << 9);
	
	//bio_for_each_segment(bv, bio, i) {
	for (i = bio->bi_idx, bv = &(bio->bi_io_vec[i]);
				i < bio->bi_vcnt; i++, bv++) {

		iovec_mem = kmap(bv->bv_page);
		offset = bv->bv_offset;
		len = bv->bv_len;

		/*
		 * cannot use req,
		 * here req is waiting to be filled via bio
		 * But we handle i/o via bio directly, skipping request!
		 */
		switch (bio_rw(bio)) {
		/* write */
		case WRITE:
			printk(KERN_INFO MD_NAME":write %d\n", len);
			memcpy(disk_mem, iovec_mem + offset, len);
			break;

		/* read */
		case READ:
		case READA:
			printk(KERN_INFO MD_NAME":read %d\n", len);
			memcpy(iovec_mem + offset, disk_mem, len);
			break;

		/* we known it shouldn't be here */
		default:
			printk(KERN_ERR MD_NAME
				":unknown bio_rw: %lu\n", bio_rw(bio));
			bio_endio(bio, -EIO);
			return 0;
		}

		kunmap(bv->bv_page);
		disk_mem += len;
	}

	bio_endio(bio, 0);
	return 0;
}

static int __init md_init(void)
{
	/* init parameters */
	blocksize = 512;
	blocks = 16 * 1024;	/* disk size is 16MB */
	major_num = 0;		/* dynamic alloc device major number */

	/* init memory disk device */
	printk(KERN_ALERT "init md\n");	
	spin_lock_init(&md.lock);
	md.size = blocks * blocksize;
	md.data = vmalloc(md.size);
	memset(md.data, 0x0, 512);
	if (md.data == NULL) {
		printk(KERN_ALERT "vmalloc err\n");
		return -ENOMEM;
	}

	/* init request queue */
	printk(KERN_ALERT "init request queue\n");	

	mdqueue = blk_alloc_queue(GFP_KERNEL);
	if (mdqueue == NULL)
		goto out;
	mdqueue->node = -1;
	/* 
	 * use our own q->make_request_fn 
	 * normally: bio->request->elevator
	 * here we skip request and elevator layer and handle I/O in our own
	 * fool_make_request func directly.
	 */
	blk_queue_make_request(mdqueue, fool_make_request);


	blk_queue_logical_block_size(mdqueue, blocksize);

	/* register block device */	
	printk(KERN_ALERT "register blk\n");
	major_num = register_blkdev(major_num, MD_NAME);
	if (major_num <= 0) {
		printk(KERN_ALERT "md:unable to get major number\n");
		goto out;
	}

	/* setup generic disk */
	printk(KERN_ALERT "setup generic disk\n");
	md.gd = alloc_disk(16);	/* device will suport 15 partitions */
	if (md.gd == NULL)
		goto out_unregister;
	md.gd->major = major_num;
	md.gd->first_minor = 0;
	md.gd->fops = &md_ops;
	md.gd->private_data = &md;
	strcpy(md.gd->disk_name, "memd0");
	set_capacity(md.gd, blocks * (blocksize / KERNEL_SECTOR_SIZE));
	md.gd->queue = mdqueue;

	/* add disk when it can handle requests*/
	printk(KERN_ALERT "add disk\n");
	add_disk(md.gd);

	printk(KERN_ALERT "memory disk init ok\n");
	return 0;

out_unregister:
	unregister_blkdev(major_num, "memdisk");
out:
	vfree(md.data);
	return -ENOMEM;
}

static void __exit md_exit(void)
{
	del_gendisk(md.gd);
	put_disk(md.gd);
	unregister_blkdev(major_num, "memdisk");
	blk_cleanup_queue(mdqueue);
	vfree(md.data);
	printk(KERN_ALERT "goodbye kernel\n");
}

MODULE_LICENSE("GPL");

module_init(md_init);
module_exit(md_exit);
