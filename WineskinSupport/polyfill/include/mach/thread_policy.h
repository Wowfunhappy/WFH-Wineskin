/* mach/thread_policy.h with the thread QoS policies the 10.10 SDK declares. 10.9's kernel answers
   KERN_INVALID_ARGUMENT to them: it has no QoS tiers. */
#include_next <mach/thread_policy.h>

#if !defined(WINESKIN_THREAD_POLICY_H) && !defined(THREAD_LATENCY_QOS_POLICY)
#define WINESKIN_THREAD_POLICY_H

#define THREAD_LATENCY_QOS_POLICY       7
struct thread_latency_qos_policy {
    integer_t thread_latency_qos_tier;
};
typedef struct thread_latency_qos_policy thread_latency_qos_policy_data_t;
typedef struct thread_latency_qos_policy *thread_latency_qos_policy_t;
#define THREAD_LATENCY_QOS_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_latency_qos_policy_data_t) / sizeof(integer_t)))

#define THREAD_THROUGHPUT_QOS_POLICY    8
struct thread_throughput_qos_policy {
    integer_t thread_throughput_qos_tier;
};
typedef struct thread_throughput_qos_policy thread_throughput_qos_policy_data_t;
typedef struct thread_throughput_qos_policy *thread_throughput_qos_policy_t;
#define THREAD_THROUGHPUT_QOS_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_throughput_qos_policy_data_t) / sizeof(integer_t)))

#endif
