#!/usr/bin/env python3
"""Test the actual Crunch POSIX semaphore and task pool without game assets.

Requires POSIX, g++ and optionally git (--check-baseline). The negative control
uses a unique test label, never Crunch's old global task-pool semaphore names.
"""
from pathlib import Path
import argparse
import resource
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
VENDOR = REPO / 'Libraries/crunch-r319/crnlib'
TEST = r'''
#include "crn_core.h"
#include "crn_threading_pthreads.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
void crnlib_assert(const char* expression,const char* file,unsigned line) { std::cerr<<file<<":"<<line<<" "<<expression<<"\n";std::abort(); }
void crnlib_fail(const char* expression,const char* file,unsigned line) { crnlib_assert(expression,file,line); }
static void NoSignal(int) {}
static void Increment(crnlib::uint64,void* pointer) { static_cast<std::atomic<int>*>(pointer)->fetch_add(1); }
static int StressPools() {
 std::atomic<int> completed(0),failures(0);
 auto exercise=[&] {
  for(int round=0;round<100;++round) {
   crnlib::task_pool pool(2);
   for(int task=0;task<8;++task)if(!pool.queue_task(Increment,0,&completed))++failures;
   pool.join();
  }
 };
 std::thread first(exercise),second(exercise);first.join();second.join();
 return failures || completed!=1600 ? 30 : 0;
}
int main(int argc,char**argv) {
 const std::string label="/og-crunch-test-"+std::to_string(getpid());
 {
  crnlib::semaphore first(label.c_str(),0,4),second(label.c_str(),0,4);
  first.release();
  if(second.wait(0)) { std::cerr<<"FAIL: independent semaphores consumed one another's token\n";return 10; }
  if(!first.wait(0))return 11;
 }
 if(argc>1 && !strcmp(argv[1],"--isolation-only"))return 0;
 {
  crnlib::semaphore first(label.c_str(),0,4);int ready[2],go[2];if(pipe(ready)||pipe(go))return 12;
  pid_t child=fork();if(child<0)return 13;
  if(!child) {
   int code=0;
   {crnlib::semaphore second(label.c_str(),0,4);char token='R';if(write(ready[1],&token,1)!=1 || read(go[0],&token,1)!=1)std::abort();if(second.wait(0))code=14;}
   _exit(code);
  }
  char token;if(read(ready[0],&token,1)!=1)return 22;first.release();if(write(go[1],"G",1)!=1)return 23;
  int status;waitpid(child,&status,0);close(ready[0]);close(ready[1]);close(go[0]);close(go[1]);
  if(!WIFEXITED(status)||WEXITSTATUS(status))return 15;
  if(!first.wait(0))return 16;
 }
 {
  crnlib::semaphore empty(label.c_str(),0,1);alarm(3);
  auto start=std::chrono::steady_clock::now();if(empty.wait(20))return 17;
  auto elapsed=std::chrono::steady_clock::now()-start;
  unsigned remaining=alarm(0);if(!remaining || elapsed<std::chrono::milliseconds(15) || elapsed>std::chrono::seconds(1))return 18;
  struct sigaction action{};action.sa_handler=NoSignal;sigemptyset(&action.sa_mask);sigaction(SIGUSR1,&action,nullptr);
  std::atomic<bool> entered(false),success(false);
  std::thread waiter([&]{entered=true;success=empty.wait();});
  while(!entered)std::this_thread::yield();std::this_thread::sleep_for(std::chrono::milliseconds(10));
  pthread_kill(waiter.native_handle(),SIGUSR1);std::this_thread::sleep_for(std::chrono::milliseconds(10));empty.release();waiter.join();if(!success)return 19;
 }
 pid_t child=fork();if(child<0)return 20;if(!child)_exit(StressPools());
 int result=StressPools(),status;waitpid(child,&status,0);
 if(result || !WIFEXITED(status)||WEXITSTATUS(status))return 21;
 std::cout<<"PASS: same-label instance/process isolation, finite waits preserve alarms, EINTR retry, 400 concurrent task-pool lifecycles / 3200 tasks\n";
}
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check-baseline', action='store_true')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='overgrowth-crunch-threading-') as directory:
        root = Path(directory)
        test = root / 'test.cpp'
        test.write_text(TEST)
        flags = ['g++', '-std=c++11', '-O1', '-g', '-pthread', '-fno-strict-aliasing', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(VENDOR), '-I' + str(VENDOR.parent / 'inc')]
        def compile_test(threading, output):
            subprocess.run([*flags, str(test), str(threading), str(VENDOR / 'crn_timer.cpp'), '-o', str(output)], check=True)
        current = root / 'current'
        compile_test(VENDOR / 'crn_threading_pthreads.cpp', current)
        subprocess.run([str(current)], check=True, timeout=25)
        if args.check_baseline:
            original = root / 'original.cpp'
            original.write_text(subprocess.check_output(['git', 'show', 'HEAD:Libraries/crunch-r319/crnlib/crn_threading_pthreads.cpp'], cwd=REPO, text=True))
            baseline = root / 'baseline'
            compile_test(original, baseline)
            result = subprocess.run([str(baseline), '--isolation-only'], timeout=5)
            if result.returncode != 10:
                raise RuntimeError(f'Expected original semaphore isolation failure (10), got {result.returncode}')
            print('PASS: original implementation fails the isolation negative control deterministically')

if __name__ == '__main__':
    main()
