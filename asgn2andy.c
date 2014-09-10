/**
 * File: asgn2.c
 * Date: 13/03/2011
 * Author: Andy Hansen
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
#include <linux/sched.h>
#include "gpio.c"

#define MYDEV_NAME "asgn2"
#define MYIOC_TYPE 'k'

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andy Hansen");
MODULE_DESCRIPTION("COSC440 asgn2");


/**
 * The node structure for the memory page linked list.
 */ 
typedef struct page_node_rec {
  struct list_head list;
  struct page *page;
} page_node;

typedef struct asgn1_dev_t {
  dev_t dev;            /* the device */
  struct cdev *cdev;
  struct list_head mem_list; 
  int num_pages;        /* number of memory pages this module currently holds */
  size_t data_size;     /* total data size in this module */
  long read_pos;
  long write_pos;
  atomic_t nprocs;      /* number of processes accessing this device */ 
  atomic_t max_nprocs;  /* max number of processes accessing this device */
  struct kmem_cache *cache;      /* cache memory */
  struct class *class;     /* the udev class */
  struct device *device;   /* the udev device node */
} asgn2_dev;

struct cbuf_t {
  char* buf;
  int write_pos;
  int read_pos;
  int count;
  int capacity;
} cbuf;

asgn2_dev asgn2_device;


int asgn2_major = 0;                      /* major number of module */  
int asgn2_minor = 0;                      /* minor number of module */
int asgn2_dev_count = 1;                  /* number of devices */

u8 top_half_byte;
int second_half = 0;


/**
 * This function frees all memory pages held by the module.
 */
void free_memory_pages(void) {
  page_node *curr;

  while (!list_empty(&asgn2_device.mem_list)) {
    curr = list_entry(asgn2_device.mem_list.next, page_node, list);
    if (NULL != curr->page) __free_page(curr->page);
    list_del(asgn2_device.mem_list.next);
    if (NULL != curr) kmem_cache_free(asgn2_device.cache, curr);
  }
  asgn2_device.data_size = 0;
  asgn2_device.num_pages = 0;
}


/**
 * This function opens the virtual disk, if it is opened in the write-only
 * mode, all memory pages will be freed.
 */
int asgn2_open(struct inode *inode, struct file *filp) {
  if (atomic_read(&asgn2_device.nprocs) >= atomic_read(&asgn2_device.max_nprocs)) {
    return -EBUSY;
  }
  atomic_inc(&asgn2_device.nprocs);
  if ((filp->f_mode & FMODE_WRITE) && !(filp->f_mode & FMODE_READ)) {
    free_memory_pages();
  }
  return 0; /* success */
}

/**
 * This function releases the virtual disk, but nothing needs to be done
 * in this case. 
 */
int asgn2_release (struct inode *inode, struct file *filp) {
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
  int begin_page_no = *f_pos / PAGE_SIZE; /* the first page which contains
					     the requested data */
  int curr_page_no = 0;     /* the current page number */
  size_t curr_size_read;    /* size read from the virtual disk in this round */
  size_t size_to_be_read;   /* size to be read in the current round in 
			       while loop */

  struct list_head *ptr = asgn2_device.mem_list.next;
  page_node *curr;

  if (*f_pos >= asgn2_device.data_size) return 0;
  count = min(asgn2_device.data_size - (size_t)*f_pos, count);

  while (size_read < count) {
    curr = list_entry(ptr, page_node, list);
    if (ptr == &asgn2_device.mem_list) {
      /* We have already passed the end of the data area of the
         ramdisk, so we quit and return the size we have read
         so far */
      printk(KERN_WARNING "invalid virtual memory access\n");
      return size_read;
    } else if (curr_page_no < begin_page_no) {
      /* haven't reached the page occupued by *f_pos yet, 
         so move on to the next page */
      ptr = ptr->next;
      curr_page_no++;
    } else {
      /* this is the page to read from */
      begin_offset = *f_pos % PAGE_SIZE;
      size_to_be_read = (size_t)min((size_t)(count - size_read), 
				    (size_t)(PAGE_SIZE - begin_offset));

      do {
        curr_size_read = size_to_be_read - 
	  copy_to_user(buf + size_read, 
	  	       page_address(curr->page) + begin_offset,
		       size_to_be_read);
        size_read += curr_size_read;
        *f_pos += curr_size_read;
        begin_offset += curr_size_read;
        size_to_be_read -= curr_size_read;
      } while (curr_size_read > 0);

      curr_page_no++;
      ptr = ptr->next;
    }
  }
  return size_read;
}

/**
 * This function writes from the user buffer to the virtual disk of this
 * module
 */
ssize_t asgn2_write(struct file *filp, const char __user *buf, size_t count,
		  loff_t *f_pos) {
  size_t orig_f_pos = *f_pos;  /* the original file position */
  size_t size_written = 0;  /* size written to virtual disk in this function */
  size_t begin_offset;      /* the offset from the beginning of a page to
			       start writing */
  int begin_page_no = *f_pos / PAGE_SIZE;  /* the first page this finction
					      should start writing to */

  int curr_page_no = 0;     /* the current page number */
  size_t curr_size_written; /* size written to virtual disk in this round */
  size_t size_to_be_written;  /* size to be read in the current round in
				 while loop */
  
  struct list_head *ptr = asgn2_device.mem_list.next;
  page_node *curr;

  while (size_written < count) {
    curr = list_entry(ptr, page_node, list);
    if (ptr == &asgn2_device.mem_list) {
      /* not enough page, so add page */
      curr = kmem_cache_alloc(asgn2_device.cache, GFP_KERNEL);
      if (NULL == curr) {
	printk(KERN_WARNING "Not enough memory left\n");
	break;
      }
      curr->page = alloc_page(GFP_KERNEL);
      if (NULL == curr->page) {
	printk(KERN_WARNING "Not enough memory left\n");
        kmem_cache_free(asgn2_device.cache, curr);
	break;
      }
      //INIT_LIST_HEAD(&curr->list);
      list_add_tail(&(curr->list), &asgn2_device.mem_list);
      asgn2_device.num_pages++;
      ptr = asgn2_device.mem_list.prev;
    } else if (curr_page_no < begin_page_no) {
      /* move on to the next page */
      ptr = ptr->next;
      curr_page_no++;
    } else {
      /* this is the page to write to */
      begin_offset = *f_pos % PAGE_SIZE;
      size_to_be_written = (size_t)min((size_t)(count - size_written),
				       (size_t)(PAGE_SIZE - begin_offset));
      do {
        curr_size_written = size_to_be_written -
	  copy_from_user(page_address(curr->page) + begin_offset,
	  	         buf + size_written, size_to_be_written);
        size_written += curr_size_written;
        begin_offset += curr_size_written;
        *f_pos += curr_size_written;
        size_to_be_written -= curr_size_written;
      } while (size_to_be_written > 0);
      curr_page_no++;
      ptr = ptr->next;
    }
  }

  /* END TRIM */


  asgn2_device.data_size = max(asgn2_device.data_size,
                               orig_f_pos + size_written);
  return size_written;
}

#define SET_NPROC_OP 1
#define TEM_SET_NPROC _IOW(MYIOC_TYPE, SET_NPROC_OP, int) 

/**
 * The ioctl function, which nothing needs to be done in this case.
 */
long asgn2_ioctl (struct file *filp, unsigned cmd, unsigned long arg) {
  int nr;
  int new_nprocs;
  int result;

  if (_IOC_TYPE(cmd) != MYIOC_TYPE) {
    printk(KERN_WARNING "%s: magic number does not match\n", MYDEV_NAME);
    return -EINVAL;
  }

  nr = _IOC_NR(cmd);

  switch (nr) {
  case SET_NPROC_OP:
    result = get_user(new_nprocs, (int *)arg);

    if (result) {
      printk(KERN_WARNING "%s: failed to get new max nprocs\n", MYDEV_NAME);
      return -EINVAL;
    }

    if (new_nprocs < 1) {
      printk(KERN_WARNING "%s: invalid new max nprocs %d\n", MYDEV_NAME, new_nprocs);
      return -EINVAL;
    }

    atomic_set(&asgn2_device.max_nprocs, new_nprocs);

    printk(KERN_WARNING "%s: max_nprocs set to %d\n",
            __stringify (KBUILD_BASENAME), atomic_read(&asgn2_device.max_nprocs));
    return 0;
  } 

  return -ENOTTY;
}

void remove_from_cbuffer(unsigned long t_arg) {
  //put in checks
  char to_return = cbuf.buf[cbuf.read_pos];
  cbuf.read_pos = cbuf.read_pos + 1 % cbuf.capacity;
  printk(KERN_WARNING "tasklet got %c\n", to_return);
}

DECLARE_TASKLET(t_name, remove_from_cbuffer, (unsigned long) &cbuf);

int add_to_cbuffer(char to_add) {
  // check buffer is not full
  if (cbuf.capacity == cbuf.count) {
    return -ENOMEM;
  }
  cbuf.buf[cbuf.write_pos] = to_add;
  cbuf.write_pos = cbuf.write_pos + 1 % cbuf.capacity;
  tasklet_schedule(&t_name);
  return 0;
}

void get_half_byte(void){
  u8 this_half_byte = read_half_byte();
  char full_byte;
  //printk(KERN_WARNING "The half byte is %u\n", this_half_byte);

  if (second_half) {
    full_byte = (char) top_half_byte << 4 | this_half_byte;
    printk(KERN_WARNING "The byte is %c\n", full_byte);
    second_half = 0;
    // add full byte to the circular buffer
    add_to_cbuffer(full_byte);
    //printk(KERN_WARNING "The byte is %c\n", remove_from_buffer());
  } else {
    //printk(KERN_WARNING "read first half\n");
    top_half_byte = this_half_byte;
    second_half = 1;
  }
  /* check flag, either store the byte, or read the first half and set 
   * the flag 
   * w = a << 4 | b
   * */
}

irqreturn_t dummyport_interrupt(int irq, void *dev_id){
  //printk(KERN_WARNING "Got the interrupt\n");
  get_half_byte();
  return 0;
}

/**
 * Displays information about current status of the module,
 * which helps debugging.
 */
int asgn2_read_procmem(char *buf, char **start, off_t offset, int count,
		     int *eof, void *data) {
  int result;

  result = snprintf(buf, count,
	            "major = %d\nnumber of pages = %d\ndata size = %u\n"
                    "disk size = %d\nnprocs = %d\nmax_nprocs = %d\n",
	            asgn2_major, asgn2_device.num_pages, 
                    asgn2_device.data_size, 
                    (int)(asgn2_device.num_pages * PAGE_SIZE),
                    atomic_read(&asgn2_device.nprocs), 
                    atomic_read(&asgn2_device.max_nprocs)); 
  *eof = 1; /* end of file */
  return result;
}

struct file_operations asgn2_fops = {
  .owner = THIS_MODULE,
  .read = asgn2_read,
  .unlocked_ioctl = asgn2_ioctl,
  .open = asgn2_open,
  .release = asgn2_release,
};

#define IRQ_NUMBER 7
static int irq_number = IRQ_NUMBER;

/**
 * Initialise the module and create the master device
 */
int __init asgn2_init_module(void){
  int result; 

  /* START TRIM */
  atomic_set(&asgn2_device.nprocs, 0);
  atomic_set(&asgn2_device.max_nprocs, 1);
  asgn2_device.read_pos = 0;
  asgn2_device.write_pos = 0;

                               asgn2_dev_count, MYDEV_NAME);

  if (result < 0) {
    printk(KERN_WARNING "asgn2: can't get major number\n");
    return -EBUSY;
  }

  asgn2_major = MAJOR(asgn2_device.dev);

  if (NULL == (asgn2_device.cdev = cdev_alloc())) {
    printk(KERN_WARNING "%s: can't allocate cdev\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_cdev;
  }

  asgn2_device.cdev->ops = &asgn2_fops;
  asgn2_device.cdev->owner = THIS_MODULE;
  
  result = cdev_add(asgn2_device.cdev, asgn2_device.dev, asgn2_dev_count);
  if (result < 0) {
    printk(KERN_WARNING "%s: can't register chrdev_region to the system\n",
           MYDEV_NAME);
    goto fail_cdev;
  }
  
  /* allocate pages */
  INIT_LIST_HEAD(&asgn2_device.mem_list);
  asgn2_device.num_pages = 0;
  asgn2_device.data_size = 0;

  if (NULL == create_proc_read_entry(MYDEV_NAME, 
				     0, /* default mode */ 
				     NULL, /* parent dir */
				     asgn2_read_procmem,
				     NULL /* client data */)) {
    printk(KERN_WARNING "%s: can't create procfs entry\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_proc_entry;
  }

  asgn2_device.cache = kmem_cache_create(MYDEV_NAME, sizeof(page_node), 
                                         0, 0, NULL); 
  
  if (NULL == asgn2_device.cache) {
    printk(KERN_WARNING "%s: can't create cache\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_kmem_cache_create;
  }
  /* END TRIM */
 
  asgn2_device.class = class_create(THIS_MODULE, MYDEV_NAME);
  if (IS_ERR(asgn2_device.class)) {
    printk(KERN_WARNING "%s: can't create udev class\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_class;
  }

  asgn2_device.device = device_create(asgn2_device.class, NULL, 
                                      asgn2_device.dev, "%s", MYDEV_NAME);
  if (IS_ERR(asgn2_device.device)) {
    printk(KERN_WARNING "%s: can't create udev device\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_device;
  }

  if(gpio_dummy_init()<0){
    printk(KERN_WARNING "%s: can't initilise gpio pins\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_gpio;
  }

  if(request_irq(irq_number, dummyport_interrupt, 0, MYDEV_NAME, asgn2_device.device)){
    printk(KERN_WARNING "%s: Unable to request IRQ for this device \n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_irq;
  }

  cbuf.write_pos = 0;
  cbuf.read_pos = 0;
  cbuf.count = 0;
  cbuf.capacity = PAGE_SIZE;
  if (NULL == (cbuf.buf = kmalloc(sizeof(char) * cbuf.capacity, GFP_KERNEL))) {
    printk(KERN_WARNING "%s: Unable allocate cicular buffer memory\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_irq;
  }
  
  printk(KERN_WARNING "set up udev entry\n");
  printk(KERN_WARNING "Hello world from %s\n", MYDEV_NAME);
  return 0;

  /* cleanup code called when any of the initialization steps fail */
fail_device:
   class_destroy(asgn2_device.class);
fail_class:
   kmem_cache_destroy(asgn2_device.cache);  
fail_kmem_cache_create:
  remove_proc_entry(MYDEV_NAME, NULL /* parent dir */);
fail_proc_entry:
  cdev_del(asgn2_device.cdev);
fail_cdev:
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
  
  free_memory_pages();
  kmem_cache_destroy(asgn2_device.cache);
  remove_proc_entry(MYDEV_NAME, NULL /* parent dir */);
  cdev_del(asgn2_device.cdev);
  unregister_chrdev_region(asgn2_device.dev, asgn2_dev_count);
  gpio_dummy_exit();
  free_irq(irq_number, asgn2_device.device);
  kfree(cbuf.buf);
  printk(KERN_WARNING "Good bye from %s\n", MYDEV_NAME);
}


module_init(asgn2_init_module);
module_exit(asgn2_exit_module);


