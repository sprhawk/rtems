/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (C) 2019 embedded brains GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <rtems/score/smpimpl.h>
#include <rtems/score/atomic.h>
#include <rtems/score/threaddispatch.h>
#include <rtems/sysinit.h>
#include <rtems.h>

#include <string.h>

#include <t.h>
#include <tmacros.h>

#define CPU_COUNT 32

const char rtems_test_name[] = "SMPMULTICAST 1";

static const T_config config = {
  .name = "SMPMultiCast",
  .putchar = T_putchar_default,
  .verbosity = T_VERBOSE,
  .now = T_now
};

typedef struct {
  Atomic_Uint id[CPU_COUNT];
} test_context;

static test_context test_instance;

static void multicast_action_irq_disabled(
  const Processor_mask *targets,
  SMP_Action_handler handler,
  void *arg
)
{
  rtems_interrupt_level level;

  rtems_interrupt_local_disable(level);
  _SMP_Multicast_action(targets, handler, arg);
  rtems_interrupt_local_enable(level);
}

static void multicast_action_dispatch_disabled(
  const Processor_mask *targets,
  SMP_Action_handler handler,
  void *arg
)
{
  Per_CPU_Control *cpu_self;

  cpu_self = _Thread_Dispatch_disable();
  _SMP_Multicast_action(targets, handler, arg);
  _Thread_Dispatch_enable(cpu_self);
}

static void action(void *arg)
{
  test_context *ctx;
  uint32_t self;
  unsigned expected;
  bool success;

  ctx = arg;
  self = rtems_scheduler_get_processor();
  expected = 0;
  success = _Atomic_Compare_exchange_uint(
    &ctx->id[self],
    &expected,
    self + 1,
    ATOMIC_ORDER_RELAXED,
    ATOMIC_ORDER_RELAXED
  );
  T_quiet_true(success, "set CPU identifier failed");
}

static void test_unicast(
  test_context *ctx,
  void (*multicast_action)(const Processor_mask *, SMP_Action_handler, void *)
)
{
  uint32_t step;
  uint32_t i;
  uint32_t n;

  T_plan(1);
  step = 0;
  n = rtems_scheduler_get_processor_maximum();

  for (i = 0; i < n; ++i) {
    Processor_mask cpus;
    uint32_t j;

    memset(ctx, 0, sizeof(*ctx));

    _Processor_mask_Zero(&cpus);
    _Processor_mask_Set(&cpus, i);
    (*multicast_action)(&cpus, action, ctx);

    for (j = 0; j < n; ++j) {
      unsigned id;

      ++step;
      id = _Atomic_Load_uint(&ctx->id[j], ATOMIC_ORDER_RELAXED);

      if (j == i) {
        T_quiet_eq_uint(j + 1, id);
      } else {
        T_quiet_eq_uint(0, id);
      }
    }
  }

  T_step_eq_u32(0, step, n * n);
}

static void test_broadcast(
  test_context *ctx,
  void (*multicast_action)(const Processor_mask *, SMP_Action_handler, void *)
)
{
  uint32_t step;
  uint32_t i;
  uint32_t n;

  T_plan(1);
  step = 0;
  n = rtems_scheduler_get_processor_maximum();

  for (i = 0; i < n; ++i) {
    uint32_t j;

    memset(ctx, 0, sizeof(*ctx));

    (*multicast_action)(NULL, action, ctx);

    for (j = 0; j < n; ++j) {
      unsigned id;

      ++step;
      id = _Atomic_Load_uint(&ctx->id[j], ATOMIC_ORDER_RELAXED);
      T_quiet_eq_uint(j + 1, id);
    }
  }

  T_step_eq_u32(0, step, n * n);
}

static void test_before_multitasking(void)
{
  test_context *ctx;

  ctx = &test_instance;

  T_case_begin("UnicastBeforeMultitasking", NULL);
  test_unicast(ctx, _SMP_Multicast_action);
  T_case_end();

  T_case_begin("UnicastBeforeMultitaskingIRQDisabled", NULL);
  test_unicast(ctx, multicast_action_irq_disabled);
  T_case_end();

  T_case_begin("UnicastBeforeMultitaskingDispatchDisabled", NULL);
  test_unicast(ctx, multicast_action_dispatch_disabled);
  T_case_end();

  T_case_begin("BroadcastBeforeMultitasking", NULL);
  test_broadcast(ctx, _SMP_Multicast_action);
  T_case_end();

  T_case_begin("BroadcastBeforeMultitaskingIRQDisabled", NULL);
  test_broadcast(ctx, multicast_action_irq_disabled);
  T_case_end();

  T_case_begin("BroadcastBeforeMultitaskingDispatchDisabled", NULL);
  test_broadcast(ctx, multicast_action_dispatch_disabled);
  T_case_end();
}

static void after_drivers(void)
{
  TEST_BEGIN();
  T_run_initialize(&config);
  test_before_multitasking();
}

RTEMS_SYSINIT_ITEM(
  after_drivers,
  RTEMS_SYSINIT_DEVICE_DRIVERS,
  RTEMS_SYSINIT_ORDER_LAST
);

static void set_wrong_cpu_state(void *arg)
{
  Per_CPU_Control *cpu_self;

  cpu_self = arg;
  T_step_eq_ptr(0, cpu_self, _Per_CPU_Get());
  cpu_self->state = 123;

  while (true) {
    /* Do nothing */
  }
}

static void test_wrong_cpu_state_to_perform_jobs(void)
{
  Per_CPU_Control *cpu_self;
  rtems_interrupt_level level;
  Processor_mask targets;
  uint32_t cpu_index;

  T_case_begin("WrongCPUStateToPerformJobs", NULL);
  T_plan(4);
  cpu_self = _Thread_Dispatch_disable();

  cpu_index = _Per_CPU_Get_index(cpu_self);
  cpu_index = (cpu_index + 1) % rtems_scheduler_get_processor_maximum();
  _Processor_mask_Zero(&targets);
  _Processor_mask_Set(&targets, cpu_index);

  rtems_interrupt_local_disable(level);

  _SMP_Multicast_action(
    &targets,
    set_wrong_cpu_state,
    _Per_CPU_Get_by_index(cpu_index)
  );

  /* If everything is all right, we don't end up here */
  rtems_interrupt_local_enable(level);
  _Thread_Dispatch_enable(cpu_self);
  rtems_fatal(RTEMS_FATAL_SOURCE_APPLICATION, 0);
}

static void Init(rtems_task_argument arg)
{
  test_context *ctx;

  ctx = &test_instance;

  T_case_begin("UnicastDuringMultitasking", NULL);
  test_unicast(ctx, _SMP_Multicast_action);
  T_case_end();

  T_case_begin("UnicastDuringMultitaskingIRQDisabled", NULL);
  test_unicast(ctx, multicast_action_irq_disabled);
  T_case_end();

  T_case_begin("UnicastDuringMultitaskingDispatchDisabled", NULL);
  test_unicast(ctx, multicast_action_dispatch_disabled);
  T_case_end();

  T_case_begin("BroadcastDuringMultitasking", NULL);
  test_broadcast(ctx, _SMP_Multicast_action);
  T_case_end();

  T_case_begin("BroadcastDuringMultitaskingIRQDisabled", NULL);
  test_broadcast(ctx, multicast_action_irq_disabled);
  T_case_end();

  T_case_begin("BroadcastDuringMultitaskingDispatchDisabled", NULL);
  test_broadcast(ctx, multicast_action_dispatch_disabled);
  T_case_end();

  if (rtems_scheduler_get_processor_maximum() > 1) {
    test_wrong_cpu_state_to_perform_jobs();
  } else {
    rtems_fatal(RTEMS_FATAL_SOURCE_APPLICATION, 0);
  }
}

static void fatal_extension(
  rtems_fatal_source source,
  bool always_set_to_false,
  rtems_fatal_code code
)
{
  bool ok;

  if (source == RTEMS_FATAL_SOURCE_SMP) {
    T_step_eq_int(1, source, RTEMS_FATAL_SOURCE_SMP);
    T_step_false(2, always_set_to_false, "unexpected argument value");
    T_step_eq_int(3, code, SMP_FATAL_WRONG_CPU_STATE_TO_PERFORM_JOBS);
    T_case_end();

    ok = T_run_finalize();
    rtems_test_assert(ok);
    TEST_END();
  } else if (source == RTEMS_FATAL_SOURCE_APPLICATION) {
    ok = T_run_finalize();
    rtems_test_assert(ok);
    TEST_END();
  }
}

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER

#define CONFIGURE_MAXIMUM_TASKS 1

#define CONFIGURE_MAXIMUM_PROCESSORS CPU_COUNT

#define CONFIGURE_INITIAL_EXTENSIONS \
  { .fatal = fatal_extension }, \
  RTEMS_TEST_INITIAL_EXTENSION

#define CONFIGURE_RTEMS_INIT_TASKS_TABLE

#define CONFIGURE_INIT

#include <rtems/confdefs.h>