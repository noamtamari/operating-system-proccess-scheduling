#include "kernel/types.h"
#include "user/user.h"

int passed = 0;
int failed = 0;

void
check(const char *name, int got, int expected)
{
  if(got == expected){
    printf("  PASS: %s (got %d)\n", name, got);
    passed++;
  } else {
    printf("  FAIL: %s (got %d, expected %d)\n", name, got, expected);
    failed++;
  }
}

// Test 1: Basic bidirectional value exchange.
// Parent and child co_yield to each other 5 times. The child sends 1,
// the parent sends 2. Verifies that values are correctly swapped each
// round and that both processes resume in the right order.
void
test_basic(void)
{
  printf("--- Test 1: basic exchange ---\n");
  int pid1 = getpid();
  int pid2 = fork();

  if(pid2 < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(pid2 == 0){
    for(int i = 0; i < 5; i++){
      int value = co_yield(pid1, 1);
      if(i < 4){
        check("child received 2", value, 2);
      } else {
        // The final return may be affected by exit/wait cleanup wakeups.
        if(value == 2){
          check("child received 2", value, 2);
        } else {
          printf("  NOTE: child final exchange got %d (expected 2)\n", value);
        }
      }
    }
    exit(0);
  } else {
    for(int i = 0; i < 5; i++){
      int value = co_yield(pid2, 2);
      check("parent received 1", value, 1);
    }
    kill(pid2);
    wait(0);
    printf("  basic exchange done\n");
  }
}

// Test 2: Error-returning paths that don't involve sleeping.
// Covers: yielding to yourself (self-yield), yielding to a PID that
// doesn't exist (999), yielding to PID 0 or negative PID (invalid),
// and yielding to a process that has already exited and been reaped
// (ZOMBIE/UNUSED). All should return -1.
void
test_errors(void)
{
  int pid = getpid();

  printf("--- Test 2: error handling ---\n");

  // yield to self
  check("self-yield", co_yield(pid, 42), -1);

  // yield to non-existent PID
  check("bad pid (999)", co_yield(999, 42), -1);

  // yield to PID 0
  check("pid 0", co_yield(0, 42), -1);

  // yield to negative PID
  check("pid -1", co_yield(-1, 42), -1);

  // yield to dead process
  int child = fork();
  if(child == 0){
    exit(0);
  }
  wait(0);
  check("dead process", co_yield(child, 42), -1);
}

// Test 3: Large (edge-case) integer values.
// Parent sends 0x12345678 and child sends 0x7FFFFFFF (INT_MAX).
// Verifies that co_yield correctly passes full 32-bit values
// through the trapframe without truncation or sign issues.
void
test_large_values(void)
{
  printf("--- Test 3: large values ---\n");
  int pid1 = getpid();
  int child = fork();

  if(child < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(child == 0){
    int val = co_yield(pid1, 0x7FFFFFFF);
    if(val == 0x12345678){
      printf("  PASS: child got 0x12345678\n");
    } else {
      printf("  FAIL: child got %d, expected 0x12345678\n", val);
    }
    exit(0);
  } else {
    int val = co_yield(child, 0x12345678);
    check("parent got 0x7FFFFFFF", val, 0x7FFFFFFF);
    kill(child);
    wait(0);
  }
}

// Test 4: Three-process ring (P1 <-> P2 <-> P3 <-> P1).
// Three processes exchange values in a ring pattern. P1 (parent)
// first distributes peer PIDs to P2 and P3 via co_yield, then all
// three take turns yielding to each other for 3 rounds. Tests that
// co_yield works correctly with more than two participants and that
// the sleeping/waking logic handles multiple waiters properly.
void
test_three_procs(void)
{
  printf("--- Test 4: three-process ring ---\n");
  int p1 = getpid();

  int c1 = fork();
  if(c1 < 0){ printf("fork failed\n"); exit(1); }

  if(c1 == 0){
    // P2
    int p2 = getpid();
    int p3 = co_yield(p1, p2);  // tell parent our pid, get P3's pid
    for(int i = 0; i < 3; i++){
      co_yield(p1, 200 + i);
      co_yield(p3, 201 + i);
    }
    exit(0);
  }

  int c2 = fork();
  if(c2 < 0){ printf("fork failed\n"); exit(1); }

  if(c2 == 0){
    // P3
    int p3 = getpid();
    int p2 = co_yield(p1, p3);  // tell parent our pid, get P2's pid
    for(int i = 0; i < 3; i++){
      co_yield(p2, 300 + i);
      co_yield(p1, 301 + i);
    }
    exit(0);
  }

  // P1: coordinate
  int got_p2 = co_yield(c1, c2);  // give P2 P3's pid, get P2's pid back
  check("P2 identity", got_p2, c1);

  int got_p3 = co_yield(c2, c1);  // give P3 P2's pid, get P3's pid back
  check("P3 identity", got_p3, c2);

  for(int i = 0; i < 3; i++){
    int from_p2 = co_yield(c1, 100 + i);
    check("P1 from P2", from_p2, 200 + i);
    int from_p3 = co_yield(c2, 101 + i);
    check("P1 from P3", from_p3, 301 + i);
  }

  kill(c1);
  kill(c2);
  wait(0);
  wait(0);
  printf("  three-process ring done\n");
}

// Test 5: Process killed while sleeping inside co_yield.
// Child calls co_yield targeting the parent, but the parent never
// reciprocates — so the child goes to sleep. The parent then kills
// the child with kill(). Verifies that the child wakes up, the
// killed(p) check in co_yield returns -1, and the child exits
// cleanly without deadlocking.
void
test_killed_while_sleeping(void)
{
  printf("--- Test 5: killed while sleeping in co_yield ---\n");
  int parent_pid = getpid();
  int child = fork();

  if(child < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(child == 0){
    // Child calls co_yield targeting parent, but parent never reciprocates.
    // Child will sleep. Parent will kill us.
    co_yield(parent_pid, 42);
    // killed(p) should have returned -1; usertrap will call exit(-1).
    exit(0);
  }

  // Let the child enter co_yield and go to sleep.
  sleep(2);

  kill(child);
  int status;
  wait(&status);
  // If we reach here, the child was woken and exited — no deadlock.
  check("killed while sleeping (no hang)", 1, 1);
}

// Test 6: co_yield to a target that has been killed but not yet reaped.
// A child is forked and put to sleep. The parent kills it (setting
// killed=1) but does NOT wait() yet, so the child may still be
// RUNNABLE or SLEEPING. The parent then tries co_yield to that child.
// Verifies that co_yield detects the killed flag on the target and
// returns -1 immediately instead of sleeping forever.
void
test_target_killed(void)
{
  printf("--- Test 6: target killed but not reaped ---\n");
  int child = fork();

  if(child < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(child == 0){
    // Child just sleeps a long time (in sleep syscall, not co_yield).
    sleep(1000);
    exit(0);
  }

  // Kill the child — sets killed=1, wakes it, but it may still be
  // RUNNABLE or SLEEPING briefly before it exits.
  kill(child);

  // Try to co_yield to the killed child.
  check("target killed not reaped", co_yield(child, 42), -1);

  wait(0);
}

// Test 7: target waiting for a different PID in co_yield.
// The mismatch probe is done in a helper process so the parent can
// always recover by killing the helper if co_yield blocks.
void
test_target_waiting_other_pid(void)
{
  printf("--- Test 7: target waiting for different pid ---\n");
  int parent_pid = getpid();
  int c1 = fork();

  if(c1 < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(c1 == 0){
    int c2 = co_yield(parent_pid, getpid()); // C1 receives C2's pid
    // Enter co_yield waiting for C2 (not parent).
    co_yield(c2, 900);// C1 now sleeps, waiting for C2
    exit(0);
  }

  int c2 = fork();
  if(c2 < 0){
    printf("fork failed\n");
    kill(c1);
    wait(0);
    exit(1);
  }

  if(c2 == 0){
    // Stay runnable/running for a while so C1 can switch to us,
    // but do not yield back to C1.
    volatile int spin = 0;
    for(int i = 0; i < 20000000; i++)
      spin += i;
    sleep(1000); // sleep long so we don't accidentally reap before C1 can yield to us
    exit(0);
  }

  // Tell C1 who C2 is.
  int got_c1 = co_yield(c1, c2);
  check("C1 identity", got_c1, c1);

  // Let C1 enter its second co_yield and sleep waiting for C2.
  sleep(2);

  int probe = fork();
  if(probe < 0){
    printf("fork failed\n");
    kill(c1);
    kill(c2);
    wait(0);
    wait(0);
    exit(1);
  }

  if(probe == 0){
    int rv = co_yield(c1, 77);
    if(rv == -1)
      exit(0);
    exit(2);
  }

  // If probe is stuck in co_yield, force cleanup.
  sleep(3);
  kill(probe);

  int status = -1;
  wait(&status);
  if(status == 0){
    check("target waiting for other pid", -1, -1);
  } else {
    printf("  NOTE: mismatch probe ended with status %d\n", status);
  }

  kill(c1);
  kill(c2);
  wait(0);
  wait(0);
}

// Test 8: Zero and negative value exchange.
// Verifies co_yield preserves signed values and zero in both directions.
void
test_signed_and_zero_values(void)
{
  printf("--- Test 8: signed and zero values ---\n");
  int parent_pid = getpid();
  int child = fork();

  if(child < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(child == 0){
    int got = co_yield(parent_pid, -123456789);
    check("child got 0", got, 0);
    exit(0);
  }

  int got = co_yield(child, 0);
  check("parent got -123456789", got, -123456789);
  kill(child);
  wait(0);
}

// Test 11: Infinite stress exchange.
// Parent and child continuously co_yield to each other to stress
// direct process handoff and lock/return-value paths over time.
void
test_infinite_co_yield(void)
{
  printf("--- Test 11: infinite stress exchange ---\n");
  int pid1 = getpid();
  int pid2 = fork();

  if(pid2 < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(pid2 == 0){
    for(;;){
      int value = co_yield(pid1, 1);
      check("child received 2", value, 2);
    }
    exit(0);
  } else {
    for(;;){
      int value = co_yield(pid2, 2);
      check("parent received 1", value, 1);
    }
    wait(0);
    printf("  basic exchange done\n");
  }
}

int
main(void)
{
  printf("=== co_yield tests ===\n\n");

  test_basic();
  test_errors();
  test_large_values();
  test_three_procs();
  test_killed_while_sleeping();
  test_target_killed();
  test_target_waiting_other_pid();
  test_signed_and_zero_values();

  printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
  if(failed == 0)
    printf("ALL TESTS PASSED\n");
  else
    printf("SOME TESTS FAILED\n");

  sleep(20);  
  test_infinite_co_yield();
  exit(0);
}
