#ifndef TASKS_H
#define TASKS_H

/* Stack in words (StackType_t). 4096 words = 32 KB on x86-64. */
#define TASK_STACK_WORDS  4096U

#define TASK_PRIO_CONTROL    3U
#define TASK_PRIO_COMMS      2U
#define TASK_PRIO_MONITOR    1U
#define TASK_PRIO_LIGHT_POLL 4U
#define TASK_PRIO_WATCHDOG   3U
#define TASK_PRIO_HEARTBEAT  2U
#define TASK_PRIO_DIAG       1U
#define TASK_PRIO_TELEMETRY  1U
#define TASK_PRIO_COMMANDS   1U
#define TASK_PRIO_CONFIRM    1U
#define TASK_PRIO_OTA        1U

void v_control_task(void *pv_parameters);
void v_comms_task(void *pv_parameters);
void v_monitor_task(void *pv_parameters);
void v_light_poll_task(void *pv_parameters);
void v_heartbeat_task(void *pv_parameters);
void v_diag_poll_task(void *pv_parameters);
void v_watchdog_task(void *pv_parameters);
void v_telemetry_task(void *pv_parameters);
void v_command_poll_task(void *pv_parameters);
void v_confirm_task(void *pv_parameters);
void v_ota_task(void *pv_parameters);

#endif /* TASKS_H */
