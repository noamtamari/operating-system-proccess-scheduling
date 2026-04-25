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
      check("child received 2", value, 2);
    }
    exit(0);
  } else {
    for(int i = 0; i < 5; i++){
      int value = co_yield(pid2, 2);
      check("parent received 1", value, 1);
    }
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

// Test 7: child enters co_yield first, then parent responds.
// Verifies that child can block in co_yield until parent is ready.
void
test_child_yields_first(void)
{
  printf("--- Test 7: child yields first ---\n");
  int parent_pid = getpid();
  int child_pid = fork();

  if(child_pid < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(child_pid == 0){
    int value = co_yield(parent_pid, 111);
    if(value == 222){
      printf("  PASS: child received 222\n");
      passed++;
    } else {
      printf("  FAIL: child received %d, expected 222\n", value);
      failed++;
    }
    exit(0);
  }

  sleep(2); // let child enter co_yield first
  int value = co_yield(child_pid, 222);
  check("parent received 111", value, 111);
  wait(0);
}

// Test 8: parent enters co_yield first, then child responds.
// Verifies that parent can block in co_yield until child is ready.
void
test_parent_yields_first(void)
{
  printf("--- Test 8: parent yields first ---\n");
  int parent_pid = getpid();
  int child_pid = fork();

  if(child_pid < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(child_pid == 0){
    sleep(2); // let parent enter co_yield first
    int value = co_yield(parent_pid, 333);
    if(value == 444){
      printf("  PASS: child received 444\n");
      passed++;
    } else {
      printf("  FAIL: child received %d, expected 444\n", value);
      failed++;
    }
    exit(0);
  }

  int value = co_yield(child_pid, 444);
  check("parent received 333", value, 333);
  wait(0);
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
  test_child_yields_first();
  test_parent_yields_first();

  printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
  if(failed == 0)
    printf("ALL TESTS PASSED\n");
  else
    printf("SOME TESTS FAILED\n");
  exit(0);
}
