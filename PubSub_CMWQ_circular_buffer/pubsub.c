#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/delay.h>
#include <linux/atomic.h>

#define BUFFER_SIZE 1000

static char message_buffer[BUFFER_SIZE][256];
static struct timespec64 publish_times[BUFFER_SIZE];
static struct timespec64 overall_start_time, overall_end_time;

static atomic_t message_count = ATOMIC_INIT(0);
static int head = 0;
static int tail = 0;
static atomic_t read_count[BUFFER_SIZE];

static struct mutex buffer_lock;
static DECLARE_WAIT_QUEUE_HEAD(subscriber_queue);
static DECLARE_WAIT_QUEUE_HEAD(publisher_queue);

struct pubsub_work {
    struct work_struct work;
    char message[256];
};

struct pubsub_id_work {
    struct work_struct work;
    int id;
};

static struct workqueue_struct *pubsub_wq;
static int num_subscribers = 1;
static int num_publishers = 1;
static ktime_t total_time_ns = 0;
static ktime_t overall_time = 0;
static bool exit_flag = false;

module_param(num_subscribers, int, 0644);
module_param(num_publishers, int, 0644);

static void broker_fn(struct work_struct *work) {
    struct pubsub_work *ps_work = container_of(work, struct pubsub_work, work);

    mutex_lock(&buffer_lock);
    while (atomic_read(&message_count) >= BUFFER_SIZE && !exit_flag) {
        mutex_unlock(&buffer_lock);
        wait_event_interruptible(publisher_queue, atomic_read(&message_count) < BUFFER_SIZE || exit_flag);
        mutex_lock(&buffer_lock);
    }

    if (!exit_flag) {
        strncpy(message_buffer[tail], ps_work->message, 256);
        ktime_get_real_ts64(&publish_times[tail]);
        atomic_set(&read_count[tail], 0);
        tail = (tail + 1) % BUFFER_SIZE;
        atomic_inc(&message_count);
        pr_info("Message published: %s\n", ps_work->message);
        wake_up_interruptible(&subscriber_queue);
    }
    mutex_unlock(&buffer_lock);
    kfree(ps_work);
}

static void publish_message(const char *msg) {
    struct pubsub_work *ps_work;

    ps_work = kmalloc(sizeof(*ps_work), GFP_KERNEL);
    if (!ps_work) {
        pr_err("Failed to allocate memory for pubsub_work\n");
        return;
    }

    strncpy(ps_work->message, msg, 256);
    INIT_WORK(&ps_work->work, broker_fn);
    queue_work(pubsub_wq, &ps_work->work);
}

static void publisher_work_fn(struct work_struct *work) {
    struct pubsub_id_work *id_work = container_of(work, struct pubsub_id_work, work);
    int id = id_work->id;
    int i;

    kfree(id_work);

    for (i = 0; i < 3; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "Publisher %d message %d", id, i);
        publish_message(msg);
        //pr_info("Publisher %d queued message %d\n", id, i);
    }
}

static void subscriber_work_fn(struct work_struct *work) {
    struct pubsub_id_work *id_work = container_of(work, struct pubsub_id_work, work);
    char msg[256];
    int id = id_work->id;
    struct timespec64 receive_time, time_diff;
    int my_read_index = 0;

    kfree(id_work);
    while (!exit_flag) {
        wait_event_interruptible_timeout(subscriber_queue, atomic_read(&message_count) > 0 || exit_flag, msecs_to_jiffies(100));

        mutex_lock(&buffer_lock);
        while (my_read_index != tail && atomic_read(&message_count) > 0) {
            int buffer_index = my_read_index;
            strncpy(msg, message_buffer[buffer_index], 256);
            ktime_get_real_ts64(&receive_time);
            time_diff = timespec64_sub(receive_time, publish_times[buffer_index]);
            total_time_ns += timespec64_to_ns(&time_diff);
            pr_info("Subscriber %d received: %s, Time taken: %lld.%.9lds\n", id, msg,
                    (long long)time_diff.tv_sec, time_diff.tv_nsec);

            atomic_inc(&read_count[buffer_index]);
            if (atomic_read(&read_count[buffer_index]) >= num_subscribers) {
                head = (head + 1) % BUFFER_SIZE;
                atomic_dec(&message_count);
                wake_up_interruptible(&publisher_queue);
            }
            my_read_index = (my_read_index + 1) % BUFFER_SIZE;
        }
        mutex_unlock(&buffer_lock);
    }
}

static int __init pubsub_init(void) {
    int i;
    ktime_get_real_ts64(&overall_start_time);
    mutex_init(&buffer_lock);
    pubsub_wq = alloc_workqueue("pubsub_wq", WQ_UNBOUND, 0);
    if (!pubsub_wq) {
        pr_err("Failed to create workqueue\n");
        return -ENOMEM;
    }

    for (i = 0; i < num_publishers; i++) {
        struct pubsub_id_work *id_work = kmalloc(sizeof(*id_work), GFP_KERNEL);
        if (!id_work) {
            pr_err("Failed to allocate memory for publisher ID %d\n", i);
            destroy_workqueue(pubsub_wq);
            return -ENOMEM;
        }
        id_work->id = i;
        INIT_WORK(&id_work->work, publisher_work_fn);
        queue_work(pubsub_wq, &id_work->work);
    }

    for (i = 0; i < num_subscribers; i++) {
        struct pubsub_id_work *id_work = kmalloc(sizeof(*id_work), GFP_KERNEL);
        if (!id_work) {
            pr_err("Failed to allocate memory for subscriber ID %d\n", i);
            destroy_workqueue(pubsub_wq);
            return -ENOMEM;
        }
        id_work->id = i;
        INIT_WORK(&id_work->work, subscriber_work_fn);
        queue_work(pubsub_wq, &id_work->work);
    }

    pr_info("Pub-Sub module loaded with %d subscribers and %d publishers\n", num_subscribers, num_publishers);
    return 0;
}

static void __exit pubsub_exit(void) {
    struct timespec64 time_diff;
    ktime_get_real_ts64(&overall_end_time);
    exit_flag = true;
    wake_up_interruptible_all(&publisher_queue);
    wake_up_interruptible_all(&subscriber_queue);
    flush_workqueue(pubsub_wq);
    destroy_workqueue(pubsub_wq);
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
MODULE_AUTHOR("NI, YING CHIH");
MODULE_DESCRIPTION("A simple Pub-Sub Linux kernel module fully using CMWQ with adjustable publishers and subscribers, and time tracking");
