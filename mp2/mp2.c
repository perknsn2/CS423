#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include "mp2_given.h"


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group_17");
MODULE_DESCRIPTION("CS-423 MP2");

#define DEBUG 1
#define FILENAME "status"
#define DIRECTORY "mp2"

#define UBBOUND 693
#define FCONV 1000

struct task_struct* mp2_dispatcher;

//structs for proc filesystem
static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *proc_entry;

spinlock_t *list_lock;

//all times are stored as jiffies
typedef struct  
{
   struct task_struct* linux_task; 
   struct timer_list wakeup_timer; 
   int period;  //p
   int computation; //c
   int pid;
   enum {READY, RUNNING, SLEEPING} status;
   struct list_head list;
   unsigned long start_time;
} mp2_task_struct;

static struct list_head processList;

static int read_end;

static struct kmem_cache *PCB_cache;

/* Function prototypes */
void timer_handler(unsigned long task);
int add_process (int pid, int computation, int period);
void list_cleanup(void);
int kernel_thread_fn(void *data);
int de_register(int pid);
int do_yield(int pid);
int admin_ctrl(int period, int comp_time);
mp2_task_struct * _getCurrentTask(void);
mp2_task_struct * _getNextTask(void);
mp2_task_struct * activeTask = NULL;

/* admin_ctrl
 * Admission control for Real-Time Scheduler
 * INPUTS: period -- period of task to be added, comp_time --  computation time of task to be added
 * OUTPUtS: 0 -- new task is schedulable, -1 -- new task is not schedulable
 */
int admin_ctrl(int period, int comp_time){
   mp2_task_struct *thisProcess;
   int comp_sum = 0;
   int period_sum = 0;
   list_for_each_entry(thisProcess, &processList, list){
      comp_sum += (thisProcess->computation);
	  period_sum += (thisProcess->period);
   }
   comp_sum += (msecs_to_jiffies(comp_time));
   period_sum += (msecs_to_jiffies(period));
   if((comp_sum*FCONV/period_sum) <= UBBOUND){
      return 0;
   }
   return -1;
}

/*
 * getCurrentTask
 * Gets current RUNNING process
 * Inputs -- NONE
 * Return Value -- The pointer to the current RUNNING process.
 * 		Returns NULL, if no such process exists
 */
mp2_task_struct* _getCurrentTask(void)
{
  mp2_task_struct *thisProcess;
 list_for_each_entry(thisProcess, &processList, list)
     {
      if(thisProcess->status == RUNNING)
         return thisProcess;
    }
    return NULL;
}

/*
 * getNextTask
 * gets the READY task with the lowest period according to spec
 * Inputs -- NONE
 * Return Value -- The pointer to the READY process with shortest period.
 * 		Returns NULL, if no such process exists
 */
mp2_task_struct* _getNextTask(void)
{
  int lowestPeriod = 999999999;
  mp2_task_struct * curLowest = NULL;
  mp2_task_struct *thisProcess;

 list_for_each_entry(thisProcess, &processList, list)
  {
      if(thisProcess->status == READY && (thisProcess->period < lowestPeriod))
      {
        lowestPeriod = thisProcess -> period;
        curLowest = thisProcess;
      }
  }
  if(lowestPeriod == 999999999)
      return NULL;
  else
      return curLowest;
}

/* 
 * kernel_thread_fn
 * Kernel thread that, when woken up does the scheduling routine.
 * It prempts the currently running task with the next READY task, 
 * if available 
 * INPUTS -- NONE
 * RETURN VALUE -- 0 on completion
 */
int kernel_thread_fn(void *data)
{
   printk( "MP2 Dispatching Thread\n");
   while(!kthread_should_stop())
   {
   mp2_task_struct * nextTask = _getNextTask();
   mp2_task_struct * curTask = _getCurrentTask();

      if(nextTask != NULL)
      {
         struct sched_param sparam; 
         wake_up_process(nextTask->linux_task); 
         sparam.sched_priority=99;
         sched_setscheduler(nextTask->linux_task, SCHED_FIFO, &sparam);
         spin_lock(list_lock);
         nextTask->status = RUNNING;
         nextTask->start_time = jiffies;
         spin_unlock(list_lock);
	//	 printk("nextTask -- %d\n", nextTask->pid);
      }
      if(curTask != NULL)
      {
         struct sched_param sparam;
         sparam.sched_priority=0; 
         sched_setscheduler(curTask->linux_task, SCHED_NORMAL, &sparam);
         spin_lock(list_lock);
         nextTask->status = READY;
         spin_unlock(list_lock);
	//	 printk("curTask -- %d\n", curTask->pid);
      }
	  set_current_state(TASK_INTERRUPTIBLE);
	  schedule();

   }
	return 0;
}

/* 
 * timer_handler
 * INPUTS: task -- Pointer to the task to be set to READY
 * SIDE EFFECTS: wakes up the Kernel thread
 */
void timer_handler(unsigned long task)
{
	mp2_task_struct * the_task = (mp2_task_struct *) task;
   spin_lock(list_lock);
	the_task -> status = READY;
   spin_unlock(list_lock);

	printk(KERN_INFO "Timer -- PID: %d\n", the_task->pid);
	//needs to wake up dispatcher thread, but thats pretty much it
  	wake_up_process(mp2_dispatcher);
}


/* mp2_read -- Callback function when reading from the proc file
 *
 * Inputs: buffer -- User space buffer to copy information to
 * 			count -- Number of bytes to copy to the buffer
 * Outputs: Copies the string to the user buffer
 * CPU usage time in the following format:
 * PID: xx | Period : xxxxx | Processing Time: xxxx
 * PID: xx | Period : xxxxx | Processing Time: xxxx
 *
 * Side Effects: Holds the list_lock
 * Return Value: Number of bytes written to the buffer.
 */

static ssize_t mp2_read (struct file *file, char __user *buffer, size_t count, loff_t *data) 
{
    char * tempBuffer = kmalloc(count, GFP_KERNEL); 
    mp2_task_struct * cursor;

    if(tempBuffer == NULL)
    {
        printk(KERN_WARNING "read malloc failed");
        return 0;
    }
  
    /* Based on read_end, we decide whether to return 0 (to indicate we are done reading)
   *  If 0 is returned, we reset the boolean */
  if (read_end == 0)
  {
     int bytes_copied = 0;

     spin_lock(list_lock);
   
	   /* Output the string into tempBuffer for all entries in the list */
	   list_for_each_entry(cursor, &processList, list)
	   {
	 	 bytes_copied += snprintf(&tempBuffer[bytes_copied], count - bytes_copied, "PID: %d | Period: %d | Processing Time: %d\n", cursor->pid, 
         jiffies_to_msecs(cursor->period), jiffies_to_msecs(cursor->computation));
	   }

	   spin_unlock(list_lock);
	   read_end = 1;
	   // Copy to user buffer
	   if (copy_to_user(buffer, tempBuffer, bytes_copied) == 0) // success
	   {
          kfree(tempBuffer);
        return bytes_copied;
     }
     else
     {
   	 kfree(tempBuffer);
   	 return 0;
     }

  } 
  else // read_end == 1 means we just return 0 for formality
  {
     read_end = 0;
     kfree(tempBuffer);
       return 0;	  
    }   
}


/* add_process(int pid)
 * Adds a new process with given pid to the linked list 
 * Inputs:  pid -- The pid of the registered process
 * Return Value: 1 on success, 0 on failure
 * Side Effects: Holds the list_lock lock
 */
int add_process (int pid, int computation, int period)
{
   mp2_task_struct * newProcess = kmem_cache_alloc(PCB_cache, GFP_KERNEL);
   if(!newProcess)
   {
      printk(KERN_WARNING "mp2 add malloc failed\n");
      return 0;
   }

   printk(KERN_INFO "Adding pid %d\n", pid);

   /* Add to the linked list */
   INIT_LIST_HEAD(&newProcess->list);

   newProcess->computation = msecs_to_jiffies(computation);
   newProcess->period = msecs_to_jiffies(period);
   newProcess->pid = pid;
   newProcess->status = SLEEPING;
   newProcess->linux_task = find_task_by_pid(pid);

   init_timer(&(newProcess->wakeup_timer)); 
   //Initialize timer to wake up work queue
   (newProcess->wakeup_timer).function = timer_handler;
   (newProcess->wakeup_timer).expires = jiffies + newProcess->period; //app must be switched off after 1 period
   (newProcess->wakeup_timer).data = (unsigned long)(newProcess);

   //When the process started running
   newProcess->start_time = jiffies;

   spin_lock(list_lock);
   list_add_tail( &newProcess->list, &processList);
   spin_unlock(list_lock);

   return 1;
}


/* mp2_write
 * Registers a new process with pid given in the user buffer
 * Inputs: buffer -- User buffer that contains the pid of the new process
 * Return Value: number of bytes copied from the user
 * Side effects: Calls add_process that holds list_lock
 */
static ssize_t mp2_write (struct file *file, const char __user *buffer, size_t count, loff_t *data) 
{
   char * tempBuffer = kmalloc(count+1, GFP_KERNEL);
   int PID, period, comp_time;
   char op;
	if(tempBuffer == NULL)
	{
		printk(KERN_WARNING "mp2 write malloc failed\n");
		return 0;
	}

	/* Get info from user */
	  
	if(copy_from_user(tempBuffer, buffer, count) != 0)
		goto write_fail;

	/* NULL terminate string */
	tempBuffer[count] = '\0';

	printk(KERN_INFO "MP2 Command: %s\n", tempBuffer);
   sscanf(tempBuffer, "%c", &op);

   printk(KERN_INFO "MP2 OP: %c\n", op);
	/* parse string */
   switch (op){
      case 'R':
         printk(KERN_INFO "MP2 Registration\n");
   		sscanf(tempBuffer, "%c, %d, %d, %d", &op, &PID, &period, &comp_time);
         if(admin_ctrl(period, comp_time) != 0)
		 {
			printk(KERN_INFO "Failed admin ctrl\n");
			kfree(tempBuffer);
			return count;
		 }
         add_process(PID,comp_time, period);
         break;

      case 'Y':
         printk(KERN_INFO "MP2 Yield\n");
         sscanf(tempBuffer, "%c, %d", &op, &PID);
         do_yield(PID);
         break;

      case 'D':
         printk(KERN_INFO "MP2 De-Registration\n");
   		sscanf(tempBuffer, "%c, %d", &op, &PID);
         de_register(PID);
         break;

      default:
         printk(KERN_WARNING "MP2 write fail\n");
         goto write_fail;
         break;
   }

	kfree(tempBuffer);

	return  count;
write_fail:
	printk(KERN_WARNING "write failed\n");
	kfree(tempBuffer);
	return 0;	
}

/* do_yield
 * Handler for the Yield function of a process
 * Inputs: pid -- Pid of the process that is yielding
 * Return Value -- 0 o nsuccess
 * Side Effects -- Puts the process into UNINTERRUPTIBLE sleep
 * 		until the next timer
 */
int do_yield(int pid){
   mp2_task_struct *thisProcess;

   list_for_each_entry(thisProcess, &processList, list)
     {
      if(thisProcess->pid == pid)
      {
         spin_lock(list_lock);
         thisProcess->status = SLEEPING;
         spin_unlock(list_lock);
         if (time_after_eq(jiffies,thisProcess->start_time + thisProcess->period))
		   {
            //I don't think we should ever technically get into this conditional
            printk(KERN_WARNING "MP2 Deadline Miss");
            //wake up next iteration in 1 period's time
            spin_lock(list_lock);
            mod_timer(&thisProcess->wakeup_timer,jiffies + thisProcess->period);
            spin_unlock(list_lock);
         }
         else{
            printk(KERN_INFO "MP2 made Deadline");
            //time until next period
            //assumes that start_time is continuously updated
            spin_lock(list_lock);
            mod_timer(&thisProcess->wakeup_timer, thisProcess->start_time + thisProcess->period - jiffies);
            spin_unlock(list_lock);
         }
         set_task_state(thisProcess->linux_task, TASK_UNINTERRUPTIBLE);
      }
   }
   return 0;
}

static const struct file_operations mp2_file = 
{
   .owner = THIS_MODULE,
   .read = mp2_read,
   .write = mp2_write,
};

/* de_register 
 * Removes a process from being scheduled.
 * Input: pid -- process to be removed
 * Return Value: 1 on success, 0 on failure
*/
int de_register(int pid){
   mp2_task_struct *thisProcess, *next;

   printk(KERN_INFO "MP2 De-Registration\n");

   list_for_each_entry_safe(thisProcess, next, &processList, list)
   {
      if (pid == thisProcess->pid){
         spin_lock(list_lock);
         del_timer(&thisProcess->wakeup_timer);
         list_del(&thisProcess->list);
         kmem_cache_free(PCB_cache, thisProcess);
         printk(KERN_INFO "MP2 deleting process %d\n", pid);
         spin_unlock(list_lock);
         return 1;
      }
   }
   
   printk(KERN_WARNING "MP2 Did not find PID\n");
   return 0;
}


/* list_cleanup
 * Helper function to delete the linked list upon kernel module removal
 * Side Effects: Holds list lock and deletes the entire processList
 */
void list_cleanup(void) 
{
   mp2_task_struct *aProcess, *tmp;

   spin_lock(list_lock);
   if (list_empty(&processList) == 0) 
   {
   #ifdef DEBUG
      printk(KERN_INFO "Cleaning up processList\n");
    #endif
     // Delete all entry
      list_for_each_entry_safe(aProcess, tmp, &processList, list) 
      {
         #ifdef DEBUG
         printk(KERN_INFO "MP2 freeing PID %d\n", aProcess->pid);
         #endif

         list_del(&aProcess->list);
         del_timer(&aProcess->wakeup_timer);
         kmem_cache_free(PCB_cache, aProcess);
      }
   }

   spin_unlock(list_lock);
}

// mp2_init - Called when module is loaded
int __init mp2_init(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "mp2 MODULE LOADING\n");
   #endif

   //create kthread
   mp2_dispatcher = kthread_create(kernel_thread_fn,NULL,"MP2_dispatcher_thread");
   printk(KERN_INFO "MP2 Hello World!\n");

   list_lock = kmalloc(sizeof(spinlock_t), GFP_KERNEL);
   if(list_lock == NULL)
   {
      printk(KERN_WARNING "spinlock malloc failed\n");
      return -1;
   }

   //Init spin lock
   spin_lock_init(list_lock);

   /* Initialize Slab allocator*/
    PCB_cache = kmem_cache_create("PCB", sizeof(mp2_task_struct), 0, 0, NULL);
    if(!PCB_cache){
       kmem_cache_destroy(PCB_cache);
       return -ENOMEM;
    }
   
   INIT_LIST_HEAD(&processList);

   proc_dir = proc_mkdir(DIRECTORY, NULL);
   proc_entry = proc_create(FILENAME, 0666, proc_dir, &mp2_file);  //create entry in proc system
   
   wake_up_process(mp2_dispatcher);
   printk(KERN_ALERT "mp2 MODULE LOADED\n");
   return 0;   
}

// mp2_exit - Called when module is unloaded
void __exit mp2_exit(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "mp2 MODULE UNLOADING\n");
   #endif
   // Insert your code here ...
   //stop the kernel thread
   kthread_stop(mp2_dispatcher);

   list_cleanup();
   kmem_cache_destroy(PCB_cache);

   remove_proc_entry(FILENAME, proc_dir);
   proc_remove(proc_dir);
   kfree(list_lock);

   printk(KERN_ALERT "MP2 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp2_init);
module_exit(mp2_exit);
