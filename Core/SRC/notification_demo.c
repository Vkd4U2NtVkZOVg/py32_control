/**
 * @file notification_demo.c
 * @brief QuarkTS simple and queued notification example.
 *
 * A producer task sends a payload to a disabled consumer using the simple
 * notification API. The consumer wakes only when notified; its callback can
 * distinguish byNotificationSimple from byNotificationQueued through e->Trigger.
 */
#include "main.h"
#include "QuarkTS.h"
#include "notification_demo.h"
#include "qport_py32.h"

#define NOTIFY_PERIOD_S (1.0f)

typedef struct {
    uint32_t sequence;
    uint32_t timestamp_ms;
} NotificationPayload_t;

static qTask_t s_producer_task;
static qTask_t s_consumer_task;
static NotificationPayload_t s_payload;

static void Notification_Consumer(qEvent_t e)
{
    const NotificationPayload_t *payload;

    if ((e == NULL) || (e->EventData == NULL)) {
        return;
    }
    payload = (const NotificationPayload_t *)e->EventData;

    /* Put application handling here. For this example, the payload is read
     * only when the asynchronous notification actually wakes the task. */
    (void)payload;
    (void)e->Trigger;
}

static void Notification_Producer(qEvent_t e)
{
    (void)e;
    s_payload.sequence++;
    s_payload.timestamp_ms = QOS_Tick_Get();
    (void)qTask_Notification_Send(&s_consumer_task, &s_payload);
}

void Notification_Demo_Init(void)
{
    s_payload.sequence = 0U;
    s_payload.timestamp_ms = 0U;

    /* Consumer is disabled: it runs only after qTask_Notification_Send(). */
    (void)qOS_Add_Task(&s_consumer_task, Notification_Consumer,
                       qMedium_Priority, 0.0f, qPeriodic, qDisabled, NULL);

    /* Producer runs every second and wakes the consumer asynchronously. */
    (void)qOS_Add_Task(&s_producer_task, Notification_Producer,
                       qHigh_Priority, (qTime_t)NOTIFY_PERIOD_S,
                       qPeriodic, qEnabled, NULL);
}
