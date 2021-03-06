#include <stdlib.h>
#include <uv.h>
#include <assert.h>
#include "meow_coroutine.h"

MEOW_GLOBALS_DECLARE(meow_coroutine);

void meow_coroutine_module_init()
{
    MEOW_COROUTINE_G(last_id) = 0;
    MEOW_COROUTINE_G(stack_size) = MEOW_CONTEXT_DEFAULT_STACK_SIZE;

    meow_coroutine_t *main_coroutine = &MEOW_COROUTINE_G(_main);
    main_coroutine->id = MEOW_COROUTINE_MAIN_ID;
    main_coroutine->state = MEOW_COROUTINE_STATE_READY;
    main_coroutine->previous = NULL;
    main_coroutine->context = NULL;

    MEOW_QUEUE_INIT(&main_coroutine->defer_tasks);

    MEOW_COROUTINE_G(main) = main_coroutine;
    MEOW_COROUTINE_G(current) = main_coroutine;
    MEOW_COROUTINE_G(last_id) = main_coroutine->id + 1;
    MEOW_COROUTINE_G(scheduler) = NULL;
}

meow_coroutine_t *meow_coroutine_create(meow_coroutine_func_t func, void *data)
{
    return meow_coroutine_create_ex(func, data, 0);
}

meow_coroutine_t *meow_coroutine_create_ex(meow_coroutine_func_t func, void *data, uint32_t stack_size)
{
    meow_coroutine_t *coroutine;

    coroutine = (meow_coroutine_t *) malloc(sizeof(meow_coroutine_t));
    if (coroutine == NULL) {
        meow_warn("Create coroutine failed: (errno %d) %s", errno, strerror(errno))
        return NULL;
    }

    coroutine->previous = NULL;
    coroutine->id = MEOW_COROUTINE_G(last_id)++;
    coroutine->state = MEOW_COROUTINE_STATE_READY;

    if (stack_size == 0) {
        stack_size = MEOW_COROUTINE_G(stack_size);
    }

    coroutine->context = meow_context_create_ex((meow_context_func_t) func, data, stack_size);
    if (coroutine->context == NULL) {
        free(coroutine);
        return NULL;
    }

    MEOW_QUEUE_INIT(&coroutine->defer_tasks);

    return coroutine;
}

meow_coroutine_t *meow_coroutine_get_current()
{
    return MEOW_COROUTINE_G(current);
}

meow_coroutine_t *meow_coroutine_get_root()
{
    return meow_coroutine_get_by_index(0);
}

meow_coroutine_t *meow_coroutine_get_by_index(uint32_t index)
{
    uint32_t count = 0;
    meow_coroutine_t *coroutine  = MEOW_COROUTINE_G(current);

    while (coroutine->previous != NULL) {
        count++;
        coroutine = coroutine->previous;
    }

    if (index == 0) {
        return coroutine;
    } else if (index > count) {
        return NULL;
    }

    coroutine = MEOW_COROUTINE_G(current);
    count -= index;

    while (count--) {
        coroutine = coroutine->previous;
    }

    return coroutine;
}

meow_bool_t meow_coroutine_is_alive(meow_coroutine_t *coroutine)
{
    return coroutine->state > MEOW_COROUTINE_STATE_READY && coroutine->state < MEOW_COROUTINE_STATE_FINISHED;
}

meow_bool_t meow_coroutine_is_resumable(meow_coroutine_t *coroutine)
{
    meow_coroutine_t *current = MEOW_COROUTINE_G(current);

    if (current->previous == coroutine) {
        return meow_true;
    }

    switch (coroutine->state) {
        case MEOW_COROUTINE_STATE_READY:
        case MEOW_COROUTINE_STATE_WAITING:
            return meow_true;
        case MEOW_COROUTINE_STATE_RUNNING:
        case MEOW_COROUTINE_STATE_DEAD:
        default:
            return meow_false;
    }
}

meow_bool_t meow_coroutine_resume(meow_coroutine_t *coroutine)
{
    if (!meow_coroutine_is_resumable(coroutine)) {
        meow_warn("Coroutine can not resumable")
        return meow_false;
    }

    coroutine->state = MEOW_COROUTINE_STATE_RUNNING;
    coroutine->previous = MEOW_COROUTINE_G(current);
    MEOW_COROUTINE_G(current) = coroutine;

    meow_context_swap_in(coroutine->context);

    if (meow_context_is_finished(coroutine->context)) {
        meow_coroutine_execute_defer_tasks();
        MEOW_COROUTINE_G(current) = coroutine->previous;
        coroutine->state = MEOW_COROUTINE_STATE_FINISHED;
        meow_coroutine_close(coroutine);
    }

    return meow_true;
}

meow_bool_t meow_coroutine_yield()
{
    meow_coroutine_t *coroutine = MEOW_COROUTINE_G(current);

    coroutine->state = MEOW_COROUTINE_STATE_WAITING;
    MEOW_COROUTINE_G(current) = coroutine->previous;
    meow_context_swap_out(coroutine->context);

    return meow_true;
}

meow_bool_t meow_coroutine_close(meow_coroutine_t *coroutine)
{
    if (!meow_context_is_finished(coroutine->context) || meow_coroutine_is_alive(coroutine)) {
        meow_warn("Coroutine[%d] is still alive! Unable to close", coroutine->id)
        return meow_false;
    }

    meow_context_free(coroutine->context);
    free(coroutine);

    return meow_true;
}

meow_coroutine_t *meow_coroutine_run(meow_coroutine_func_t func, void *data)
{
    meow_coroutine_t *coroutine;

    coroutine = meow_coroutine_create(func, data);
    if (coroutine == NULL) {
        return NULL;
    }

    if (!meow_coroutine_resume(coroutine)) {
        meow_coroutine_close(coroutine);
        return NULL;
    }

    return coroutine;
}

meow_bool_t meow_coroutine_defer(meow_coroutine_defer_func_t func, void *data)
{
    meow_coroutine_defer_task_t *task;
    meow_coroutine_t *coroutine = MEOW_COROUTINE_G(current);

    task = (meow_coroutine_defer_task_t *) malloc(sizeof(meow_coroutine_defer_task_t));
    if (task == NULL) {
        meow_warn("Create defer task failed")
        return meow_false;
    }

    task->data = data;
    task->func = func;

    MEOW_QUEUE_INSERT_TAIL(&coroutine->defer_tasks, &task->node);

    return meow_true;
}

void meow_coroutine_execute_defer_tasks()
{
    meow_coroutine_defer_task_t *task;
    meow_coroutine_t *coroutine = MEOW_COROUTINE_G(current);

    while ((task = MEOW_QUEUE_FRONT_DATA(&coroutine->defer_tasks, meow_coroutine_defer_task_t, node))) {
        MEOW_QUEUE_REMOVE(&task->node);
        task->func(task->data);
        free(task);
    }
}

static void meow_coroutine_sleep_timeout_func(uv_timer_t *timer)
{
    meow_coroutine_resume((meow_coroutine_t *) timer->data);
}

meow_bool_t meow_coroutine_sleep(long seconds)
{
    uv_timer_t *timer;
    meow_coroutine_t *coroutine = MEOW_COROUTINE_G(current);

    if (seconds < 0) {
        return meow_coroutine_yield();
    }

    timer = (uv_timer_t *) malloc(sizeof(uv_timer_t));
    assert(timer != NULL);

    timer->data = coroutine;

    uv_timer_init(uv_default_loop(), timer);
    uv_timer_start(timer, meow_coroutine_sleep_timeout_func, seconds * 1000, 0);

    meow_coroutine_yield();

    uv_timer_stop(timer);
    free(timer);

    return meow_true;
}
