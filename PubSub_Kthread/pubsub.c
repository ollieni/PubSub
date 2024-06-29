#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/delay.h>

#define BUFFER_SIZE 100

static char message_buffer[BUFFER_SIZE][256];
static struct timespec64 publish_times[BUFFER_SIZE];
static struct timespec64 overall_start_time, overall_end_time;
static int message_count = 0;
static int write_index = 0;

static struct mutex buffer_lock;
static DECLARE_WAIT_QUEUE_HEAD(subscriber_queue);

struct message_entry {
    struct list_head list;
    char message[256];
};

static LIST_HEAD(message_list);
static struct mutex list_lock;
static DECLARE_WAIT_QUEUE_HEAD(broker_queue);

static int num_subscribers = 1;
static int num_publishers = 1;
static ktime_t total_time_ns = 0; // Variable to keep track of the total time in nanoseconds
static ktime_t overall_time = 0;
static bool exit_flag = false; // Flag to signal the threads to exit

module_param(num_subscribers, int, 0644);
module_param(num_publishers, int, 0644);

static int broker_thread(void *data) {
    struct message_entry *entry;

    while (!exit_flag) {
        wait_event_interruptible(broker_queue, !list_empty(&message_list) || exit_flag);

        if (exit_flag)
            break;

        mutex_lock(&list_lock);
        if (!list_empty(&message_list)) {
            entry = list_first_entry(&message_list, struct message_entry, list);
            list_del(&entry->list);
            mutex_unlock(&list_lock);

            mutex_lock(&buffer_lock);
            if (message_count < BUFFER_SIZE) {
                strncpy(message_buffer[write_index], entry->message, 256);
                ktime_get_real_ts64(&publish_times[write_index]);
                write_index = (write_index + 1) % BUFFER_SIZE;
                message_count++;
                wake_up_interruptible(&subscriber_queue);
            }
            mutex_unlock(&buffer_lock);

            kfree(entry);
        } else {
            mutex_unlock(&list_lock);
        }
    }

    return 0;
}

static void publish_message(const char *msg) {
    struct message_entry *entry;

    entry = kmalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        pr_err("Failed to allocate memory for message_entry\n");
        return;
    }

    strncpy(entry->message, msg, 256);
    INIT_LIST_HEAD(&entry->list);

    mutex_lock(&list_lock);
    list_add_tail(&entry->list, &message_list);
    mutex_unlock(&list_lock);

    wake_up_interruptible(&broker_queue);
}

static int publisher_thread(void *data) {
    int id = *(int *)data;
    int i;

    for (i = 0; i < 3; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "Publisher %d message %d", id, i);
        publish_message(msg);
    }

    kfree(data);
    return 0;
}

static int subscriber_thread(void *data) {
    int id = *(int *)data;
    char msg[256];
    struct timespec64 receive_time, time_diff;
    int my_read_index = 0;

    while (!exit_flag) {
        if (wait_event_interruptible(subscriber_queue, message_count > my_read_index || exit_flag))
            continue; // handle signal

        if (exit_flag)
            break;

        if (message_count > my_read_index) {
            int buffer_index = (my_read_index + write_index - message_count + BUFFER_SIZE) % BUFFER_SIZE;
            strncpy(msg, message_buffer[buffer_index], 256);
            ktime_get_real_ts64(&receive_time);
            time_diff = timespec64_sub(receive_time, publish_times[buffer_index]);
            total_time_ns += timespec64_to_ns(&time_diff); // Update the total time
            pr_info("Subscriber %d received: %s, Time taken: %lld.%.9lds\n", id, msg,
                    (long long)time_diff.tv_sec, time_diff.tv_nsec);
            my_read_index++;
        }
    }

    kfree(data);
    return 0;
}

static int __init pubsub_init(void) {
    int i;
    int *id;
    struct task_struct *thread;
    ktime_get_real_ts64(&overall_start_time);
    mutex_init(&buffer_lock);
    mutex_init(&list_lock);

    thread = kthread_run(broker_thread, NULL, "broker_thread");
    if (IS_ERR(thread)) {
        pr_err("Failed to create broker thread\n");
        return PTR_ERR(thread);
    }

    for (i = 0; i < num_subscribers; i++) {
        id = kmalloc(sizeof(*id), GFP_KERNEL);
        if (!id) {
            pr_err("Failed to allocate memory for subscriber ID %d\n", i);
            return -ENOMEM;
        }
        *id = i;
        thread = kthread_run(subscriber_thread, id, "subscriber_thread_%d", i);
        if (IS_ERR(thread)) {
            pr_err("Failed to create subscriber thread %d\n", i);
            kfree(id);
            return PTR_ERR(thread);
        }
    }

    for (i = 0; i < num_publishers; i++) {
        id = kmalloc(sizeof(*id), GFP_KERNEL);
        if (!id) {
            pr_err("Failed to allocate memory for publisher ID %d\n", i);
            return -ENOMEM;
        }
        *id = i;
        thread = kthread_run(publisher_thread, id, "publisher_thread_%d", i);
        if (IS_ERR(thread)) {
            pr_err("Failed to create publisher thread %d\n", i);
            kfree(id);
            return PTR_ERR(thread);
        }
    }

    pr_info("Pub-Sub module loaded with %d subscribers and %d publishers\n", num_subscribers, num_publishers);
    return 0;
}

static void __exit pubsub_exit(void) {
    exit_flag = true;
    struct timespec64 time_diff;
    ktime_get_real_ts64(&overall_end_time);
    wake_up_interruptible(&broker_queue);
    wake_up_interruptible(&subscriber_queue);
    time_diff = timespec64_sub(overall_end_time, overall_start_time);
    overall_time = timespec64_to_ns(&time_diff);
    pr_info("Total time taken by all subscribers: %lld.%.9lds\n",
            (long long)total_time_ns / 1000000000, (long)total_time_ns % 1000000000);
    pr_info("Overall time taken by all subscribers: %lld.%.9lds\n",
            (long long)overall_time / 1000000000, (long)overall_time % 1000000000);
    pr_info("Pub-Sub module unloaded\n");
}

module_init(pubsub_init);
module_exit(pubsub_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple Pub-Sub Linux kernel module using kthreads with a broker function, adjustable publishers and subscribers, and time tracking");
