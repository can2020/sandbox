// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sandbox/linux/seccomp-bpf/sandbox_bpf.h"
#include "sandbox/linux/seccomp-bpf/verifier.h"

// The kernel gives us a sandbox, we turn it into a playground :-)
// This is version 2 of the playground; version 1 was built on top of
// pre-BPF seccomp mode.
namespace playground2 {

Sandbox::ErrorCode Sandbox::probeEvaluator(int signo) {
  switch (signo) {
  case __NR_getpid:
    // Return EPERM so that we can check that the filter actually ran.
    return (ErrorCode)EPERM;
  case __NR_exit_group:
    // Allow exit() with a non-default return code.
    return SB_ALLOWED;
  default:
    // Make everything else fail in an easily recognizable way.
    return (ErrorCode)EINVAL;
  }
}

bool Sandbox::kernelSupportSeccompBPF(int proc_fd) {
  // Block all signals before forking a child process. This prevents an
  // attacker from manipulating our test by sending us an unexpected signal.
  sigset_t oldMask, newMask;
  if (sigfillset(&newMask) ||
      sigprocmask(SIG_BLOCK, &newMask, &oldMask)) {
    die("sigprocmask() failed");
  }
  int fds[2];
  if (pipe2(fds, O_NONBLOCK|O_CLOEXEC)) {
    die("pipe() failed");
  }

  pid_t pid = fork();
  if (pid < 0) {
    // Die if we cannot fork(). We would probably fail a little later
    // anyway, as the machine is likely very close to running out of
    // memory.
    // But what we don't want to do is return "false", as a crafty
    // attacker might cause fork() to fail at will and could trick us
    // into running without a sandbox.
    sigprocmask(SIG_SETMASK, &oldMask, NULL);  // OK, if it fails
    die("fork() failed unexpectedly");
  }

  // In the child process
  if (!pid) {
    // Test a very simple sandbox policy to verify that we can
    // successfully turn on sandboxing.
    dryRun_ = true;
    if (HANDLE_EINTR(close(fds[0])) ||
        dup2(fds[1], 2) != 2 ||
        HANDLE_EINTR(close(fds[1]))) {
      static const char msg[] = "Failed to set up stderr\n";
      if (HANDLE_EINTR(write(fds[1], msg, sizeof(msg)-1))) { }
    } else {
      evaluators_.clear();
      setSandboxPolicy(probeEvaluator, NULL);
      setProcFd(proc_fd);
      startSandbox();
      if (syscall(__NR_getpid) < 0 && errno == EPERM) {
        syscall(__NR_exit_group, (intptr_t)100);
      }
    }
    die(NULL);
  }

  // In the parent process
  if (HANDLE_EINTR(close(fds[1]))) {
    die("close() failed");
  }
  if (sigprocmask(SIG_SETMASK, &oldMask, NULL)) {
    die("sigprocmask() failed");
  }
  int status;
  if (HANDLE_EINTR(waitpid(pid, &status, 0)) != pid) {
    die("waitpid() failed unexpectedly");
  }
  bool rc = WIFEXITED(status) && WEXITSTATUS(status) == 100;

  // If we fail to support sandboxing, there might be an additional
  // error message. If so, this was an entirely unexpected and fatal
  // failure. We should report the failure and somebody most fix
  // things. This is probably a security-critical bug in the sandboxing
  // code.
  if (!rc) {
    char buf[4096];
    ssize_t len = HANDLE_EINTR(read(fds[0], buf, sizeof(buf) - 1));
    if (len > 0) {
      while (len > 1 && buf[len-1] == '\n') {
        --len;
      }
      buf[len] = '\000';
      die(buf);
    }
  }
  if (HANDLE_EINTR(close(fds[0]))) {
    die("close() failed");
  }

  return rc;
}

Sandbox::SandboxStatus Sandbox::supportsSeccompSandbox(int proc_fd) {
  // It the sandbox is currently active, we clearly must have support for
  // sandboxing.
  if (status_ == STATUS_ENABLED) {
    return status_;
  }

  // Even if the sandbox was previously available, something might have
  // changed in our run-time environment. Check one more time.
  if (status_ == STATUS_AVAILABLE) {
    if (!isSingleThreaded(proc_fd)) {
      status_ = STATUS_UNAVAILABLE;
    }
    return status_;
  }

  if (status_ == STATUS_UNAVAILABLE && isSingleThreaded(proc_fd)) {
    // All state transitions resulting in STATUS_UNAVAILABLE are immediately
    // preceded by STATUS_AVAILABLE. Furthermore, these transitions all
    // happen, if and only if they are triggered by the process being multi-
    // threaded.
    // In other words, if a single-threaded process is currently in the
    // STATUS_UNAVAILABLE state, it is safe to assume that sandboxing is
    // actually available.
    status_ = STATUS_AVAILABLE;
    return status_;
  }

  // If we have not previously checked for availability of the sandbox or if
  // we otherwise don't believe to have a good cached value, we have to
  // perform a thorough check now.
  if (status_ == STATUS_UNKNOWN) {
    status_ = kernelSupportSeccompBPF(proc_fd)
      ? STATUS_AVAILABLE : STATUS_UNSUPPORTED;

    // As we are performing our tests from a child process, the run-time
    // environment that is visible to the sandbox is always guaranteed to be
    // single-threaded. Let's check here whether the caller is single-
    // threaded. Otherwise, we mark the sandbox as temporarily unavailable.
    if (status_ == STATUS_AVAILABLE && !isSingleThreaded(proc_fd)) {
      status_ = STATUS_UNAVAILABLE;
    }
  }
  return status_;
}

void Sandbox::setProcFd(int proc_fd) {
  proc_fd_ = proc_fd;
}

void Sandbox::startSandbox() {
  if (status_ == STATUS_UNSUPPORTED || status_ == STATUS_UNAVAILABLE) {
    die("Trying to start sandbox, even though it is known to be unavailable");
  } else if (status_ == STATUS_ENABLED) {
    die("Cannot start sandbox recursively. Use multiple calls to "
        "setSandboxPolicy() to stack policies instead");
  }
  if (proc_fd_ < 0) {
    proc_fd_ = open("/proc", O_RDONLY|O_DIRECTORY);
  }
  if (proc_fd_ < 0) {
    // For now, continue in degraded mode, if we can't access /proc.
    // In the future, we might want to tighten this requirement.
  }
  if (!isSingleThreaded(proc_fd_)) {
    die("Cannot start sandbox, if process is already multi-threaded");
  }

  // We no longer need access to any files in /proc. We want to do this
  // before installing the filters, just in case that our policy denies
  // close().
  if (proc_fd_ >= 0) {
    if (HANDLE_EINTR(close(proc_fd_))) {
      die("Failed to close file descriptor for /proc");
    }
    proc_fd_ = -1;
  }

  // Install the filters.
  installFilter();

  // We are now inside the sandbox.
  status_ = STATUS_ENABLED;
}

bool Sandbox::isSingleThreaded(int proc_fd) {
  if (proc_fd < 0) {
    // Cannot determine whether program is single-threaded. Hope for
    // the best...
    return true;
  }

  struct stat sb;
  int task = -1;
  if ((task = openat(proc_fd, "self/task", O_RDONLY|O_DIRECTORY)) < 0 ||
      fstat(task, &sb) != 0 ||
      sb.st_nlink != 3 ||
      HANDLE_EINTR(close(task))) {
    if (task >= 0) {
      if (HANDLE_EINTR(close(task))) { }
    }
    return false;
  }
  return true;
}

static bool isDenied(Sandbox::ErrorCode code) {
  return code == Sandbox::SB_TRAP ||
        (code >= (Sandbox::ErrorCode)1 &&
         code <= (Sandbox::ErrorCode)4095);  // errno value
}

void Sandbox::policySanityChecks(EvaluateSyscall syscallEvaluator,
                                 EvaluateArguments) {
  // Do some sanity checks on the policy. This will warn users if they do
  // things that are likely unsafe and unintended.
  // We also have similar checks later, when we actually compile the BPF
  // program. That catches problems with incorrectly stacked evaluators.
  if (!isDenied(syscallEvaluator(-1))) {
    die("Negative system calls should always be disallowed by policy");
  }
#ifndef NDEBUG
#if defined(__i386__) || defined(__x86_64__)
#if defined(__x86_64__) && defined(__ILP32__)
  for (unsigned int sysnum = MIN_SYSCALL & ~0x40000000u;
       sysnum <= (MAX_SYSCALL & ~0x40000000u);
       ++sysnum) {
    if (!isDenied(syscallEvaluator(sysnum))) {
      die("In x32 mode, you should not allow any non-x32 system calls");
    }
  }
#else
  for (unsigned int sysnum = MIN_SYSCALL | 0x40000000u;
       sysnum <= (MAX_SYSCALL | 0x40000000u);
       ++sysnum) {
    if (!isDenied(syscallEvaluator(sysnum))) {
      die("x32 system calls should be explicitly disallowed");
    }
  }
#endif
#endif
#endif
  // Check interesting boundary values just outside of the valid system call
  // range: 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, MIN_SYSCALL-1, MAX_SYSCALL+1.
  // They all should be denied.
  if (!isDenied(syscallEvaluator(std::numeric_limits<int>::max())) ||
      !isDenied(syscallEvaluator(std::numeric_limits<int>::min())) ||
      !isDenied(syscallEvaluator(-1)) ||
      !isDenied(syscallEvaluator(static_cast<int>(MIN_SYSCALL) - 1)) ||
      !isDenied(syscallEvaluator(static_cast<int>(MAX_SYSCALL) + 1))) {
    die("Even for default-allow policies, you must never allow system calls "
        "outside of the standard system call range");
  }
  return;
}

void Sandbox::setSandboxPolicy(EvaluateSyscall syscallEvaluator,
                               EvaluateArguments argumentEvaluator) {
  policySanityChecks(syscallEvaluator, argumentEvaluator);
  evaluators_.push_back(std::make_pair(syscallEvaluator, argumentEvaluator));
}

void Sandbox::installFilter() {
  // Verify that the user pushed a policy.
  if (evaluators_.empty()) {
  filter_failed:
    die("Failed to configure system call filters");
  }

  // Set new SIGSYS handler
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = &sigSys;
  sa.sa_flags = SA_SIGINFO;
  if (sigaction(SIGSYS, &sa, NULL) < 0) {
    goto filter_failed;
  }

  // Unmask SIGSYS
  sigset_t mask;
  if (sigemptyset(&mask) ||
      sigaddset(&mask, SIGSYS) ||
      sigprocmask(SIG_UNBLOCK, &mask, NULL)) {
    goto filter_failed;
  }

  // We can't handle stacked evaluators, yet. We'll get there eventually
  // though. Hang tight.
  if (evaluators_.size() != 1) {
    die("Not implemented");
  }

  // Assemble the BPF filter program.
  Program *program = new Program();
  if (!program) {
    die("Out of memory");
  }

  // If the architecture doesn't match SECCOMP_ARCH, disallow the
  // system call.
  program->push_back((struct sock_filter)
    BPF_STMT(BPF_LD+BPF_W+BPF_ABS, offsetof(struct arch_seccomp_data, arch)));
  program->push_back((struct sock_filter)
    BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SECCOMP_ARCH, 1, 0));

  // TODO: Instead of killing outright, we should raise a SIGSYS and
  //       report a useful error message. SIGKILL cannot be trapped by the
  //       debugger and essentially makes the program fail in a way that is
  //       almost impossible to debug.
  program->push_back((struct sock_filter)
    BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_KILL));

  // Grab the system call number, so that we can implement jump tables.
  program->push_back((struct sock_filter)
    BPF_STMT(BPF_LD+BPF_W+BPF_ABS, offsetof(struct arch_seccomp_data, nr)));

  // On Intel architectures, verify that system call numbers are in the
  // expected number range. The older i386 and x86-64 APIs clear bit 30
  // on all system calls. The newer x86-32 API always sets bit 30.
#if defined(__i386__) || defined(__x86_64__)
#if defined(__x86_64__) && defined(__ILP32__)
  program->push_back((struct sock_filter)
    BPF_JUMP(BPF_JMP+BPF_JSET+BPF_K, 0x40000000, 1, 0));
#else
  program->push_back((struct sock_filter)
    BPF_JUMP(BPF_JMP+BPF_JSET+BPF_K, 0x40000000, 0, 1));
#endif
  // TODO: raise a suitable SIGSYS signal
  program->push_back((struct sock_filter)
    BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_KILL));
#endif

  // Evaluate all possible system calls and group their ErrorCodes into
  // ranges of identical codes.
  Ranges ranges;
  findRanges(&ranges);

  // Compile the system call ranges to an optimized BPF program.
  rangesToBPF(program, ranges);

  // Everything that isn't allowed is forbidden. Eventually, we would
  // like to have a way to log forbidden calls, when in debug mode.
  program->push_back((struct sock_filter)
    BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ERRNO + SECCOMP_DENY_ERRNO));

  // Make sure compilation resulted in BPF program that executes
  // correctly. Otherwise, there is an internal error in our BPF compiler.
  // There is really nothing the caller can do until the bug is fixed.
#ifndef NDEBUG
  const char *err = NULL;
  if (!Verifier::verifyBPF(*program, evaluators_, &err)) {
    die(err);
  }
#endif

  // We want to be very careful in not imposing any requirements on the
  // policies that are set with setSandboxPolicy(). This means, as soon as
  // the sandbox is active, we shouldn't be relying on libraries that could
  // be making system calls. This, for example, means we should avoid
  // using the heap and we should avoid using STL functions.
  // Temporarily copy the contents of the "program" vector into a
  // stack-allocated array; and then explicitly destroy that object.
  // This makes sure we don't ex- or implicitly call new/delete after we
  // installed the BPF filter program in the kernel. Depending on the
  // system memory allocator that is in effect, these operators can result
  // in system calls to things like munmap() or brk().
  struct sock_filter bpf[program->size()];
  const struct sock_fprog prog = { program->size(), bpf };
  memcpy(bpf, &(*program)[0], sizeof(bpf));
  delete program;

  // Install BPF filter program
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
    die(dryRun_ ? NULL : "Kernel refuses to enable no-new-privs");
  } else {
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
      die(dryRun_ ? NULL : "Kernel refuses to turn on BPF filters");
    }
  }

  return;
}

void Sandbox::findRanges(Ranges *ranges) {
  // Please note that "struct seccomp_data" defines system calls as a signed
  // int32_t, but BPF instructions always operate on unsigned quantities. We
  // deal with this disparity by enumerating from MIN_SYSCALL to MAX_SYSCALL,
  // and then verifying that the rest of the number range (both positive and
  // negative) all return the same ErrorCode.
  EvaluateSyscall evaluateSyscall = evaluators_.begin()->first;
  uint32_t oldSysnum              = 0;
  ErrorCode oldErr                = evaluateSyscall(oldSysnum);
  for (uint32_t sysnum = std::max(1u, MIN_SYSCALL);
       sysnum <= MAX_SYSCALL + 1;
       ++sysnum) {
    ErrorCode err = evaluateSyscall(static_cast<int>(sysnum));
    if (err != oldErr) {
      ranges->push_back(Range(oldSysnum, sysnum-1, oldErr));
      oldSysnum = sysnum;
      oldErr    = err;
    }
  }

  // As we looped all the way past the valid system calls (i.e. MAX_SYSCALL+1),
  // "oldErr" should at this point be the "default" policy for all system  call
  // numbers that don't have an explicit handler in the system call evaluator.
  // But as we are quite paranoid, we perform some more sanity checks to verify
  // that there actually is a consistent "default" policy in the first place.
  // We don't actually iterate over all possible 2^32 values, though. We just
  // perform spot checks at the boundaries.
  // The cases that we test are:  0x7FFFFFFF, 0x80000000, 0xFFFFFFFF.
  if (oldErr != evaluateSyscall(std::numeric_limits<int>::max()) ||
      oldErr != evaluateSyscall(std::numeric_limits<int>::min()) ||
      oldErr != evaluateSyscall(-1)) {
    die("Invalid seccomp policy");
  }
  ranges->push_back(
    Range(oldSysnum, std::numeric_limits<unsigned>::max(), oldErr));
}

void Sandbox::rangesToBPF(Program *program, const Ranges& ranges) {
  // TODO: We currently search linearly through all ranges. An improved
  //       algorithm should be doing a binary search.

  // System call ranges must cover the entire number range.
  if (ranges.empty() ||
      ranges.begin()->from != 0 ||
      ranges.back().to != std::numeric_limits<unsigned>::max()) {
  rangeError:
    die("Invalid set of system call ranges");
  }
  uint32_t from = 0;
  for (Ranges::const_iterator iter = ranges.begin();
       iter != ranges.end();
       ++iter) {
    // Ranges must be contiguous and monotonically increasing.
    if (iter->from > iter->to ||
        iter->from != from) {
      goto rangeError;
    }
    from = iter->to + 1;

    // Convert ErrorCodes to return values that are acceptable for
    // BPF filters.
    int ret;
    switch (iter->err) {
    case SB_INSPECT_ARG_1...SB_INSPECT_ARG_6:
      die("Not implemented");
    case SB_TRAP:
      ret = SECCOMP_RET_TRAP;
      break;
    case SB_ALLOWED:
      ret = SECCOMP_RET_ALLOW;
      break;
    default:
      if (iter->err >= static_cast<ErrorCode>(1) &&
          iter->err <= static_cast<ErrorCode>(4096)) {
        // We limit errno values to a reasonable range. In fact, the Linux ABI
        // doesn't support errno values outside of this range.
        ret = SECCOMP_RET_ERRNO + iter->err;
      } else {
        die("Invalid ErrorCode reported by sandbox system call evaluator");
      }
      break;
    }

    // Emit BPF instructions matching this range.
    if (iter->to != std::numeric_limits<unsigned>::max()) {
      program->push_back((struct sock_filter)
        BPF_JUMP(BPF_JMP+BPF_JGT+BPF_K, iter->to, 1, 0));
    }
    program->push_back((struct sock_filter)
      BPF_STMT(BPF_RET+BPF_K, ret));
  }
  return;
}

void Sandbox::sigSys(int nr, siginfo_t *info, void *void_context) {
  if (nr != SIGSYS || info->si_code != SYS_SECCOMP || !void_context) {
    // die() can call LOG(FATAL). This is not normally async-signal safe
    // and can lead to bugs. We should eventually implement a different
    // logging and reporting mechanism that is safe to be called from
    // the sigSys() handler.
    die("Unexpected SIGSYS received");
  }
  ucontext_t *ctx = reinterpret_cast<ucontext_t *>(void_context);
  int old_errno   = errno;

  // In case of error, set the REG_RESULT CPU register to the default
  // errno value (i.e. EPERM).
  // We need to be very careful when doing this, as some of our target
  // platforms have pointer types and CPU registers that are wider than
  // ints. Furthermore, the kernel ABI requires us to return a negative
  // value, but errno values are usually positive. And in fact, it would
  // be perfectly reasonable for somebody to have defined them as unsigned
  // properties. This makes the correct incantation of type casts rather
  // subtle. Sometimes, C++ is just too smart for its own good.
  void *rc        = (void *)(intptr_t)-(int)SECCOMP_DENY_ERRNO;

  // This is where we can add extra code to handle complex system calls.
  // ...

  ctx->uc_mcontext.gregs[REG_RESULT] = reinterpret_cast<greg_t>(rc);
  errno                              = old_errno;
  return;
}


bool Sandbox::dryRun_                   = false;
Sandbox::SandboxStatus Sandbox::status_ = STATUS_UNKNOWN;
int    Sandbox::proc_fd_                = -1;
Sandbox::Evaluators Sandbox::evaluators_;

}  // namespace
