
/**
 * File: asgn2.c
 * Date: 13/03/2011
 * Author: Laura Kingsley
 * Version: 0.1
 *
 * This is a module which serves as a virtual ramdisk which disk size is
 * limited by the amount of memory available and serves as the requirement for
 * COSC440 assignment 1 in 2012.
 *
 * Note: multiple devices and concurrent modules are not supported in this
 *       version.
 */
 
/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

/* TODO think through updating f_pos pointer */
/* TODO look through init setup from fb thread */
/* TODO print statements to test */
/* TODO test 	*/


#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/device.h>
#include "gpio.c"

#define MYDEV_NAME "asgn2"
#define MYIOC_TYPE 'k'
#define IRQ_NUMBER 7

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Laura Kingsley");
MODULE_DESCRIPTION("COSC440 asgn2");


/**
 * The node structure for the memory page linked list.
 */ 
typedef struct page_node_rec {
  struct list_head list;
  struct page *page;
	/*TODO where to init null_addr */
	size_t null_addr; 
} page_node;

typedef struct asgn2_dev_t {
  dev_t dev;            /* the device */
  struct cdev *cdev;
  struct list_head mem_list; 
  int num_pages;        /* number of memory pages this module currently holds */
  size_t data_size;     /* total data size in this module */
  atomic_t nprocs;      /* number of processes accessing this device */ 
  atomic_t max_nprocs;  /* max number of processes accessing this device */
  struct kmem_cache *cache;      /* cache memory */
  struct class *class;     /* the udev class */
  struct device *device;   /* the udev device node */
} asgn2_dev;

asgn2_dev asgn2_device;

typedef struct circ_buf_t {
	size_t size; 
	int count;
	int start;
	u8 *array;
} circ_buf;

circ_buf cbuf;

/* initialize values for reading half bytes */ 
u8 msb_bytes = 0;
u8 lsb_bytes = 0;
u8 result = 0;
int is_msb = 1; 

int null_count = 0; 
/* read index */
size_t read_pos = 0;

int already = 0;
/* write index */
size_t write_pos = 0; 

static int irq_number = IRQ_NUMBER;				/* create irq_number variable*/
int asgn2_major = 0;                      /* major number of module */  
int asgn2_minor = 0;                      /* minor number of module */
int asgn2_dev_count = 1;                  /* number of devices */
struct proc_dir_entry *proc_entry;	  		/* initial proc entry */
struct proc_dir_entry *maj_min_num_proc;	/* major number proc entry */


/**
 * This function frees all memory pages held by the module.
 */
void free_memory_pages(void) {
  page_node *curr;
	struct list_head *tmp;
	/*struct list_head *ptr = asgn2_device.mem_list.next; */
	struct list_head *ptr;
  /**
   * Loop through the entire page list {
   *   if (node has a page) {
   *     free the page
   *   }
   *   remove the node from the page list
   *   free the node
   * }
   * reset device data size, and num_pages
   */  
	
	printk(KERN_WARNING "WANT TO FREE %d PAGES FROM %s\n",
														asgn2_device.num_pages,
														MYDEV_NAME);
	list_for_each_safe(ptr, tmp,  &asgn2_device.mem_list) {
	
		curr = list_entry(ptr, page_node, list);
		if (curr->page) {
			__free_page(curr->page);
		}
		
		list_del(&curr->list);
		kfree(curr);		
	
	}	 
		
	asgn2_device.data_size = 0;
	asgn2_device.num_pages = 0; 
	printk(KERN_WARNING "FREE PAGES SUCCESS \n");


}


/**
 * This function opens the virtual disk, if it is opened in the write-only
 * mode, all memory pages will be freed.
 */
int asgn2_open(struct inode *inode, struct file *filp) {
  /**
   * Increment process count, if exceeds max_nprocs, return -EBUSY
   *
   * if opened in write-only mode, free all memory pages
   *
   */

	/* increment number of processes accessing device*/
	atomic_inc(&asgn2_device.nprocs); 

	/* check the number of processes against the max number of processes*/
	if (atomic_read(&asgn2_device.nprocs) > atomic_read(&asgn2_device.max_nprocs)) {

		return -EBUSY; 

	}	 

	/* check the APPEND flag and reset to file positin to EOF */
	if (filp->f_flags & O_APPEND) {
		filp->f_pos = asgn2_device.data_size;
	} 

	/* if the file is written is WRONLY and O_TRUNC then free memory pages */
	else if ((filp->f_flags & O_WRONLY) && (filp->f_flags & O_TRUNC)) {
		free_memory_pages();
	}

	already = 0;
  return 0; /* success */
}


/**
 * This function releases the virtual disk, but nothing needs to be done
 * in this case. 
 */
int asgn2_release (struct inode *inode, struct file *filp) {
  /**
   * decrement process count
   */

	atomic_dec(&asgn2_device.nprocs);

  return 0;
}


/**
 * This function reads contents of the virtual disk and writes to the user 
 */
ssize_t asgn2_read(struct file *filp, char __user *buf, size_t count,
		 loff_t *f_pos) {
  size_t size_read = 0;     /* size read from virtual disk in this function */
  size_t begin_offset;      /* the offset from the beginning of a page to
			       start reading */
  int begin_page_no = read_pos / PAGE_SIZE; /* the first page which contains
					     the requested data */
  int curr_page_no = 0;     /* the current page number */
  size_t curr_size_read;    /* size read from the virtual disk in this round */
  size_t size_to_be_read;   /* size to be read in the current round in 
			       while loop */

	size_t size_not_read;		/*size not read returned from copy to user */
  struct list_head *ptr = asgn2_device.mem_list.next;
  page_node *curr;
	
	int end_of_ram = 0;

	int eof_found = 0; 
	int i = 0;
	int found_null = 0; 
	int data_race_safe = 1;
	char *check_addr; 

	size_t adjust_data_size;
	size_t search_amt;

  /**
   * check f_pos, if beyond data_size, return 0
   * 
   * Traverse the list, once the first requested page is reached,
   *   - use copy_to_user to copy the data to the user-space buf page by page
   *   - you also need to work out the start / end offset within a page
   *   - Also needs to handle the situation where copy_to_user copy less
   *       data than requested, and
   *       copy_to_user should be called again to copy the rest of the
   *       unprocessed data, and the second and subsequent calls still
   *       need to check whether copy_to_user copies all data requested.
   *       This is best done by a while / do-while loop.
   *
   * if end of data area of ramdisk reached before copying the requested
   *   return the size copied to the user space so far
   */

	/* check f_pos if beyond data_size */
	//if (*f_pos >= asgn2_device.data_size) {
	//	printk(KERN_WARNING "BEGINING OF READ CHECK -> ret 0\n");
	//	return 0;
	//}
	
	printk(KERN_WARNING "INITAL READ CONDITIONS\n");
	printk(KERN_WARNING "read data_size= %d\n", asgn2_device.data_size);
	printk(KERN_WARNING "read read_pos = %u\n", read_pos);
	printk(KERN_WARNING "SEEK BEGIN PAGE NO: %d\n",begin_page_no);
	
	/*TODO check if there is anything in buffer*/

	/* traverse through page list to access first read page*/
	list_for_each(ptr, &asgn2_device.mem_list) {
		
		curr = list_entry(ptr, page_node, list);
		
		if (curr_page_no == begin_page_no) {
			/* found first page -> break to start reading*/
			printk(KERN_WARNING "FOUND PAGE NO: %d\n",curr_page_no);
			break;
		}
		curr_page_no++;
	}

	if (already) {
		return 0;
	}

	if (null_count == 0 && (write_pos > read_pos)) {
		return 0;
		/*TODO make it return busy */ 
	}
 
	if (write_pos == 0 || (read_pos >= write_pos -1)) {
		/* TODO put read process to sleep yo */ 
		/* or is this in open? */
		return 0; 
	} 

	/* calculate beginning offset of first page*/
	begin_offset = read_pos % PAGE_SIZE;
	printk(KERN_WARNING "BEGIN OFFSET: %d\n",begin_offset);
	
	/* adjust the data size applicable*/
	//adjust_data_size = asgn2_device.data_size - begin_offset;

	//printk(KERN_WARNING "ADJUSTED D SIZE: %d\n",adjust_data_size);

//	adjust_data_size = min((int)(count - begin_offset),(int)adjust_data_size);
//	printk(KERN_WARNING "A D_SIZE after count-offset compare:%d\n",adjust_data_size);
	
	/* check that count is not beyond the data size */
	//if (*f_pos + count > asgn2_device.data_size) {
		/* *f_pos + count is greater than data size */

//		printk(KERN_WARNING "*F_POS + COUNT GREATER THAN DATA SIZE\n");
//		printk(KERN_WARNING "COUNT SHRUNK FROM %d TO ",count);
		/* adjust count to available left to read*/
//		count = asgn2_device.data_size - *f_pos;
//		printk(KERN_WARNING "%d\n",count);
		
//	}

	printk(KERN_WARNING "\nENTER READ WHILE LOOP!!\n");
	
	/* begin read while loop*/
	while (!(eof_found) && data_race_safe) {	
		//printk(KERN_WARNING "READ ADJUST DATA SIZE= %d\n", adjust_data_size);
		printk(KERN_WARNING "SIZE READ = %d\n", size_read);
		printk(KERN_WARNING "READ_POS = %u\n", read_pos);
		
		curr = list_entry(ptr, page_node, list);
		

		/* check if eof is on page*/
		printk(KERN_WARNING "CUR PAGE EOF FLAG: %d\n",curr->null_addr);
		
		begin_offset = read_pos % PAGE_SIZE; 
		printk(KERN_WARNING "BEGIN OFFSET: %d\n",begin_offset);

		if (curr->null_addr == -1) {
			/* NO NULL ON THIS PAGE */
			size_to_be_read = PAGE_SIZE - begin_offset; 	
		}  
		else {
			/* NULL ON THIS PAGE */
			size_to_be_read = curr->null_addr - begin_offset;
			eof_found = 1;

		} 
	
		printk(KERN_WARNING "size to be read: %u\n",size_to_be_read);
		printk(KERN_WARNING "eof_found: %d\n",eof_found); 


		/* avoid data race */ 
		/* check if read_pos + size_to_be_read > write_pos */
		/* if so size to be read is write_pos - read_pos - 1 */
		/* then raise flag that will tell program at the end of 
		the loop to put reader to sleep */ 

		if ((read_pos + size_to_be_read) >= write_pos) {
			/* data race issue -> shrink size to be read */
			data_race_safe = 0;
			size_to_be_read = write_pos - read_pos; /* TODO should adjust by 1?*/
 
		} 
		

		/* calculate size to be read in this run of loop*/
		//size_to_be_read = min((int)(PAGE_SIZE - begin_offset),(int)(count-size_read));
		printk(KERN_WARNING "WANT TO READ = %d\n", size_to_be_read);
	
		printk(KERN_WARNING "ON PAGE %d\n", curr_page_no);

		/* copy size to user buffer*/
		/* TODO mem copy thing */ 
		size_not_read = copy_to_user(buf + size_read, 
														page_address(curr->page) + begin_offset,
														size_to_be_read);

		
		printk(KERN_WARNING "SIZE NOT READ = %d\n", size_not_read);
		/* calculate the size read during copy function*/
		curr_size_read = size_to_be_read - size_not_read;
	
		printk(KERN_WARNING "USER READ %d bytes\n",curr_size_read);

		/* update file position as result of read*/
		read_pos += curr_size_read;
		printk(KERN_WARNING "UPDATED R_POS = %u\n", read_pos);
 

		/* update size read*/
		size_read += curr_size_read;
		printk(KERN_WARNING "TOTAL SIZE READ = %d \n",size_read);

		/* check eof_found */
		/* if eof_found need to recalculate the page node null addr value */
		
		/* TODO how is it different when data race */ 
		/* TODO what about when null is last byte on page */ 
		if (eof_found) {
			/*TODO maybe keep add one to r_pos?*/
	
			null_count--; 
			printk(KERN_WARNING "in eof_found\n");
			for (i = curr->null_addr + 1; i < PAGE_SIZE; i++) {
				check_addr = page_address(curr->page) + i;
				if (*check_addr == "\0") {
					found_null = 1; 
					curr->null_addr = i; 
					printk(KERN_WARNING "NULL FOUND ADDR: %u\n",curr->null_addr);
					break;
				} 
			} 

			if (!found_null) {
				curr->null_addr = -1;
				printk(KERN_WARNING "NO NULL FOUND ON REST OF PAGE\n");
				printk(KERN_WARNING "curr->null_addr: %d",curr->null_addr); 
			} 
		} 

		if (!data_race_safe && !eof_found ) {
			printk(KERN_WARNING "IN DATA RACE NOT SAFE IF \n");
			/* need to put this process to sleep */
				/*TODO implement */ 
			break;
		} 

//		if (!eof_found ||(found_null && (i==PAGE_SIZE))) { 

		/* move pointer to next in mem_list*/
		ptr = ptr->next;

		/* retrieve the next page address and set to current page */
//		curr = list_entry(ptr, page_node, list);

		/* update page count */ 
		curr_page_no ++; 
		
		if ((!eof_found && curr_size_read > 0) || (found_null && (i==PAGE_SIZE))) { 
			/* need to free page, update write_pos, read_pos */
			printk(KERN_WARNING "WANT TO FREE PAGE\n");
			if (curr->page != NULL) {
				__free_page(curr->page);
				printk(KERN_WARNING "FREED PAGE\n");
			}

			list_del(asgn2_device.mem_list.next);

			printk(KERN_WARNING "DEL MEM LIST\n"); 

			if (NULL != curr) {
				kmem_cache_free(asgn2_device.cache, curr);
				printk(KERN_WARNING "FREED CURR PAGE NODE\n");
			} 

			asgn2_device.data_size -= PAGE_SIZE;
			asgn2_device.num_pages --;

			printk(KERN_WARNING "WRITE_POS UPDATE FROM %d\n",write_pos);
			write_pos -= PAGE_SIZE;
			printk(KERN_WARNING "WRITE_POS UPDATE to %d\n",write_pos);
			printk(KERN_WARNING "READ_POS UPDATE FROM %d\n",read_pos);
			read_pos -= PAGE_SIZE; 
			printk(KERN_WARNING "READ_POS UPDATE to %d\n",read_pos);
			
		} 

		/* retrieve the next page address and set to current page */
//		curr = list_entry(ptr, page_node, list);

		/* update page count */ 
	//	curr_page_no ++; 

		/* if the total size read equals amount supposed to read*/
//		if (size_read == adjust_data_size){
	//		printk(KERN_WARNING "SIZE_READ == ADJUST_DATA_SIZE\n");
		//	printk(KERN_WARNING "RETURN size_read TO USER\n");
		//	return size_read;

	//	} 

		/* check if copied nothing */
		if (size_not_read == size_to_be_read) {
			printk(KERN_WARNING "COPY TO USER COPIED NOTHING -> RET -EFAULT\n");
			return -EFAULT;
		}
		
		/* check if copy did not complete fully*/
		if (size_not_read > 0) {
			/* break loop and return size read up to now to user*/
			printk(KERN_WARNING "SIZE_NOT READ = %d\n",size_not_read);
			printk(KERN_WARNING "SIZE_NOT_READ > 0 -> break while loop\n");
			break;
		}
	
		/* after first through of loop set begin_offset to 0*/			
		//begin_offset = 0;

		/* move pointer to next in mem_list*/
		//ptr = ptr->next;

		/* retrieve the next page address and set to current page */
		//curr = list_entry(ptr, page_node, list);

		/* update page count */ 
		//curr_page_no ++; 

	}

	if (size_read > 0) {
		already = 1;
	}

	printk(KERN_WARNING "OUT OF READ WHILE LOOP\n");
	printk(KERN_WARNING "RETURN SIZE_READ: %d TO USER\n",size_read);
  return size_read;
}

/**
 * This function writes from the user buffer to the virtual disk of this
 * module
 */

/**
 * This function writes from the user buffer to the virtual disk of this
 * module
 */
ssize_t asgn2_write(char byte_in) {
  size_t orig_f_pos = write_pos;  /* the original file position */
  size_t size_written = 0;  /* size written to virtual disk in this function */
  size_t begin_offset;      /* the offset from the beginning of a page to
			       start writing */
  int begin_page_no = write_pos / PAGE_SIZE;  /* the first page this finction
					      should start writing to */

  int curr_page_no = 0;     /* the current page number */
  size_t curr_size_written; /* size written to virtual disk in this round */
  size_t size_to_be_written;  /* size to be read in the current round in 
				 while loop */
	size_t count = 1; 
  
  struct list_head *ptr = asgn2_device.mem_list.next;
  page_node *curr;

	int end_page_no;				/* the highest page number to write this function call */

	page_node *ce;					/* to iteratively create page nodes */

	size_t size_not_written;						/* copy_from_user result */
  
	
  /**
   * Traverse the list until the first page reached, and add nodes if necessary
   *
   * Then write the data page by page, remember to handle the situation
   *   when copy_from_user() writes less than the amount you requested.
   *   a while loop / do-while loop is recommended to handle this situation. 
   */


	printk(KERN_WARNING "IN WRITE\n");
	
	/* Calculate the end ending page of write and compare to current number of pages*/
	end_page_no = (write_pos + count -1) / PAGE_SIZE;
	printk(KERN_WARNING "END PAGE NUMBER = %d\n",end_page_no);
	printk(KERN_WARNING "DEVICE PAGES = %d\n",asgn2_device.num_pages);
	printk(KERN_WARNING "BYTE IN WRITE: %c",byte_in); 
	
	/* malloc space for pages and add to list as needed*/
	while(asgn2_device.num_pages <= end_page_no) {
		ce = kmalloc(sizeof(page_node), GFP_KERNEL);
		/* check for null pointer indicating no memory available */
		if (!(ce)) {
			printk(KERN_WARNING "kmalloc fail: ce -> NULL pointer-> exit\n");
			return -ENOMEM;/*failure*/
		}
		
		/* allocate page and point to the page from current node*/
		ce->page = alloc_pages(GFP_KERNEL,0);
		
		/* initialize null address to negative value*/
		ce->null_addr = -1;
	
		/* add this node to the tail of the page memory list*/
		list_add_tail(&(ce->list),&(asgn2_device.mem_list));
		
		printk(KERN_WARNING "CREATED PAGE NO: %d\n",asgn2_device.num_pages);
		
		/* increase device number of pages by one*/
		asgn2_device.num_pages++;
	
	}

	printk(KERN_WARNING "UPDATED NUM PAGES: %d\n",asgn2_device.num_pages);

	/* traverse memory list to find starting node */
	printk(KERN_WARNING "LOOKING FOR BEG PAGE: %d",begin_page_no);
	list_for_each(ptr, &asgn2_device.mem_list) {
		
		curr = list_entry(ptr, page_node, list);
		if (curr_page_no == begin_page_no) {
			/* found beginning page -> break to start writing*/
			printk(KERN_WARNING "FOUND PAGE %d \n",curr_page_no);
			break;
		} 
		printk(KERN_WARNING "C_PG: %d != B_PG: %d\n",curr_page_no,begin_page_no);
		
		/* increase page number to continue searching for beginning node*/
		curr_page_no++;
	}

	/* calculate write page offset */
	begin_offset = write_pos % PAGE_SIZE;

	printk(KERN_WARNING "BEGIN F_POS : %u\n",write_pos);
	printk(KERN_WARNING "BEGIN OFFSET: %d\n",begin_offset);
	printk(KERN_WARNING "SIZE_WRITTEN: %d\nCOUNT: %d\n",size_written,count);

	memmove(page_address(curr->page)+begin_offset,&byte_in,1); 
	printk(KERN_WARNING "BYTE WRITTEN TO PAGE %d: %c",curr_page_no, byte_in);			
	write_pos++;
	printk(KERN_WARNING "WRITE_POS UPDATE: %d\n",write_pos);
	asgn2_device.data_size++;
		/* check for null character*/
	if (byte_in == '\0') {
		null_count++;
		printk(KERN_WARNING "NULL CHARACTER FOUND\n");
		printk(KERN_WARNING "CURR->NULL_ADDR: %d\n",curr->null_addr);
		if (curr->null_addr == -1) {
			/* set current page null addresss to write offset*/ 
			curr->null_addr = begin_offset; 
			printk(KERN_WARNING "null at position %d\n",begin_offset);
		} 
	} 
	
  return size_written;
}

#define SET_NPROC_OP 1
#define TEM_SET_NPROC _IOW(MYIOC_TYPE, SET_NPROC_OP, int) 
#define GET_DATA_SIZE 2
#define TEM_GET_DSIZE _IOR(MYIOC_TYPE, GET_DATA_SIZE, size_t)
#define GET_MAJOR 3
#define TEM_AVAIL_DATA _IOR(MYIOC_TYPE, GET_MAJOR, int)

/**
 * The ioctl function, which nothing needs to be done in this case.
*/
long asgn2_ioctl (struct file *filp, unsigned cmd, unsigned long arg) {
  int nr;
  int new_nprocs;
  int result;
	size_t avail;
  /** 
   * check whether cmd is for our device, if not for us, return -EINVAL 
   *
   * get command, and if command is SET_NPROC_OP, then get the data, and
     set max_nprocs accordingly, don't forget to check validity of the 
     value before setting max_nprocs
   */
	/*Check if command is for our device */
	if (_IOC_TYPE(cmd) != MYIOC_TYPE) {
		printk(KERN_WARNING "CMD IS NOT FOR OUR DEVICE -> RETURN ERROR\n");
		return -EINVAL;
	}	

	
	printk(KERN_WARNING "IN IOCTL \n");
	/* get sequential number of the command with the device */
	nr = _IOC_NR(cmd);
	printk(KERN_WARNING "NR = %d\n",nr);
	printk(KERN_WARNING "SET_NPROC_OP value = %d\n",SET_NPROC_OP);
	printk(KERN_WARNING "CMD = %u\n",cmd); 
	
	/* switch the ioctl command versus the set of valid commands for device*/
	switch (nr) {
		/* set number of processes command*/
		case SET_NPROC_OP:
			printk(KERN_WARNING "CMD = SET_NPROC_OP\n"); 
			/* get value for n_procs from user*/
			result = copy_from_user((int*) &new_nprocs, arg, sizeof(int));
			/* result check */			
			if (result < 0 ) {
				printk(KERN_WARNING "MMAP SET_N_PROCS copy from user failure");
				return -EINVAL;
			}
			
			printk(KERN_WARNING "new_nprocs = %d\n",new_nprocs);

			/* check for valid new max number of processes*/
			if (new_nprocs < atomic_read(&asgn2_device.nprocs)) {
					printk(KERN_WARNING "%d new_nprocs INVALID -> return error\n",new_nprocs);
					return -EINVAL;
			}
			
			/* set the new max to the given value from user*/			
  		atomic_set(&asgn2_device.max_nprocs,new_nprocs);	
			printk(KERN_WARNING "NEW MAX_NPROCS: %d\n",atomic_read(&asgn2_device.max_nprocs));

			return 0;

		case GET_DATA_SIZE:
			/* command to push the value of the device data size to user*/
			printk(KERN_WARNING "CMD = GET_DATA_SIZE");
			result = put_user(asgn2_device.data_size, (size_t*)arg);
			/* check the put_user result */
			if (result < 0 ) {
				printk(KERN_WARNING "MMAP GET_DATA_SIZE put_user failure");
				return -EINVAL;
			}
			return 0;
		
		case GET_MAJOR:
			/* command to send the major number of device to user*/
			printk(KERN_WARNING "CMD = GET_MAJOR");
			printk(KERN_WARNING "MAJOR NUM= %d\n",asgn2_major);
			result = put_user(asgn2_major, (int *)arg);
			/* check put_user result for failure*/
			if (result < 0 ) {
				printk(KERN_WARNING "MMAP GET_MAJOR put_user failure");
				return -EINVAL;
			}

			return 0;
		
	
	
	default:
			printk(KERN_WARNING "cmd did not match any of cases -> return error\n");
			return -EINVAL;
	}

  return -ENOTTY;/* error in switch case*/
}

/**
 * Displays information about current status of the module,
 * which helps debugging.
 */
int asgn2_read_procmem(char *buf, char **start, off_t offset, int count,
		     int *eof, void *data) {
  /* stub */
  int result;

  /**
   * use snprintf to print some info to buf, up to size count
   * set eof
   */
	
	/* check the available buffer size against the minimum number of bytes needed */
	if (count < 61)  {
		printk(KERN_WARNING "NOT ENOUGH BUFFER SPACE FOR PROCMEM OUTPUT\n");
		return -EINVAL;
	}

	/* print results to user*/
	result = sprintf(buf,"DEVICE DATA SIZE: %d bytes\nNUM PAGES: %d\nMAX PROCS: %d\n",
										asgn2_device.data_size,
										asgn2_device.num_pages,
										atomic_read(&asgn2_device.max_nprocs));
	/* set eof to 1 */
	*eof = 1;											
  return result;
}

/**
 * Displays information about current status of the module,
 * which helps debugging.
 */

/* read function for the major and minor number proc entry*/
/* prints out device major and minor number to user*/
int asgn2_read_nums(char *buf, char **start, off_t offset, int count,
		     int *eof, void *data) {
 
  int result;

  /**
   * use snprintf to print some info to buf, up to size count
   * set eof
   */
	
	/* check the available buffer size against the minimum number of bytes needed */
	if (count < 61)  {
		printk(KERN_WARNING "NOT ENOUGH BUFFER SPACE FOR PROCMEM OUTPUT\n");
		return -EINVAL;
	}

	result = sprintf(buf,"Device major number %d \nDevice minor number: %d\n",asgn2_major,asgn2_minor);
	/* set eof to 1 */
	*eof = 1;											
  return result;
}

struct file_operations asgn2_fops = {
  .owner = THIS_MODULE,
  .read = asgn2_read,
  .unlocked_ioctl = asgn2_ioctl,
  .open = asgn2_open,
  .release = asgn2_release,
};


/**
 * Initialise the module and create the master device
 */
int __init asgn2_init_module(void){
  int result; 
  
  /**
   * set nprocs and max_nprocs of the device
   *
   * allocate major number
   * allocate cdev, and set ops and owner field 
   * add cdev
   * initialize the page list
   * create proc entries
   */

	printk(KERN_WARNING " IN INIT MODULE");	
  
	/* initialize the number of processes*/
	atomic_set(&asgn2_device.nprocs, 0);
  atomic_set(&asgn2_device.max_nprocs,16);	  
	
	/* allocate character device region*/
  result = alloc_chrdev_region (
						&asgn2_device.dev,
						asgn2_minor, 
						asgn2_dev_count,
						MYDEV_NAME);
  
	/* check result of allocation*/
	if (result < 0) {
		printk(KERN_WARNING "error in register chrdev");
		goto fail_device;
  } 

	/*set up major number */
	asgn2_major = MAJOR(asgn2_device.dev);

	/*allocate cdev region */
	asgn2_device.cdev = cdev_alloc();
	asgn2_device.cdev->ops = &asgn2_fops;
	asgn2_device.cdev->owner = THIS_MODULE;
  
  result = cdev_add(asgn2_device.cdev, asgn2_device.dev, asgn2_dev_count);
 
	/*check that the cdev added successfully */
	if (result<0) {
		printk(KERN_WARNING "Unable to add cdev");
		goto fail_device;
	} 

  INIT_LIST_HEAD(&asgn2_device.mem_list);	 
	
	/* create and initialize first proc entry for device*/
  proc_entry = create_proc_entry("driver/procmem", S_IRUGO | S_IWUSR, NULL);
  
  if (!proc_entry) {	
		/* create proc entry failed*/
		printk(KERN_WARNING "I failed to make driver/procmem\n");
		goto fail_device;

  }  

	/* set read function for proc*/
	proc_entry->read_proc = asgn2_read_procmem;
	printk(KERN_WARNING "I created driver/procmem\n");
  
	/* create major minor number proc */
  maj_min_num_proc = create_proc_entry("driver/numbers", S_IRUGO | S_IWUSR, NULL);
  
  if (!maj_min_num_proc) {	
		/* creating major minor number proc failed*/
		printk(KERN_WARNING "I failed to make driver/numbers\n");
		goto fail_device;
  }  

	/* set read function for major minor number proc*/
	maj_min_num_proc->read_proc = asgn2_read_nums;

	printk(KERN_WARNING "I created driver/numbers\n");

	/* create device class*/
  asgn2_device.class = class_create(THIS_MODULE, MYDEV_NAME);
  if (IS_ERR(asgn2_device.class)) {
  }

  asgn2_device.device = device_create(asgn2_device.class, NULL, 
                                      asgn2_device.dev, "%s", MYDEV_NAME);
  if (IS_ERR(asgn2_device.device)) {
    printk(KERN_WARNING "%s: can't create udev device\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_device;
  }

	/* initialize data size to 0*/
	asgn2_device.data_size = 0;
  
  printk(KERN_WARNING "set up udev entry\n");
  
	/* run gpio initialize function */ 
	if (gpio_dummy_init() < 0) {
		printk(KERN_WARNING "%s: cannot init gpio", MYDEV_NAME);
		result = -ENOMEM;
		goto fail_gpio;
	}

	/* request irq number for device and check for error */
	if (request_irq(irq_number, dummyport_interrupt, 0, MYDEV_NAME, asgn2_device.device)) {
		printk(KERN_WARNING "%s: cannot request irq",MYDEV_NAME);
		result = -ENOMEM;
		goto fail_irq; 
	} 

	/*initialize circular buffer*/
	cbuf.size = PAGE_SIZE;
	cbuf.count = 0;
	cbuf.start = 0;
	cbuf.array = kmalloc(cbuf.size, GFP_KERNEL);

	printk(KERN_WARNING "Hello world from %s\n", MYDEV_NAME);

  return 0;

  /* cleanup code called when any of the initialization steps fail */
fail_device:

   kfree(cbuf.array);

   class_destroy(asgn2_device.class);

  /* CLEANUP CODE */
	
	/* if the proc entries exist, remove them*/
	if (proc_entry) {
		remove_proc_entry("driver/procmem",NULL);
	} 

	if (maj_min_num_proc) {
		remove_proc_entry("driver/numbers",NULL);
	}

	/* free cdev */
	kfree(asgn2_device.cdev);

	/* delete the cdev */
	cdev_del(asgn2_device.cdev);
	
	/* unregister device */
	unregister_chrdev_region(asgn2_device.dev, asgn2_dev_count);

fail_gpio:
	gpio_dummy_exit();

fail_irq:
	free_irq(irq_number, asgn2_device.device);

  return result;
}


/**
 * Finalise the module
 */
void __exit asgn2_exit_module(void){
  device_destroy(asgn2_device.class, asgn2_device.dev);
  class_destroy(asgn2_device.class);
  printk(KERN_WARNING "cleaned up udev entry\n");
  
  /**
   * free all pages in the page list 
   * cleanup in reverse order
   */

	/* free the circular buffer array */   
	kfree(cbuf.array);

	/* free memory pages*/
	free_memory_pages();

	/* if the proc entries exist, remove them*/	
	if (proc_entry) {
		remove_proc_entry("driver/procmem",NULL);
	} 

	if (maj_min_num_proc) {
		remove_proc_entry("driver/numbers",NULL);
	}

	/*delete the cdev */
	cdev_del(asgn2_device.cdev);

	/*unregister the device */
	unregister_chrdev_region(asgn2_device.dev, asgn2_dev_count);

	gpio_dummy_exit();

	free_irq(irq_number, asgn2_device.device);

	printk(KERN_WARNING "Good bye from %s\n", MYDEV_NAME);
}

/* define tasklet function */
/* that removes the value from the buffer */ 

void t_fun() {
	u8 removed;
	if (cbuf.count == 0) {
		printk(KERN_WARNING "EMPTY BUFFER");
		return;
	} 

	removed = cbuf.array[cbuf.start];
	printk(KERN_WARNING "REMOVED FROM BUF: %c\n",removed); 
	
	asgn2_write(removed);
	printk(KERN_WARNING "%c sent to write",removed);

  cbuf.start = (cbuf.start + 1)	% cbuf.size; 
  cbuf.count--;  

} 

/* declare tasklet */
DECLARE_TASKLET(t_name, t_fun, &cbuf.array);

void add_to_buf(u8 byte) {
	int index;

	if (cbuf.count == cbuf.size) {
		printk(KERN_WARNING "BUFFER FULL->CANNOT ADD ");
		return;
	} 
	/* implement adding to circular buffer*/
	index = (cbuf.start + cbuf.count) % cbuf.size;
	cbuf.array[index] = byte;
	cbuf.count ++;
	printk(KERN_WARNING "ADD TO BUF: %c\n", byte);	

	/* schedule tasklet */
	tasklet_schedule(&t_name);

} 

/* interrupt handler function */ 
irqreturn_t dummyport_interrupt(int irq, void *dev_id) {
 if (is_msb) {
  msb_bytes = read_half_byte() << 4;
	printk(KERN_WARNING "MSB IN",msb_bytes);
	is_msb = 0; 
 } 
 else {
  lsb_bytes = read_half_byte();
	printk(KERN_WARNING "LSB IN\n");
	result = msb_bytes | lsb_bytes;
	printk(KERN_WARNING "RESULT = %c\n",result);
	add_to_buf(result); 
	is_msb = 1; 
 }    
 return IRQ_HANDLED; 
} 

module_init(asgn2_init_module);
module_exit(asgn2_exit_module); 
