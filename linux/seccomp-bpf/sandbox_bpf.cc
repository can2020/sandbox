// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sandbox/linux/seccomp-bpf/sandbox_bpf.h"

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
    suppressLogging_ = true;
    evaluators_.clear();
    setSandboxPolicy(probeEvaluator, NULL);
    setProcFd(proc_fd);
    startSandbox();
    if (syscall(__NR_getpid) < 0 && errno == EPERM) {
      syscall(__NR_exit_group, (intptr_t)100);
    }
    die(NULL);
  }

  // In the parent process
  if (sigprocmask(SIG_SETMASK, &oldMask, NULL)) {
    die("sigprocmask() failed");
  }
  int status;
  if (HANDLE_EINTR(waitpid(pid, &status, 0)) != pid) {
    die("waitpid() failed unexpectedly");
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 100;
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
      (void) HANDLE_EINTR(close(task));
    }
    return false;
  }
  return true;
}

void Sandbox::setSandboxPolicy(EvaluateSyscall syscallEvaluator,
                               EvaluateArguments argumentEvaluator) {
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

  // If the architecture doesn't match SECCOMP_ARCH, disallow the
  // system call.
  std::vector<struct sock_filter> program;
  program.push_back((struct sock_filter)
                    BPF_STMT(BPF_LD+BPF_W+BPF_ABS,
                             offsetof(struct arch_seccomp_data, arch)));
  program.push_back((struct sock_filter)
    BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SECCOMP_ARCH, 1, 0));
  program.push_back((struct sock_filter)
    BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ERRNO + SECCOMP_DENY_ERRNO));

  // Grab the system call number, so that we can implement jump tables.
  program.push_back((struct sock_filter)
    BPF_STMT(BPF_LD+BPF_W+BPF_ABS, offsetof(struct arch_seccomp_data, nr)));

  // Evaluate all possible system calls and depending on their
  // exit codes generate a BPF filter.
  // This is very inefficient right now. We need to be much smarter
  // eventually.
  // We currently incur a O(N) overhead on each system call, with N
  // being the number of system calls. It is easy to get this down to
  // O(log_2(M)) with M being the number of system calls that need special
  // treatment.
  EvaluateSyscall evaluateSyscall = evaluators_.begin()->first;
  for (int sysnum = MIN_SYSCALL; sysnum <= MAX_SYSCALL; ++sysnum) {
    ErrorCode err = evaluateSyscall(sysnum);
    int ret;
    switch (err) {
    case SB_INSPECT_ARG_1...SB_INSPECT_ARG_6:
      die("Not implemented");
    case SB_TRAP:
      ret = SECCOMP_RET_TRAP;
      break;
    case SB_ALLOWED:
      ret = SECCOMP_RET_ALLOW;
      break;
    default:
      if (err >= static_cast<ErrorCode>(1) &&
          err <= static_cast<ErrorCode>(4096)) {
        // We limit errno values to a reasonable range. In fact, the Linux ABI
        // doesn't support errno values outside of this range.
        ret = SECCOMP_RET_ERRNO + err;
      } else {
        die("Invalid ErrorCode reported by sandbox system call evaluator");
      }
      break;
    }
    program.push_back((struct sock_filter)
      BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, sysnum, 0, 1));
    program.push_back((struct sock_filter)
      BPF_STMT(BPF_RET+BPF_K, ret));
  }

  // Everything that isn't allowed is forbidden. Eventually, we would
  // like to have a way to log forbidden calls, when in debug mode.
  program.push_back((struct sock_filter)
    BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ERRNO + SECCOMP_DENY_ERRNO));

  // Install BPF filter program
  const struct sock_fprog prog = { program.size(), &program[0] };
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
      prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
    goto filter_failed;
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


bool Sandbox::suppressLogging_          = false;
Sandbox::SandboxStatus Sandbox::status_ = STATUS_UNKNOWN;
int    Sandbox::proc_fd_                = -1;
std::vector<std::pair<Sandbox::EvaluateSyscall,
                      Sandbox::EvaluateArguments> > Sandbox::evaluators_;

}  // namespace